// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

// sd-bus comes from libsystemd or, on systems without systemd, from basu
// (meson.build defines FARLAND_SD_BUS_BASU then).
#if defined(FARLAND_SD_BUS_BASU)
#if __has_include(<basu/sd-bus.h>)
#include <basu/sd-bus.h>
#else
#include <sd-bus.h>
#endif
#else
#include <systemd/sd-bus.h>
#endif

#include <memory>

namespace farland::platform::portal::detail {

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

/// Owns an sd_bus_error (zero-initialised, which is SD_BUS_ERROR_NULL).
class BusError {
public:
    BusError() = default;
    BusError(const BusError&) = delete;
    BusError& operator=(const BusError&) = delete;
    BusError(BusError&&) = delete;
    BusError& operator=(BusError&&) = delete;
    ~BusError() { sd_bus_error_free(&error_); }

    [[nodiscard]] sd_bus_error* get() noexcept { return &error_; }
    [[nodiscard]] const char* name() const noexcept { return error_.name != nullptr ? error_.name : ""; }
    [[nodiscard]] const char* message() const noexcept { return error_.message != nullptr ? error_.message : ""; }

private:
    sd_bus_error error_{};
};

}  // namespace farland::platform::portal::detail
