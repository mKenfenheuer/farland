// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "enrol.hpp"

#include <farland/auth/credential_store.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/base/log.hpp>
#include <farland/base/text.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>

#if defined(FARLAND_HAVE_LIBSYSTEMD) && defined(FARLAND_HAVE_PAM)
#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <security/pam_appl.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <vector>
#endif

namespace farland::daemon {

namespace {

[[maybe_unused]] constexpr std::string_view log_component = "daemon.enrol";

}  // namespace

Result<void> store_enrolment(const std::filesystem::path& path, const std::string& account, std::string_view domain,
                             std::string_view password)
{
    if (!auth::CredentialStore::valid_local_account(account) || !auth::CredentialStore::valid_name(account, false) ||
        !auth::CredentialStore::valid_name(domain, true) || password.empty()) {
        return fail(Errc::invalid_value, "invalid account, domain or password");
    }
    FARLAND_TRY(auto store, auth::CredentialStore::load(path));
    auto hash = auth::ntlm::nt_hash(password);
    store.set(account, domain, hash);
    secure_zero(hash);
    static_cast<void>(store.set_local_account(account, domain, account));
    return store.save(path);
}

#if defined(FARLAND_HAVE_LIBSYSTEMD) && defined(FARLAND_HAVE_PAM)

namespace {

/// Answers PAM's password prompts with the password; nothing else.
extern "C" int password_conversation(int count, const pam_message** messages, pam_response** responses, void* data)
{
    if (count <= 0 || count > PAM_MAX_NUM_MSG) {
        return PAM_CONV_ERR;
    }
    const auto* password = static_cast<const std::string*>(data);
    // PAM frees the responses with free(), so they come from calloc/strdup.
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    auto* replies = static_cast<pam_response*>(std::calloc(static_cast<std::size_t>(count), sizeof(pam_response)));
    if (replies == nullptr) {
        return PAM_BUF_ERR;
    }
    const std::span message_list(messages, static_cast<std::size_t>(count));
    const std::span reply_list(replies, static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < message_list.size(); ++i) {
        if (message_list[i]->msg_style == PAM_PROMPT_ECHO_OFF) {
            reply_list[i].resp = ::strdup(password->c_str());
        }
    }
    *responses = replies;
    return PAM_SUCCESS;
}

}  // namespace

bool check_account_password(const std::string& account, std::string_view password)
{
    std::string secret(password);
    const pam_conv conversation{password_conversation, &secret};
    pam_handle_t* handle = nullptr;
    int rc = pam_start("farland", account.c_str(), &conversation, &handle);
    if (rc == PAM_SUCCESS) {
        rc = pam_authenticate(handle, PAM_DISALLOW_NULL_AUTHTOK);
    }
    if (rc == PAM_SUCCESS) {
        rc = pam_acct_mgmt(handle, PAM_DISALLOW_NULL_AUTHTOK);
    }
    if (rc != PAM_SUCCESS) {
        log::warn(log_component, "PAM refuses {}: {}", account, handle != nullptr ? pam_strerror(handle, rc) : "?");
    }
    if (handle != nullptr) {
        pam_end(handle, rc);
    }
    secure_zero(std::as_writable_bytes(std::span(secret)));
    return rc == PAM_SUCCESS;
}

struct ControlService::Impl {
    Impl(std::filesystem::path store, SeatTakeoverGate* gate) : store_path(std::move(store)), seat(gate) {}
    ~Impl()
    {
        stop = true;
        if (thread.joinable()) {
            thread.join();
        }
        sd_bus_slot_unref(slot);
        sd_bus_flush_close_unref(bus);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    static int enrol_self(sd_bus_message* m, void* data, sd_bus_error* error);
    /// The display manager's PAM module, before it lets somebody in at the
    /// machine: is this account's session held by a client, and may the
    /// login go ahead?
    static int seat_takeover_pending(sd_bus_message* m, void* data, sd_bus_error* error);
    static int await_seat_takeover(sd_bus_message* m, void* data, sd_bus_error* error);
    [[nodiscard]] bool authorized(sd_bus_message* m, sd_bus_error* error) const;
    /// Only root asks about a login at the machine: the PAM module runs
    /// there, and an answer of "no" keeps somebody out.
    [[nodiscard]] static bool from_root(sd_bus_message* m);

    std::filesystem::path store_path;
    SeatTakeoverGate* seat = nullptr;
    sd_bus* bus = nullptr;
    sd_bus_slot* slot = nullptr;
    std::thread thread;
    std::atomic<bool> stop{false};
};

namespace {

// clang-format off
constexpr sd_bus_vtable control_vtable[] = {  // NOLINT(cppcoreguidelines-avoid-c-arrays): sd-bus takes a C array
    SD_BUS_VTABLE_START(0),
    // EnrolSelf(s password, s domain)
    SD_BUS_METHOD("EnrolSelf", "ss", "", ControlService::Impl::enrol_self, SD_BUS_VTABLE_UNPRIVILEGED),
    // SeatTakeoverPending(s account) -> (b asking, t cookie); answers at once.
    SD_BUS_METHOD("SeatTakeoverPending", "s", "bt", ControlService::Impl::seat_takeover_pending, 0),
    // AwaitSeatTakeover(t cookie, u timeout_ms) -> b allowed; waits.
    SD_BUS_METHOD("AwaitSeatTakeover", "tu", "b", ControlService::Impl::await_seat_takeover, 0),
    SD_BUS_VTABLE_END,
};
// clang-format on

}  // namespace

bool ControlService::Impl::authorized(sd_bus_message* m, sd_bus_error* error) const
{
    // polkit CheckAuthorization(subject, action_id, details, flags, cancellation_id)
    // with the caller's unique bus name as subject; flag 1 allows user
    // interaction (an agent asks for the password).
    const char* sender = sd_bus_message_get_sender(m);
    sd_bus_message* reply = nullptr;
    const int rc = sd_bus_call_method(bus, "org.freedesktop.PolicyKit1", "/org/freedesktop/PolicyKit1/Authority",
                                      "org.freedesktop.PolicyKit1.Authority", "CheckAuthorization", error, &reply,
                                      "(sa{sv})sa{ss}us", "system-bus-name", 1, "name", "s", sender,
                                      enrol_action.data(), 0, std::uint32_t{1}, "");
    if (rc < 0) {
        sd_bus_message_unref(reply);
        return false;
    }
    int is_authorized = 0;
    int is_challenge = 0;
    const bool ok = sd_bus_message_enter_container(reply, 'r', "bba{ss}") >= 0 &&
                    sd_bus_message_read(reply, "bb", &is_authorized, &is_challenge) >= 0 && is_authorized != 0;
    sd_bus_message_unref(reply);
    return ok;
}

bool ControlService::Impl::from_root(sd_bus_message* m)
{
    sd_bus_creds* creds = nullptr;
    uid_t uid = 0;
    const bool have_uid = sd_bus_query_sender_creds(m, SD_BUS_CREDS_EUID | SD_BUS_CREDS_AUGMENT, &creds) >= 0 &&
                          sd_bus_creds_get_euid(creds, &uid) >= 0;
    sd_bus_creds_unref(creds);
    return have_uid && uid == 0;
}

int ControlService::Impl::seat_takeover_pending(sd_bus_message* m, void* data, sd_bus_error* error)
{
    auto* self = static_cast<Impl*>(data);
    const char* account = nullptr;
    if (int rc = sd_bus_message_read(m, "s", &account); rc < 0) {
        return rc;
    }
    if (!from_root(m)) {
        return sd_bus_error_set(error, "org.farland.Farland1.Error.NotAllowed",
                                "only root asks about a login at the machine");
    }
    // Nothing to ask about is the common answer, and the caller is holding
    // up a login: say so at once.
    const auto cookie =
        self->seat != nullptr ? self->seat->begin(account != nullptr ? account : "") : std::optional<std::uint64_t>();
    if (!cookie) {
        return sd_bus_reply_method_return(m, "bt", 0, std::uint64_t{0});
    }
    log::info(log_component, "{} is logging in at the machine, and a client holds their session", account);
    return sd_bus_reply_method_return(m, "bt", 1, *cookie);
}

int ControlService::Impl::await_seat_takeover(sd_bus_message* m, void* data, sd_bus_error* error)
{
    auto* self = static_cast<Impl*>(data);
    std::uint64_t cookie = 0;
    std::uint32_t timeout_ms = 0;
    if (int rc = sd_bus_message_read(m, "tu", &cookie, &timeout_ms); rc < 0) {
        return rc;
    }
    if (!from_root(m)) {
        return sd_bus_error_set(error, "org.farland.Farland1.Error.NotAllowed",
                                "only root asks about a login at the machine");
    }
    if (self->seat == nullptr) {
        return sd_bus_reply_method_return(m, "b", 1);
    }
    const auto wait = std::min(std::chrono::milliseconds(timeout_ms),
                               std::chrono::duration_cast<std::chrono::milliseconds>(max_seat_takeover_wait));
    const bool allowed = self->seat->await(cookie, wait);
    return sd_bus_reply_method_return(m, "b", allowed ? 1 : 0);
}

int ControlService::Impl::enrol_self(sd_bus_message* m, void* data, sd_bus_error* error)
{
    auto* self = static_cast<Impl*>(data);
    const char* password = nullptr;
    const char* domain = nullptr;
    if (int rc = sd_bus_message_read(m, "ss", &password, &domain); rc < 0) {
        return rc;
    }
    sd_bus_creds* creds = nullptr;
    uid_t uid = 0;
    const bool have_uid = sd_bus_query_sender_creds(m, SD_BUS_CREDS_EUID | SD_BUS_CREDS_AUGMENT, &creds) >= 0 &&
                          sd_bus_creds_get_euid(creds, &uid) >= 0;
    sd_bus_creds_unref(creds);
    const passwd* user = have_uid ? ::getpwuid(uid) : nullptr;
    if (user == nullptr || uid == 0) {
        return sd_bus_error_set(error, "org.farland.Farland1.Error.NotAllowed", "only regular accounts enrol");
    }
    const std::string account = user->pw_name;
    if (!self->authorized(m, error)) {
        log::warn(log_component, "enrolment of {} not authorized by polkit", account);
        if (sd_bus_error_is_set(error) != 0) {
            return -EACCES;
        }
        return sd_bus_error_set(error, "org.farland.Farland1.Error.NotAuthorized", "not authorized");
    }
    if (!check_account_password(account, password)) {
        std::this_thread::sleep_for(std::chrono::seconds(2));  // as a failed login would
        return sd_bus_error_set(error, "org.farland.Farland1.Error.AuthenticationFailed",
                                "the password is not the account's password");
    }
    if (auto stored = store_enrolment(self->store_path, account, domain, password); !stored) {
        log::error(log_component, "cannot store the enrolment of {}: {}", account, stored.error().message());
        return sd_bus_error_set(error, "org.farland.Farland1.Error.Failed", "cannot write the credential store");
    }
    log::info(log_component, "{} enrolled for remote desktop login{}", account,
              *domain != '\0' ? std::string(" in domain ") + domain : std::string());
    return sd_bus_reply_method_return(m, "");
}

ControlService::ControlService(std::filesystem::path credential_store, SeatTakeoverGate* seat)
    : impl_(std::make_unique<Impl>(std::move(credential_store), seat))
{
}

ControlService::~ControlService() = default;

Result<void> ControlService::start()
{
    auto& s = *impl_;
    if (int rc = sd_bus_open_system(&s.bus); rc < 0) {
        log::warn(log_component, "no system bus for {}: {}", bus_name, std::strerror(-rc));
        return fail(Errc::io, "no system bus");
    }
    if (int rc =
            sd_bus_add_object_vtable(s.bus, &s.slot, object_path.data(), interface_name.data(), control_vtable, &s);
        rc < 0) {
        return fail(Errc::io, "cannot export the control object");
    }
    if (int rc = sd_bus_request_name(s.bus, bus_name.data(), 0); rc < 0) {
        log::warn(log_component, "cannot own {} (is the D-Bus policy installed?): {}", bus_name, std::strerror(-rc));
        return fail(Errc::io, "cannot own the bus name");
    }
    s.thread = std::thread([&s] {
        // One call at a time: polkit may wait for the user, and the store
        // has one writer.
        while (!s.stop.load()) {
            const int rc = sd_bus_process(s.bus, nullptr);
            if (rc < 0) {
                log::warn(log_component, "D-Bus: {}", std::strerror(-rc));
                break;
            }
            if (rc == 0) {
                sd_bus_wait(s.bus, 500'000);
            }
        }
    });
    log::info(log_component, "serving {} on the system bus", bus_name);
    return {};
}

#else

bool check_account_password(const std::string& /*account*/, std::string_view /*password*/)
{
    return false;
}

struct ControlService::Impl {
    std::filesystem::path store;
};

ControlService::ControlService(std::filesystem::path credential_store, SeatTakeoverGate* /*seat*/)
    : impl_(std::make_unique<Impl>(Impl{std::move(credential_store)}))
{
}

ControlService::~ControlService() = default;

Result<void> ControlService::start()
{
    return fail(Errc::unsupported, "this build has no D-Bus control API (needs libsystemd and PAM)");
}

#endif

}  // namespace farland::daemon
