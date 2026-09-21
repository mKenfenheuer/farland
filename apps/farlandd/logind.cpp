// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "logind.hpp"

#include <farland/base/log.hpp>

#include <thread>

#ifdef FARLAND_HAVE_LIBSYSTEMD
#include <cstdlib>
#include <cstring>
#include <memory>
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#endif

namespace farland::daemon {

namespace {

[[maybe_unused]] constexpr std::string_view log_component = "daemon.systemd";

}  // namespace

bool is_local_graphical(const LoginSession& session)
{
    const bool graphical = session.type == "wayland" || session.type == "x11" || session.type == "mir";
    const bool live = session.state == "active" || session.state == "online";
    return !session.seat.empty() && !session.remote && graphical && session.session_class == "user" && live;
}

std::optional<LoginSession> local_graphical_session(uid_t uid)
{
    for (auto& session : user_sessions(uid)) {
        if (is_local_graphical(session)) {
            return std::move(session);
        }
    }
    return std::nullopt;
}

bool session_shows_at_the_machine(uid_t uid)
{
    const auto session = local_graphical_session(uid);
    return session && session->state == "active";
}

#ifdef FARLAND_HAVE_LIBSYSTEMD

namespace {

struct BusUnref {
    void operator()(sd_bus* bus) const noexcept { sd_bus_flush_close_unref(bus); }
};
using Bus = std::unique_ptr<sd_bus, BusUnref>;

struct MessageUnref {
    void operator()(sd_bus_message* message) const noexcept { sd_bus_message_unref(message); }
};
using Message = std::unique_ptr<sd_bus_message, MessageUnref>;

/// sd_bus_error that frees itself.
class BusError {
public:
    BusError() = default;
    BusError(const BusError&) = delete;
    BusError& operator=(const BusError&) = delete;
    BusError(BusError&&) = delete;
    BusError& operator=(BusError&&) = delete;
    ~BusError() { sd_bus_error_free(&error_); }
    sd_bus_error* get() noexcept { return &error_; }
    [[nodiscard]] std::string describe(int rc) const
    {
        if (error_.message != nullptr) {
            return error_.message;
        }
        return std::strerror(-rc);
    }

private:
    sd_bus_error error_{};  // SD_BUS_ERROR_NULL, which is a C compound literal
};

/// A string from sd-login that must be freed.
std::string take(char* text)
{
    std::string out = text != nullptr ? text : "";
    std::free(text);  // NOLINT(cppcoreguidelines-no-malloc): sd-login allocates with malloc
    return out;
}

Bus open_system_bus()
{
    sd_bus* raw = nullptr;
    if (const int rc = sd_bus_open_system(&raw); rc < 0) {
        log::warn(log_component, "cannot connect to the system bus: {}", std::strerror(-rc));
        return nullptr;
    }
    return Bus(raw);
}

Bus open_user_bus(const std::string& user)
{
    sd_bus* raw = nullptr;
    // "user@.host": that user's service manager on this machine, through
    // systemd-machined's container logic (systemd 248); root may do this for
    // any user whose manager runs.
    const std::string machine = user + "@.host";
    if (const int rc = sd_bus_open_user_machine(&raw, machine.c_str()); rc < 0) {
        log::warn(log_component, "cannot connect to the service manager of {}: {}", user, std::strerror(-rc));
        return nullptr;
    }
    return Bus(raw);
}

/// Reads a string or object path variant; skips anything else.
std::optional<std::string> read_string_variant(sd_bus_message* m)
{
    char type = 0;
    const char* contents = nullptr;
    if (sd_bus_message_peek_type(m, &type, &contents) < 0 || contents == nullptr) {
        return std::nullopt;
    }
    if (std::strcmp(contents, "s") != 0 && std::strcmp(contents, "o") != 0) {
        sd_bus_message_skip(m, "v");
        return std::nullopt;
    }
    const char* value = nullptr;
    if (sd_bus_message_enter_container(m, 'v', contents) < 0 || sd_bus_message_read_basic(m, contents[0], &value) < 0) {
        return std::nullopt;
    }
    sd_bus_message_exit_container(m);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
}

/// GDM's displays (ObjectManager on /org/gnome/DisplayManager/Displays): the
/// session id of the one that runs a user session of `uid`, once GDM has set it.
std::optional<std::string> find_user_display_session(sd_bus* bus, uid_t uid)
{
    BusError error;
    sd_bus_message* raw = nullptr;
    const int rc = sd_bus_call_method(bus, "org.gnome.DisplayManager", "/org/gnome/DisplayManager/Displays",
                                      "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", error.get(), &raw, "");
    const Message reply(raw);
    if (rc < 0) {
        log::debug(log_component, "GDM GetManagedObjects: {}", error.describe(rc));
        return std::nullopt;
    }
    sd_bus_message* m = reply.get();
    if (sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}") < 0) {
        return std::nullopt;
    }
    std::optional<std::string> found;
    while (sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}") > 0) {
        const char* path = nullptr;
        sd_bus_message_read(m, "o", &path);
        std::optional<std::string> session_id;
        if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") > 0) {
            while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
                const char* interface = nullptr;
                sd_bus_message_read(m, "s", &interface);
                if (sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
                    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
                        const char* name = nullptr;
                        sd_bus_message_read(m, "s", &name);
                        const std::string_view property = name != nullptr ? name : "";
                        if (property == "SessionId") {
                            session_id = read_string_variant(m);
                        } else {
                            sd_bus_message_skip(m, "v");
                        }
                        sd_bus_message_exit_container(m);
                    }
                    sd_bus_message_exit_container(m);
                }
                sd_bus_message_exit_container(m);
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
        if (session_id && !session_id->empty()) {
            uid_t owner = 0;
            char* session_class = nullptr;
            const bool users = sd_session_get_uid(session_id->c_str(), &owner) >= 0 && owner == uid &&
                               sd_session_get_class(session_id->c_str(), &session_class) >= 0 &&
                               take(session_class) == "user";
            log::debug(log_component, "GDM display {} has session {}{}", path != nullptr ? path : "?", *session_id,
                       users ? " of the user" : "");
            if (users) {
                found = session_id;
            }
        }
    }
    return found;
}

}  // namespace

