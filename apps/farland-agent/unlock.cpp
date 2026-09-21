// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "unlock.hpp"

#include <farland/base/log.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#ifdef FARLAND_HAVE_LIBSYSTEMD
#include <cstdint>
#include <cstring>
#include <memory>
#include <systemd/sd-bus.h>
#endif

namespace farland::agent {

namespace {

[[maybe_unused]] constexpr std::string_view log_component = "agent.unlock";

}  // namespace

#ifdef FARLAND_HAVE_LIBSYSTEMD

namespace {

struct BusUnref {
    void operator()(sd_bus* bus) const noexcept { sd_bus_flush_close_unref(bus); }
};
using Bus = std::unique_ptr<sd_bus, BusUnref>;

struct MessageUnref {
    void operator()(sd_bus_message* m) const noexcept { sd_bus_message_unref(m); }
};
using Message = std::unique_ptr<sd_bus_message, MessageUnref>;

struct SlotUnref {
    void operator()(sd_bus_slot* slot) const noexcept { sd_bus_slot_unref(slot); }
};
using Slot = std::unique_ptr<sd_bus_slot, SlotUnref>;

class BusError {
public:
    BusError() = default;
    BusError(const BusError&) = delete;
    BusError& operator=(const BusError&) = delete;
    BusError(BusError&&) = delete;
    BusError& operator=(BusError&&) = delete;
    ~BusError() { sd_bus_error_free(&error_); }

