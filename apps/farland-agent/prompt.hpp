// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/server/broker.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

/// The takeover prompt ([policy] takeover, apps/farlandd/consent.hpp). When
/// a connection would take this session away from whoever is using it,
/// farlandd holds that connection and asks the agent; the agent puts the
/// question on the user's desktop as a notification with two actions over
/// org.freedesktop.Notifications, which KDE and GNOME both serve, and
/// answers with what they said:
///
///     Hand over your session?
///     Someone is trying to connect to your session via RDP. Would you
///     like to hand over your session?
///     alice from 192.0.2.10:50123 (WORKSTATION), 14:32
///     [ Yes (30s) ]  [ Cancel ]
///
/// The countdown runs in the button that the timeout would press. Where no
/// notification service answers within a moment, the agent reports
/// `unavailable` instead of waiting, and farlandd applies its default.
namespace farland::agent {

/// What the user is asked, from farlandd's ConsentRequest.
struct ConsentQuestion {
    std::string user;         ///< the connecting user
    std::string peer;         ///< the client's address
    std::string client_name;  ///< what the client calls itself; may be empty
    std::chrono::seconds timeout{30};
    /// The timeout hands the session over, so the countdown runs in "Yes".
    bool allow_on_timeout = true;
    /// Somebody is logging in at the machine to take the session back, so
    /// the question names that instead of a connection.
    bool from_seat = false;
    /// The bus to ask on; empty: the user's session bus. The tests point it
    /// at their own.
    std::string bus_address;
};

/// Asks and waits for the answer, on a thread of its own. Returns early
/// when `cancel` becomes true; the answer is then thrown away.
using ConsentAsker =
    std::function<server::broker::ConsentAnswer(const ConsentQuestion& question, const std::atomic<bool>& cancel)>;

/// The real asker, over org.freedesktop.Notifications.
[[nodiscard]] server::broker::ConsentAnswer ask_over_notifications(const ConsentQuestion& question,
                                                                   const std::atomic<bool>& cancel);

/// The notification's one-line summary.
[[nodiscard]] std::string consent_summary();
/// Its body: the question, then the connection on a line of its own.
/// `when` is the local time, as "14:32".
[[nodiscard]] std::string consent_body(const ConsentQuestion& question, std::string_view when);
/// The buttons, in order: hand the session over, keep it. `left` runs down
/// in whichever of them the timeout would press.
[[nodiscard]] std::pair<std::string, std::string> consent_buttons(const ConsentQuestion& question,
                                                                  std::chrono::seconds left);

/// The local time as the prompt shows it.
[[nodiscard]] std::string local_time_now();

/// The action keys the notification carries.
inline constexpr std::string_view consent_action_allow = "farland-allow";
inline constexpr std::string_view consent_action_deny = "farland-deny";

}  // namespace farland::agent
