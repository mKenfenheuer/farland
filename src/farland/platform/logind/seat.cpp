// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/logind/seat.hpp>
#include <farland/platform/portal/portal_bus.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace farland::platform::logind {

using portal::detail::fail;
using portal::detail::MessageReader;

namespace {

constexpr std::string_view log_component = "platform.logind";
constexpr const char* login1_service = "org.freedesktop.login1";
constexpr const char* session_interface = "org.freedesktop.login1.Session";

/// The user's graphical session: its logind object path. This process runs in
/// a unit of the user's systemd manager, which is a session of its own
/// without a seat, so the session has to be looked up through the user.
LogindResult<std::string> display_session_path(sd_bus* bus)
{
    portal::detail::BusError error;
    sd_bus_message* reply = nullptr;
    const std::string user_path = std::format("/org/freedesktop/login1/user/_{}", ::getuid());
    const int r = sd_bus_get_property(bus, login1_service, user_path.c_str(), "org.freedesktop.login1.User", "Display",
                                      error.get(), &reply, "(so)");
    const portal::detail::MessagePtr owned(reply);
    if (r < 0) {
        return portal::detail::fail_call("reading the user's display session", error.get(), r);
    }
    MessageReader reader(owned.get());
    std::string session_id;
    std::string path;
    if (!reader.enter('r', "so") || !reader.string(session_id) || !reader.string(path) || !reader.exit()) {
        return fail(PortalErrc::protocol, "malformed display session");
    }
    if (path.empty() || path == "/") {
        return fail(PortalErrc::unavailable, "the user has no graphical session");
    }
    return path;
}

/// The session this agent belongs to: the one farlandd named in
/// XDG_SESSION_ID (the login session it started the agent in), else the
/// user's graphical one.
LogindResult<std::string> our_session_path(sd_bus* bus)
{
    const char* id = std::getenv("XDG_SESSION_ID");  // NOLINT(concurrency-mt-unsafe): set before any thread
    if (id == nullptr || *id == '\0') {
        return display_session_path(bus);
    }
    portal::detail::BusError error;
    sd_bus_message* reply = nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): sd-bus's typed call
    const int r = sd_bus_call_method(bus, login1_service, "/org/freedesktop/login1",
                                     "org.freedesktop.login1.Manager", "GetSession", error.get(), &reply, "s", id);
    const portal::detail::MessagePtr owned(reply);
    if (r < 0) {
        return display_session_path(bus);
    }
    std::string path;
    if (!MessageReader(owned.get()).string(path) || path.empty()) {
        return display_session_path(bus);
    }
    return path;
}

/// A boolean property of a logind session.
bool session_flag(sd_bus* bus, const std::string& path, const char* name)
{
    portal::detail::BusError error;
    int value = 0;
    return sd_bus_get_property_trivial(bus, login1_service, path.c_str(), session_interface, name, error.get(), 'b',
                                       &value) >= 0 &&
           value != 0;
}

/// The logind seat of the session at `path`; empty for a session without one.
std::string seat_path_of(sd_bus* bus, const std::string& path)
{
    portal::detail::BusError error;
    sd_bus_message* reply = nullptr;
    const int r =
        sd_bus_get_property(bus, login1_service, path.c_str(), session_interface, "Seat", error.get(), &reply, "(so)");
    const portal::detail::MessagePtr owned(reply);
    if (r < 0) {
        return {};
    }
    MessageReader reader(owned.get());
    std::string seat_id;
    std::string seat_path;
    if (!reader.enter('r', "so") || !reader.string(seat_id) || !reader.string(seat_path) || !reader.exit() ||
        seat_id.empty()) {
        return {};
    }
    return seat_path;
}

/// The freedesktop display manager object of the seat a logind seat path
/// names: SDDM and LightDM publish one per seat, named after the seat
/// ("/org/freedesktop/DisplayManager/Seat0").
std::string display_manager_seat(const std::string& logind_seat_path)
{
    const auto slash = logind_seat_path.rfind('/');
    if (slash == std::string::npos || slash + 1 >= logind_seat_path.size()) {
        return {};
    }
    std::string id = logind_seat_path.substr(slash + 1);
    if (id.empty()) {
        return {};
    }
    // logind names them "seat0"; the display manager names the object "Seat0".
    id[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(id[0])));
    return std::format("/org/freedesktop/DisplayManager/{}", id);
}

}  // namespace

