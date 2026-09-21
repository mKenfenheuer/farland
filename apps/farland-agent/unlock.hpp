// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/text.hpp>

#include <functional>
#include <string>

/// Coming back to a session that is locked at the machine.
///
/// A GNOME session on a seat locks itself -- somebody locks it, or it idles
/// -- and a locked session refuses to be shared at all: Mutter answers
/// RemoteDesktop.CreateSession with "Session creation inhibited" for as long
/// as the lock screen is up. Without this, a client whose session locked
/// while they were away could never get back in without walking over to the
/// machine. Letting them in on the credentials they already presented is
/// what every other remote desktop does.
///
/// The unlock is GDM's own doing, not farland's: OpenReauthenticationChannel
/// gives back a private D-Bus channel, the UserVerifier conversation on it
/// runs the real PAM stack for the password, and on VerificationComplete the
/// shell takes its lock screen down by itself. farland neither decides that
/// the password is good nor reaches past anything that says it is not -- a
/// wrong password simply ends in VerificationFailed and the session stays
/// locked.
///
/// This must run from inside the user's own session: GDM works out which
/// session to reauthenticate from the caller's logind session, and a caller
/// that has none gets "No session available". That is why it lives in the
/// agent, which runs in the user's service manager, and not in farlandd,
/// which is a system service with no session of its own -- and it is the
/// better place for it anyway, since farlandd never has to hold the
/// authority to unlock anybody's screen.
namespace farland::agent {

/// Asks GDM to reauthenticate `user` with `password`, which on success
/// takes the lock screen down. An error means the session is still locked
/// and names why, for the log.
/// `still_waiting`, where given, is called while the conversation runs, so
/// that whoever is watching the caller keeps hearing from it: a PAM stack
/// can sit on a wrong password for seconds, and the agent's loop is held
/// for all of it.
[[nodiscard]] Result<void> unlock_the_session(const std::string& user, const SecretString& password,
                                              const std::function<void()>& still_waiting = {});

}  // namespace farland::agent
