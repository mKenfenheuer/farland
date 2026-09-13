// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

/// Minimal structured-enough logging. `component` is a dotted subsystem name
/// such as "proto.x224" or "auth.ntlm". Formatting only happens when the level
/// is enabled. A journald sink replaces the stderr default in farlandd (M7).
namespace farland::log {

enum class Level : std::uint8_t { trace, debug, info, warn, error, off };

[[nodiscard]] std::string_view to_string(Level level) noexcept;

using Sink = void (*)(Level level, std::string_view component, std::string_view message);

/// Replaces the output sink; `nullptr` restores the default stderr sink.
void set_sink(Sink sink) noexcept;
void set_level(Level level) noexcept;
[[nodiscard]] bool enabled(Level level) noexcept;
void write(Level level, std::string_view component, std::string_view message);

template <class... Args>
void trace(std::string_view component, std::format_string<Args...> fmt, Args&&... args)
{
    if (enabled(Level::trace)) {
        write(Level::trace, component, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <class... Args>
void debug(std::string_view component, std::format_string<Args...> fmt, Args&&... args)
{
    if (enabled(Level::debug)) {
        write(Level::debug, component, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <class... Args>
void info(std::string_view component, std::format_string<Args...> fmt, Args&&... args)
{
    if (enabled(Level::info)) {
        write(Level::info, component, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <class... Args>
void warn(std::string_view component, std::format_string<Args...> fmt, Args&&... args)
{
    if (enabled(Level::warn)) {
        write(Level::warn, component, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <class... Args>
void error(std::string_view component, std::format_string<Args...> fmt, Args&&... args)
{
    if (enabled(Level::error)) {
        write(Level::error, component, std::format(fmt, std::forward<Args>(args)...));
    }
}

}  // namespace farland::log
