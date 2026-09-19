// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// farlandd's configuration, /etc/farland/farland.toml (docs/ROADMAP.md M7).
/// It configures an installed package completely: everything farland-server
/// takes on the command line that makes sense for a daemon-run session has a
/// key here, named after its option. data/farland.toml is the shipped file
/// and lists every key with its default.
///
///     [server]
///     bind = "0.0.0.0"                         # an IPv4 or IPv6 address
///     port = 3389
///     certificate = "/etc/farland/tls.crt"     # both or neither; neither: a self-signed one
///     private_key = "/etc/farland/tls.key"
///     log_level = "info"                       # trace | debug | info | warn | error
///
///     [auth]
///     mode = "store"                           # store | kerberos
///     credential_store = "/var/lib/farland/users"
///     keytab = "/etc/krb5.keytab"              # kerberos only
///
///     [session]
///     desktop = "gnome"                        # gnome | plasma | sway | labwc | cage | test
///     command = ["firefox", "--kiosk"]         # cage only, and required there
///
///     [policy]
///     disconnected_timeout = "1h"              # seconds, or a number with s, m, h or d; 0: never
///     idle_timeout = 0
///     activation_timeout = "60s"
///     max_sessions = 0                         # 0: no limit
///     max_sessions_per_user = 1
///     on_local_session = "separate"            # separate | attach | replace | refuse
///     takeover = "ask"                         # ask | always | never
///     takeover_timeout = "30s"
///     takeover_on_timeout = "allow"            # allow | deny
///
///     [graphics]
///     gfx_codec = "progressive"                # progressive | planar | avc420 | avc444
///     bitmap_codec = "planar"                  # planar | raw
///     h264_encoder = "auto"                    # auto | nvenc | vaapi | openh264 | x264
///     openh264 = "libopenh264.so.8"            # a soname or an absolute path
///     render_node = "/dev/dri/renderD128"
///     zero_copy = true
///     clearcodec = true
///     refine = true
///     frames_per_second = 30
///
///     [network]
///     autodetect = "full"                      # full | continuous | off
///
///     [audio]
///     playback = true
///     microphone = true
///
///     [clipboard]
///     enabled = true
///
/// Every key is optional. Unknown sections and keys are errors, so that a
/// typo cannot silently fall back to a default.
namespace farland::daemon {

inline constexpr std::string_view default_config_path = "/etc/farland/farland.toml";

enum class AuthMode : std::uint8_t {
    store,     ///< NT hashes in the credential store, enrolled after a PAM password check
    kerberos,  ///< GSSAPI with a keytab (not implemented yet)
};

/// `test` is the synthetic test pattern, for CI and first tests of farlandd.
enum class DesktopKind : std::uint8_t { gnome, plasma, sway, labwc, cage, test };

/// What to do when the user already has a session on a local seat.
enum class LocalSessionPolicy : std::uint8_t {
    refuse,    ///< refuse the connection (ERRINFO_SERVER_DENIED_CONNECTION)
    attach,    ///< share that session instead of starting a headless one
    replace,   ///< end that session and start a headless one instead
    separate,  ///< leave it alone and start a second, headless desktop
};

/// What a connection may do to a session someone else is holding: another
/// client, or the user at the machine with on_local_session = "attach".
enum class TakeoverPolicy : std::uint8_t {
    ask,     ///< the holder is asked and decides (a prompt in their session)
    always,  ///< take the session over without asking
    never,   ///< refuse while someone holds it
};

/// What happens when nobody answers the takeover prompt in time.
enum class TakeoverDefault : std::uint8_t { allow, deny };

/// farland-server's --log-level, in order of severity.
enum class LogLevel : std::uint8_t { trace, debug, info, warn, error };

/// Codec for the Graphics Pipeline (farland-server --gfx-codec).
enum class GfxCodec : std::uint8_t { progressive, planar, avc420, avc444 };

/// Codec for bitmap updates, for clients without the Graphics Pipeline
/// (farland-server --codec).
enum class BitmapCodec : std::uint8_t { planar, raw };

/// H.264 backend for avc420 and avc444 (farland-server --h264-encoder).
/// `automatic` tries the backends this build has, GPU first.
enum class H264Encoder : std::uint8_t { automatic, nvenc, vaapi, openh264, x264 };

/// Network characteristics detection (farland-server --autodetect).
enum class AutoDetect : std::uint8_t {
    off,         ///< no measurements; the quality tiers follow the GFX acknowledgements alone
    continuous,  ///< measure once the connection is active
    full,        ///< also measure at connect time, before licensing
};

struct ServerSection {
    std::string bind = "0.0.0.0";
    std::uint16_t port = 3389;
    std::optional<std::filesystem::path> certificate;
    std::optional<std::filesystem::path> private_key;
    /// The daemon's, the agents' and the network processes' log level;
    /// --log-level on the command line wins.
    LogLevel log_level = LogLevel::info;
};

struct AuthSection {
    AuthMode mode = AuthMode::store;
    std::filesystem::path credential_store = "/var/lib/farland/users";
    std::optional<std::filesystem::path> keytab;
};

struct SessionSection {
    DesktopKind desktop = DesktopKind::gnome;
    /// The kiosk application for cage, as an argument vector (no shell).
    std::vector<std::string> command;
};

struct PolicySection {
    /// How long a session without a client lives on; 0: forever.
    std::chrono::seconds disconnected_timeout{0};
    /// How long a connected client may stay without input before it is
    /// disconnected; 0: forever.
    std::chrono::seconds idle_timeout{0};
    /// How long a client may take from the TCP connection to an active RDP
    /// connection, pre-authentication and starting the desktop included.
    std::chrono::seconds activation_timeout{60};
    unsigned max_sessions = 0;  ///< 0: no limit
    unsigned max_sessions_per_user = 1;
    LocalSessionPolicy on_local_session = LocalSessionPolicy::separate;
    /// Whether whoever holds a session is asked before another connection
    /// takes it away.
    TakeoverPolicy takeover = TakeoverPolicy::ask;
    /// How long that question stands before `takeover_on_timeout` applies.
    std::chrono::seconds takeover_timeout{30};
    TakeoverDefault takeover_on_timeout = TakeoverDefault::allow;
    /// What somebody logging in at the machine may do to a session a client
    /// holds. Only the PAM module in the display manager's stack
    /// (packaging/pam/) makes this happen: without it the login goes ahead
    /// and the client simply loses the session.
    TakeoverPolicy seat_takeover = TakeoverPolicy::ask;
};

struct GraphicsSection {
    GfxCodec gfx_codec = GfxCodec::progressive;
    BitmapCodec bitmap_codec = BitmapCodec::planar;
    H264Encoder h264_encoder = H264Encoder::automatic;
    /// OpenH264 library to load; unset: the usual sonames.
    std::optional<std::string> openh264;
    /// DRM render node for the compositor and for NVENC and VA-API; unset:
    /// the first that works.
    std::optional<std::filesystem::path> render_node;
    /// Hand captured dmabufs to an H.264 encoder that takes them.
    bool zero_copy = true;
    /// Progressive surfaces: ClearCodec for text and UI tiles, and coarse
    /// first passes refined while the picture stands still.
    bool clearcodec = true;
    bool refine = true;
    /// Progressive surfaces: tiles that keep changing go through H.264, and a
    /// picture that stands still is sent once more losslessly.
    bool video_regions = true;
    bool lossless_still = true;
    unsigned frames_per_second = 30;
};

struct NetworkSection {
    AutoDetect autodetect = AutoDetect::full;
};

struct AudioSection {
    bool playback = true;    ///< the desktop's output on the client (rdpsnd)
    bool microphone = true;  ///< the client's microphone as a local source (audin)
};

struct CameraSection {
    bool enabled = true;  ///< the client's camera as a local camera (rdpecam)
};

struct ClipboardSection {
    bool enabled = true;  ///< cliprdr, where the desktop has a clipboard
};

struct Config {
    ServerSection server;
    AuthSection auth;
    SessionSection session;
    PolicySection policy;
    GraphicsSection graphics;
    NetworkSection network;
    AudioSection audio;
    CameraSection camera;
    ClipboardSection clipboard;
};

struct ConfigError {
    std::string message;
    std::uint32_t line = 0;  ///< 1-based; 0 when the error has no position
    std::uint32_t column = 0;

