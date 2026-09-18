// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/mutter/mutter_clipboard.hpp>
#include <farland/platform/portal/portal_bus.hpp>

#include <cstring>
#include <format>

// org.gnome.Mutter.RemoteDesktop.Session's clipboard methods (Mutter 50),
// as gnome-remote-desktop's grd-session.c and grd-clipboard.c use them.
namespace farland::platform::mutter {

using portal::detail::Clock;
using portal::detail::MessageReader;
using portal::detail::MessageWriter;

namespace {

constexpr std::string_view log_component = "platform.mutter";
constexpr const char* session_interface = "org.gnome.Mutter.RemoteDesktop.Session";

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

/// A call on the session object.
MutterResult<portal::detail::MessagePtr> session_call(MutterSession& session, const char* member)
{
    return session.bus()->new_call(session_interface, member, session.session_path().c_str(), remote_desktop_service);
}

/// Appends {"mime-types": <as>} as the call's a{sv}.
int append_mime_types(sd_bus_message* m, const std::vector<std::string>& mime_types)
{
    int r = sd_bus_message_open_container(m, 'a', "{sv}");
    const auto step = [&r](int result) { r = r < 0 ? r : result; };
    step(sd_bus_message_open_container(m, 'e', "sv"));
    step(sd_bus_message_append_basic(m, 's', "mime-types"));
    step(sd_bus_message_open_container(m, 'v', "as"));
    step(sd_bus_message_open_container(m, 'a', "s"));
    for (const auto& mime : mime_types) {
        step(sd_bus_message_append_basic(m, 's', mime.c_str()));
    }
    for (int i = 0; i < 4; ++i) {
        step(sd_bus_message_close_container(m));
    }
    return r;
}

}  // namespace

MutterClipboard::MutterClipboard(MutterSession& session, portal::ClipboardTransferOptions options)
    : session_(session), pipes_(options, [this](std::uint32_t serial, bool success) { write_done(serial, success); })
{
}

MutterClipboard::~MutterClipboard()
{
    pipes_.cancel_writes();
    if (enabled_ && !session_.closed()) {
        if (auto call = session_call(session_, "DisableClipboard")) {
            session_.bus()->send(call->get());
        }
    }
}

MutterResult<std::unique_ptr<MutterClipboard>> MutterClipboard::create(MutterSession& session,
                                                                       portal::ClipboardTransferOptions options)
{
    auto* bus = session.bus();
    if (bus == nullptr || session.closed()) {
        return portal::detail::fail(PortalErrc::invalid_state, "the Mutter session is closed");
    }
    std::unique_ptr<MutterClipboard> clipboard(new MutterClipboard(session, options));
    // Subscribed before EnableClipboard, which announces the current owner
    // before it returns.
    for (const auto& [member, handler] : {std::pair{"SelectionOwnerChanged", &MutterClipboard::on_owner_changed},
                                          std::pair{"SelectionTransfer", &MutterClipboard::on_transfer}}) {
        sd_bus_slot* raw = nullptr;
        const int r =
            sd_bus_match_signal_async(bus->get(), &raw, session.mutter_owner().c_str(), session.session_path().c_str(),
                                      session_interface, member, handler, nullptr, clipboard.get());
        if (r < 0) {
            return portal::detail::fail(PortalErrc::protocol,
                                        std::format("cannot subscribe to {}: {}", member, std::strerror(-r)));
        }
        clipboard->watches_.emplace_back(raw);
    }
    FARLAND_TRY(auto call, session_call(session, "EnableClipboard"));
    MessageWriter(call.get()).options({});
    FARLAND_TRY_VOID(bus->call(call.get(), "EnableClipboard", Clock::now() + session.call_timeout(), -1));
    clipboard->enabled_ = true;
    return clipboard;
}

int MutterClipboard::on_owner_changed(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/)
{
    static_cast<MutterClipboard*>(userdata)->owner_changed(message);
    return 0;
}

void MutterClipboard::owner_changed(sd_bus_message* message)
{
    MessageReader reader(message);
    std::vector<std::string> mimes;
    bool own = false;
    const bool ok = reader.vardict([&](std::string_view key, std::string_view signature, bool& good) {
        if (key == "mime-types" && signature == "(as)") {
            // Mutter wraps the list in a tuple (g_variant_new ("(^as)", ...)).
            good = reader.enter('r', "as") && read_strings(reader, mimes) && reader.exit();
        } else if (key == "mime-types" && signature == "as") {
            good = read_strings(reader, mimes);
        } else if (key == "session-is-owner" && signature == "b") {
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
    if (own) {
        mime_types_.reset();
        return;
    }
    // No mime-types at all: nobody owns the clipboard any more.
    mime_types_ = mimes;
    events_.emplace_back(clipboard_event::OwnerChanged{std::move(mimes)});
}

int MutterClipboard::on_transfer(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/)
{
    auto* self = static_cast<MutterClipboard*>(userdata);
    MessageReader reader(message);
    std::string mime;
    std::uint32_t serial = 0;
    if (!reader.string(mime) || !reader.u32(serial)) {
        log::warn(log_component, "malformed SelectionTransfer");
        return 0;
    }
    self->events_.emplace_back(clipboard_event::TransferRequested{serial, std::move(mime)});
    return 0;
}

void MutterClipboard::set_selection(const std::vector<std::string>& mime_types)
{
    auto call = session_call(session_, "SetSelection");
    if (!call) {
        return;
    }
    if (const int r = append_mime_types(call->get(), mime_types); r < 0) {
        log::warn(log_component, "cannot build the SetSelection call: {}", std::strerror(-r));
        return;
    }
    mime_types_.reset();
    session_.bus()->send(call->get());
}

void MutterClipboard::write_done(std::uint32_t serial, bool success)
{
    auto call = session_call(session_, "SelectionWriteDone");
    if (!call) {
        return;
    }
    MessageWriter writer(call->get());
    writer.u32(serial);
    const int flag = success ? 1 : 0;  // sd-bus booleans are ints
    if (writer.status() < 0 || sd_bus_message_append_basic(call->get(), 'b', &flag) < 0) {
        return;
    }
    session_.bus()->send(call->get());
}

void MutterClipboard::write(std::uint32_t serial, std::optional<std::vector<std::byte>> data)
{
    if (!data) {
        write_done(serial, false);
        return;
    }
    auto call = session_call(session_, "SelectionWrite");
    if (!call) {
        write_done(serial, false);
        return;
    }
    MessageWriter(call->get()).u32(serial);
    auto reply = session_.bus()->call(call->get(), "SelectionWrite", Clock::now() + session_.call_timeout(), -1);
    auto fd = reply ? portal::take_reply_fd(reply->get(), "SelectionWrite") : std::unexpected(std::move(reply).error());
    if (!fd) {
        log::warn(log_component, "{}", fd.error().message);
        write_done(serial, false);
        return;
    }
    pipes_.write(serial, std::move(*fd), std::move(*data));
}

std::uint64_t MutterClipboard::read(const std::string& mime_type)
{
    const std::uint64_t id = next_read_++;
    queued_reads_.emplace_back(id, mime_type);
    start_reads();
    return id;
}

void MutterClipboard::start_reads()
{
    while (!pipes_.reading() && !queued_reads_.empty()) {
        auto [id, mime_type] = std::move(queued_reads_.front());
        queued_reads_.pop_front();
        auto call = session_call(session_, "SelectionRead");
        if (!call) {
            events_.emplace_back(clipboard_event::ReadFinished{id, std::nullopt});
            continue;
        }
        MessageWriter(call->get()).string(mime_type);
        auto reply = session_.bus()->call(call->get(), "SelectionRead", Clock::now() + session_.call_timeout(), -1);
        auto fd =
            reply ? portal::take_reply_fd(reply->get(), "SelectionRead") : std::unexpected(std::move(reply).error());
        if (!fd) {
            log::warn(log_component, "{}", fd.error().message);
            events_.emplace_back(clipboard_event::ReadFinished{id, std::nullopt});
            continue;
        }
        pipes_.read(id, std::move(*fd));
    }
}

std::optional<ClipboardEvent> MutterClipboard::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

void MutterClipboard::dispatch()
{
    session_.process();
    pipes_.pump(events_);
    start_reads();
}

}  // namespace farland::platform::mutter
