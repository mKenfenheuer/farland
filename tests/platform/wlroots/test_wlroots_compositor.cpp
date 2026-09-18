// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The wlroots desktop against real headless compositors (PLAN §5.4): sway,
// labwc and cage with the pixman renderer, each skipped when it is not
// installed. sway's configuration binds keys to commands that leave marker
// files, which shows that keys, modifiers and typed Unicode arrive.

#include "wlroots_headless.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <linux/input-event-codes.h>
#include <optional>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <variant>

namespace app = farland::app;
namespace platform = farland::platform;
using Clock = std::chrono::steady_clock;

namespace {

bool installed(const std::string& program)
{
    const char* path = std::getenv("PATH");
    std::string_view dirs = path != nullptr ? path : "/usr/bin:/bin";
    while (!dirs.empty()) {
        const auto colon = dirs.find(':');
        const std::filesystem::path candidate = std::filesystem::path(dirs.substr(0, colon)) / program;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
        dirs = colon == std::string_view::npos ? std::string_view() : dirs.substr(colon + 1);
    }
    return false;
}

/// Sets an environment variable for the scope.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name)
    {
        if (const char* old = std::getenv(name)) {
            old_ = old;
        }
        ::setenv(name, value.c_str(), 1);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ~ScopedEnv()
    {
        if (old_) {
            ::setenv(name_, old_->c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }

private:
    const char* name_;
    std::optional<std::string> old_;
};

class TempDir {
public:
    TempDir()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "farland-wlroots-test-XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        path_ = pattern;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

/// Runs the desktop (and its clipboard) until `done` or the time is up.
bool pump(app::Desktop& desktop, const std::function<bool()>& done,
          std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        std::vector<pollfd> fds;
        for (const int fd : desktop.dispatch_fds()) {
            fds.push_back(pollfd{fd, POLLIN, 0});
        }
        for (std::size_t i = 0; i < desktop.screen_count(); ++i) {
            fds.push_back(pollfd{desktop.screen_frames(i).wake_fd(), POLLIN, 0});
        }
        if (auto* clipboard = desktop.clipboard()) {
            for (const auto& fd : clipboard->poll_fds()) {
                fds.push_back(pollfd{fd.fd, fd.events, 0});
            }
        }
        ::poll(fds.data(), fds.size(), 50);
        desktop.dispatch();
        if (auto* clipboard = desktop.clipboard()) {
            clipboard->dispatch();
        }
        REQUIRE_FALSE(desktop.closed());
        if (done()) {
            return true;
        }
    }
    return false;
}

/// Takes frames until one has this size.
bool wait_for_size(app::Desktop& desktop, std::uint32_t width, std::uint32_t height)
{
    return pump(desktop, [&] {
        if (auto frame = desktop.frames().take_frame()) {
            return frame->image.width == width && frame->image.height == height &&
                   frame->image.data.size() >= std::size_t{frame->image.stride} * height;
        }
        return false;
    });
}

/// Moves the pointer and waits for the cursor session to report it there.
void check_pointer(app::Desktop& desktop, std::int32_t x, std::int32_t y)
{
    auto* cursor = desktop.cursor();
    if (cursor == nullptr) {
        // wlroots before 0.19 has no ext-image-copy-capture: screen copies
        // with the cursor in the picture, and no position to check.
        WARN("the compositor has no cursor capture; pointer position not checked");
        return;
    }
    desktop.input().pointer_motion_absolute(x, y);
    desktop.input().flush();
    std::optional<std::pair<std::int32_t, std::int32_t>> position;
    CHECK(pump(desktop, [&] {
        while (auto update = cursor->take_cursor()) {
            if (update->position) {
                position = update->position;
            }
        }
        return position == std::pair{x, y};
    }));
    CHECK(position == std::pair{x, y});
}

void check_start_and_resize(app::Desktop& desktop)
{
    REQUIRE(desktop.screen_count() == 1);
    CHECK(desktop.resizable());
    CHECK(wait_for_size(desktop, 640, 480));
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 1> sizes{std::pair{800U, 600U}};
    desktop.request_screen_sizes(sizes);
    CHECK(wait_for_size(desktop, 800, 600));
    CHECK(desktop.frames().size() == std::pair{800U, 600U});
    const std::array<std::optional<platform::Rect>, 1> targets{platform::Rect{0, 0, 800, 600}};
    CHECK(desktop.set_screen_targets(targets));
}

bool wait_for_file(app::Desktop& desktop, const std::filesystem::path& path)
{
    return pump(desktop, [&] { return std::filesystem::exists(path); });
}

}  // namespace

TEST_CASE("Headless sway: frames, resizing, cursor, keys, Unicode and the clipboard", "[wlroots][compositor][sway]")
{
    if (!installed("sway")) {
        SKIP("sway is not installed");
    }
    if (::geteuid() == 0) {
        SKIP("sway does not run as root");
    }
    TempDir dir;
    const auto marks = dir.path() / "marks";
    std::filesystem::create_directories(dir.path() / "sway");
    std::filesystem::create_directories(marks);
    {
        std::ofstream config(dir.path() / "sway" / "config");
        config << std::format("bindsym Shift+a exec touch {}/shift-a\n", marks.string())
               << std::format("bindsym EuroSign exec touch {}/euro\n", marks.string())
               << std::format("bindsym Ctrl+Alt+u exec touch {}/ctrl-alt-u\n", marks.string());
    }
    const ScopedEnv config_home("XDG_CONFIG_HOME", dir.path().string());

    app::HeadlessOptions options;
    options.kind = app::HeadlessKind::sway;
    options.width = 640;
    options.height = 480;
    options.keymap_layout = "us";
    options.timeout = std::chrono::seconds(20);
    auto started = app::start_wlroots_headless(options);
    REQUIRE(started);
    auto& desktop = **started;

    check_start_and_resize(desktop);
    check_pointer(desktop, 123, 234);
    check_pointer(desktop, 700, 50);

    auto& input = desktop.input();
    input.key(KEY_LEFTSHIFT, true);
    input.key(KEY_A, true);
    input.key(KEY_A, false);
    input.key(KEY_LEFTSHIFT, false);
    input.flush();
    CHECK(wait_for_file(desktop, marks / "shift-a"));

    input.key(KEY_LEFTCTRL, true);
    input.key(KEY_LEFTALT, true);
    input.key(KEY_U, true);
    input.key(KEY_U, false);
    input.key(KEY_LEFTALT, false);
    input.key(KEY_LEFTCTRL, false);
    input.flush();
    CHECK(wait_for_file(desktop, marks / "ctrl-alt-u"));

    // Not on a US keyboard: a spare key gets it.
    input.text(U'€');
    input.flush();
    CHECK(wait_for_file(desktop, marks / "euro"));

    // The clipboard: our own selection, read back through the compositor.
    auto* clipboard = desktop.clipboard();
    REQUIRE(clipboard != nullptr);
    clipboard->set_selection({"text/plain;charset=utf-8"});
    CHECK_FALSE(pump(desktop, [] { return false; }, std::chrono::milliseconds(300)));
    CHECK_FALSE(clipboard->mime_types());  // ours: nothing announced
    const auto id = clipboard->read("text/plain;charset=utf-8");
    std::optional<std::vector<std::byte>> data;
    bool finished = false;
    const std::string text = "farland clipboard";
    CHECK(pump(desktop, [&] {
        while (auto event = clipboard->poll_event()) {
            if (const auto* request = std::get_if<platform::clipboard_event::TransferRequested>(&*event)) {
                CHECK(request->mime_type == "text/plain;charset=utf-8");
                const auto bytes = std::as_bytes(std::span(text));
                clipboard->write(request->serial, std::vector<std::byte>(bytes.begin(), bytes.end()));
            } else if (const auto* read = std::get_if<platform::clipboard_event::ReadFinished>(&*event)) {
                CHECK(read->id == id);
                data = read->data;
                finished = true;
            }
        }
        return finished;
    }));
    REQUIRE(data);
    const auto expected = std::as_bytes(std::span(text));
    CHECK(std::ranges::equal(*data, expected));
}

TEST_CASE("Headless labwc: frames, resizing and the cursor", "[wlroots][compositor][labwc]")
{
    if (!installed("labwc")) {
        SKIP("labwc is not installed");
    }
    TempDir dir;
    const ScopedEnv config_home("XDG_CONFIG_HOME", dir.path().string());
    app::HeadlessOptions options;
    options.kind = app::HeadlessKind::labwc;
    options.width = 640;
    options.height = 480;
    options.timeout = std::chrono::seconds(20);
    auto started = app::start_wlroots_headless(options);
    REQUIRE(started);
    auto& desktop = **started;
    check_start_and_resize(desktop);
    check_pointer(desktop, 321, 123);
    CHECK(desktop.clipboard() != nullptr);
}

TEST_CASE("Headless cage: screen copies and resizing", "[wlroots][compositor][cage]")
{
    if (!installed("cage")) {
        SKIP("cage is not installed");
    }
    // cage 0.1 aborts on an output configuration (wlr_scene_output_layout_add_output).
    {
        std::string version;
        if (FILE* pipe = ::popen("cage -v 2>&1", "r")) {
            std::array<char, 128> line{};
            while (std::fgets(line.data(), static_cast<int>(line.size()), pipe) != nullptr) {
                version += line.data();
            }
            ::pclose(pipe);
        }
        if (version.find("version 0.1") != std::string::npos) {
            SKIP("cage 0.1 cannot resize its outputs");
        }
    }
    TempDir dir;
    const ScopedEnv config_home("XDG_CONFIG_HOME", dir.path().string());
    app::HeadlessOptions options;
    options.kind = app::HeadlessKind::cage;
    options.width = 640;
    options.height = 480;
    options.cage_command = {"sleep", "60"};
    options.timeout = std::chrono::seconds(20);
    auto started = app::start_wlroots_headless(options);
    REQUIRE(started);
    auto& desktop = **started;
    check_start_and_resize(desktop);
    // cage has no capture cursor and no data-control.
    CHECK(desktop.cursor() == nullptr);
    CHECK(desktop.clipboard() == nullptr);
    desktop.input().pointer_motion_absolute(10, 10);
    desktop.input().flush();
    CHECK_FALSE(pump(desktop, [] { return false; }, std::chrono::milliseconds(200)));
}