std::vector<LoginSession> user_sessions(uid_t uid)
{
    std::vector<LoginSession> sessions;
    char** ids = nullptr;
    const int count = sd_uid_get_sessions(uid, 0, &ids);
    if (count <= 0 || ids == nullptr) {
        std::free(static_cast<void*>(ids));  // NOLINT(cppcoreguidelines-no-malloc): sd-login allocates with malloc
        return sessions;
    }
    const std::span list(ids, static_cast<std::size_t>(count));
    for (char* id : list) {
        LoginSession session;
        session.id = id;
        char* text = nullptr;
        if (sd_session_get_seat(id, &text) >= 0) {
            session.seat = take(text);
        }
        text = nullptr;
        if (sd_session_get_type(id, &text) >= 0) {
            session.type = take(text);
        }
        text = nullptr;
        if (sd_session_get_class(id, &text) >= 0) {
            session.session_class = take(text);
        }
        text = nullptr;
        if (sd_session_get_state(id, &text) >= 0) {
            session.state = take(text);
        }
        session.remote = sd_session_is_remote(id) > 0;
        std::free(id);  // NOLINT(cppcoreguidelines-no-malloc)
        sessions.push_back(std::move(session));
    }
    std::free(static_cast<void*>(ids));  // NOLINT(cppcoreguidelines-no-malloc)
    return sessions;
}

bool session_locked_at_the_machine(uid_t uid)
{
    const auto session = local_graphical_session(uid);
    if (!session) {
        return false;
    }
    // sd-login has no accessor for LockedHint, so it comes off the bus.
    const Bus bus = open_system_bus();
    if (!bus) {
        return false;
    }
    BusError error;
    int locked = 0;
    // Object paths take only [A-Za-z0-9_], and systemd escapes the rest as
    // _<hex>; a session id of digits comes out as _3<digit> per digit.
    std::string path = "/org/freedesktop/login1/session/";
    for (const char c : session->id) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           ((c >= '0' && c <= '9') && path.back() != '/');
        if (plain) {
            path += c;
        } else {
            static constexpr std::string_view hex = "0123456789abcdef";
            path += '_';
            path += hex[(static_cast<unsigned char>(c) >> 4U) & 0xFU];
            path += hex[static_cast<unsigned char>(c) & 0xFU];
        }
    }
    if (sd_bus_get_property_trivial(bus.get(), "org.freedesktop.login1", path.c_str(),
                                    "org.freedesktop.login1.Session", "LockedHint", error.get(), 'b', &locked) < 0) {
        return false;
    }
    return locked != 0;
}

Result<void> terminate_login_session(const std::string& id)
{
    const Bus bus = open_system_bus();
    if (!bus) {
        return fail(Errc::io, "no system bus");
    }
    BusError error;
    const int rc =
        sd_bus_call_method(bus.get(), "org.freedesktop.login1", "/org/freedesktop/login1",
                           "org.freedesktop.login1.Manager", "TerminateSession", error.get(), nullptr, "s", id.c_str());
    if (rc < 0) {
        log::warn(log_component, "cannot terminate logind session {}: {}", id, error.describe(rc));
        return fail(Errc::io, "logind TerminateSession failed");
    }
    return {};
}

