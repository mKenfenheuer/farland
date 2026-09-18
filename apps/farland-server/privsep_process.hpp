// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/gss.hpp>
#include <farland/auth/tls_identity.hpp>
#include <farland/base/unique_fd.hpp>

#include "session.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

/// The process split of privilege separation (docs/PLAN.md §6).
///
/// For every client the server (the monitor) spawns itself as a network
/// process. That process owns the client's socket: it runs TLS and
/// pre-authentication in a sandbox, asks the monitor to verify NTLM responses
/// (farland::server::privsep), and then relays the decrypted RDP stream over
/// a plaintext socket pair. The monitor, which alone reads the credential
/// store, runs the session on that stream once it has accepted the result.
namespace farland::app {

/// Descriptors the network process inherits.
inline constexpr int child_client_fd = 3;   ///< the client's TCP socket
inline constexpr int child_control_fd = 4;  ///< privsep messages to and from the monitor
inline constexpr int child_plain_fd = 5;    ///< the RDP stream after pre-authentication

/// Builds the CredSSP acceptor factory around a verifier.
using NlaFactoryMaker = std::function<server::PreAuth::NlaFactory(auth::NtlmVerifier& verifier)>;

/// How to start the network process: this executable, with the arguments
/// that recreate its configuration after "--privsep-child".
struct ChildLaunch {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
};

/// Monitor side of one client: spawns the network process, answers its
/// verification requests with `verifier`, then runs the session on the
/// stream it relays. Closes `client_fd`; reaps the process.
void run_monitored_session(int client_fd, const std::string& peer, const ChildLaunch& launch,
                           auth::NtlmVerifier& verifier, const SessionOptions& options, const std::atomic<bool>& stop);

/// A client whose network process finished pre-authentication: the
/// plaintext RDP stream it relays from now on.
struct AuthenticatedClient {
    server::Negotiation negotiation;
    UniqueFd plain;
    /// Relays until either side closes the stream; reap it with
    /// wait_network_process().
    pid_t network_process = -1;
};

/// The first half of run_monitored_session(), for farlandd: spawns the
/// network process for `client_fd` (closed here) and answers its
/// verification requests until pre-authentication is done. nullopt when the
/// client did not get through (the process is reaped then); `started` is
/// when the client connected, for the activation timeout.
[[nodiscard]] std::optional<AuthenticatedClient>
authenticate_monitored(int client_fd, const std::string& peer, const ChildLaunch& launch, auth::NtlmVerifier& verifier,
                       const SessionOptions& options, const std::atomic<bool>& stop,
                       std::chrono::steady_clock::time_point started);

/// Waits for a network process to exit, however long it relays, and logs
/// how it ended.
void wait_network_process(pid_t pid);

/// Network process main, after enter_network_sandbox(): serves the client on
/// the inherited descriptors. `make_nla` may be empty (TLS only). Returns the
/// exit status.
[[nodiscard]] int run_network_child(const std::string& peer, const auth::TlsIdentity& identity,
                                    const SessionOptions& options, const NlaFactoryMaker& make_nla);

/// The absolute path of the running executable, for ChildLaunch.
[[nodiscard]] std::filesystem::path current_executable(const char* argv0);

}  // namespace farland::app
