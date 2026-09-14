// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/platform/backend.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ei;
struct ei_event;
struct ei_seat;

/// Input injection through libei (docs/PLAN.md §3.3): the sender side of the
/// emulated-input protocol whose socket the RemoteDesktop portal's
/// ConnectToEIS hands out. The compositor (the EIS server) offers a seat;
/// EiInput binds it and sends keys, pointer motion, buttons and wheel steps
/// through the devices the compositor creates for it.
///
/// Threading: libei is not thread-safe. Everything, including dispatch(),
/// runs on the session thread: poll fd() and call dispatch() when it becomes
/// readable.
namespace farland::platform::portal {

class EiInput final : public InputSink {
public:
    enum class State : std::uint8_t {
        connecting,    ///< The handshake with the EIS server is still running.
        connected,     ///< The server accepted the client (devices may still be missing).
        disconnected,  ///< The server went away or rejected the client. Final.
    };

    /// A device capability, as the EIS server grants them (libei's
    /// ei_device_capability).
    enum class Capability : std::uint8_t { keyboard, pointer, pointer_absolute, button, scroll, touch };

    /// Where one compositor output appears in the desktop, for absolute
    /// motion. `mapping_id` is the "mapping_id" of the portal's ScreenCast
    /// stream for that output, which the compositor also attaches to the EIS
    /// region covering it (libei >= 1.1, xdg-desktop-portal >= 1.18).
    struct Output {
        Rect desktop;
        std::string mapping_id;
    };

    /// Takes ownership of `fd`, the socket from the portal's ConnectToEIS,
    /// even on failure. The socket is switched to non-blocking mode.
    [[nodiscard]] static Result<std::unique_ptr<EiInput>> connect_fd(int fd);
    /// Connects to an EIS server listening on a Unix socket (tests, or a
    /// compositor's LIBEI_SOCKET). A relative `path` is relative to
    /// $XDG_RUNTIME_DIR.
    [[nodiscard]] static Result<std::unique_ptr<EiInput>> connect_socket(const std::string& path);

    /// Releases every key and button still held on a device that can take
    /// events, then disconnects.
    ~EiInput() override;

    /// Poll this for readability, then call dispatch().
    [[nodiscard]] int fd() const noexcept;
    /// Reads from the EIS server and handles seats, devices and disconnection.
    void dispatch();
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool closed() const noexcept { return state_ == State::disconnected; }
    /// True if a device with `capability` currently accepts events.
    [[nodiscard]] bool can_send(Capability capability) const noexcept;

    /// Tells absolute motion how the desktop is assembled from outputs.
    /// Without outputs (or when no output's mapping_id matches a region), each
    /// region is assumed to start at its own offset in desktop pixels and to
    /// cover its size times its physical scale: a 1920x1080 region with scale
    /// 2 is a 3840x2160 stream.
    void set_outputs(std::vector<Output> outputs);

    void key(std::uint32_t evdev_code, bool pressed) override;
    void pointer_motion_absolute(double x, double y) override;
    void pointer_motion_relative(double dx, double dy) override;
    void button(std::uint32_t evdev_button, bool pressed) override;
    void scroll_discrete(std::int32_t x_v120, std::int32_t y_v120) override;
    /// Ignored: EIS has no text input, only keycodes interpreted through the
    /// compositor's keymap (docs/PLAN.md §7).
    void text(char32_t codepoint) override;
    void flush() override;

    EiInput(const EiInput&) = delete;
    EiInput& operator=(const EiInput&) = delete;
    EiInput(EiInput&&) = delete;
    EiInput& operator=(EiInput&&) = delete;

private:
    struct Device;
    struct Target;

    explicit EiInput(struct ei* context) noexcept;
    [[nodiscard]] static Result<std::unique_ptr<EiInput>> finish_setup(struct ei* context, int rc);

    void handle_event(struct ei_event* event);
    void device_added(struct ei_event* event);
    void device_resumed(Device& device);
    static void device_paused(Device& device);
    void device_removed(struct ei_event* event);
    void drain_events();

    [[nodiscard]] Device* find(struct ei_event* event) const noexcept;
    [[nodiscard]] Device* pick(Capability capability) const noexcept;
    [[nodiscard]] std::vector<Target> absolute_targets() const;
    static void send_held(Device& device, bool key, std::uint32_t code, bool pressed);
    void press_or_release(bool key, std::uint32_t code, bool pressed);

    struct ei* ei_ = nullptr;
    struct ei_seat* seat_ = nullptr;
    State state_ = State::connecting;
    std::uint32_t sequence_ = 0;
    std::vector<std::unique_ptr<Device>> devices_;
    std::vector<Output> outputs_;
    /// The device that got the latest pointer motion; buttons and wheel steps
    /// prefer it so they land where the pointer is.
    Device* last_pointer_ = nullptr;
};

}  // namespace farland::platform::portal