namespace {

/// The object path and interface of one of GDM's two display factories.
struct Factory {
    const char* path;
    const char* interface;
    const char* what;
};

Factory factory_of(DisplayFactory factory) noexcept
{
    if (factory == DisplayFactory::local) {
        return Factory{"/org/gnome/DisplayManager/LocalDisplayFactory",
                       "org.gnome.DisplayManager.LocalDisplayFactory", "seat"};
    }
    return Factory{"/org/gnome/DisplayManager/RemoteDisplayFactory", "org.gnome.DisplayManager.RemoteDisplayFactory",
                   "headless"};
}

}  // namespace

Result<std::string> create_gdm_user_display(const std::string& user, uid_t uid, DisplayFactory factory,
                                            std::chrono::seconds timeout)
{
    const Bus bus = open_system_bus();
    if (!bus) {
        return fail(Errc::io, "no system bus");
    }
    // Which session GDM made. A remote display says so itself, over its
    // SessionId property; a local one does not -- GDM (50.0, Ubuntu 26.04)
    // publishes a LocalDisplay object with no properties at all, so nothing
    // on that side ever names the session. logind does: a local display is
    // precisely a graphical session of the user on a seat, which is what
    // local_graphical_session() looks for.
    const auto session_of_the_display = [&bus, uid, factory]() -> std::optional<std::string> {
        if (factory == DisplayFactory::remote) {
            return find_user_display_session(bus.get(), uid);
        }
        if (auto session = local_graphical_session(uid)) {
            return session->id;
        }
        return std::nullopt;
    };
    // An earlier session of the user (farlandd restarted, say) is simply
    // used again.
    if (auto session = session_of_the_display()) {
        log::info(log_component, "GDM already runs session {} for {}", *session, user);
        return std::move(*session);
    }
    const Factory which = factory_of(factory);
    BusError error;
    const int rc = sd_bus_call_method(bus.get(), "org.gnome.DisplayManager", which.path, which.interface,
                                      "CreateUserDisplay", error.get(), nullptr, "s", user.c_str());
    if (rc < 0) {
        log::warn(log_component, "GDM CreateUserDisplay ({}) for {}: {}", which.what, user, error.describe(rc));
        return fail(Errc::io, "GDM refused to create a session");
    }
    log::info(log_component, "GDM starts a {} GNOME session for {}",
              factory == DisplayFactory::local ? "seat" : "headless", user);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto session = session_of_the_display()) {
            return std::move(*session);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    log::warn(log_component, "GDM's session for {} did not appear within {} s", user, timeout.count());
    static_cast<void>(destroy_gdm_user_display(user));
    return fail(Errc::io, "GDM's session did not start in time");
}

Result<void> destroy_gdm_user_display(const std::string& user)
{
    const Bus bus = open_system_bus();
    if (!bus) {
        return fail(Errc::io, "no system bus");
    }
    // Which factory has the display is farlandd's bookkeeping, and a restart
    // may have lost it; the other one simply has no display of that user.
    bool destroyed = false;
    for (auto factory : {DisplayFactory::local, DisplayFactory::remote}) {
        const Factory which = factory_of(factory);
        BusError error;
        const int rc = sd_bus_call_method(bus.get(), "org.gnome.DisplayManager", which.path, which.interface,
                                          "DestroyUserDisplay", error.get(), nullptr, "s", user.c_str());
        if (rc < 0) {
            log::debug(log_component, "GDM DestroyUserDisplay ({}) for {}: {}", which.what, user, error.describe(rc));
            continue;
        }
        destroyed = true;
    }
    if (!destroyed) {
        log::warn(log_component, "GDM has no display of {} to destroy", user);
        return fail(Errc::io, "GDM DestroyUserDisplay failed");
    }
    return {};
}

