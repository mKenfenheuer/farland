// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace farland::detail {

[[noreturn]] void assert_failed(const char* expression, const char* file, int line) noexcept;

}  // namespace farland::detail

/// Always-on check for programmer errors (violated preconditions), also in
/// release builds. Never use it for anything a peer can trigger: malformed
/// input is a decoding error and is reported through `Result`.
#define FARLAND_ASSERT(cond)                                                                                           \
    ((cond) ? static_cast<void>(0) : ::farland::detail::assert_failed(#cond, __FILE__, __LINE__))
