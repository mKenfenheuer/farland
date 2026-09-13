// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>

#include <atomic>
#include <iostream>
#include <mutex>

namespace farland::log {

namespace {

// Process-wide logger state: global by design, atomics make it safe to change at runtime.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::mutex stderr_mutex;

void stderr_sink(Level level, std::string_view component, std::string_view message)
{
    const std::scoped_lock lock(stderr_mutex);
    std::clog << '[' << to_string(level) << "] " << component << ": " << message << '\n';
}

std::atomic<Level> current_level{Level::info};
std::atomic<Sink> current_sink{&stderr_sink};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace

std::string_view to_string(Level level) noexcept
{
    switch (level) {
    case Level::trace:
        return "trace";
    case Level::debug:
        return "debug";
    case Level::info:
        return "info";
    case Level::warn:
        return "warn";
    case Level::error:
        return "error";
    case Level::off:
        return "off";
    }
    return "unknown";
}

void set_sink(Sink sink) noexcept
{
    current_sink.store(sink != nullptr ? sink : &stderr_sink, std::memory_order_relaxed);
}

void set_level(Level level) noexcept
{
    current_level.store(level, std::memory_order_relaxed);
}

bool enabled(Level level) noexcept
{
    return level != Level::off && level >= current_level.load(std::memory_order_relaxed);
}

void write(Level level, std::string_view component, std::string_view message)
{
    current_sink.load(std::memory_order_relaxed)(level, component, message);
}

}  // namespace farland::log
