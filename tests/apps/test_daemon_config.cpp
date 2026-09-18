// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace farland::daemon;
using namespace std::chrono_literals;

TEST_CASE("on_local_session takes attach, replace and refuse")
{
    for (const auto& [text, policy] :
         {std::pair{"attach", LocalSessionPolicy::attach}, std::pair{"replace", LocalSessionPolicy::replace},
          std::pair{"separate", LocalSessionPolicy::separate}, std::pair{"refuse", LocalSessionPolicy::refuse}}) {
        const auto config = parse_config(std::string("[policy]\non_local_session = \"") + text + "\"\n");
        REQUIRE(config.has_value());
        CHECK(config->policy.on_local_session == policy);
        CHECK(to_string(config->policy.on_local_session) == text);
    }
}

TEST_CASE("seat_takeover takes ask, always and never")
{
    for (const auto& [text, policy] :
         {std::pair{"ask", TakeoverPolicy::ask}, std::pair{"always", TakeoverPolicy::always},
          std::pair{"never", TakeoverPolicy::never}}) {
        const auto config = parse_config(std::string("[policy]\nseat_takeover = \"") + text + "\"\n");
        REQUIRE(config.has_value());
        CHECK(config->policy.seat_takeover == policy);
    }
    // Asking is the default, and only a policy it knows is taken.
    CHECK(parse_config("[policy]\n").value().policy.seat_takeover == TakeoverPolicy::ask);
    const auto bad = parse_config("[policy]\nseat_takeover = \"sometimes\"\n");
    REQUIRE(!bad.has_value());
    CHECK(bad.error().message.find("one of ask, always, never") != std::string::npos);
}

TEST_CASE("takeover takes ask, always and never")
{
    for (const auto& [text, policy] :
         {std::pair{"ask", TakeoverPolicy::ask}, std::pair{"always", TakeoverPolicy::always},
          std::pair{"never", TakeoverPolicy::never}}) {
        const auto config = parse_config(std::string("[policy]\ntakeover = \"") + text + "\"\n");
        REQUIRE(config.has_value());
        CHECK(config->policy.takeover == policy);
        CHECK(to_string(config->policy.takeover) == text);
    }
    const auto asked = parse_config(R"(
[policy]
takeover = "ask"
takeover_timeout = "45s"
takeover_on_timeout = "deny"
)")
                           .value();
    CHECK(asked.policy.takeover == TakeoverPolicy::ask);
    CHECK(asked.policy.takeover_timeout == 45s);
    CHECK(asked.policy.takeover_on_timeout == TakeoverDefault::deny);
    CHECK(to_string(asked.policy.takeover_on_timeout) == "deny");
    CHECK(to_string(TakeoverDefault::allow) == "allow");
    const auto printed = describe(asked);
    CHECK(printed.find("policy.takeover = \"ask\"\n") != std::string::npos);
    CHECK(printed.find("policy.takeover_timeout = 45\n") != std::string::npos);
    CHECK(printed.find("policy.takeover_on_timeout = \"deny\"\n") != std::string::npos);
    // Minutes are a duration like any other.
    CHECK(parse_config("[policy]\ntakeover_timeout = \"1m\"\n").value().policy.takeover_timeout == 60s);
    CHECK(parse_config("[policy]\ntakeover_timeout = 300\n").value().policy.takeover_timeout == 300s);
}

