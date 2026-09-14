// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/portal/portal_bus.hpp>
#include <farland/platform/portal/portal_session.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <random>
#include <sys/eventfd.h>
#include <unistd.h>

// The call sequence follows the xdg-desktop-portal documentation of
// org.freedesktop.portal.RemoteDesktop, .ScreenCast, .Request and .Session
// (RemoteDesktop version 2, ScreenCast version 5).
namespace farland::platform::portal {

using detail::Bus;
using detail::Clock;
using detail::fail;
using detail::MessagePtr;
using detail::MessageReader;
using detail::MessageWriter;
using detail::Options;
using detail::SlotPtr;

namespace {

constexpr std::string_view log_component = "platform.portal";
/// For calls that answer at once (OpenPipeWireRemote, ConnectToEIS).
constexpr auto call_timeout = std::chrono::seconds(30);
/// Keeps `now + timeout` far from overflowing.
constexpr auto max_timeout = std::chrono::hours(24 * 366);

/// ":1.42" becomes "1_42" (org.freedesktop.portal.Request).
std::string sender_path_element(std::string_view unique_name)
{
    std::string element;
    for (const char c : unique_name.substr(unique_name.starts_with(':') ? 1 : 0)) {
        element += c == '.' ? '_' : c;
    }
    return element;
}

/// A handle token: a valid object path element, unique and not guessable.
std::string new_token()
{
    static std::uint32_t counter = 0;
    std::random_device random;
    const std::uint64_t value = (static_cast<std::uint64_t>(random()) << 32U) | random();
    return std::format("farland_{:016x}_{}", value, ++counter);
}

PortalResult<SlotPtr> watch_signal(Bus& bus, const std::string& sender, const std::string& path, const char* interface,
                                   const char* member, sd_bus_message_handler_t callback, void* userdata)
{
    // Asynchronous AddMatch: the bus daemon handles it before the method call
    // that follows on the same connection, so no signal is missed.
    sd_bus_slot* raw = nullptr;
    const int r = sd_bus_match_signal_async(bus.get(), &raw, sender.c_str(), path.c_str(), interface, member, callback,
                                            nullptr, userdata);
    if (r < 0) {
        return fail(PortalErrc::protocol, std::format("cannot subscribe to {}: {}", member, std::strerror(-r)));
    }
    return SlotPtr(raw);
}

/// What a portal request needs from the session.
struct RequestContext {
    Bus* bus = nullptr;
    const std::string* portal_owner = nullptr;
    Clock::time_point deadline;
    int cancel_fd = -1;
    const bool* closed = nullptr;
};

/// Calls a method that answers with a Request handle and waits for the
/// Request's Response signal. On success, the returned message is positioned
/// at the results vardict.
PortalResult<MessagePtr> portal_request(const RequestContext& ctx, const char* interface, const char* method,
                                        const std::string* session, const std::string* parent_window,
                                        const Options& options)
{
    const std::string token = new_token();
    const std::string predicted =
        std::format("{}/request/{}/{}", detail::portal_path, sender_path_element(ctx.bus->unique_name()), token);

    MessagePtr response;
    const auto on_response = [](sd_bus_message* m, void* userdata, sd_bus_error* /*error*/) -> int {
        auto& slot = *static_cast<MessagePtr*>(userdata);
        if (!slot) {
            slot.reset(sd_bus_message_ref(m));
        }
        return 0;
    };
    std::vector<SlotPtr> watches;
    FARLAND_TRY(auto watch, watch_signal(*ctx.bus, *ctx.portal_owner, predicted, detail::request_interface, "Response",
                                         on_response, &response));
    watches.push_back(std::move(watch));

    FARLAND_TRY(auto call, ctx.bus->new_call(interface, method));
    MessageWriter writer(call.get());
    if (session != nullptr) {
        writer.object_path(*session);
    }
    if (parent_window != nullptr) {
        writer.string(*parent_window);
    }
    Options all{{"handle_token", token}};
    all.insert(all.end(), options.begin(), options.end());
    writer.options(all);
    if (writer.status() < 0) {
        return fail(PortalErrc::protocol, std::format("cannot build the {} call", method));
    }
    log::debug(log_component, "calling {}", method);
    FARLAND_TRY(auto reply, ctx.bus->call(call.get(), method, ctx.deadline, ctx.cancel_fd));
    std::string handle;
    if (!MessageReader(reply.get()).string(handle)) {
        return fail(PortalErrc::protocol, std::format("{}: the reply has no request handle", method));
    }
    if (handle != predicted) {
        // xdg-desktop-portal before 0.9 chose the path itself.
        log::debug(log_component, "{}: request handle {} is not the expected {}", method, handle, predicted);
        FARLAND_TRY(auto actual, watch_signal(*ctx.bus, *ctx.portal_owner, handle, detail::request_interface,
                                              "Response", on_response, &response));
        watches.push_back(std::move(actual));
    }

    auto waited = ctx.bus->run_until([&] { return response != nullptr || *ctx.closed; }, ctx.deadline, ctx.cancel_fd);
    if (!waited) {
        auto error = std::move(waited).error();
        if (error.code == PortalErrc::timed_out || error.code == PortalErrc::aborted) {
            // Ends the dialog; the portal sends no Response afterwards.
            if (auto close = ctx.bus->new_call(detail::request_interface, "Close", handle.c_str())) {
                ctx.bus->send(close->get());
            }
        }
        error.message = std::format("{}: {}", method, error.message);
        return std::unexpected(std::move(error));
    }
    if (!response) {
        return fail(PortalErrc::closed, std::format("{}: the portal closed the session", method));
    }
    std::uint32_t code = 0;
    if (!MessageReader(response.get()).u32(code)) {
        return fail(PortalErrc::protocol, std::format("{}: malformed Response signal", method));
    }
    switch (code) {
    case 0:
        return response;
    case 1:
        return fail(PortalErrc::cancelled, std::format("{}: the user cancelled the request", method));
    default:
        return fail(PortalErrc::failed, std::format("{}: the portal ended the request (response {})", method, code));
    }
}

PortalResult<void> read_stream(MessageReader& reader, PortalStream& stream)
{
    if (!reader.u32(stream.node_id)) {
        return fail(PortalErrc::protocol, "Start: malformed stream");
    }
    const bool ok = reader.vardict([&](std::string_view key, std::string_view signature, bool& good) {
        if (key == "id" && signature == "s") {
            good = reader.string(stream.id);
        } else if (key == "mapping_id" && signature == "s") {
            good = reader.string(stream.mapping_id);
        } else if (key == "source_type" && signature == "u") {
            good = reader.u32(stream.source_type);
        } else if ((key == "position" || key == "size") && signature == "(ii)") {
            std::pair<std::int32_t, std::int32_t> value;
            good = reader.int_pair(value);
            (key == "position" ? stream.position : stream.size) = value;
        } else {
            return false;
        }
        return true;
    });
    if (!ok) {
        return fail(PortalErrc::protocol, "Start: malformed stream properties");
    }
    return {};
}

PortalResult<std::vector<PortalStream>> read_streams(MessageReader& reader)
{
    std::vector<PortalStream> streams;
    if (!reader.enter('a', "(ua{sv})")) {
        return fail(PortalErrc::protocol, "Start: malformed streams");
    }
    for (;;) {
        const int r = sd_bus_message_enter_container(reader.get(), 'r', "ua{sv}");
        if (r < 0) {
            return fail(PortalErrc::protocol, "Start: malformed streams");
        }
        if (r == 0) {
            break;
        }
        PortalStream stream;
        FARLAND_TRY_VOID(read_stream(reader, stream));
        if (!reader.exit()) {
            return fail(PortalErrc::protocol, "Start: malformed streams");
        }
        streams.push_back(std::move(stream));
    }
    if (!reader.exit()) {
        return fail(PortalErrc::protocol, "Start: malformed streams");
    }
    return streams;
}

PortalResult<UniqueFd> read_fd(sd_bus_message* reply, std::string_view what)
{
    int borrowed = -1;
    if (!MessageReader(reply).fd(borrowed)) {
        return fail(PortalErrc::protocol, std::format("{}: the reply has no file descriptor", what));
    }
    // The message owns its fds; keep a copy.
    UniqueFd fd(::fcntl(borrowed, F_DUPFD_CLOEXEC, 3));  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (!fd.valid()) {
        return fail(PortalErrc::protocol, std::format("{}: cannot duplicate the fd: {}", what, std::strerror(errno)));
    }
    return fd;
}

std::string describe_stream(const PortalStream& stream)
{
    std::string text = std::format("node {}", stream.node_id);
    if (stream.position) {
        text += std::format(" at {},{}", stream.position->first, stream.position->second);
    }
    if (stream.size) {
        text += std::format(" size {}x{}", stream.size->first, stream.size->second);
    }
    text += std::format(" type {}", stream.source_type);
    if (!stream.mapping_id.empty()) {
        text += std::format(" mapping {}", stream.mapping_id);
    }
    return text;
}

}  // namespace

std::string_view to_string(PortalErrc code) noexcept
{
    switch (code) {
    case PortalErrc::unavailable:
        return "portal unavailable";
    case PortalErrc::unsupported:
        return "unsupported by the portal";
    case PortalErrc::cancelled:
        return "cancelled by the user";
    case PortalErrc::failed:
        return "portal request failed";
    case PortalErrc::timed_out:
        return "timed out";
    case PortalErrc::aborted:
        return "aborted";
    case PortalErrc::closed:
        return "session closed";
    case PortalErrc::protocol:
        return "D-Bus protocol error";
    case PortalErrc::invalid_state:
        return "invalid state";
    }
    return "unknown error";
}

// --- UniqueFd

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept
{
    if (this != &other) {
        reset();
        fd_ = other.release();
    }
    return *this;
}

UniqueFd::~UniqueFd()
{
    reset();
}

void UniqueFd::reset() noexcept
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// --- PortalSession

PortalSession::PortalSession() : cancel_fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

PortalSession::~PortalSession()
{
    close_portal_session();
    closed_watches_.clear();
}

void PortalSession::cancel() noexcept
{
    if (cancel_fd_.valid()) {
        const std::uint64_t one = 1;
        [[maybe_unused]] const auto written = ::write(cancel_fd_.get(), &one, sizeof one);
    }
}

PortalResult<void> PortalSession::start(const PortalOptions& options)
{
    if (state_ != State::idle) {
        return fail(PortalErrc::invalid_state, "a portal session can only be started once");
    }
    state_ = State::starting;
    const auto timeout = std::min<std::chrono::milliseconds>(options.timeout, max_timeout);
    auto result = run_start(options, Clock::now() + timeout);
    if (!result) {
        log::warn(log_component, "cannot start the portal session: {}", result.error().message);
        close_portal_session();
        state_ = State::failed;
        return result;
    }
    state_ = State::started;
    return {};
}

PortalResult<void> PortalSession::read_capabilities(Clock::time_point deadline)
{
    for (const char* interface : {detail::remote_desktop_interface, detail::screen_cast_interface}) {
        FARLAND_TRY(auto call, bus_->new_call("org.freedesktop.DBus.Properties", "GetAll"));
        MessageWriter(call.get()).string(interface);
        auto reply =
            bus_->call(call.get(), std::format("reading the {} properties", interface), deadline, cancel_fd_.get());
        if (!reply) {
            if (reply.error().code == PortalErrc::failed || reply.error().code == PortalErrc::unavailable) {
                return fail(PortalErrc::unavailable,
                            std::format("xdg-desktop-portal is not running or has no {} portal: {}", interface,
                                        reply.error().message));
            }
            return std::unexpected(std::move(reply).error());
        }
        const bool remote_desktop = std::string_view(interface) == detail::remote_desktop_interface;
        MessageReader reader(reply->get());
        const bool ok = reader.vardict([&](std::string_view key, std::string_view signature, bool& good) {
            if (signature != "u") {
                return false;
            }
            std::uint32_t* target = nullptr;
            if (key == "version") {
                target = remote_desktop ? &capabilities_.remote_desktop_version : &capabilities_.screen_cast_version;
            } else if (remote_desktop && key == "AvailableDeviceTypes") {
                target = &capabilities_.device_types;
            } else if (!remote_desktop && key == "AvailableSourceTypes") {
                target = &capabilities_.source_types;
            } else if (!remote_desktop && key == "AvailableCursorModes") {
                target = &capabilities_.cursor_modes;
            } else {
                return false;
            }
            good = reader.u32(*target);
            return true;
        });
        if (!ok) {
            return fail(PortalErrc::protocol, std::format("malformed {} properties", interface));
        }
        if ((remote_desktop ? capabilities_.remote_desktop_version : capabilities_.screen_cast_version) == 0) {
            return fail(PortalErrc::unavailable, std::format("the portal has no {} interface", interface));
        }
    }
    log::info(log_component,
              "portal: RemoteDesktop v{} (devices {:#x}), ScreenCast v{} (sources {:#x}, cursor modes {:#x})",
              capabilities_.remote_desktop_version, capabilities_.device_types, capabilities_.screen_cast_version,
              capabilities_.source_types, capabilities_.cursor_modes);
    return {};
}

PortalResult<void> PortalSession::watch_session(const std::string& handle)
{
    FARLAND_TRY(auto watch, watch_signal(*bus_, portal_owner_, handle, detail::session_interface, "Closed",
                                         &PortalSession::on_closed, this));
    closed_watches_.push_back(std::move(watch));
    return {};
}

int PortalSession::on_closed(sd_bus_message* /*message*/, void* userdata, sd_bus_error* /*error*/)
{
    static_cast<PortalSession*>(userdata)->mark_closed("the portal closed the session");
    return 0;
}

void PortalSession::mark_closed(std::string_view reason)
{
    if (closed_) {
        return;
    }
    closed_ = true;
    log::info(log_component, "{}", reason);
    if (closed_callback_) {
        closed_callback_();
    }
}

PortalResult<void> PortalSession::run_start(const PortalOptions& options, Clock::time_point deadline)
{
    FARLAND_TRY(bus_, Bus::open(options.bus_address));
    FARLAND_TRY_VOID(read_capabilities(deadline));
    if (options.clipboard) {
        FARLAND_TRY_VOID(read_clipboard_version(deadline));
    }

    // Signals are matched on the portal's unique name: a well-known name
    // cannot be checked locally, and anyone can emit a signal.
    {
        FARLAND_TRY(auto call, bus_->new_call("org.freedesktop.DBus", "GetNameOwner", "/org/freedesktop/DBus",
                                              "org.freedesktop.DBus"));
        MessageWriter(call.get()).string(detail::portal_service);
        FARLAND_TRY(auto reply, bus_->call(call.get(), "GetNameOwner", deadline, cancel_fd_.get()));
        if (!MessageReader(reply.get()).string(portal_owner_)) {
            return fail(PortalErrc::protocol, "GetNameOwner: malformed reply");
        }
    }

    const auto& caps = capabilities_;
    std::uint32_t device_types = (device_keyboard | device_pointer) & caps.device_types;
    if (options.touchscreen) {
        device_types |= device_touchscreen & caps.device_types;
    }
    if (device_types == 0) {
        return fail(PortalErrc::unsupported, "the portal offers neither keyboard nor pointer control");
    }
    std::uint32_t source_types = 0;
    if (options.monitors) {
        source_types |= source_monitor & caps.source_types;
    }
    if (options.virtual_monitor) {
        source_types |= source_virtual & caps.source_types;
    }
    if (source_types == 0) {
        return fail(
            PortalErrc::unsupported,
            std::format("the portal offers none of the requested source types (it has {:#x})", caps.source_types));
    }
    if (caps.screen_cast_version >= 2) {
        if (options.cursor_metadata && (caps.cursor_modes & static_cast<std::uint32_t>(CursorMode::metadata)) != 0) {
            cursor_mode_ = CursorMode::metadata;
        } else if ((caps.cursor_modes & static_cast<std::uint32_t>(CursorMode::embedded)) != 0) {
            cursor_mode_ = CursorMode::embedded;
        } else if ((caps.cursor_modes & static_cast<std::uint32_t>(CursorMode::hidden)) != 0) {
            cursor_mode_ = CursorMode::hidden;
            log::warn(log_component, "the portal can only hide the cursor");
        }
    }

    const RequestContext ctx{bus_.get(), &portal_owner_, deadline, cancel_fd_.get(), &closed_};

    // 1. RemoteDesktop.CreateSession
    const std::string session_token = new_token();
    const std::string predicted_session =
        std::format("{}/session/{}/{}", detail::portal_path, sender_path_element(bus_->unique_name()), session_token);
    FARLAND_TRY_VOID(watch_session(predicted_session));
    {
        FARLAND_TRY(auto results, portal_request(ctx, detail::remote_desktop_interface, "CreateSession", nullptr,
                                                 nullptr, {{"session_handle_token", session_token}}));
        MessageReader reader(results.get());
        const bool ok = reader.vardict([&](std::string_view key, std::string_view signature, bool& good) {
            // Documented as 'o', sent as 's' by xdg-desktop-portal.
            if (key != "session_handle" || (signature != "s" && signature != "o")) {
                return false;
            }
            good = reader.string(session_handle_);
            return true;
        });
        if (!ok || session_handle_.empty()) {
            return fail(PortalErrc::protocol, "CreateSession: no session handle in the response");
        }
        if (session_handle_ != predicted_session) {
            FARLAND_TRY_VOID(watch_session(session_handle_));
        }
    }

    // 2. RemoteDesktop.SelectDevices. Persistence belongs here: the portal
    // refuses persist_mode and restore_token on SelectSources of a remote
    // desktop session.
    {
        Options select{{"types", device_types}};
        if (caps.remote_desktop_version >= 2) {
            if (options.persist) {
                select.push_back({"persist_mode", std::uint32_t{2}});
            }
            if (options.restore_token) {
                if (valid_restore_token(*options.restore_token)) {
                    select.push_back({"restore_token", *options.restore_token});
                } else {
                    log::warn(log_component, "ignoring a malformed restore token");
                }
            }
        } else if (options.restore_token) {
            log::info(log_component, "RemoteDesktop v{} cannot restore sessions; the portal will ask",
                      caps.remote_desktop_version);
        }
        FARLAND_TRY(auto results, portal_request(ctx, detail::remote_desktop_interface, "SelectDevices",
                                                 &session_handle_, nullptr, select));
    }

    // 3. ScreenCast.SelectSources on the same session.
    {
        Options select{{"types", source_types}, {"multiple", options.multiple}};
        if (cursor_mode_ != CursorMode::none) {
            select.push_back({"cursor_mode", static_cast<std::uint32_t>(cursor_mode_)});
        }
        FARLAND_TRY(auto results, portal_request(ctx, detail::screen_cast_interface, "SelectSources", &session_handle_,
                                                 nullptr, select));
    }

    // Clipboard.RequestClipboard must come before Start.
    if (options.clipboard && capabilities_.clipboard_version >= 1) {
        FARLAND_TRY_VOID(request_clipboard(deadline));
    }

    // 4. RemoteDesktop.Start: the dialog.
    FARLAND_TRY(auto results, portal_request(ctx, detail::remote_desktop_interface, "Start", &session_handle_,
                                             &options.parent_window, {}));
    MessageReader reader(results.get());
    std::optional<PortalError> stream_error;
    const bool ok = reader.vardict([&](std::string_view key, std::string_view signature, bool& good) {
        if (key == "devices" && signature == "u") {
            good = reader.u32(devices_);
        } else if (key == "clipboard_enabled" && signature == "b") {
            good = reader.boolean(clipboard_enabled_);
        } else if (key == "restore_token" && signature == "s") {
            std::string token;
            good = reader.string(token);
            if (valid_restore_token(token)) {
                restore_token_ = std::move(token);
            } else if (good) {
                log::warn(log_component, "the portal returned a malformed restore token");
            }
        } else if (key == "streams" && signature == "a(ua{sv})") {
            auto streams = read_streams(reader);
            if (streams) {
                streams_ = std::move(*streams);
            } else {
                stream_error = std::move(streams).error();
                good = false;
            }
        } else {
            return false;
        }
        return true;
    });
    if (stream_error) {
        return std::unexpected(std::move(*stream_error));
    }
    if (!ok) {
        return fail(PortalErrc::protocol, "Start: malformed response");
    }
    if (streams_.empty()) {
        return fail(PortalErrc::failed, "Start: the portal granted no screen cast streams");
    }
    log::info(log_component, "portal session {} started: devices {:#x}, cursor mode {}, {} stream(s), {}",
              session_handle_, devices_, static_cast<std::uint32_t>(cursor_mode_), streams_.size(),
              restore_token_ ? "restore token received" : "no restore token");
    for (const auto& stream : streams_) {
        log::info(log_component, "  stream {}", describe_stream(stream));
    }
    return {};
}

PortalResult<void> PortalSession::check_started() const
{
    if (state_ != State::started) {
        return fail(PortalErrc::invalid_state, "the portal session is not started");
    }
    if (closed_) {
        return fail(PortalErrc::closed, "the portal session is closed");
    }
    return {};
}

PortalResult<UniqueFd> PortalSession::open_pipewire_remote()
{
    FARLAND_TRY_VOID(check_started());
    FARLAND_TRY(auto call, bus_->new_call(detail::screen_cast_interface, "OpenPipeWireRemote"));
    MessageWriter writer(call.get());
    writer.object_path(session_handle_).options({});
    if (writer.status() < 0) {
        return fail(PortalErrc::protocol, "cannot build the OpenPipeWireRemote call");
    }
    FARLAND_TRY(auto reply,
                bus_->call(call.get(), "OpenPipeWireRemote", Clock::now() + call_timeout, cancel_fd_.get()));
    return read_fd(reply.get(), "OpenPipeWireRemote");
}

PortalResult<std::optional<UniqueFd>> PortalSession::connect_to_eis()
{
    FARLAND_TRY_VOID(check_started());
    if (capabilities_.remote_desktop_version < 2) {
        return std::optional<UniqueFd>{};
    }
    FARLAND_TRY(auto call, bus_->new_call(detail::remote_desktop_interface, "ConnectToEIS"));
    MessageWriter writer(call.get());
    writer.object_path(session_handle_).options({});
    if (writer.status() < 0) {
        return fail(PortalErrc::protocol, "cannot build the ConnectToEIS call");
    }
    FARLAND_TRY(auto reply, bus_->call(call.get(), "ConnectToEIS", Clock::now() + call_timeout, cancel_fd_.get()));
    FARLAND_TRY(auto fd, read_fd(reply.get(), "ConnectToEIS"));
    return std::optional<UniqueFd>(std::move(fd));
}

int PortalSession::fd() const noexcept
{
    return bus_ ? sd_bus_get_fd(bus_->get()) : -1;
}

short PortalSession::events() const noexcept
{
    if (!bus_) {
        return 0;
    }
    const int events = sd_bus_get_events(bus_->get());
    return static_cast<short>(events > 0 ? events : 0);
}

void PortalSession::process()
{
    if (!bus_ || state_ != State::started) {
        return;
    }
    if (auto processed = bus_->process_pending(); !processed) {
        mark_closed(processed.error().message);
    }
}

void PortalSession::close_portal_session() noexcept
{
    if (!bus_ || session_handle_.empty() || closed_) {
        return;
    }
    closed_ = true;
    auto call = bus_->new_call(detail::session_interface, "Close", session_handle_.c_str());
    if (call) {
        sd_bus_message_set_expect_reply(call->get(), 0);
        sd_bus_send(bus_->get(), call->get(), nullptr);
        sd_bus_flush(bus_->get());
    }
}

}  // namespace farland::platform::portal
