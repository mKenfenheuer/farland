// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/portal/portal_bus.hpp>
#include <farland/platform/portal/portal_clipboard.hpp>

#include <cstring>
#include <format>

// org.freedesktop.portal.Clipboard, version 1 (xdg-desktop-portal 1.18).
namespace farland::platform::portal {

using detail::Clock;
using detail::MessagePtr;
using detail::MessageReader;
using detail::MessageWriter;

namespace {

constexpr std::string_view log_component = "platform.portal";
constexpr const char* clipboard_interface = "org.freedesktop.portal.Clipboard";
/// SelectionRead, SelectionWrite and RequestClipboard answer at once.
constexpr auto call_timeout = std::chrono::seconds(5);

PortalResult<detail::SlotPtr> subscribe(sd_bus* bus, const std::string& sender, const char* member,
                                        sd_bus_message_handler_t handler, void* userdata)
{
    sd_bus_slot* raw = nullptr;
    const int r = sd_bus_match_signal_async(bus, &raw, sender.c_str(), detail::portal_path, clipboard_interface, member,
                                            handler, nullptr, userdata);
    if (r < 0) {
        return detail::fail(PortalErrc::protocol, std::format("cannot subscribe to {}: {}", member, std::strerror(-r)));
    }
    return detail::SlotPtr(raw);
}

/// Reads an "as" into `out`.
bool read_strings(MessageReader& reader, std::vector<std::string>& out)
{
    if (!reader.enter('a', "s")) {
        return false;
    }
    for (;;) {
        const char* text = nullptr;
        const int r = sd_bus_message_read_basic(reader.get(), 's', static_cast<void*>(&text));
        if (r < 0) {
            return false;
        }
        if (r == 0) {
            break;
        }
        out.emplace_back(text != nullptr ? text : "");
    }
    return reader.exit();
}

}  // namespace

// --- PortalSession's part: asked for before Start

PortalResult<void> PortalSession::read_clipboard_version(Clock::time_point deadline)
{
    FARLAND_TRY(auto call, bus_->new_call("org.freedesktop.DBus.Properties", "Get"));
    MessageWriter(call.get()).string(clipboard_interface).string("version");
    auto reply = bus_->call(call.get(), "reading the Clipboard version", deadline, cancel_fd_.get());
    if (!reply) {
        log::info(log_component, "the portal has no Clipboard interface: {}", reply.error().message);
        return {};
    }
    MessageReader reader(reply->get());
    std::uint32_t version = 0;
    if (reader.enter('v', "u") && reader.u32(version)) {
        capabilities_.clipboard_version = version;
    }
    return {};
}

PortalResult<void> PortalSession::request_clipboard(Clock::time_point deadline)
{
    // Subscribed before Start: the portal may announce the desktop's
    // clipboard before the Start response arrives.
    sd_bus_slot* raw = nullptr;
    const int r = sd_bus_match_signal_async(
        bus_->get(), &raw, portal_owner_.c_str(), detail::portal_path, clipboard_interface, "SelectionOwnerChanged",
        [](sd_bus_message* m, void* userdata, sd_bus_error* /*error*/) -> int {
            static_cast<PortalSession*>(userdata)->clipboard_owner_signal_.reset(sd_bus_message_ref(m));
            return 0;
        },
        nullptr, this);
    if (r < 0) {
        return detail::fail(PortalErrc::protocol,
                            std::format("cannot subscribe to SelectionOwnerChanged: {}", std::strerror(-r)));
    }
    clipboard_watch_.reset(raw);
    FARLAND_TRY(auto call, bus_->new_call(clipboard_interface, "RequestClipboard"));
    MessageWriter writer(call.get());
    writer.object_path(session_handle_).options({});
    if (writer.status() < 0) {
        return detail::fail(PortalErrc::protocol, "cannot build the RequestClipboard call");
    }
    log::debug(log_component, "calling RequestClipboard");
    FARLAND_TRY_VOID(
        bus_->call(call.get(), "RequestClipboard", std::min(deadline, Clock::now() + call_timeout), cancel_fd_.get()));
    return {};
}

detail::MessagePtr PortalSession::take_clipboard_owner_signal()
{
    clipboard_watch_.reset();
    return std::move(clipboard_owner_signal_);
}

// --- PortalClipboard

PortalClipboard::PortalClipboard(PortalSession& session, PortalClipboardOptions options)
    : session_(session), pipes_(options, [this](std::uint32_t serial, bool success) { write_done(serial, success); })
{
}

PortalClipboard::~PortalClipboard()
{
    pipes_.cancel_writes();
}

PortalResult<std::unique_ptr<PortalClipboard>> PortalClipboard::create(PortalSession& session,
                                                                       PortalClipboardOptions options)
{
    auto* bus = session.bus();
    if (bus == nullptr || !session.clipboard_enabled()) {
        return detail::fail(PortalErrc::invalid_state, "the portal session has no clipboard access");
    }
    std::unique_ptr<PortalClipboard> clipboard(new PortalClipboard(session, options));
    FARLAND_TRY(auto owner, subscribe(bus->get(), session.portal_owner(), "SelectionOwnerChanged",
                                      &PortalClipboard::on_owner_changed, clipboard.get()));
    FARLAND_TRY(auto transfer, subscribe(bus->get(), session.portal_owner(), "SelectionTransfer",
                                         &PortalClipboard::on_transfer, clipboard.get()));
    clipboard->watches_.push_back(std::move(owner));
    clipboard->watches_.push_back(std::move(transfer));
    if (auto early = session.take_clipboard_owner_signal()) {
        clipboard->owner_changed(early.get());
    }
    return clipboard;
}

int PortalClipboard::on_owner_changed(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/)
{
    static_cast<PortalClipboard*>(userdata)->owner_changed(message);
    return 0;
}

void PortalClipboard::owner_changed(sd_bus_message* message)
{
    MessageReader reader(message);
    std::string handle;
    std::vector<std::string> mimes;
    bool own = false;
    const bool ok =
        reader.string(handle) && reader.vardict([&](std::string_view key, std::string_view signature, bool& good) {
            if (key == "mime_types" && signature == "as") {
                good = read_strings(reader, mimes);
            } else if (key == "session_is_owner" && signature == "b") {
                good = reader.boolean(own);
            } else {
                return false;
            }
            return true;
        });
    if (!ok) {
        log::warn(log_component, "malformed SelectionOwnerChanged");
        return;
    }
    if (handle != session_.session_handle()) {
        return;
    }
    if (own) {
        mime_types_.reset();
        return;
    }
    mime_types_ = mimes;
    events_.emplace_back(clipboard_event::OwnerChanged{std::move(mimes)});
}

int PortalClipboard::on_transfer(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/)
{
    auto* self = static_cast<PortalClipboard*>(userdata);
    MessageReader reader(message);
    std::string handle;
    std::string mime;
    std::uint32_t serial = 0;
    if (!reader.string(handle) || !reader.string(mime) || !reader.u32(serial)) {
        log::warn(log_component, "malformed SelectionTransfer");
        return 0;
    }
    if (handle == self->session_.session_handle()) {
        self->events_.emplace_back(clipboard_event::TransferRequested{serial, std::move(mime)});
    }
    return 0;
}

void PortalClipboard::set_selection(const std::vector<std::string>& mime_types)
{
    auto* bus = session_.bus();
    auto call = bus->new_call(clipboard_interface, "SetSelection");
    if (!call) {
        return;
    }
    sd_bus_message* m = call->get();
    int r = sd_bus_message_append_basic(m, 'o', session_.session_handle().c_str());
    const auto open = [&](char type, const char* contents) {
        r = r < 0 ? r : sd_bus_message_open_container(m, type, contents);
    };
    const auto close = [&] { r = r < 0 ? r : sd_bus_message_close_container(m); };
    open('a', "{sv}");
    open('e', "sv");
    r = r < 0 ? r : sd_bus_message_append_basic(m, 's', "mime_types");
    open('v', "as");
    open('a', "s");
    for (const auto& mime : mime_types) {
        r = r < 0 ? r : sd_bus_message_append_basic(m, 's', mime.c_str());
    }
    close();
    close();
    close();
    close();
    if (r < 0) {
        log::warn(log_component, "cannot build the SetSelection call: {}", std::strerror(-r));
        return;
    }
    mime_types_.reset();
    bus->send(m);
}

void PortalClipboard::write_done(std::uint32_t serial, bool success)
{
    auto call = session_.bus()->new_call(clipboard_interface, "SelectionWriteDone");
    if (!call) {
        return;
    }
    MessageWriter writer(call->get());
    writer.object_path(session_.session_handle()).u32(serial);
    const int flag = success ? 1 : 0;  // sd-bus booleans are ints
    if (writer.status() < 0 || sd_bus_message_append_basic(call->get(), 'b', &flag) < 0) {
        return;
    }
    session_.bus()->send(call->get());
}

void PortalClipboard::write(std::uint32_t serial, std::optional<std::vector<std::byte>> data)
{
    if (!data) {
        write_done(serial, false);
        return;
    }
    auto call = session_.bus()->new_call(clipboard_interface, "SelectionWrite");
    if (!call) {
        write_done(serial, false);
        return;
    }
    MessageWriter(call->get()).object_path(session_.session_handle()).u32(serial);
    auto reply = session_.bus()->call(call->get(), "SelectionWrite", Clock::now() + call_timeout, -1);
    auto fd = reply ? take_reply_fd(reply->get(), "SelectionWrite") : std::unexpected(std::move(reply).error());
    if (!fd) {
        log::warn(log_component, "{}", fd.error().message);
        write_done(serial, false);
        return;
    }
    pipes_.write(serial, std::move(*fd), std::move(*data));
}

std::uint64_t PortalClipboard::read(const std::string& mime_type)
{
    const std::uint64_t id = next_read_++;
    auto call = session_.bus()->new_call(clipboard_interface, "SelectionRead");
    if (!call) {
        events_.emplace_back(clipboard_event::ReadFinished{id, std::nullopt});
        return id;
    }
    MessageWriter(call->get()).object_path(session_.session_handle()).string(mime_type);
    auto reply = session_.bus()->call(call->get(), "SelectionRead", Clock::now() + call_timeout, -1);
    auto fd = reply ? take_reply_fd(reply->get(), "SelectionRead") : std::unexpected(std::move(reply).error());
    if (!fd) {
        log::warn(log_component, "{}", fd.error().message);
        events_.emplace_back(clipboard_event::ReadFinished{id, std::nullopt});
        return id;
    }
    pipes_.read(id, std::move(*fd));
    return id;
}

std::optional<ClipboardEvent> PortalClipboard::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

void PortalClipboard::dispatch()
{
    session_.process();
    pipes_.pump(events_);
}

}  // namespace farland::platform::portal
