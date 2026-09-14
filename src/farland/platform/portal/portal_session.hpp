// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/platform/portal/sd_bus.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// The xdg-desktop-portal client of the portal backend (docs/PLAN.md §3.3):
/// one RemoteDesktop session with ScreenCast sources, from which the capture
/// code gets a PipeWire remote and the input code an EIS connection (or the
/// portal's Notify* methods, see portal_input.hpp).
///
/// It runs in the server's main process, never in the network process
/// (docs/PLAN.md §6).
namespace farland::platform::portal {

namespace detail {
class Bus;
}  // namespace detail

enum class PortalErrc : std::uint8_t {
    unavailable,    ///< No session bus, no portal, or the portal lacks RemoteDesktop or ScreenCast.
    unsupported,    ///< The portal lacks something farland needs, e.g. any usable source type.
    cancelled,      ///< The user dismissed the dialog (Request response 1).
    failed,         ///< The portal ended the request another way (response 2) or refused a call.
    timed_out,      ///< No answer within PortalOptions::timeout.
    aborted,        ///< PortalSession::cancel() was called.
    closed,         ///< The portal closed the session (the user stopped sharing).
    protocol,       ///< The bus connection broke, or a reply did not have the documented types.
    invalid_state,  ///< Called in the wrong state: start() twice, fds before start().
};

[[nodiscard]] std::string_view to_string(PortalErrc code) noexcept;

/// Portal errors carry text from the other side (D-Bus error names and
/// messages), so unlike farland::Error the message is owned.
struct PortalError {
    PortalErrc code{};
    std::string message;
};

template <class T>
using PortalResult = std::expected<T, PortalError>;

/// Owns a file descriptor.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept;
    ~UniqueFd();

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    /// Gives up ownership, for APIs that take it (pw_context_connect_fd, ei_setup_backend_fd).
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
    void reset() noexcept;

private:
    int fd_ = -1;
};

/// Bitmasks of org.freedesktop.portal.RemoteDesktop.AvailableDeviceTypes.
inline constexpr std::uint32_t device_keyboard = 1;
inline constexpr std::uint32_t device_pointer = 2;
inline constexpr std::uint32_t device_touchscreen = 4;

/// Bitmasks of org.freedesktop.portal.ScreenCast.AvailableSourceTypes.
inline constexpr std::uint32_t source_monitor = 1;
inline constexpr std::uint32_t source_window = 2;
inline constexpr std::uint32_t source_virtual = 4;

/// org.freedesktop.portal.ScreenCast.AvailableCursorModes.
enum class CursorMode : std::uint32_t {
    none = 0,  ///< Not negotiated (ScreenCast version 1): the portal's default, hidden.
    hidden = 1,
    embedded = 2,  ///< Drawn into the frames.
    metadata = 4,  ///< Sent as SPA_META_Cursor, for Pointer PDUs.
};

struct PortalOptions {
    /// D-Bus address of the bus the portal is on; empty means the session bus.
    std::string bus_address;
    /// A token from an earlier session (load_restore_token), so the portal
    /// can skip the dialog. Needs RemoteDesktop version 2; ignored before.
    std::optional<std::string> restore_token;
    /// Ask for persist_mode 2 (until revoked), which makes Start return a new
    /// restore token. Needs RemoteDesktop version 2.
    bool persist = true;
    /// Capture monitors (source type MONITOR).
    bool monitors = true;
    /// Also ask for a VIRTUAL source (a new monitor the compositor creates)
    /// when the portal offers it.
    bool virtual_monitor = false;
    /// Let the user pick several sources.
    bool multiple = true;
    /// Ask for touchscreen control too when the portal offers it.
    bool touchscreen = true;
    /// Cursor as metadata when offered (else embedded in the frames).
    bool cursor_metadata = true;
    /// Window identifier for the dialog (xdg-desktop-portal "parent_window"); empty for none.
    std::string parent_window;
    /// How long start() waits in total, including for the user to answer the dialog.
    std::chrono::milliseconds timeout = std::chrono::minutes(5);
};

/// What the portal offers, read before the session is created.
struct PortalCapabilities {
    std::uint32_t remote_desktop_version = 0;
    std::uint32_t screen_cast_version = 0;
    std::uint32_t device_types = 0;  ///< device_* bits
    std::uint32_t source_types = 0;  ///< source_* bits
    std::uint32_t cursor_modes = 0;  ///< CursorMode bits; 0 before ScreenCast version 2
};

/// One ScreenCast stream of the started session.
struct PortalStream {
    /// The PipeWire node to connect to on the remote from open_pipewire_remote().
    std::uint32_t node_id = 0;
    /// Opaque stream id, stable across restored sessions (ScreenCast v4); may be empty.
    std::string id;
    /// Position in the compositor's logical coordinate space; monitors only.
    std::optional<std::pair<std::int32_t, std::int32_t>> position;
    /// Logical size (may differ from the pixel size of the PipeWire buffers).
    std::optional<std::pair<std::int32_t, std::int32_t>> size;
    /// A source_* value; 0 when the portal does not say (ScreenCast < v3).
    std::uint32_t source_type = 0;
    /// Matches the libei region of this stream (ScreenCast v5); may be empty.
    std::string mapping_id;
};

