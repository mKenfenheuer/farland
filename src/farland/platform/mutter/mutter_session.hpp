// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/portal/portal_session.hpp>
#include <farland/platform/portal/sd_bus.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// The Mutter backend (docs/PLAN.md §3.3, ROADMAP M7): a GNOME desktop
/// driven through Mutter's own D-Bus API, as gnome-remote-desktop does it,
/// without xdg-desktop-portal and without a permission dialog. One
/// org.gnome.Mutter.RemoteDesktop session with a ScreenCast session tied to
/// it; each screen is a virtual monitor (ScreenCast.Session.RecordVirtual)
/// whose PipeWire stream the portal backend's PipeWireCapture consumes on the
/// user's PipeWire daemon; input goes through libei (ConnectToEIS, the
/// portal backend's EiInput) and the clipboard through MutterClipboard.
///
/// Mutter's API is private to the desktop and unversioned beyond its Version
/// properties; this follows Mutter 50 (RemoteDesktop 1, ScreenCast 4). Mutter
/// lets only the D-Bus connection that created a session use it, so every
/// call goes over the session's own connection.
namespace farland::platform::mutter {

/// The D-Bus plumbing and its error type come from the portal backend.
using portal::PortalErrc;
using portal::PortalError;
using portal::UniqueFd;
template <class T>
using MutterResult = portal::PortalResult<T>;

inline constexpr const char* remote_desktop_service = "org.gnome.Mutter.RemoteDesktop";
inline constexpr const char* screen_cast_service = "org.gnome.Mutter.ScreenCast";

/// Bits of org.gnome.Mutter.RemoteDesktop.SupportedDeviceTypes (and of
/// ConnectToEIS's "device-types").
inline constexpr std::uint32_t device_keyboard = 1;
inline constexpr std::uint32_t device_pointer = 2;
inline constexpr std::uint32_t device_touchscreen = 4;

struct MutterOptions {
    /// D-Bus address of the desktop's session bus; empty: this process's
    /// session bus (DBUS_SESSION_BUS_ADDRESS).
    std::string bus_address;
    /// How long create() waits for Mutter's services to appear on the bus
    /// (a compositor that is still starting), and for each call.
    std::chrono::milliseconds timeout = std::chrono::seconds(30);
    /// Asked while create() waits for Mutter; false ends the wait (the
    /// compositor that was to provide it exited). Null: wait the timeout.
    std::function<bool()> keep_waiting;
    /// ScreenCast "disable-animations": the desktop drops animations while
    /// the session runs (gnome-remote-desktop does this).
    bool disable_animations = false;
    /// RecordVirtual "keep-rendering-when-inactive": the session keeps
    /// drawing our virtual monitors while it is not active on its seat, so a
    /// client holds it while the seat shows a login screen. Ignored where
    /// Mutter has no ScreenCast 5 (MutterCapabilities::keep_rendering).
    bool keep_rendering_when_inactive = false;
};

/// What Mutter offers, read when the session is created.
struct MutterCapabilities {
    std::int32_t remote_desktop_version = 0;
    std::int32_t screen_cast_version = 0;
    std::uint32_t device_types = 0;  ///< device_* bits
    /// RemoteDesktop.Session.KeymapCapabilities lists XKB text keymaps, or
    /// is there but empty (Mutter 50.1).
    bool xkb_keymaps = false;
    /// RecordVirtual takes "keep-rendering-when-inactive" (ScreenCast 5):
    /// a virtual monitor of this session keeps being drawn while the session
    /// is not active on its seat.
    bool keep_rendering = false;
};

/// A virtual monitor's screen cast stream.
struct VirtualStream {
    std::string path;
    /// Names the libei region that covers this monitor (the stream's
    /// "mapping-id" parameter); empty when Mutter gives none.
    std::string mapping_id;
    /// The PipeWire node, once Mutter announced it (PipeWireStreamAdded,
    /// after the stream started).
    std::optional<std::uint32_t> node_id;
};

using StreamId = std::uint64_t;

/// A monitor of the session, as org.gnome.Mutter.DisplayConfig lists it.
struct Monitor {
    std::string connector;
    std::string vendor;
    std::string product;
    /// The mode it runs, else its preferred one (a mode id for set_monitors).
    std::string mode;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// A virtual monitor, as RecordVirtual creates them.
    bool is_virtual = false;
    /// It shows something (it has a logical monitor).
    bool active = false;
    bool primary = false;
    /// Where its logical monitor sits and how it is drawn, while it is
    /// active (for putting a layout back as it was).
    std::int32_t x = 0;
    std::int32_t y = 0;
    double scale = 1.0;
    std::uint32_t transform = 0;
};

/// One monitor of a configuration for set_monitors().
struct LogicalMonitor {
    std::int32_t x = 0;
    std::int32_t y = 0;
    double scale = 1.0;
    std::uint32_t transform = 0;
    bool primary = false;
    std::string connector;
    /// A mode id of that monitor (Monitor::mode).
    std::string mode;
};

/// The session's monitors with the serial its configuration has now.
struct MonitorState {
    std::uint32_t serial = 0;
    std::vector<Monitor> monitors;
    /// The virtual monitors, in the order Mutter created them (ours, in
    /// screen order), as connector names.
    [[nodiscard]] std::vector<std::string> virtual_connectors() const;
    /// The monitors that show something, as a configuration set_monitors()
    /// takes: what to put back after a takeover.
    [[nodiscard]] std::vector<LogicalMonitor> active_layout() const;
    /// `connectors` side by side at scale 1, the first one primary.
    [[nodiscard]] std::vector<LogicalMonitor> side_by_side(std::span<const std::string> connectors) const;
};

/// A Mutter RemoteDesktop session with its ScreenCast session.
///
/// Threading: not thread-safe; create() blocks, afterwards every call comes
/// from one thread at a time (the session thread). sd-bus refuses to work
/// after fork(), so create it in the process that uses it.
class MutterSession {
public:
    /// Connects to the bus, waits until Mutter's RemoteDesktop and ScreenCast
    /// services are there, and creates the two sessions (not started).
    [[nodiscard]] static MutterResult<std::unique_ptr<MutterSession>> create(const MutterOptions& options);