TEST_CASE("An empty farland.toml gives the defaults")
{
    const auto config = parse_config("").value();
    CHECK(config.server.bind == "0.0.0.0");
    CHECK(config.server.port == 3389);
    CHECK_FALSE(config.server.certificate.has_value());
    CHECK(config.auth.mode == AuthMode::store);
    CHECK(config.auth.credential_store == "/var/lib/farland/users");
    CHECK(config.session.desktop == DesktopKind::gnome);
    CHECK(config.session.command.empty());
    CHECK(config.server.log_level == LogLevel::info);
    CHECK(config.policy.disconnected_timeout == 0s);
    CHECK(config.policy.idle_timeout == 0s);
    CHECK(config.policy.activation_timeout == 60s);
    CHECK(config.policy.max_sessions == 0);
    CHECK(config.policy.max_sessions_per_user == 1);
    CHECK(config.policy.on_local_session == LocalSessionPolicy::separate);
    CHECK(to_string(config.policy.on_local_session) == "separate");
    CHECK(config.policy.takeover == TakeoverPolicy::ask);
    CHECK(config.policy.takeover_timeout == 30s);
    CHECK(config.policy.takeover_on_timeout == TakeoverDefault::allow);
    // The graphics, network, audio and clipboard defaults are
    // farland-server's, so the daemon and the command line agree.
    CHECK(config.graphics.gfx_codec == GfxCodec::progressive);
    CHECK(config.graphics.bitmap_codec == BitmapCodec::planar);
    CHECK(config.graphics.h264_encoder == H264Encoder::automatic);
    CHECK_FALSE(config.graphics.openh264.has_value());
    CHECK_FALSE(config.graphics.render_node.has_value());
    CHECK(config.graphics.zero_copy);
    CHECK(config.graphics.clearcodec);
    CHECK(config.graphics.refine);
    CHECK(config.graphics.frames_per_second == 30);
    CHECK(config.network.autodetect == AutoDetect::full);
    CHECK(config.audio.playback);
    CHECK(config.audio.microphone);
    CHECK(config.clipboard.enabled);
}

TEST_CASE("The shipped data/farland.toml parses and is the defaults")
{
    const auto path = std::filesystem::path(FARLAND_SOURCE_DATA_DIR) / "farland.toml";
    const auto config = load_config(path);
    REQUIRE(config.has_value());
    // Every key the file sets is commented out or set to its default, so a
    // key that the parser and the file disagree about shows up here.
    CHECK(describe(*config) == describe(Config{}));
}

TEST_CASE("Graphics, network, audio and clipboard parse")
{
    const auto config = parse_config(R"(
[graphics]
gfx_codec = "avc444"
bitmap_codec = "raw"
h264_encoder = "vaapi"
openh264 = "libopenh264.so.7"
render_node = "/dev/dri/renderD129"
zero_copy = false
clearcodec = false
refine = false
frames_per_second = 60

[network]
autodetect = "off"

[audio]
playback = false
microphone = false

[clipboard]
enabled = false

[server]
log_level = "debug"

[policy]
activation_timeout = "90s"
)")
                            .value();
    CHECK(config.graphics.gfx_codec == GfxCodec::avc444);
    CHECK(config.graphics.bitmap_codec == BitmapCodec::raw);
    CHECK(config.graphics.h264_encoder == H264Encoder::vaapi);
    CHECK(config.graphics.openh264 == "libopenh264.so.7");
    CHECK(config.graphics.render_node == std::filesystem::path("/dev/dri/renderD129"));
    CHECK_FALSE(config.graphics.zero_copy);
    CHECK_FALSE(config.graphics.clearcodec);
    CHECK_FALSE(config.graphics.refine);
    CHECK(config.graphics.frames_per_second == 60);
    CHECK(config.network.autodetect == AutoDetect::off);
    CHECK_FALSE(config.audio.playback);
    CHECK_FALSE(config.audio.microphone);
    CHECK_FALSE(config.clipboard.enabled);
    CHECK(config.server.log_level == LogLevel::debug);
    CHECK(config.policy.activation_timeout == 90s);

    CHECK(to_string(GfxCodec::avc420) == "avc420");
    CHECK(to_string(BitmapCodec::raw) == "raw");
    CHECK(to_string(H264Encoder::automatic) == "auto");
    CHECK(to_string(AutoDetect::continuous) == "continuous");
    CHECK(to_string(LogLevel::error) == "error");
    // --check-config prints one line per setting, in order.
    const auto printed = describe(config);
    CHECK(printed.starts_with("server.bind = \"0.0.0.0\"\n"));
    CHECK(printed.find("graphics.gfx_codec = \"avc444\"\n") != std::string::npos);
    CHECK(printed.find("graphics.openh264 = \"libopenh264.so.7\"\n") != std::string::npos);
    CHECK(printed.find("clipboard.enabled = false\n") != std::string::npos);
    CHECK(describe(Config{}).find("graphics.render_node = unset\n") != std::string::npos);
}

