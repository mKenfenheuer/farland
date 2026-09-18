// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "prompt.hpp"

#include <farland/base/log.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <memory>
#include <string>

#if defined(FARLAND_HAVE_LIBSYSTEMD)
#include <optional>
#include <systemd/sd-bus.h>
#include <vector>
#endif

namespace farland::agent {

namespace {

namespace broker = server::broker;
[[maybe_unused]] constexpr std::string_view log_component = "agent.consent";

/// Notification bodies may carry a little markup, so what a client and NLA
/// gave us goes in escaped.
std::string escaped(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '&') {
            out += "&amp;";
        } else if (c == '<') {
            out += "&lt;";
        } else if (c == '>') {
            out += "&gt;";
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

std::string consent_summary()
{
    return "Hand over your session?";
}

std::string consent_body(const ConsentQuestion& question, std::string_view when)
{
    std::string details = escaped(question.user);
    if (!question.peer.empty()) {
        details += details.empty() ? "" : " from ";
        details += escaped(question.peer);
    }
    if (!question.client_name.empty()) {
        details += std::format(" ({})", escaped(question.client_name));
    }
    if (!when.empty()) {
        details += details.empty() ? "" : ", ";
        details += escaped(when);
    }
    if (question.from_seat) {
        return std::format("Someone is logging in at the machine and wants this session back. Would you like to hand "
                           "over your session?\n{}",
                           details);
    }
    return std::format("Someone is trying to connect to your session via RDP. Would you like to hand over your "
                       "session?\n{}",
                       details);
}

std::pair<std::string, std::string> consent_buttons(const ConsentQuestion& question, std::chrono::seconds left)
{
    const auto seconds = std::max<std::int64_t>(left.count(), 0);
    const std::string countdown = std::format(" ({}s)", seconds);
    if (question.allow_on_timeout) {
        return {"Yes" + countdown, "Cancel"};
    }
    return {"Yes", "Cancel" + countdown};
}

std::string local_time_now()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    if (::localtime_r(&now, &local) == nullptr) {
        return {};
    }
    std::array<char, 16> text{};
    const std::size_t written = std::strftime(text.data(), text.size(), "%H:%M", &local);
    return std::string(text.data(), written);
}

#if defined(FARLAND_HAVE_LIBSYSTEMD)

namespace {

constexpr const char* notifications_service = "org.freedesktop.Notifications";
constexpr const char* notifications_path = "/org/freedesktop/Notifications";
constexpr const char* notifications_interface = "org.freedesktop.Notifications";
/// How long the notification service has to take the question; a desktop
/// without one must not hold the waiting client.
constexpr std::uint64_t notify_timeout_us = 2'000'000;
constexpr std::uint64_t update_timeout_us = 1'000'000;
/// How often the loop looks at the clock and the cancellation.
constexpr std::uint64_t poll_us = 100'000;
/// NotificationClosed reason 4 is "undefined"; a service that reports a
/// replaced notification that way has not lost the prompt.
constexpr std::uint32_t closed_undefined = 4;

struct BusDeleter {
    void operator()(sd_bus* bus) const noexcept { sd_bus_flush_close_unref(bus); }
};
struct MessageDeleter {
    void operator()(sd_bus_message* message) const noexcept { sd_bus_message_unref(message); }
};
struct SlotDeleter {
    void operator()(sd_bus_slot* slot) const noexcept { sd_bus_slot_unref(slot); }
};
using BusPtr = std::unique_ptr<sd_bus, BusDeleter>;
using MessagePtr = std::unique_ptr<sd_bus_message, MessageDeleter>;
using SlotPtr = std::unique_ptr<sd_bus_slot, SlotDeleter>;

class Error {
public:
    Error() = default;
    Error(const Error&) = delete;
    Error& operator=(const Error&) = delete;
    Error(Error&&) = delete;
    Error& operator=(Error&&) = delete;
    ~Error() { sd_bus_error_free(&error_); }