Result<void> start_user_unit(const std::string& user, const std::string& unit, const std::vector<std::string>& argv,
                             const std::vector<std::string>& environment)
{
    const Bus bus = open_user_bus(user);
    if (!bus) {
        return fail(Errc::io, "cannot reach the user's service manager");
    }
    // The strv appends take a NULL-terminated char* array.
    const auto strv = [](const std::vector<std::string>& strings) {
        std::vector<char*> pointers;
        pointers.reserve(strings.size() + 1);
        for (const auto& s : strings) {
            pointers.push_back(
                const_cast<char*>(s.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast): sd-bus only reads
        }
        pointers.push_back(nullptr);
        return pointers;
    };
    auto argv_pointers = strv(argv);
    auto environment_pointers = strv(environment);

    sd_bus_message* raw = nullptr;
    int rc = sd_bus_message_new_method_call(bus.get(), &raw, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                                            "org.freedesktop.systemd1.Manager", "StartTransientUnit");
    const Message m(raw);
    const auto check = [&rc](int result) {
        if (rc >= 0 && result < 0) {
            rc = result;
        }
    };
    check(rc);
    check(sd_bus_message_append(m.get(), "ss", unit.c_str(), "fail"));
    check(sd_bus_message_open_container(m.get(), 'a', "(sv)"));
    check(sd_bus_message_append(m.get(), "(sv)", "Description", "s", "farland remote desktop session agent"));
    check(sd_bus_message_append(m.get(), "(sv)", "Type", "s", "exec"));
    check(sd_bus_message_append(m.get(), "(sv)", "CollectMode", "s", "inactive-or-failed"));
    check(sd_bus_message_open_container(m.get(), 'r', "sv"));
    check(sd_bus_message_append(m.get(), "s", "ExecStart"));
    check(sd_bus_message_open_container(m.get(), 'v', "a(sasb)"));
    check(sd_bus_message_open_container(m.get(), 'a', "(sasb)"));
    check(sd_bus_message_open_container(m.get(), 'r', "sasb"));
    check(sd_bus_message_append(m.get(), "s", argv.front().c_str()));
    check(sd_bus_message_append_strv(m.get(), argv_pointers.data()));
    check(sd_bus_message_append(m.get(), "b", 0));
    check(sd_bus_message_close_container(m.get()));
    check(sd_bus_message_close_container(m.get()));
    check(sd_bus_message_close_container(m.get()));
    check(sd_bus_message_close_container(m.get()));
    check(sd_bus_message_open_container(m.get(), 'r', "sv"));
    check(sd_bus_message_append(m.get(), "s", "Environment"));
    check(sd_bus_message_open_container(m.get(), 'v', "as"));
    check(sd_bus_message_append_strv(m.get(), environment_pointers.data()));
    check(sd_bus_message_close_container(m.get()));
    check(sd_bus_message_close_container(m.get()));
    check(sd_bus_message_close_container(m.get()));
    check(sd_bus_message_append(m.get(), "a(sa(sv))", 0));
    if (rc < 0) {
        log::warn(log_component, "cannot build StartTransientUnit: {}", std::strerror(-rc));
        return fail(Errc::io, "cannot build StartTransientUnit");
    }
    BusError error;
    sd_bus_message* reply = nullptr;
    rc = sd_bus_call(bus.get(), m.get(), 0, error.get(), &reply);
    sd_bus_message_unref(reply);
    if (rc < 0) {
        log::warn(log_component, "cannot start {} for {}: {}", unit, user, error.describe(rc));
        return fail(Errc::io, "StartTransientUnit failed");
    }
    return {};
}

Result<void> stop_user_unit(const std::string& user, const std::string& unit)
{
    const Bus bus = open_user_bus(user);
    if (!bus) {
        return fail(Errc::io, "cannot reach the user's service manager");
    }
    BusError error;
    const int rc = sd_bus_call_method(bus.get(), "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                                      "org.freedesktop.systemd1.Manager", "StopUnit", error.get(), nullptr, "ss",
                                      unit.c_str(), "replace");
    if (rc < 0) {
        log::debug(log_component, "cannot stop {} for {}: {}", unit, user, error.describe(rc));
        return fail(Errc::io, "StopUnit failed");
    }
    return {};
}

#else  // without libsystemd

std::vector<LoginSession> user_sessions(uid_t /*uid*/)
{
    return {};
}

bool session_locked_at_the_machine(uid_t /*uid*/)
{
    return false;
}

Result<void> terminate_login_session(const std::string& /*id*/)
{
    return fail(Errc::unsupported, "this build has no systemd support");
}

Result<std::string> create_gdm_user_display(const std::string& /*user*/, uid_t /*uid*/,
                                            DisplayFactory /*factory*/, std::chrono::seconds /*timeout*/)
{
    return fail(Errc::unsupported, "this build has no systemd support");
}

Result<void> destroy_gdm_user_display(const std::string& /*user*/)
{
    return fail(Errc::unsupported, "this build has no systemd support");
}

Result<void> start_user_unit(const std::string& /*user*/, const std::string& /*unit*/,
                             const std::vector<std::string>& /*argv*/, const std::vector<std::string>& /*environment*/)
{
    return fail(Errc::unsupported, "this build has no systemd support");
}

Result<void> stop_user_unit(const std::string& /*user*/, const std::string& /*unit*/)
{
    return fail(Errc::unsupported, "this build has no systemd support");
}

#endif

}  // namespace farland::daemon