/// A portal RemoteDesktop session with screen cast sources.
///
/// Threading: not thread-safe. start() blocks the calling thread; afterwards
/// every call must come from one thread at a time (normally the session
/// thread), except cancel(), which any thread and signal handlers may call.
/// sd-bus refuses to be used after fork(), so create the session in the
/// process that uses it.
class PortalSession {
public:
    PortalSession();
    PortalSession(const PortalSession&) = delete;
    PortalSession& operator=(const PortalSession&) = delete;
    PortalSession(PortalSession&&) = delete;
    PortalSession& operator=(PortalSession&&) = delete;
    /// Closes the portal session (org.freedesktop.portal.Session.Close).
    ~PortalSession();

    /// Creates and starts the session. The portal typically shows a dialog,
    /// so this can take as long as the user needs, up to `options.timeout`.
    /// Can only be called once; after an error the session is closed and a
    /// new PortalSession is needed.
    [[nodiscard]] PortalResult<void> start(const PortalOptions& options);

    /// Makes a running or later start() fail with PortalErrc::aborted.
    /// Thread- and async-signal-safe.
    void cancel() noexcept;

    /// A new fd for the PipeWire remote that carries the streams
    /// (ScreenCast.OpenPipeWireRemote). Pass it to pw_context_connect_fd().
    [[nodiscard]] PortalResult<UniqueFd> open_pipewire_remote();
    /// An fd for libei (RemoteDesktop.ConnectToEIS, ei_setup_backend_fd()),
    /// or nullopt when the portal is older than RemoteDesktop version 2. The
    /// portal allows one call per session; afterwards it rejects Notify*
    /// input, so PortalNotifyInput can no longer be used.
    [[nodiscard]] PortalResult<std::optional<UniqueFd>> connect_to_eis();

    // After start():
    [[nodiscard]] const PortalCapabilities& capabilities() const noexcept { return capabilities_; }
    [[nodiscard]] const std::vector<PortalStream>& streams() const noexcept { return streams_; }
    /// device_* bits the user granted.
    [[nodiscard]] std::uint32_t devices() const noexcept { return devices_; }
    [[nodiscard]] CursorMode cursor_mode() const noexcept { return cursor_mode_; }
    [[nodiscard]] bool clipboard_enabled() const noexcept { return clipboard_enabled_; }
    /// The token to pass as PortalOptions::restore_token next time (save it
    /// with save_restore_token); nullopt when the portal did not grant
    /// persistence. Tokens are single-use: always store the newest one.
    [[nodiscard]] const std::optional<std::string>& restore_token() const noexcept { return restore_token_; }
    [[nodiscard]] const std::string& session_handle() const noexcept { return session_handle_; }

    /// Session lifetime after start(): poll `fd()` for `events()` (POLLIN,
    /// plus POLLOUT while messages are queued) and call `process()` when it
    /// is ready. It dispatches D-Bus traffic, including the Closed signal
    /// (the portal or the user ended the session). -1 before start().
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] short events() const noexcept;
    void process();
    /// True once the portal closed the session or the bus connection broke.
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    /// Called from start() or process() when the session gets closed by the other side.
    void set_closed_callback(std::function<void()> callback) { closed_callback_ = std::move(callback); }

    /// For PortalNotifyInput.
    [[nodiscard]] detail::Bus* bus() const noexcept { return bus_.get(); }

private:
    enum class State : std::uint8_t { idle, starting, started, failed };

    PortalResult<void> run_start(const PortalOptions& options, std::chrono::steady_clock::time_point deadline);
    PortalResult<void> read_capabilities(std::chrono::steady_clock::time_point deadline);
    PortalResult<void> watch_session(const std::string& handle);
    [[nodiscard]] PortalResult<void> check_started() const;
    void mark_closed(std::string_view reason);
    void close_portal_session() noexcept;
    static int on_closed(sd_bus_message* message, void* userdata, sd_bus_error* error);

    UniqueFd cancel_fd_;
    std::unique_ptr<detail::Bus> bus_;
    State state_ = State::idle;
    std::string portal_owner_;
    std::string session_handle_;
    std::vector<detail::SlotPtr> closed_watches_;
    bool closed_ = false;
    std::function<void()> closed_callback_;

    PortalCapabilities capabilities_;
    std::vector<PortalStream> streams_;
    std::uint32_t devices_ = 0;
    CursorMode cursor_mode_ = CursorMode::none;
    bool clipboard_enabled_ = false;
    std::optional<std::string> restore_token_;
};

/// Where farland keeps the restore token: $XDG_STATE_HOME/farland/portal-restore-token,
/// else ~/.local/state/farland/portal-restore-token; nullopt without either variable.
[[nodiscard]] std::optional<std::filesystem::path> default_restore_token_path();
/// Reads a token saved by save_restore_token; nullopt when the file does not
/// exist. Refuses files that group or others can access, and malformed tokens.
[[nodiscard]] Result<std::optional<std::string>> load_restore_token(const std::filesystem::path& path);
/// Writes the token atomically with mode 0600, creating the directory with
/// mode 0700 if needed.
[[nodiscard]] Result<void> save_restore_token(const std::filesystem::path& path, std::string_view token);
/// Deletes the token file; a missing file is fine.
[[nodiscard]] Result<void> remove_restore_token(const std::filesystem::path& path);
/// Tokens are 1 to 1024 printable ASCII characters without spaces.
[[nodiscard]] bool valid_restore_token(std::string_view token) noexcept;

}  // namespace farland::platform::portal
