// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/unique_fd.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace farland::platform::portal::detail {
class Bus;
}

/// Input into KWin without the portal: KWin's EIS plugin hands out libei
/// sockets on the session bus (org.kde.KWin, /org/kde/KWin/EIS/RemoteDesktop,
/// org.kde.KWin.EIS.RemoteDesktop.connectToEIS(i capabilities) -> (h, i
/// cookie)). KWin drops the socket's devices when the caller leaves the bus,
/// so the connection stays open as long as the input.
namespace farland::platform::kwin {

class KWinEis {
public:
    /// connectToEIS capabilities (the portal's device types).
    static constexpr int keyboard = 1;
    static constexpr int pointer = 2;
    static constexpr int touch = 4;

    /// Connects to the bus at `bus_address` (the session bus when empty) and
    /// asks KWin for an EIS socket, retrying until `timeout` while KWin has
    /// not claimed its name yet.
    [[nodiscard]] static Result<std::unique_ptr<KWinEis>> connect(const std::string& bus_address, int capabilities,
                                                                  std::chrono::milliseconds timeout);

    KWinEis(const KWinEis&) = delete;
    KWinEis& operator=(const KWinEis&) = delete;
    KWinEis(KWinEis&&) = delete;
    KWinEis& operator=(KWinEis&&) = delete;
    /// Tells KWin the socket is no longer needed.
    ~KWinEis();

    /// The EIS socket, for EiInput::connect_fd (which takes it).
    [[nodiscard]] int release_socket() noexcept { return socket_.release(); }
    /// The bus connection: poll it and call dispatch() when readable.
    [[nodiscard]] int bus_fd() const;
    void dispatch();

private:
    KWinEis() = default;

    std::unique_ptr<portal::detail::Bus> bus_;
    UniqueFd socket_;
    int cookie_ = 0;
};

}  // namespace farland::platform::kwin
