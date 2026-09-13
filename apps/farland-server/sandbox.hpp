// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace farland::app {

/// Confines the network process before it reads anything from a client
/// (docs/PLAN.md §6). Call single-threaded, after every file it needs is
/// loaded. Steps, each skipped where the platform lacks it:
///
/// - running as root: switch to user and group "nobody";
/// - no_new_privs, and a Landlock domain without any file system or TCP
///   access (Linux 5.13+, TCP from 6.7);
/// - a seccomp filter allowing only what I/O on inherited sockets, memory
///   allocation, time and randomness need (x86-64 and AArch64).
///
/// False when a step that must work failed (dropping root); the process
/// must then exit. Missing kernel support is logged, not fatal.
[[nodiscard]] bool enter_network_sandbox();

}  // namespace farland::app