    MutterSession(const MutterSession&) = delete;
    MutterSession& operator=(const MutterSession&) = delete;
    MutterSession(MutterSession&&) = delete;
    MutterSession& operator=(MutterSession&&) = delete;
    /// Stops the session: Mutter removes its virtual monitors.
    ~MutterSession();

    /// A new virtual monitor (RecordVirtual with cursor-mode metadata and
    /// is-platform, so the desktop treats it as a real monitor). No "modes":
    /// it takes the size its PipeWire consumer asks for. Once the session
    /// runs, the stream is started at once.
    [[nodiscard]] MutterResult<StreamId> record_virtual();
    /// Starts the remote desktop session, and with it every stream recorded
    /// so far.
    [[nodiscard]] MutterResult<void> start();
    /// Stops a stream: its virtual monitor goes away. Does not wait.
    void stop_stream(StreamId id);
    /// The stream `id`; null once stopped or never recorded.
    [[nodiscard]] const VirtualStream* stream(StreamId id) const;

    /// The session's monitors (DisplayConfig.GetCurrentState).
    [[nodiscard]] MutterResult<MonitorState> monitors();
    /// Makes `layout` the session's monitors; every monitor not in it is
    /// switched off. The configuration is temporary, so it is not stored in
    /// the user's monitor configuration. `serial` comes from the monitors()
    /// the layout is based on.
    [[nodiscard]] MutterResult<void> set_monitors(std::uint32_t serial, std::span<const LogicalMonitor> layout);