    [[nodiscard]] sd_bus_error* get() noexcept { return &error_; }
    [[nodiscard]] const char* message() const noexcept
    {
        if (error_.message != nullptr) {
            return error_.message;
        }
        return error_.name != nullptr ? error_.name : "no reply";
    }

private:
    sd_bus_error error_{};
};

/// The prompt on screen, as the signal handlers see it.
struct Prompt {
    std::uint32_t id = 0;
    std::optional<broker::ConsentAnswer> answer;
};

extern "C" int on_action_invoked(sd_bus_message* message, void* data, sd_bus_error* /*error*/)
{
    auto* prompt = static_cast<Prompt*>(data);
    std::uint32_t id = 0;
    const char* action = nullptr;
    if (sd_bus_message_read(message, "us", &id, &action) < 0 || action == nullptr) {
        return 0;
    }
    if (prompt->id == 0 || id != prompt->id || prompt->answer) {
        return 0;
    }
    const std::string_view key(action);
    if (key == consent_action_allow) {
        prompt->answer = broker::ConsentAnswer::allowed;
    } else if (key == consent_action_deny) {
        prompt->answer = broker::ConsentAnswer::denied;
    }
    return 0;
}

extern "C" int on_notification_closed(sd_bus_message* message, void* data, sd_bus_error* /*error*/)
{
    auto* prompt = static_cast<Prompt*>(data);
    std::uint32_t id = 0;
    std::uint32_t reason = 0;
    if (sd_bus_message_read(message, "uu", &id, &reason) < 0) {
        return 0;
    }
    if (prompt->id == 0 || id != prompt->id || prompt->answer || reason == closed_undefined) {
        return 0;
    }
    // Dismissed without answering: the default applies, as for a timeout.
    prompt->answer = broker::ConsentAnswer::timed_out;
    return 0;
}

BusPtr open_bus(const std::string& address)
{
    sd_bus* raw = nullptr;
    if (address.empty()) {
        return sd_bus_open_user(&raw) < 0 ? BusPtr() : BusPtr(raw);
    }
    if (sd_bus_new(&raw) < 0) {
        return {};
    }
    BusPtr bus(raw);
    if (sd_bus_set_address(raw, address.c_str()) < 0 || sd_bus_set_bus_client(raw, 1) < 0 || sd_bus_start(raw) < 0) {
        return {};
    }
    return bus;
}

/// Sends one Notify, replacing notification `id` (0: a new one). Returns the
/// notification's id, or 0 when the service did not take it.
std::uint32_t notify(sd_bus* bus, const ConsentQuestion& question, std::uint32_t id, std::chrono::seconds left,
                     std::string_view when, std::uint64_t timeout_us)
{
    sd_bus_message* raw = nullptr;
    if (sd_bus_message_new_method_call(bus, &raw, notifications_service, notifications_path, notifications_interface,
                                       "Notify") < 0) {
        return 0;
    }
    const MessagePtr call(raw);
    const std::string summary = consent_summary();
    const std::string body = consent_body(question, when);
    const auto [allow, deny] = consent_buttons(question, left);
    // No "default" action: clicking the notification itself must not decide
    // anything. The two buttons are key and label, as the specification has
    // them.
    std::array<std::string, 4> actions{std::string(consent_action_allow), allow, std::string(consent_action_deny),
                                       deny};
    std::vector<char*> strv;
    for (auto& action : actions) {
        strv.push_back(action.data());
    }
    strv.push_back(nullptr);
    if (sd_bus_message_append(raw, "susss", "farland", id, "", summary.c_str(), body.c_str()) < 0 ||
        sd_bus_message_append_strv(raw, strv.data()) < 0 ||
        // Critical urgency keeps it on screen; the countdown ends it, not
        // the service.
        sd_bus_message_append(raw, "a{sv}", 2, "urgency", "y", 2, "category", "s", "device") < 0 ||
        sd_bus_message_append(raw, "i", 0) < 0) {
        return 0;
    }
    Error error;
    sd_bus_message* reply_raw = nullptr;
    if (sd_bus_call(bus, raw, timeout_us, error.get(), &reply_raw) < 0) {
        log::warn(log_component, "the notification service did not take the takeover prompt: {}", error.message());
        return 0;
    }
    const MessagePtr reply(reply_raw);
    std::uint32_t assigned = 0;
    if (sd_bus_message_read(reply_raw, "u", &assigned) < 0) {
        return 0;
    }
    return assigned;
}

void close_notification(sd_bus* bus, std::uint32_t id)
{
    Error error;
    static_cast<void>(sd_bus_call_method(bus, notifications_service, notifications_path, notifications_interface,
                                         "CloseNotification", error.get(), nullptr, "u", id));
}

}  // namespace

broker::ConsentAnswer ask_over_notifications(const ConsentQuestion& question, const std::atomic<bool>& cancel)
{
    using Clock = std::chrono::steady_clock;
    auto bus = open_bus(question.bus_address);
    if (!bus) {
        log::warn(log_component, "no session bus to ask about the takeover on");
        return broker::ConsentAnswer::unavailable;
    }
    Prompt prompt;
    sd_bus_slot* action_slot = nullptr;
    sd_bus_slot* closed_slot = nullptr;
    const bool matched =
        sd_bus_match_signal(bus.get(), &action_slot, notifications_service, notifications_path, notifications_interface,
                            "ActionInvoked", on_action_invoked, &prompt) >= 0 &&
        sd_bus_match_signal(bus.get(), &closed_slot, notifications_service, notifications_path, notifications_interface,
                            "NotificationClosed", on_notification_closed, &prompt) >= 0;
    const SlotPtr actions(action_slot);
    const SlotPtr closed(closed_slot);
    if (!matched) {
        return broker::ConsentAnswer::unavailable;
    }

    const auto started = Clock::now();
    const auto deadline = started + question.timeout;
    const std::string when = local_time_now();
    const auto left_at = [&deadline](Clock::time_point now) {
        return std::chrono::duration_cast<std::chrono::seconds>(deadline - now + std::chrono::milliseconds(999));
    };
    prompt.id = notify(bus.get(), question, 0, left_at(started), when, notify_timeout_us);
    if (prompt.id == 0) {
        return broker::ConsentAnswer::unavailable;
    }
    if (question.from_seat) {
        log::info(log_component, "asking whether {}, logging in at the machine, may take the session back ({} s)",
                  question.user, question.timeout.count());
    } else {
        log::info(log_component, "asking whether {} from {} may take the session over ({} s)", question.user,
                  question.peer, question.timeout.count());
    }

    auto shown = left_at(started);
    while (!prompt.answer && !cancel.load() && Clock::now() < deadline) {
        const int processed = sd_bus_process(bus.get(), nullptr);
        if (processed < 0) {
            return broker::ConsentAnswer::unavailable;
        }
        if (processed == 0 && sd_bus_wait(bus.get(), poll_us) < 0) {
            return broker::ConsentAnswer::unavailable;
        }
        if (const auto left = left_at(Clock::now()); left != shown && !prompt.answer && !cancel.load()) {
            shown = left;
            // The countdown runs down in the button: the same notification,
            // replaced with new labels.
            if (notify(bus.get(), question, prompt.id, left, when, update_timeout_us) == 0) {
                break;
            }
        }
    }
    close_notification(bus.get(), prompt.id);
    return prompt.answer.value_or(broker::ConsentAnswer::timed_out);
}

#else

broker::ConsentAnswer ask_over_notifications(const ConsentQuestion& /*question*/, const std::atomic<bool>& /*cancel*/)
{
    // No sd-bus in this build; farlandd applies takeover_on_timeout.
    return broker::ConsentAnswer::unavailable;
}

#endif

}  // namespace farland::agent
