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

/// farlandd's configuration, /etc/farland/farland.toml (docs/ROADMAP.md M7):
///
///     [server]
///     bind = "0.0.0.0"                         # an IPv4 or IPv6 address
///     port = 3389
///     certificate = "/etc/farland/tls.crt"     # both or neither; neither: a self-signed one
///     private_key = "/etc/farland/tls.key"
///
///     [auth]
///     mode = "store"                           # store | kerberos
///     credential_store = "/var/lib/farland/users"
///     keytab = "/etc/krb5.keytab"              # kerberos only
///
///     [session]
///     desktop = "gnome"                        # gnome | plasma | sway | labwc | cage
///     command = ["firefox", "--kiosk"]         # cage only, and required there
///
///     [policy]
///     disconnected_timeout = "1h"              # seconds, or a number with s, m, h or d; 0: never
///     idle_timeout = 0
///     max_sessions = 0                         # 0: no limit
///     max_sessions_per_user = 1
///     on_local_session = "refuse"              # refuse | attach
///
/// Every key is optional. Unknown sections and keys are errors, so that a
/// typo cannot silently fall back to a default.
namespace farland::daemon {

inline constexpr std::string_view default_config_path = "/etc/farland/farland.toml";

enum class AuthMode : std::uint8_t {
    store,     ///< NT hashes in the credential store, enrolled after a PAM password check
    kerberos,  ///< GSSAPI with a keytab (not implemented yet)
};

enum class DesktopKind : std::uint8_t { gnome, plasma, sway, labwc, cage };

/// What to do when the user already has a session on a local seat.
enum class LocalSessionPolicy : std::uint8_t {
    refuse,  ///< refuse the connection (ERRINFO_SERVER_DENIED_CONNECTION)
    attach,  ///< share that session instead of starting a headless one
};

struct ServerSection {
    std::string bind = "0.0.0.0";
    std::uint16_t port = 3389;
    std::optional<std::filesystem::path> certificate;
    std::optional<std::filesystem::path> private_key;
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
    unsigned max_sessions = 0;  ///< 0: no limit
    unsigned max_sessions_per_user = 1;
    LocalSessionPolicy on_local_session = LocalSessionPolicy::refuse;
};

struct Config {
    ServerSection server;
    AuthSection auth;
    SessionSection session;
    PolicySection policy;
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

[[nodiscard]] std::string_view to_string(AuthMode mode) noexcept;
[[nodiscard]] std::string_view to_string(DesktopKind desktop) noexcept;
[[nodiscard]] std::string_view to_string(LocalSessionPolicy policy) noexcept;

}  // namespace farland::daemon