TEST_CASE("A complete farland.toml parses")
{
    const auto config = parse_config(R"(# farlandd
[server]
bind = "::"
port = 3390
certificate = "/etc/farland/tls.crt"
private_key = "/etc/farland/tls.key"

[auth]
mode = "store"
credential_store = "/var/lib/farland/nla-users"

[session]
desktop = "cage"
command = ["firefox", "--kiosk", "https://example.org"]

[policy]
disconnected_timeout = "2h"
idle_timeout = 900
max_sessions = 20
max_sessions_per_user = 1
on_local_session = "attach"
)")
                            .value();
    CHECK(config.server.bind == "::");
    CHECK(config.server.port == 3390);
    CHECK(config.server.certificate == std::filesystem::path("/etc/farland/tls.crt"));
    CHECK(config.server.private_key == std::filesystem::path("/etc/farland/tls.key"));
    CHECK(config.auth.credential_store == "/var/lib/farland/nla-users");
    CHECK(config.session.desktop == DesktopKind::cage);
    CHECK(config.session.command == std::vector<std::string>{"firefox", "--kiosk", "https://example.org"});
    CHECK(config.policy.disconnected_timeout == 2h);
    CHECK(config.policy.idle_timeout == 900s);
    CHECK(config.policy.max_sessions == 20);
    CHECK(config.policy.on_local_session == LocalSessionPolicy::attach);

    const auto kerberos = parse_config("[auth]\nmode = \"kerberos\"\nkeytab = \"/etc/krb5.keytab\"\n").value();
    CHECK(kerberos.auth.mode == AuthMode::kerberos);
    CHECK(kerberos.auth.keytab == std::filesystem::path("/etc/krb5.keytab"));
    CHECK(to_string(kerberos.auth.mode) == "kerberos");
    CHECK(to_string(DesktopKind::labwc) == "labwc");
}

TEST_CASE("Timeouts take seconds or a number with a unit")
{
    const auto timeout = [](std::string_view value) {
        return parse_config("[policy]\nidle_timeout = " + std::string(value) + "\n").value().policy.idle_timeout;
    };
    CHECK(timeout("0") == 0s);
    CHECK(timeout("3600") == 1h);
    CHECK(timeout("\"90s\"") == 90s);
    CHECK(timeout("\"30m\"") == 30min);
    CHECK(timeout("\"2h\"") == 2h);
    CHECK(timeout("\"1d\"") == 24h);
    CHECK(timeout("\"0s\"") == 0s);
}