    /// "farland.toml:12:5: message".
    [[nodiscard]] std::string describe(std::string_view source) const;
};

[[nodiscard]] std::expected<Config, ConfigError> parse_config(std::string_view text);
/// Reads and parses `path`; a missing file gives the defaults.
[[nodiscard]] std::expected<Config, ConfigError> load_config(const std::filesystem::path& path);

/// Every setting, one "section.key = value" per line, in the order above:
/// what `farlandd --check-config` prints.
[[nodiscard]] std::string describe(const Config& config);

[[nodiscard]] std::string_view to_string(AuthMode mode) noexcept;
[[nodiscard]] std::string_view to_string(DesktopKind desktop) noexcept;
[[nodiscard]] std::string_view to_string(LocalSessionPolicy policy) noexcept;
[[nodiscard]] std::string_view to_string(TakeoverPolicy takeover) noexcept;
[[nodiscard]] std::string_view to_string(TakeoverDefault action) noexcept;
[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;
[[nodiscard]] std::string_view to_string(GfxCodec codec) noexcept;
[[nodiscard]] std::string_view to_string(BitmapCodec codec) noexcept;
[[nodiscard]] std::string_view to_string(H264Encoder encoder) noexcept;
[[nodiscard]] std::string_view to_string(AutoDetect autodetect) noexcept;

}  // namespace farland::daemon
