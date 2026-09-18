// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/server/broker.hpp>

#include "config.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// [policy] takeover: whoever holds a session is asked before another
/// connection takes it away (docs/ROADMAP.md M7). farlandd holds the new
/// connection meanwhile and sends the session's agent a ConsentRequest; the
/// agent puts the question on the user's desktop
/// (apps/farland-agent/consent.hpp) and answers with a ConsentReply.
///
/// This is the bookkeeping alone: which prompts are out, when each runs out
/// of time, and what its answer means. No I/O and no clock of its own, so
/// tests drive it with any time they like; farlandd does what it says.
namespace farland::daemon {

class ConsentBroker {
public:
    using Clock = std::chrono::steady_clock;

    /// What a connection may do with a session someone is holding.
    enum class Admission : std::uint8_t {
        hand_over,  ///< nobody holds it, or takeover = "always"
        ask,        ///< takeover = "ask": the holder decides
        refuse,     ///< takeover = "never"
    };

    /// Who wants the session, as the prompt names them.
    struct Party {
        std::string user;         ///< the connecting user, as NLA named them
        std::string peer;         ///< the client's address
        std::string client_name;  ///< what the client calls itself; may be empty
    };

    /// A prompt that is over, one way or another.
    struct Resolved {
        std::uint32_t session = 0;
        std::uint64_t connection = 0;
        bool allowed = false;
        /// The agent still shows the prompt; it takes a ConsentCancel.
        bool withdraw = false;
        std::string reason;  ///< for the log, and for the refused client
    };

    /// How much longer than the prompt's own countdown farlandd waits for
    /// the agent's answer, so that a slow notification service still gets
    /// its word in before the default applies.
    static constexpr Clock::duration answer_grace = std::chrono::seconds(3);

    explicit ConsentBroker(PolicySection policy) : policy_(policy) {}

    /// What to do with a connection for a session that `held` says someone
    /// is holding.
    [[nodiscard]] Admission admit(bool held) const noexcept;
    /// Why a refusal happened, for the log and the client.
    [[nodiscard]] std::string refusal(std::string_view account) const;

    /// Records the prompt for `connection` in `session` and returns the
    /// message to send. An earlier prompt for that connection is replaced.
    [[nodiscard]] server::broker::ConsentRequest ask(std::uint32_t session, std::uint64_t connection, const Party& who,
                                                     Clock::time_point now);

    /// The agent answered; nullopt when no prompt for `connection` is out.
    [[nodiscard]] std::optional<Resolved> answered(std::uint64_t connection, server::broker::ConsentAnswer answer);
    /// Nobody holds `session` any more: its prompts succeed and the agent
    /// takes them down.
    [[nodiscard]] std::vector<Resolved> released(std::uint32_t session);
    /// `session` is gone; its prompts fail and there is nobody left to tell.
    [[nodiscard]] std::vector<Resolved> ended(std::uint32_t session);
    /// The prompts whose time is up; each is reported once.
    [[nodiscard]] std::vector<Resolved> due(Clock::time_point now);
    /// Forget the prompt for `connection` without resolving it.
    void forget(std::uint64_t connection);

    [[nodiscard]] bool waiting(std::uint64_t connection) const;

private:
    struct Prompt {
        std::uint32_t session = 0;
        std::uint64_t connection = 0;
        Clock::time_point deadline;
    };

    /// What takeover_on_timeout says, and why, for the log. `withdraw`:
    /// the agent still has the prompt on screen.
    [[nodiscard]] Resolved on_default(const Prompt& prompt, std::string_view why, bool withdraw) const;
    /// Takes the prompts of `session` out of the list.
    [[nodiscard]] std::vector<Prompt> take(std::uint32_t session);

    PolicySection policy_;
    std::vector<Prompt> prompts_;
};

}  // namespace farland::daemon