TEST_CASE("Configuration errors name the setting and its line")
{
    const auto check = [](std::string_view text, std::string_view message, std::uint32_t line) {
        const auto config = parse_config(text);
        REQUIRE_FALSE(config.has_value());
        INFO(config.error().message);
        CHECK(config.error().message.find(message) != std::string::npos);
        CHECK(config.error().line == line);
    };
    check("[server]\nport = 70000\n", "[server] port must be between 1 and 65535", 2);
    check("[server]\nport = \"3389\"\n", "[server] port must be an integer", 2);
    check("[server]\nbind = \"localhost\"\n", "IPv4 or IPv6 address", 2);
    check("[server]\ncertificate = \"/etc/farland/tls.crt\"\n", "certificate and private_key go together", 2);
    check("[sever]\nport = 1\n", "unknown section [sever]", 1);
    check("server = 1\n", "server must be a section", 1);
    check("[policy]\n\nidle_timout = 5\n", "unknown key idle_timout in [policy]", 3);
    check("[auth]\nmode = \"pam\"\n", "[auth] mode must be one of store, kerberos, not \"pam\"", 2);
    check("[auth]\ncredential_store = \"users\"\n", "must be an absolute path", 2);
    check("[auth]\nkeytab = \"/etc/krb5.keytab\"\n", "keytab needs mode = \"kerberos\"", 2);
    check("[session]\ndesktop = \"kde\"\n", "one of gnome, plasma, sway, labwc, cage", 2);
    check("[session]\ndesktop = \"cage\"\n", "needs [session] command", 2);
    check("[session]\ndesktop = \"cage\"\ncommand = []\n", "non-empty array of strings", 3);
    check("[session]\ndesktop = \"cage\"\ncommand = [\"a\", 1]\n", "only strings", 3);
    check("[session]\ncommand = [\"firefox\"]\n", "only for desktop = \"cage\"", 2);
    check("[policy]\nmax_sessions_per_user = 2\n", "can only be 1", 2);
    check("[policy]\nmax_sessions = -1\n", "between 0 and", 2);
    check("[policy]\non_local_session = \"steal\"\n", "one of refuse, attach, replace, separate", 2);
    check("[policy]\ntakeover = \"sometimes\"\n", "[policy] takeover must be one of ask, always, never", 2);
    check("[policy]\ntakeover_on_timeout = \"ask\"\n", "[policy] takeover_on_timeout must be one of allow, deny", 2);
    check("[policy]\ntakeover_timeout = \"2s\"\n", "takeover_timeout must be between 5 s and 5 min", 2);
    check("[policy]\ntakeover_timeout = \"10m\"\n", "takeover_timeout must be between 5 s and 5 min", 2);
    check("[policy]\ntakeover_timout = \"30s\"\n", "unknown key takeover_timout in [policy]", 2);

    check("[policy]\ndisconnected_timeout = \"5w\"\n", "must be seconds or a number with s, m, h or d", 2);
    check("[policy]\ndisconnected_timeout = \"-5m\"\n", "must be seconds", 2);
    check("[policy]\ndisconnected_timeout = \"999999d\"\n", "at most 366 days", 2);
    check("[policy]\nidle_timeout = 1.5\n", "must be seconds or a string", 2);
    check("[server]\nlog_level = \"verbose\"\n", "[server] log_level must be one of trace, debug", 2);
    check("[policy]\nactivation_timeout = \"2s\"\n", "activation_timeout must be between 5 s and 1 h", 2);
    check("[policy]\nactivation_timeout = \"2h\"\n", "activation_timeout must be between 5 s and 1 h", 2);
    check("[graphics]\ngfx_codec = \"rfx\"\n", "one of progressive, planar, avc420, avc444", 2);
    check("[graphics]\nbitmap_codec = \"nsc\"\n", "[graphics] bitmap_codec must be one of planar, raw", 2);
    check("[graphics]\nh264_encoder = \"qsv\"\n", "one of auto, nvenc, vaapi, openh264, x264", 2);
    check("[graphics]\nopenh264 = \"\"\n", "[graphics] openh264 must not be empty", 2);
    check("[graphics]\nopenh264 = \"lib/libopenh264.so\"\n", "soname or an absolute path", 2);
    check("[graphics]\nrender_node = \"renderD128\"\n", "must be an absolute path", 2);
    check("[graphics]\nzero_copy = \"yes\"\n", "[graphics] zero_copy must be true or false", 2);
    check("[graphics]\nframes_per_second = 0\n", "must be between 1 and 240", 2);
    check("[graphics]\nframes_per_second = 1000\n", "must be between 1 and 240", 2);
    check("[graphics]\nfps = 30\n", "unknown key fps in [graphics]", 2);
    check("[network]\nautodetect = \"auto\"\n", "one of off, continuous, full", 2);
    check("[audio]\nplayback = 1\n", "[audio] playback must be true or false", 2);
    check("[clipboard]\nfiles = false\n", "unknown key files in [clipboard]", 2);
    check("[graphic]\n", "unknown section [graphic]", 1);
    check("\n[server\n", "", 2);  // TOML syntax errors come from toml++

    const auto error = parse_config("[server]\nport = 0\n").error();
    CHECK(error.describe("farland.toml").starts_with("farland.toml:2:8: [server] port"));
    CHECK(ConfigError{"cannot read it"}.describe("x.toml") == "x.toml: cannot read it");
}

TEST_CASE("load_config reads the file; a missing file gives the defaults")
{
    std::random_device rd;
    const auto dir = std::filesystem::temp_directory_path() / ("farland-config-" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    CHECK(load_config(dir / "missing.toml").value().server.port == 3389);

    const auto path = dir / "farland.toml";
    std::ofstream(path) << "[session]\ndesktop = \"sway\"\n";
    CHECK(load_config(path).value().session.desktop == DesktopKind::sway);
    std::ofstream(path) << "[session]\ndesktop = \"weston\"\n";
    CHECK_FALSE(load_config(path).has_value());
    std::filesystem::remove_all(dir);
}