    /// A socket for libei (RemoteDesktop.Session.ConnectToEIS) exposing the
    /// `device_types` Mutter supports.
    [[nodiscard]] MutterResult<UniqueFd> connect_to_eis(std::uint32_t device_types);
    /// Makes `keymap`, an XKB keymap in text format version 1, the desktop's
    /// keymap (RemoteDesktop.Session.SetKeymap; not locked, so the user can
    /// switch layouts). Fails with PortalErrc::unsupported where Mutter has
    /// no SetKeymap.
    [[nodiscard]] MutterResult<void> set_keymap(std::string_view keymap);

    [[nodiscard]] const MutterCapabilities& capabilities() const noexcept { return capabilities_; }
    [[nodiscard]] const std::string& session_path() const noexcept { return session_path_; }
    /// Mutter's unique bus name, which signals are matched on.
    [[nodiscard]] const std::string& mutter_owner() const noexcept { return owner_; }
    [[nodiscard]] bool started() const noexcept { return started_; }

    /// Poll fd() for events() and call process() when it is ready: it
    /// dispatches D-Bus traffic, including PipeWireStreamAdded and Closed.
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] short events() const noexcept;
    void process();
    /// Dispatches until `done()`, up to `timeout`; false on timeout, when
    /// the session closed or the connection broke.
    [[nodiscard]] bool process_until(const std::function<bool()>& done, std::chrono::milliseconds timeout);
    /// True once Mutter closed the session, Mutter left the bus, or the
    /// connection broke.
    [[nodiscard]] bool closed() const noexcept { return closed_; }

    /// For MutterClipboard.
    [[nodiscard]] portal::detail::Bus* bus() const noexcept { return bus_.get(); }
    [[nodiscard]] std::chrono::milliseconds call_timeout() const noexcept { return timeout_; }

private:
    struct Stream {
        MutterSession* session = nullptr;
        StreamId id = 0;
        VirtualStream info;
        portal::detail::SlotPtr watch;
    };

    MutterSession() = default;
    [[nodiscard]] MutterResult<void> connect(const MutterOptions& options);
    [[nodiscard]] MutterResult<void> wait_for_mutter(std::chrono::steady_clock::time_point until,
                                                     const std::function<bool()>& keep_waiting);
    [[nodiscard]] MutterResult<void> read_capabilities();
    [[nodiscard]] MutterResult<void> create_sessions(const MutterOptions& options);
    [[nodiscard]] MutterResult<portal::detail::SlotPtr> watch(const std::string& path, const char* interface,
                                                              const char* member, sd_bus_message_handler_t handler,
                                                              void* userdata);
    [[nodiscard]] MutterResult<void> start_stream(const std::string& path);
    [[nodiscard]] std::chrono::steady_clock::time_point deadline() const;
    void mark_closed(std::string_view reason);
    static int on_closed(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int on_name_owner_changed(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int on_stream_added(sd_bus_message* message, void* userdata, sd_bus_error* error);

    std::unique_ptr<portal::detail::Bus> bus_;
    std::chrono::milliseconds timeout_{};
    std::string owner_;
    std::string session_path_;
    std::string screen_cast_path_;
    MutterCapabilities capabilities_;
    /// MutterOptions::keep_rendering_when_inactive, and Mutter can do it.
    bool keep_rendering_when_inactive_ = false;
    std::vector<portal::detail::SlotPtr> watches_;
    std::map<StreamId, std::unique_ptr<Stream>> streams_;
    StreamId next_stream_ = 1;
    bool started_ = false;
    bool closed_ = false;
};

/// An XKB keymap in text format version 1 for `layout`: an XKB layout name
/// ("de"), with a variant in parentheses ("de(nodeadkeys)"), or several
/// separated by commas ("us,de"), compiled with the evdev rules and model
/// pc105. nullopt when the layout is unknown or farland was built without
/// libxkbcommon.
[[nodiscard]] std::optional<std::string> xkb_keymap_for_layout(std::string_view layout);

}  // namespace farland::platform::mutter
