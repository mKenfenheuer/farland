// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/mutter/mutter_session.hpp>
#include <farland/platform/portal/portal_bus.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <poll.h>
#include <span>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

// The call sequence follows Mutter's data/dbus-interfaces/
// org.gnome.Mutter.RemoteDesktop.xml and org.gnome.Mutter.ScreenCast.xml
// (Mutter 50) and gnome-remote-desktop's grd-session.c and grd-stream.c.
namespace farland::platform::mutter {

using portal::detail::Bus;
using portal::detail::Clock;
using portal::detail::fail;
using portal::detail::MessageReader;
using portal::detail::MessageWriter;
using portal::detail::Options;
using portal::detail::SlotPtr;

namespace {

constexpr std::string_view log_component = "platform.mutter";
constexpr const char* remote_desktop_path = "/org/gnome/Mutter/RemoteDesktop";
constexpr const char* screen_cast_path = "/org/gnome/Mutter/ScreenCast";
constexpr const char* remote_desktop_interface = "org.gnome.Mutter.RemoteDesktop";
constexpr const char* remote_desktop_session_interface = "org.gnome.Mutter.RemoteDesktop.Session";
constexpr const char* screen_cast_interface = "org.gnome.Mutter.ScreenCast";
constexpr const char* screen_cast_session_interface = "org.gnome.Mutter.ScreenCast.Session";
constexpr const char* stream_interface = "org.gnome.Mutter.ScreenCast.Stream";
constexpr const char* properties_interface = "org.freedesktop.DBus.Properties";
/// ScreenCast cursor mode "metadata": the cursor comes as SPA_META_Cursor.
constexpr std::uint32_t cursor_mode_metadata = 2;
/// KeymapCapabilities: keymap type XKB, format XKB_TEXT_V1.
constexpr std::uint32_t keymap_type_xkb = 0;
constexpr std::uint32_t xkb_format_text_v1 = 1;
constexpr const char* display_config_service = "org.gnome.Mutter.DisplayConfig";
constexpr const char* display_config_path = "/org/gnome/Mutter/DisplayConfig";
constexpr const char* display_config_interface = "org.gnome.Mutter.DisplayConfig";
/// The vendor Mutter gives the monitors RecordVirtual creates.
constexpr std::string_view virtual_monitor_vendor = "MetaVendor";
/// ApplyMonitorsConfig method 1: apply now, do not store it in the user's
/// monitor configuration.
constexpr std::uint32_t monitor_config_temporary = 1;
/// How often wait_for_mutter() asks whether Mutter is on the bus yet.
constexpr auto name_poll_interval = std::chrono::milliseconds(100);

/// Reads an "au" into `out`.
bool read_u32_array(MessageReader& reader, std::vector<std::uint32_t>& out)
{
    if (!reader.enter('a', "u")) {
        return false;
    }
    for (;;) {
        std::uint32_t value = 0;
        const int r = sd_bus_message_read_basic(reader.get(), 'u', &value);
        if (r < 0) {
            return false;
        }
        if (r == 0) {
            break;
        }
        out.push_back(value);
    }
    return reader.exit();
}

/// A sealed memfd holding `text` and its terminating NUL, as Wayland hands
/// out keymaps; Mutter maps it and insists on the grow, shrink and write seals.
MutterResult<UniqueFd> sealed_memfd(std::string_view text)
{
    UniqueFd fd(::memfd_create("farland-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!fd.valid()) {
        return fail(PortalErrc::failed, std::format("memfd_create: {}", std::strerror(errno)));
    }
    std::string data(text);
    data.push_back('\0');
    std::size_t written = 0;
    while (written < data.size()) {
        const auto rest = std::span(data).subspan(written);
        const auto n = ::write(fd.get(), rest.data(), rest.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return fail(PortalErrc::failed, std::format("cannot write the keymap: {}", std::strerror(errno)));
        }
        written += static_cast<std::size_t>(n);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl's interface
    if (::fcntl(fd.get(), F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE) != 0) {
        return fail(PortalErrc::failed, std::format("cannot seal the keymap: {}", std::strerror(errno)));
    }
    return fd;
}

/// The number in a connector Mutter numbers, as in "Meta-3"; without one,
/// the largest number, so it sorts last.
std::uint64_t connector_number(std::string_view connector)
{
    const auto dash = connector.rfind('-');
    std::uint64_t number = 0;
    if (dash == std::string_view::npos || dash + 1 == connector.size()) {
        return UINT64_MAX;
    }
    for (const char c : connector.substr(dash + 1)) {
        if (c < '0' || c > '9') {
            return UINT64_MAX;
        }
        number = (number * 10) + static_cast<std::uint64_t>(c - '0');
    }
    return number;
}

/// One monitor of GetCurrentState's `monitors`, positioned at its entry.
MutterResult<Monitor> read_monitor(MessageReader& reader)
{
    sd_bus_message* m = reader.get();
    Monitor monitor;
    std::string monitor_serial;
    if (!reader.enter('r', "ssss") || !reader.string(monitor.connector) || !reader.string(monitor.vendor) ||
        !reader.string(monitor.product) || !reader.string(monitor_serial) || !reader.exit()) {
        return fail(PortalErrc::protocol, "GetCurrentState: malformed monitor");
    }
    monitor.is_virtual = monitor.vendor == virtual_monitor_vendor;
    if (!reader.enter('a', "(siiddada{sv})")) {
        return fail(PortalErrc::protocol, "GetCurrentState: malformed modes");
    }
    bool have_current = false;
    bool have_preferred = false;
    for (;;) {
        const int r = sd_bus_message_enter_container(m, 'r', "siiddada{sv}");
        if (r < 0) {
            return fail(PortalErrc::protocol, "GetCurrentState: malformed mode");
        }
        if (r == 0) {
            break;
        }
        std::string id;
        std::int32_t width = 0;
        std::int32_t height = 0;
        double refresh = 0;
        double scale = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): sd-bus's typed read
        if (!reader.string(id) || sd_bus_message_read(m, "iidd", &width, &height, &refresh, &scale) < 0 ||
            sd_bus_message_skip(m, "ad") < 0) {
            return fail(PortalErrc::protocol, "GetCurrentState: malformed mode");
        }
        bool current = false;
        bool preferred = false;
        const bool ok = reader.vardict([&](std::string_view key, std::string_view signature, bool& good) {
            if (signature != "b" || (key != "is-current" && key != "is-preferred")) {
                return false;
            }
            good = reader.boolean(key == "is-current" ? current : preferred);
            return true;
        });
        if (!ok || !reader.exit()) {
            return fail(PortalErrc::protocol, "GetCurrentState: malformed mode properties");
        }
        // The mode it runs; before that its preferred one, else the first.
        if (current || (preferred && !have_current) || (!have_current && !have_preferred && monitor.mode.empty())) {
            monitor.mode = id;
            monitor.width = static_cast<std::uint32_t>(std::max(width, 0));
            monitor.height = static_cast<std::uint32_t>(std::max(height, 0));
        }
        have_current = have_current || current;
        have_preferred = have_preferred || preferred;
    }
    if (!reader.exit() || sd_bus_message_skip(m, "a{sv}") < 0) {
        return fail(PortalErrc::protocol, "GetCurrentState: malformed monitor properties");
    }
    return monitor;
}

}  // namespace

std::vector<std::string> MonitorState::virtual_connectors() const
{
    std::vector<std::string> connectors;
    for (const auto& monitor : monitors) {
        if (monitor.is_virtual) {
            connectors.push_back(monitor.connector);
        }
    }
    std::ranges::sort(connectors, [](const std::string& a, const std::string& b) {
        const auto left = connector_number(a);
        const auto right = connector_number(b);
        return left != right ? left < right : a < b;
    });
    return connectors;
}

// --- Setup

MutterResult<std::unique_ptr<MutterSession>> MutterSession::create(const MutterOptions& options)
{
    std::unique_ptr<MutterSession> session(new MutterSession());
    FARLAND_TRY_VOID(session->connect(options));
    FARLAND_TRY_VOID(session->read_capabilities());
    FARLAND_TRY_VOID(session->create_sessions(options));
    return session;
}

MutterSession::~MutterSession()
{
    streams_.clear();
    if (bus_ && !session_path_.empty() && !closed_) {
        // Stop ends the ScreenCast session with it; a session never started
        // cannot be stopped, but goes away with our connection.
        if (auto call = bus_->new_call(remote_desktop_session_interface, "Stop", session_path_.c_str(),
                                       remote_desktop_service)) {
            sd_bus_message_set_expect_reply(call->get(), 0);
            sd_bus_send(bus_->get(), call->get(), nullptr);
            sd_bus_flush(bus_->get());
        }
    }
    watches_.clear();
}

Clock::time_point MutterSession::deadline() const
{
    return Clock::now() + timeout_;
}

MutterResult<void> MutterSession::connect(const MutterOptions& options)
{
    timeout_ = options.timeout;
    FARLAND_TRY(bus_, Bus::open(options.bus_address));
    const auto until = deadline();
    FARLAND_TRY_VOID(wait_for_mutter(until, options.keep_waiting));

    // Mutter leaving the bus (the compositor ended) closes the session too.
    FARLAND_TRY(auto owner_watch, watch("/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                                        &MutterSession::on_name_owner_changed, this));
    watches_.push_back(std::move(owner_watch));
    return {};
}

MutterResult<void> MutterSession::wait_for_mutter(Clock::time_point until, const std::function<bool()>& keep_waiting)
{
    bool waited = false;
    for (;;) {
        std::array<std::string, 2> owners;
        bool present = true;
        for (std::size_t i = 0; i < 2 && present; ++i) {
            FARLAND_TRY(auto call, bus_->new_call("org.freedesktop.DBus", "GetNameOwner", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus"));
            MessageWriter(call.get()).string(i == 0 ? remote_desktop_service : screen_cast_service);
            auto reply = bus_->call(call.get(), "GetNameOwner", until, -1);
            if (!reply) {
                if (reply.error().code == PortalErrc::timed_out || reply.error().code == PortalErrc::protocol) {
                    return std::unexpected(std::move(reply).error());
                }
                present = false;
            } else if (!MessageReader(reply->get()).string(owners.at(i))) {
                return fail(PortalErrc::protocol, "GetNameOwner: malformed reply");
            }
        }
        if (present) {
            if (owners[0] != owners[1]) {
                return fail(PortalErrc::protocol,
                            std::format("{} and {} are owned by different connections ({}, {})", remote_desktop_service,
                                        screen_cast_service, owners[0], owners[1]));
            }
            owner_ = owners[0];
            if (waited) {
                log::info(log_component, "Mutter is on the bus");
            }
            return {};
        }
        if (keep_waiting && !keep_waiting()) {
            return fail(PortalErrc::unavailable, "the compositor exited before Mutter's services appeared");
        }
        if (Clock::now() + name_poll_interval >= until) {
            return fail(PortalErrc::unavailable,
                        std::format("{} did not appear on the session bus: no GNOME Shell (Mutter) is running there",
                                    remote_desktop_service));
        }
        if (!waited) {
            log::info(log_component, "waiting for Mutter's {} service", remote_desktop_service);
            waited = true;
        }
        std::this_thread::sleep_for(name_poll_interval);
    }
}

MutterResult<void> MutterSession::read_capabilities()
{
    for (const char* interface : {remote_desktop_interface, screen_cast_interface}) {
        const bool remote_desktop = std::string_view(interface) == remote_desktop_interface;
        FARLAND_TRY(auto call, bus_->new_call(properties_interface, "GetAll",
                                              remote_desktop ? remote_desktop_path : screen_cast_path,
                                              remote_desktop ? remote_desktop_service : screen_cast_service));
        MessageWriter(call.get()).string(interface);
        FARLAND_TRY(auto reply,
                    bus_->call(call.get(), std::format("reading the {} properties", interface), deadline(), -1));
        MessageReader reader(reply.get());
        const bool ok = reader.vardict([&](std::string_view key, std::string_view signature, bool& good) {
            if (key == "Version" && signature == "i") {
                auto& version =
                    remote_desktop ? capabilities_.remote_desktop_version : capabilities_.screen_cast_version;
                good = sd_bus_message_read_basic(reader.get(), 'i', &version) > 0;
                return true;
            }
            if (remote_desktop && key == "SupportedDeviceTypes" && signature == "u") {
                good = reader.u32(capabilities_.device_types);
                return true;
            }
            return false;
        });
        if (!ok) {
            return fail(PortalErrc::protocol, std::format("malformed {} properties", interface));
        }
    }
    if (capabilities_.screen_cast_version < 3) {
        // is-platform (virtual monitors that count as real ones) came with
        // ScreenCast version 3; before, a RecordVirtual monitor shows the
        // screen sharing indicator and is not resizable.
        return fail(PortalErrc::unsupported,
                    std::format("Mutter's ScreenCast version {} is too old (3 or later needed: GNOME 43 or later)",
                                capabilities_.screen_cast_version));
    }
    // "keep-rendering-when-inactive" (a virtual monitor that keeps being
    // drawn while the session is not on its seat) came with ScreenCast 5.
    capabilities_.keep_rendering = capabilities_.screen_cast_version >= 5;
    log::info(log_component, "Mutter: RemoteDesktop v{} (devices {:#x}), ScreenCast v{}{}",
              capabilities_.remote_desktop_version, capabilities_.device_types, capabilities_.screen_cast_version,
              capabilities_.keep_rendering ? " (it draws virtual monitors off the seat)" : "");
    return {};
}

MutterResult<SlotPtr> MutterSession::watch(const std::string& path, const char* interface, const char* member,
                                           sd_bus_message_handler_t handler, void* userdata)
{
    // Signals are matched on the sender's unique name: a well-known name
    // cannot be checked locally, and anyone can emit a signal. The daemon's
    // own signals come from org.freedesktop.DBus. Asynchronous AddMatch: the
    // bus handles it before any method call that follows on this connection.
    const bool daemon = std::string_view(interface) == "org.freedesktop.DBus";
    sd_bus_slot* raw = nullptr;
    const int r = sd_bus_match_signal_async(bus_->get(), &raw, daemon ? "org.freedesktop.DBus" : owner_.c_str(),
                                            path.c_str(), interface, member, handler, nullptr, userdata);
    if (r < 0) {
        return fail(PortalErrc::protocol, std::format("cannot subscribe to {}: {}", member, std::strerror(-r)));
    }
    return SlotPtr(raw);
}

MutterResult<void> MutterSession::create_sessions(const MutterOptions& options)
{
    if (options.keep_rendering_when_inactive && !capabilities_.keep_rendering) {
        log::info(log_component,
                  "this Mutter draws a virtual monitor only while the session is on its seat "
                  "(ScreenCast v{}, 5 needed)",
                  capabilities_.screen_cast_version);
    }
    keep_rendering_when_inactive_ = options.keep_rendering_when_inactive && capabilities_.keep_rendering;

    // RemoteDesktop.CreateSession, then its SessionId.
    {
        FARLAND_TRY(auto call, bus_->new_call(remote_desktop_interface, "CreateSession", remote_desktop_path,
                                              remote_desktop_service));
        FARLAND_TRY(auto reply, bus_->call(call.get(), "RemoteDesktop.CreateSession", deadline(), -1));
        if (!MessageReader(reply.get()).string(session_path_)) {
            return fail(PortalErrc::protocol, "RemoteDesktop.CreateSession: malformed reply");
        }
    }
    FARLAND_TRY(auto closed_watch,
                watch(session_path_, remote_desktop_session_interface, "Closed", &MutterSession::on_closed, this));
    watches_.push_back(std::move(closed_watch));
    std::string session_id;
    {
        FARLAND_TRY(auto call,
                    bus_->new_call(properties_interface, "Get", session_path_.c_str(), remote_desktop_service));
        MessageWriter(call.get()).string(remote_desktop_session_interface).string("SessionId");
        FARLAND_TRY(auto reply, bus_->call(call.get(), "reading the SessionId", deadline(), -1));
        MessageReader reader(reply.get());
        if (!reader.enter('v', "s") || !reader.string(session_id) || session_id.empty()) {
            return fail(PortalErrc::protocol, "the remote desktop session has no SessionId");
        }
    }
    // KeymapCapabilities (Mutter 49 and later); missing before.
    {
        FARLAND_TRY(auto call,
                    bus_->new_call(properties_interface, "Get", session_path_.c_str(), remote_desktop_service));
        MessageWriter(call.get()).string(remote_desktop_session_interface).string("KeymapCapabilities");
        if (auto reply = bus_->call(call.get(), "reading the KeymapCapabilities", deadline(), -1)) {
            MessageReader reader(reply->get());
            std::vector<std::uint32_t> types;
            std::vector<std::uint32_t> formats;
            const bool ok = reader.enter('v', "a{sv}") &&
                            reader.vardict([&](std::string_view key, std::string_view signature, bool& good) {
                                if (signature != "au") {
                                    return false;
                                }
                                if (key == "supported-keymap-types") {
                                    good = read_u32_array(reader, types);
                                } else if (key == "supported-xkb-keymap-formats") {
                                    good = read_u32_array(reader, formats);
                                } else {
                                    return false;
                                }
                                return true;
                            });
            // Mutter 50.1 has SetKeymap but leaves the capabilities empty;
            // it takes XKB text keymaps all the same.
            const bool listed = std::ranges::find(types, keymap_type_xkb) != types.end() &&
                                std::ranges::find(formats, xkb_format_text_v1) != formats.end();
            capabilities_.xkb_keymaps = ok && (listed || (types.empty() && formats.empty()));
        }
    }

    // ScreenCast.CreateSession tied to it: started and stopped with it.
    {
        FARLAND_TRY(auto call,
                    bus_->new_call(screen_cast_interface, "CreateSession", screen_cast_path, screen_cast_service));
        MessageWriter writer(call.get());
        Options properties{{"remote-desktop-session-id", session_id}};
        if (options.disable_animations) {
            properties.push_back({"disable-animations", true});
        }
        writer.options(properties);
        if (writer.status() < 0) {
            return fail(PortalErrc::protocol, "cannot build the ScreenCast.CreateSession call");
        }
        FARLAND_TRY(auto reply, bus_->call(call.get(), "ScreenCast.CreateSession", deadline(), -1));
        if (!MessageReader(reply.get()).string(screen_cast_path_)) {
            return fail(PortalErrc::protocol, "ScreenCast.CreateSession: malformed reply");
        }
    }
    FARLAND_TRY(auto cast_watch,
                watch(screen_cast_path_, screen_cast_session_interface, "Closed", &MutterSession::on_closed, this));
    watches_.push_back(std::move(cast_watch));
    log::info(log_component, "Mutter remote desktop session {} (screen cast {})", session_path_, screen_cast_path_);
    return {};
}

// --- Streams

MutterResult<StreamId> MutterSession::record_virtual()
{
    if (closed_) {
        return fail(PortalErrc::closed, "the Mutter session is closed");
    }
    FARLAND_TRY(auto call, bus_->new_call(screen_cast_session_interface, "RecordVirtual", screen_cast_path_.c_str(),
                                          screen_cast_service));
    MessageWriter writer(call.get());
    Options options{{"cursor-mode", cursor_mode_metadata}, {"is-platform", true}};
    if (keep_rendering_when_inactive_) {
        // Mutter keeps drawing this monitor while the session is not active
        // on its seat, so the client holds it while the seat shows a login
        // screen (ScreenCast 5; older Mutter ignores what it does not know).
        options.push_back({"keep-rendering-when-inactive", true});
    }
    writer.options(options);
    if (writer.status() < 0) {
        return fail(PortalErrc::protocol, "cannot build the RecordVirtual call");
    }
    FARLAND_TRY(auto reply, bus_->call(call.get(), "RecordVirtual", deadline(), -1));
    auto stream = std::make_unique<Stream>();
    stream->session = this;
    stream->id = next_stream_++;
    if (!MessageReader(reply.get()).string(stream->info.path)) {
        return fail(PortalErrc::protocol, "RecordVirtual: malformed reply");
    }
    FARLAND_TRY(stream->watch, watch(stream->info.path, stream_interface, "PipeWireStreamAdded",
                                     &MutterSession::on_stream_added, stream.get()));

    // The "mapping-id" parameter names the stream's libei region.
    {
        FARLAND_TRY(auto get,
                    bus_->new_call(properties_interface, "Get", stream->info.path.c_str(), screen_cast_service));
        MessageWriter(get.get()).string(stream_interface).string("Parameters");
        FARLAND_TRY(auto parameters, bus_->call(get.get(), "reading the stream parameters", deadline(), -1));
        MessageReader reader(parameters.get());
        const bool ok = reader.enter('v', "a{sv}") &&
                        reader.vardict([&](std::string_view key, std::string_view signature, bool& good) {
                            if (key != "mapping-id" || signature != "s") {
                                return false;
                            }
                            good = reader.string(stream->info.mapping_id);
                            return true;
                        });
        if (!ok) {
            return fail(PortalErrc::protocol, "malformed stream parameters");
        }
    }
    if (started_) {
        FARLAND_TRY_VOID(start_stream(stream->info.path));
    }
    log::info(log_component, "virtual monitor {} (mapping {})", stream->info.path,
              stream->info.mapping_id.empty() ? "none" : stream->info.mapping_id);
    const StreamId id = stream->id;
    streams_.emplace(id, std::move(stream));
    return id;
}

MutterResult<void> MutterSession::start_stream(const std::string& path)
{
    FARLAND_TRY(auto call, bus_->new_call(stream_interface, "Start", path.c_str(), screen_cast_service));
    FARLAND_TRY_VOID(bus_->call(call.get(), "Stream.Start", deadline(), -1));
    return {};
}

MutterResult<void> MutterSession::start()
{
    if (started_) {
        return fail(PortalErrc::invalid_state, "the Mutter session is already started");
    }
    if (closed_) {
        return fail(PortalErrc::closed, "the Mutter session is closed");
    }
    FARLAND_TRY(auto call, bus_->new_call(remote_desktop_session_interface, "Start", session_path_.c_str(),
                                          remote_desktop_service));
    FARLAND_TRY_VOID(bus_->call(call.get(), "RemoteDesktop.Session.Start", deadline(), -1));
    started_ = true;
    return {};
}

void MutterSession::stop_stream(StreamId id)
{
    const auto found = streams_.find(id);
    if (found == streams_.end()) {
        return;
    }
    if (!closed_) {
        if (auto call =
                bus_->new_call(stream_interface, "Stop", found->second->info.path.c_str(), screen_cast_service)) {
            bus_->send(call->get());
        }
    }
    log::info(log_component, "removing virtual monitor {}", found->second->info.path);
    streams_.erase(found);
}

const VirtualStream* MutterSession::stream(StreamId id) const
{
    const auto found = streams_.find(id);
    return found != streams_.end() ? &found->second->info : nullptr;
}

int MutterSession::on_stream_added(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/)
{
    auto* stream = static_cast<Stream*>(userdata);
    std::uint32_t node = 0;
    if (!MessageReader(message).u32(node)) {
        log::warn(log_component, "malformed PipeWireStreamAdded");
        return 0;
    }
    log::debug(log_component, "{}: PipeWire node {}", stream->info.path, node);
    stream->info.node_id = node;
    return 0;
}

// --- Monitors

MutterResult<MonitorState> MutterSession::monitors()
{
    FARLAND_TRY(auto call, bus_->new_call(display_config_interface, "GetCurrentState", display_config_path,
                                          display_config_service));
    FARLAND_TRY(auto reply, bus_->call(call.get(), "GetCurrentState", deadline(), -1));
    sd_bus_message* m = reply.get();
    MessageReader reader(m);
    MonitorState state;
    if (!reader.u32(state.serial) || !reader.enter('a', "((ssss)a(siiddada{sv})a{sv})")) {
        return fail(PortalErrc::protocol, "GetCurrentState: malformed reply");
    }
    for (;;) {
        const int r = sd_bus_message_enter_container(m, 'r', "(ssss)a(siiddada{sv})a{sv}");
        if (r < 0) {
            return fail(PortalErrc::protocol, "GetCurrentState: malformed monitors");
        }
        if (r == 0) {
            break;
        }
        FARLAND_TRY(auto monitor, read_monitor(reader));
        state.monitors.push_back(std::move(monitor));
        if (!reader.exit()) {
            return fail(PortalErrc::protocol, "GetCurrentState: malformed monitors");
        }
    }
    // The logical monitors say which monitors show something, and which one
    // of them is the primary.
    if (!reader.exit() || !reader.enter('a', "(iiduba(ssss)a{sv})")) {
        return fail(PortalErrc::protocol, "GetCurrentState: malformed logical monitors");
    }
    for (;;) {
        const int r = sd_bus_message_enter_container(m, 'r', "iiduba(ssss)a{sv}");
        if (r < 0) {
            return fail(PortalErrc::protocol, "GetCurrentState: malformed logical monitor");
        }
        if (r == 0) {
            break;
        }
        std::int32_t x = 0;
        std::int32_t y = 0;
        double scale = 1.0;
        std::uint32_t transform = 0;
        int primary = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): sd-bus's typed read
        if (sd_bus_message_read(m, "iidub", &x, &y, &scale, &transform, &primary) < 0 || !reader.enter('a', "(ssss)")) {
            return fail(PortalErrc::protocol, "GetCurrentState: malformed logical monitor");
        }
        for (;;) {
            const int rm = sd_bus_message_enter_container(m, 'r', "ssss");
            if (rm < 0) {
                return fail(PortalErrc::protocol, "GetCurrentState: malformed logical monitor");
            }
            if (rm == 0) {
                break;
            }
            std::string connector;
            if (!reader.string(connector) || sd_bus_message_skip(m, "sss") < 0 || !reader.exit()) {
                return fail(PortalErrc::protocol, "GetCurrentState: malformed logical monitor");
            }
            for (auto& monitor : state.monitors) {
                if (monitor.connector == connector) {
                    monitor.active = true;
                    monitor.primary = primary != 0;
                    monitor.x = x;
                    monitor.y = y;
                    monitor.scale = scale;
                    monitor.transform = transform;
                }
            }
        }
        if (!reader.exit() || sd_bus_message_skip(m, "a{sv}") < 0 || !reader.exit()) {
            return fail(PortalErrc::protocol, "GetCurrentState: malformed logical monitor");
        }
    }
    return state;
}

std::vector<LogicalMonitor> MonitorState::active_layout() const
{
    std::vector<LogicalMonitor> layout;
    for (const auto& monitor : monitors) {
        if (monitor.active) {
            layout.push_back(LogicalMonitor{monitor.x, monitor.y, monitor.scale, monitor.transform, monitor.primary,
                                            monitor.connector, monitor.mode});
        }
    }
    return layout;
}

std::vector<LogicalMonitor> MonitorState::side_by_side(std::span<const std::string> connectors) const
{
    std::vector<LogicalMonitor> layout;
    std::int32_t x = 0;
    for (const auto& connector : connectors) {
        const auto found = std::ranges::find(monitors, connector, &Monitor::connector);
        if (found == monitors.end()) {
            return {};
        }
        layout.push_back(LogicalMonitor{x, 0, 1.0, 0, layout.empty(), found->connector, found->mode});
        x += static_cast<std::int32_t>(found->width);
    }
    return layout;
}

MutterResult<void> MutterSession::set_monitors(std::uint32_t serial, std::span<const LogicalMonitor> layout)
{
    if (layout.empty()) {
        return fail(PortalErrc::invalid_state, "a session needs at least one monitor");
    }
    FARLAND_TRY(auto call, bus_->new_call(display_config_interface, "ApplyMonitorsConfig", display_config_path,
                                          display_config_service));
    sd_bus_message* m = call.get();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): sd-bus's typed append
    int r = sd_bus_message_append(m, "uu", serial, monitor_config_temporary);
    const auto step = [&r](int result) { r = r < 0 ? r : result; };
    step(sd_bus_message_open_container(m, 'a', "(iiduba(ssa{sv}))"));
    for (const auto& entry : layout) {
        step(sd_bus_message_open_container(m, 'r', "iiduba(ssa{sv})"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): sd-bus's typed append
        step(sd_bus_message_append(m, "iidub", entry.x, entry.y, entry.scale, entry.transform, entry.primary ? 1 : 0));
        step(sd_bus_message_open_container(m, 'a', "(ssa{sv})"));
        step(sd_bus_message_open_container(m, 'r', "ssa{sv}"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): sd-bus's typed append
        step(sd_bus_message_append(m, "ss", entry.connector.c_str(), entry.mode.c_str()));
        step(sd_bus_message_open_container(m, 'a', "{sv}"));
        for (int i = 0; i < 4; ++i) {
            step(sd_bus_message_close_container(m));
        }
    }
    step(sd_bus_message_close_container(m));
    step(sd_bus_message_append(m, "a{sv}", 0));  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (r < 0) {
        return fail(PortalErrc::protocol,
                    std::format("cannot build the ApplyMonitorsConfig call: {}", std::strerror(-r)));
    }
    FARLAND_TRY_VOID(bus_->call(m, "ApplyMonitorsConfig", deadline(), -1));
    return {};
}

// --- Input

MutterResult<UniqueFd> MutterSession::connect_to_eis(std::uint32_t device_types)
{
    FARLAND_TRY(auto call, bus_->new_call(remote_desktop_session_interface, "ConnectToEIS", session_path_.c_str(),
                                          remote_desktop_service));
    MessageWriter writer(call.get());
    writer.options({{"device-types", device_types & capabilities_.device_types}});
    if (writer.status() < 0) {
        return fail(PortalErrc::protocol, "cannot build the ConnectToEIS call");
    }
    FARLAND_TRY(auto reply, bus_->call(call.get(), "ConnectToEIS", deadline(), -1));
    int borrowed = -1;
    if (!MessageReader(reply.get()).fd(borrowed)) {
        return fail(PortalErrc::protocol, "ConnectToEIS: the reply has no file descriptor");
    }
    // The message owns its fds; keep a copy.
    UniqueFd fd(::fcntl(borrowed, F_DUPFD_CLOEXEC, 3));  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (!fd.valid()) {
        return fail(PortalErrc::protocol,
                    std::format("ConnectToEIS: cannot duplicate the fd: {}", std::strerror(errno)));
    }
    return fd;
}

MutterResult<void> MutterSession::set_keymap(std::string_view keymap)
{
    if (!capabilities_.xkb_keymaps) {
        return fail(PortalErrc::unsupported, "Mutter takes no XKB keymaps (SetKeymap needs GNOME 49 or later)");
    }
    FARLAND_TRY(auto fd, sealed_memfd(keymap));
    FARLAND_TRY(auto call, bus_->new_call(remote_desktop_session_interface, "SetKeymap", session_path_.c_str(),
                                          remote_desktop_service));
    sd_bus_message* m = call.get();
    const int handle = fd.get();
    const std::uint32_t type = keymap_type_xkb;
    const std::uint32_t format = xkb_format_text_v1;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): sd-bus's typed append
    const int r = sd_bus_message_append(m, "a{sv}", 3, "keymap-type", "u", type, "xkb-keymap-format", "u", format,
                                        "xkb-keymap", "h", handle);
    if (r < 0) {
        return fail(PortalErrc::protocol, std::format("cannot build the SetKeymap call: {}", std::strerror(-r)));
    }
    FARLAND_TRY_VOID(bus_->call(m, "SetKeymap", deadline(), -1));
    return {};
}

// --- Lifetime

int MutterSession::fd() const noexcept
{
    return bus_ ? sd_bus_get_fd(bus_->get()) : -1;
}

short MutterSession::events() const noexcept
{
    if (!bus_) {
        return 0;
    }
    const int events = sd_bus_get_events(bus_->get());
    return static_cast<short>(events > 0 ? events : 0);
}

void MutterSession::process()
{
    if (!bus_) {
        return;
    }
    if (auto processed = bus_->process_pending(); !processed) {
        mark_closed(processed.error().message);
    }
}

bool MutterSession::process_until(const std::function<bool()>& done, std::chrono::milliseconds timeout)
{
    const auto until = Clock::now() + timeout;
    auto waited = bus_->run_until([&] { return done() || closed_; }, until, -1);
    if (!waited) {
        if (waited.error().code != PortalErrc::timed_out) {
            mark_closed(waited.error().message);
        }
        return false;
    }
    return !closed_ || done();
}

void MutterSession::mark_closed(std::string_view reason)
{
    if (closed_) {
        return;
    }
    closed_ = true;
    log::info(log_component, "{}", reason);
}

int MutterSession::on_closed(sd_bus_message* /*message*/, void* userdata, sd_bus_error* /*error*/)
{
    static_cast<MutterSession*>(userdata)->mark_closed("Mutter closed the remote desktop session");
    return 0;
}

int MutterSession::on_name_owner_changed(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/)
{
    auto* self = static_cast<MutterSession*>(userdata);
    MessageReader reader(message);
    std::string name;
    std::string old_owner;
    std::string new_owner;
    if (reader.string(name) && reader.string(old_owner) && reader.string(new_owner) && old_owner == self->owner_ &&
        (name == remote_desktop_service || name == self->owner_) && new_owner != self->owner_) {
        self->compositor_gone_ = true;
        self->mark_closed("Mutter left the session bus (GNOME Shell ended)");
    }
    return 0;
}

}  // namespace farland::platform::mutter
