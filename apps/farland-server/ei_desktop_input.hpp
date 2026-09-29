// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "desktop.hpp"

#include <farland/platform/portal/ei_input.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace farland::app {

/// A desktop's input through libei, which needs nothing but its socket and
/// so can have a thread of its own. `companion` is whatever else belongs to
/// the input and must live and be serviced with it (KWin's EIS D-Bus
/// connection, whose end takes the devices away): its descriptors and
/// dispatch come along, and it goes after the libei connection.
class EiDesktopInput final : public DesktopInput {
public:
    struct Companion {
        std::shared_ptr<void> owner;
        std::function<int()> fd;
        std::function<void()> dispatch;
    };

    explicit EiDesktopInput(std::unique_ptr<platform::portal::EiInput> ei, Companion companion = {})
        : companion_(std::move(companion)), ei_(std::move(ei))
    {
    }
    ~EiDesktopInput() override { ei_.reset(); }  // before the companion
    EiDesktopInput(const EiDesktopInput&) = delete;
    EiDesktopInput& operator=(const EiDesktopInput&) = delete;
    EiDesktopInput(EiDesktopInput&&) = delete;
    EiDesktopInput& operator=(EiDesktopInput&&) = delete;

    [[nodiscard]] platform::portal::EiInput& ei() noexcept { return *ei_; }

    [[nodiscard]] platform::InputSink& sink() override { return *ei_; }
    [[nodiscard]] std::vector<int> fds() const override
    {
        std::vector<int> fds{ei_->fd()};
        if (companion_.fd) {
            fds.push_back(companion_.fd());
        }
        return fds;
    }
    void dispatch() override
    {
        if (companion_.dispatch) {
            companion_.dispatch();
        }
        ei_->dispatch();
        closed_.store(ei_->closed());
    }
    [[nodiscard]] bool closed() const override { return closed_.load(); }

private:
    Companion companion_;
    std::unique_ptr<platform::portal::EiInput> ei_;
    std::atomic<bool> closed_{false};
};

}  // namespace farland::app
