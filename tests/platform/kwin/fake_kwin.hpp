// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/unique_fd.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/// A stand-in for KWin in the backend's tests: a libwayland-server display
/// on a thread of its own with one client, offering one wl_output and a
/// seat, zkde_screencast_unstable_v1, kde_output_device_v2 and
/// kde_output_management_v2 (custom modes), and ext-data-control-v1.
/// The methods run on the server thread and return when done there.
namespace farland::test {

class FakeKWin {
public:
    struct Options {
        std::string output_name = "Virtual-0";
        int width = 1920;
        int height = 1080;
        bool screencast = true;
        /// Streams fail instead of getting a node.
        bool fail_streams = false;
        /// Output configurations fail.
        bool refuse_configurations = false;
    };

    FakeKWin();
    explicit FakeKWin(Options options);
    FakeKWin(const FakeKWin&) = delete;
    FakeKWin& operator=(const FakeKWin&) = delete;
    FakeKWin(FakeKWin&&) = delete;
    FakeKWin& operator=(FakeKWin&&) = delete;
    ~FakeKWin();

    /// The client end of the connection, once.
    [[nodiscard]] int take_client_fd();

    /// KWin ends every stream.
    void close_streams();
    /// The output's current mode.
    [[nodiscard]] std::pair<int, int> current_mode();
    /// Configurations applied (or refused) so far, and custom mode lists set.
    [[nodiscard]] int configurations();
    [[nodiscard]] int custom_mode_lists();

    /// A desktop application copies `data` as each of `mime_types`.
    void desktop_copy(std::vector<std::string> mime_types, std::string data);
    /// A desktop application pastes the client's selection: the read end of
    /// the transfer, invalid when the client owns no selection.
    [[nodiscard]] UniqueFd desktop_paste(const std::string& mime_type);
    /// The types of the client's selection; empty when it owns none.
    [[nodiscard]] std::vector<std::string> client_selection();

    struct State;

private:
    void run(const std::function<void()>& task);
    std::unique_ptr<State> state_;
};

}  // namespace farland::test