LogindResult<void> activate_user_session()
{
    sd_bus* raw = nullptr;
    if (const int r = sd_bus_open_system(&raw); r < 0) {
        return fail(PortalErrc::unavailable, std::format("cannot connect to the system bus: {}", std::strerror(-r)));
    }
    const portal::detail::BusPtr bus(raw);
    FARLAND_TRY(const std::string path, our_session_path(raw));
    portal::detail::BusError error;
    // A session without a seat (a headless one the display manager started)
    // is always drawn.
    const std::string seat_path = seat_path_of(raw, path);
    if (seat_path.empty()) {
        return {};
    }
    std::uint32_t vt = 0;
    static_cast<void>(sd_bus_get_property_trivial(raw, login1_service, path.c_str(), session_interface, "VTNr",
                                                  error.get(), 'u', &vt));
    if (session_flag(raw, path, "Active")) {
        return {};
    }
    // Activate asks logind to bring it to the seat; where that is not enough
    // (GNOME 50 on some systems), the seat's own VT switch is.
    static_cast<void>(sd_bus_call_method(raw, login1_service, path.c_str(), session_interface, "Activate", error.get(),
                                         nullptr, ""));
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (int i = 0; i < 20 && !session_flag(raw, path, "Active"); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (session_flag(raw, path, "Active")) {
            return {};
        }
        if (attempt == 0 && !seat_path.empty() && vt != 0) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): sd-bus's typed call
            static_cast<void>(sd_bus_call_method(raw, login1_service, seat_path.c_str(), "org.freedesktop.login1.Seat",
                                                 "SwitchTo", error.get(), nullptr, "u", vt));
        }
    }
    return fail(PortalErrc::failed, "the session stays off its seat, so the desktop is not drawn");
}

namespace {

/// A greeter already on `seat_path`, if there is one: its logind object
/// path. Every display manager leaves the login screen running as a session
/// of class "greeter" on the seat.
std::string greeter_on_seat(sd_bus* bus, const std::string& seat_path)
{
    if (seat_path.empty()) {
        return {};
    }
    portal::detail::BusError error;
    sd_bus_message* reply = nullptr;
    const int r = sd_bus_get_property(bus, login1_service, seat_path.c_str(), "org.freedesktop.login1.Seat", "Sessions",
                                      error.get(), &reply, "a(so)");
    const portal::detail::MessagePtr owned(reply);
    if (r < 0) {
        return {};
    }
    MessageReader reader(owned.get());
    if (!reader.enter('a', "(so)")) {
        return {};
    }
    std::string found;
    while (found.empty() && reader.enter('r', "so")) {
        std::string id;
        std::string path;
        const bool read = reader.string(id) && reader.string(path);
        static_cast<void>(reader.exit());
        if (!read || path.empty()) {
            continue;
        }
        portal::detail::BusError class_error;
        char* session_class = nullptr;
        const int got = sd_bus_get_property_string(bus, login1_service, path.c_str(), session_interface, "Class",
                                                   class_error.get(), &session_class);
        if (got >= 0 && session_class != nullptr && std::string_view(session_class) == "greeter") {
            found = path;
        }
        ::free(session_class);  // NOLINT(cppcoreguidelines-no-malloc): sd-bus allocates it
    }
    static_cast<void>(reader.exit());
    return found;
}

}  // namespace