    [[nodiscard]] sd_bus_error* get() noexcept { return &error_; }
    [[nodiscard]] std::string describe(int rc) const
    {
        if (error_.message != nullptr) {
            return error_.message;
        }
        return std::strerror(rc < 0 ? -rc : rc);
    }

private:
    sd_bus_error error_{};
};

/// GDM exports the verifier here on the private channel it hands back, and
/// it is the only object on it.
constexpr const char* verifier_path = "/org/gnome/DisplayManager/Session";
constexpr const char* verifier_interface = "org.gnome.DisplayManager.UserVerifier";
/// The PAM service GDM runs for a password login, the same one its greeter
/// uses: the password is checked by the system's own stack.
constexpr const char* password_service = "gdm-password";
/// How long the whole conversation may take. A PAM stack sleeps for a
/// moment after a wrong password, so this leaves room; a client is waiting
/// behind it, and the agent's loop is held meanwhile.
constexpr auto conversation_timeout = std::chrono::seconds(20);
constexpr std::uint64_t poll_us = 100000;

/// What GDM has asked, and how it ended. The conversation runs on signals:
/// GDM asks who is logging in, then for their secret, then says which way
/// it went.
struct Conversation {
    sd_bus* bus = nullptr;
    const std::string* user = nullptr;
    const SecretString* password = nullptr;
    bool verified = false;
    bool finished = false;
    std::string problem;
};

void answer(Conversation& talk, const char* service, std::string_view what, std::string_view text)
{
    BusError error;
    // sd-bus takes a NUL-terminated string, so the secret is copied once
    // more here; SecretString wipes that copy as this returns.
    const SecretString once{std::string(text)};
    const int rc = sd_bus_call_method(talk.bus, nullptr, verifier_path, verifier_interface, "AnswerQuery", error.get(),
                                      nullptr, "ss", service, once.view().data());
    if (rc < 0) {
        log::warn(log_component, "GDM would not take the {}: {}", what, error.describe(rc));
        talk.finished = true;
    }
}

extern "C" int on_verifier_signal(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/)
{
    auto* talk = static_cast<Conversation*>(userdata);
    const char* member = sd_bus_message_get_member(message);
    if (member == nullptr) {
        return 0;
    }
    const std::string_view name(member);
    if (name == "InfoQuery" || name == "SecretInfoQuery") {
        const char* service = nullptr;
        const char* query = nullptr;
        if (sd_bus_message_read(message, "ss", &service, &query) < 0 || service == nullptr) {
            return 0;
        }
        log::debug(log_component, "GDM asks ({}) {}: {}", name, service, query != nullptr ? query : "-");
        // A reauthentication of a session GDM already knows goes straight
        // to the password; one that asks who we are gets the account name.
        if (name == "InfoQuery") {
            log::debug(log_component, "answering with the account name [{}]", *talk->user);
            answer(*talk, service, "user name", *talk->user);
        } else {
            answer(*talk, service, "password", talk->password->view());
        }
        return 0;
    }
    if (name == "VerificationComplete") {
        talk->verified = true;
        talk->finished = true;
        return 0;
    }
    if (name == "VerificationFailed" || name == "ConversationStopped") {
        talk->finished = true;
        return 0;
    }
    if (name == "Problem" || name == "ServiceUnavailable" || name == "Info") {
        const char* service = nullptr;
        const char* text = nullptr;
        if (sd_bus_message_read(message, "ss", &service, &text) >= 0 && text != nullptr) {
            log::debug(log_component, "GDM says {}: {}", name, text);
            if (name != "Info") {
                talk->problem = text;
            }
        }
        return 0;
    }
    log::debug(log_component, "GDM says {}", name);
    return 0;
}

/// The UserVerifier conversation on the channel GDM handed back. Success is
/// VerificationComplete and nothing else.
[[nodiscard]] Result<void> verify_with_gdm(const std::string& address, const std::string& user,
                                           const SecretString& password,
                                           const std::function<void()>& still_waiting)
{
    sd_bus* raw = nullptr;
    if (sd_bus_new(&raw) < 0) {
        return fail(Errc::io, "no bus for the reauthentication channel");
    }
    const Bus bus(raw);
    // The channel is a private connection, not a bus: no Hello, no names,
    // and every call goes to the one peer at the other end.
    if (sd_bus_set_address(raw, address.c_str()) < 0 || sd_bus_set_bus_client(raw, 0) < 0 || sd_bus_start(raw) < 0) {
        return fail(Errc::io, "cannot open the reauthentication channel");
    }
    Conversation talk;
    talk.bus = raw;
    talk.user = &user;
    talk.password = &password;
    sd_bus_slot* slot = nullptr;
    if (sd_bus_add_filter(raw, &slot, on_verifier_signal, &talk) < 0) {
        return fail(Errc::io, "cannot listen on the reauthentication channel");
    }
    const Slot filter(slot);

    BusError error;
    const int rc = sd_bus_call_method(raw, nullptr, verifier_path, verifier_interface, "BeginVerification",
                                      error.get(), nullptr, "s", password_service);
    if (rc < 0) {
        log::warn(log_component, "GDM BeginVerification: {}", error.describe(rc));
        return fail(Errc::io, "GDM would not start the verification");
    }
    const auto deadline = std::chrono::steady_clock::now() + conversation_timeout;
    while (!talk.finished && std::chrono::steady_clock::now() < deadline) {
        const int processed = sd_bus_process(raw, nullptr);
        if (processed < 0) {
            return fail(Errc::io, "the reauthentication channel closed");
        }
        if (processed == 0 && sd_bus_wait(raw, poll_us) < 0) {
            return fail(Errc::io, "the reauthentication channel closed");
        }
        if (still_waiting) {
            still_waiting();
        }
    }
    if (!talk.verified) {
        BusError cancel;
        static_cast<void>(
            sd_bus_call_method(raw, nullptr, verifier_path, verifier_interface, "Cancel", cancel.get(), nullptr, ""));
        log::info(log_component, "GDM did not accept the credentials of {}{}", user,
                  talk.problem.empty() ? std::string() : ": " + talk.problem);
        return fail(Errc::io, "GDM did not accept the credentials");
    }
    return {};
}

}  // namespace

Result<void> unlock_the_session(const std::string& user, const SecretString& password,
                                const std::function<void()>& still_waiting)
{
    if (password.empty()) {
        // Nothing to prove anything with: the client delegated no password
        // (NLA without delegation, or a smart card). Whoever is at the
        // machine unlocks it, as they would for anybody else.
        return fail(Errc::io, "the client delegated no password to unlock with");
    }
    sd_bus* raw_system = nullptr;
    if (sd_bus_open_system(&raw_system) < 0) {
        return fail(Errc::io, "no system bus");
    }
    const Bus system(raw_system);

    BusError error;
    sd_bus_message* raw_reply = nullptr;
    const int rc = sd_bus_call_method(system.get(), "org.gnome.DisplayManager", "/org/gnome/DisplayManager/Manager",
                                      "org.gnome.DisplayManager.Manager", "OpenReauthenticationChannel", error.get(),
                                      &raw_reply, "s", user.c_str());
    const Message reply(raw_reply);
    if (rc < 0) {
        log::warn(log_component, "GDM OpenReauthenticationChannel for {}: {}", user, error.describe(rc));
        return fail(Errc::io, "GDM would not open a reauthentication channel");
    }
    const char* address = nullptr;
    if (sd_bus_message_read(raw_reply, "s", &address) < 0 || address == nullptr) {
        return fail(Errc::io, "GDM named no reauthentication channel");
    }
    FARLAND_TRY_VOID(verify_with_gdm(address, user, password, still_waiting));
    // GDM tells the shell itself; there is nothing here to force.
    log::info(log_component, "the screen at the machine accepted the credentials and unlocked the session");
    return {};
}

#else

Result<void> unlock_the_session(const std::string& /*user*/, const SecretString& /*password*/,
                                const std::function<void()>& /*still_waiting*/)
{
    return fail(Errc::unsupported, "this build has no systemd support");
}

#endif

}  // namespace farland::agent