LogindResult<void> switch_seat_to_greeter()
{
    // The display manager's factory lives on the system bus; its policy lets
    // the user at the seat ask for a greeter, which is what "switch user"
    // does. GDM has its own API, SDDM and LightDM the freedesktop one.
    sd_bus* raw = nullptr;
    if (const int r = sd_bus_open_system(&raw); r < 0) {
        return fail(PortalErrc::unavailable, std::format("cannot connect to the system bus: {}", std::strerror(-r)));
    }
    const portal::detail::BusPtr system_bus(raw);

    // A login screen is usually already there, and GDM's CreateTransientDisplay
    // makes another one every time it is called: four calls leave four
    // greeters, each with a GNOME Shell of its own, until the machine stops
    // answering. Switch to the one that exists instead.
    std::string seat_path;
    if (const auto path = our_session_path(raw); path) {
        seat_path = seat_path_of(raw, *path);
    }
    if (const std::string greeter = greeter_on_seat(raw, seat_path); !greeter.empty()) {
        portal::detail::BusError error;
        sd_bus_message* reply = nullptr;
        const int r = sd_bus_call_method(raw, login1_service, greeter.c_str(), session_interface, "Activate",
                                         error.get(), &reply, "");
        const portal::detail::MessagePtr owned(reply);
        if (r >= 0) {
            log::debug(log_component, "the seat already shows a greeter ({}), switching to it", greeter);
            return {};
        }
        log::debug(log_component, "cannot switch to the greeter on the seat: {}",
                   error.get()->message != nullptr ? error.get()->message : std::strerror(-r));
    }

    portal::detail::BusError gdm_error;
    sd_bus_message* reply = nullptr;
    const int gdm = sd_bus_call_method(raw, "org.gnome.DisplayManager", "/org/gnome/DisplayManager/LocalDisplayFactory",
                                       "org.gnome.DisplayManager.LocalDisplayFactory", "CreateTransientDisplay",
                                       gdm_error.get(), &reply, "");
    const portal::detail::MessagePtr owned(reply);
    if (gdm >= 0) {
        return {};
    }
    log::debug(log_component, "no greeter from GDM: {}", gdm_error.get()->message != nullptr ? gdm_error.get()->message
                                                                                            : std::strerror(-gdm));
    // SDDM and LightDM: the seat object of the seat this session is on.
    std::string seat = display_manager_seat(seat_path);
    if (seat.empty()) {
        seat = "/org/freedesktop/DisplayManager/Seat0";
    }
    portal::detail::BusError error;
    sd_bus_message* dm_reply = nullptr;
    const int r = sd_bus_call_method(raw, "org.freedesktop.DisplayManager", seat.c_str(),
                                     "org.freedesktop.DisplayManager.Seat", "SwitchToGreeter", error.get(), &dm_reply,
                                     "");
    const portal::detail::MessagePtr dm_owned(dm_reply);
    if (r < 0) {
        return portal::detail::fail_call("SwitchToGreeter", error.get(), r);
    }
    return {};
}

// --- SeatWatch

LogindResult<std::unique_ptr<SeatWatch>> SeatWatch::create()
{
    auto watch = std::unique_ptr<SeatWatch>(new SeatWatch());
    sd_bus* raw = nullptr;
    if (const int r = sd_bus_open_system(&raw); r < 0) {
        return fail(PortalErrc::unavailable, std::format("cannot connect to the system bus: {}", std::strerror(-r)));
    }
    watch->bus_.reset(raw);
    FARLAND_TRY(watch->session_path_, our_session_path(raw));
    sd_bus_slot* slot = nullptr;
    const int r = sd_bus_match_signal_async(raw, &slot, login1_service, watch->session_path_.c_str(),
                                            "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                            &SeatWatch::on_properties_changed, nullptr, watch.get());
    if (r < 0) {
        return fail(PortalErrc::protocol, std::format("cannot watch the session: {}", std::strerror(-r)));
    }
    watch->watch_.reset(slot);
    watch->active_ = session_flag(raw, watch->session_path_, "Active");
    return watch;
}

int SeatWatch::fd() const noexcept
{
    return bus_ ? sd_bus_get_fd(bus_.get()) : -1;
}

void SeatWatch::process()
{
    if (!bus_) {
        return;
    }
    while (sd_bus_process(bus_.get(), nullptr) > 0) {
    }
}

void SeatWatch::set_active(bool active)
{
    // Only a session that was ours to draw can lose its seat.
    if (active_ && !active) {
        left_seat_ = true;
    }
    // And only one that lost it can come back: somebody logged in at the
    // machine, and the seat has the session again.
    if (left_seat_ && !active_ && active) {
        returned_ = true;
    }
    active_ = active;
}

int SeatWatch::on_properties_changed(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/)
{
    auto* self = static_cast<SeatWatch*>(userdata);
    MessageReader reader(message);
    std::string interface;
    if (!reader.string(interface) || interface != session_interface) {
        return 0;
    }
    bool active = false;
    bool seen = false;
    const bool ok = reader.vardict([&](std::string_view key, std::string_view signature, bool& good) {
        if (key != "Active" || signature != "b") {
            return false;
        }
        good = reader.boolean(active);
        seen = good;
        return true;
    });
    if (ok && seen) {
        self->set_active(active);
    }
    return 0;
}

}  // namespace farland::platform::logind
