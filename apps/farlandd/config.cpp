// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "config.hpp"

#include "metrics.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <toml++/toml.hpp>
#include <utility>

namespace farland::daemon {

namespace {

using Error = std::unexpected<ConfigError>;

constexpr std::int64_t max_timeout_seconds = std::int64_t{366} * 24 * 3600;
constexpr std::int64_t max_session_limit = 100'000;
/// A client that reaches no active connection in this long is dropped; a
/// headless GNOME session through GDM needs most of a minute.
constexpr std::chrono::seconds min_activation_timeout{5};
constexpr std::chrono::seconds max_activation_timeout{3600};
/// How long the takeover prompt may stand. The client waits meanwhile, so
/// it has to leave room inside the activation timeout.
constexpr std::chrono::seconds min_takeover_timeout{5};
constexpr std::chrono::seconds max_takeover_timeout{300};
/// The frame rate the session aims for; the quality tiers lower it.
constexpr std::int64_t min_frame_rate = 1;
constexpr std::int64_t max_frame_rate = 240;
/// kbit/s. 0 means "not set"; the ceiling is video::max_bitrate_kbps.
constexpr std::int64_t max_bitrate_kbps = 1'000'000;

Error error_at(const toml::source_region& where, std::string message)
{
    return Error(ConfigError{std::move(message), where.begin.line, where.begin.column});
}

Error error_at(const toml::node& node, std::string message)
{
    return error_at(node.source(), std::move(message));
}

/// Unknown keys are typos; report the first at its position.
std::expected<void, ConfigError> check_keys(const toml::table& table, std::string_view section,
                                            std::initializer_list<std::string_view> known)
{
    for (const auto& [key, value] : table) {
        if (std::ranges::find(known, key.str()) == known.end()) {
            return error_at(key.source(), std::format("unknown key {} in [{}]", key.str(), section));
        }
    }
    return {};
}

std::expected<std::string, ConfigError> get_string(const toml::node& node, std::string_view name)
{
    const auto* value = node.as_string();
    if (value == nullptr) {
        return error_at(node, std::format("{} must be a string", name));
    }
    return value->get();
}

std::expected<std::int64_t, ConfigError> get_integer(const toml::node& node, std::string_view name, std::int64_t min,
                                                     std::int64_t max)
{
    const auto* value = node.as_integer();
    if (value == nullptr) {
        return error_at(node, std::format("{} must be an integer", name));
    }
    if (value->get() < min || value->get() > max) {
        return error_at(node, std::format("{} must be between {} and {}", name, min, max));
    }
    return value->get();
}

std::expected<bool, ConfigError> get_bool(const toml::node& node, std::string_view name)
{
    const auto* value = node.as_boolean();
    if (value == nullptr) {
        return error_at(node, std::format("{} must be true or false", name));
    }
    return value->get();
}

std::expected<std::filesystem::path, ConfigError> get_path(const toml::node& node, std::string_view name)
{
    auto text = get_string(node, name);
    if (!text) {
        return std::unexpected(std::move(text).error());
    }
    std::filesystem::path path(*text);
    if (!path.is_absolute()) {
        return error_at(node, std::format("{} must be an absolute path", name));
    }
    return path;
}

/// A shared library: a soname the loader searches for, or an absolute path.
std::expected<std::string, ConfigError> get_library(const toml::node& node, std::string_view name)
{
    auto text = get_string(node, name);
    if (!text) {
        return std::unexpected(std::move(text).error());
    }
    if (text->empty()) {
        return error_at(node, std::format("{} must not be empty", name));
    }
    if (text->find('/') != std::string::npos && !std::filesystem::path(*text).is_absolute()) {
        return error_at(node, std::format("{} must be a soname or an absolute path, not \"{}\"", name, *text));
    }
    return text;
}

/// An enum given by name; `names[i]` names the value `i`.
template <class Enum, std::size_t N>
std::expected<Enum, ConfigError> get_enum(const toml::node& node, std::string_view name,
                                          const std::array<std::string_view, N>& names)
{
    auto text = get_string(node, name);
    if (!text) {
        return std::unexpected(std::move(text).error());
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (*text == names.at(i)) {
            return static_cast<Enum>(i);
        }
    }
    std::string choices;
    for (const auto choice : names) {
        choices += choices.empty() ? "" : ", ";
        choices += choice;
    }
    return error_at(node, std::format("{} must be one of {}, not \"{}\"", name, choices, *text));
}

/// Seconds as an integer, or a string with a unit: "90s", "30m", "2h", "1d".
std::expected<std::chrono::seconds, ConfigError> get_duration(const toml::node& node, std::string_view name)
{
    std::int64_t seconds = 0;
    if (node.is_integer()) {
        auto value = get_integer(node, name, 0, max_timeout_seconds);
        if (!value) {
            return std::unexpected(std::move(value).error());
        }
        seconds = *value;
    } else if (const auto* text = node.as_string()) {
        const std::string_view s = text->get();
        // Decimal digits, capped just above the limit so that the product
        // below cannot overflow; then the unit.
        std::size_t digits = 0;
        std::int64_t number = 0;
        while (digits < s.size() && s[digits] >= '0' && s[digits] <= '9') {
            number = std::min<std::int64_t>((number * 10) + (s[digits] - '0'), max_timeout_seconds + 1);
            ++digits;
        }
        const std::string_view unit = s.substr(digits);
        std::int64_t scale = 0;
        if (unit == "s") {
            scale = 1;
        } else if (unit == "m") {
            scale = 60;
        } else if (unit == "h") {
            scale = 3600;
        } else if (unit == "d") {
            scale = 86400;
        }
        if (digits == 0 || scale == 0 || number > max_timeout_seconds / scale) {
            return error_at(node, std::format("{} must be seconds or a number with s, m, h or d (at most 366 days), "
                                              "not \"{}\"",
                                              name, s));
        }
        seconds = number * scale;
    } else {
        return error_at(node, std::format("{} must be seconds or a string like \"30m\"", name));
    }
    return std::chrono::seconds(seconds);
}

bool is_ip_address(const std::string& text)
{
    std::array<unsigned char, 16> buffer{};
    return ::inet_pton(AF_INET, text.c_str(), buffer.data()) == 1 ||
           ::inet_pton(AF_INET6, text.c_str(), buffer.data()) == 1;
}

// Each section parser takes the table and fills its part of the config.

std::expected<void, ConfigError> parse_server(const toml::table& table, ServerSection& out)
{
    if (auto ok = check_keys(table, "server", {"bind", "port", "certificate", "private_key", "log_level"}); !ok) {
        return ok;
    }
    if (const auto* node = table.get("bind")) {
        auto bind = get_string(*node, "[server] bind");
        if (!bind) {
            return std::unexpected(std::move(bind).error());
        }
        if (!is_ip_address(*bind)) {
            return error_at(*node, std::format("[server] bind must be an IPv4 or IPv6 address, not \"{}\"", *bind));
        }
        out.bind = std::move(*bind);
    }
    if (const auto* node = table.get("port")) {
        auto port = get_integer(*node, "[server] port", 1, std::numeric_limits<std::uint16_t>::max());
        if (!port) {
            return std::unexpected(std::move(port).error());
        }
        out.port = static_cast<std::uint16_t>(*port);
    }
    for (auto [key, field] : {std::pair{"certificate", &out.certificate}, std::pair{"private_key", &out.private_key}}) {
        if (const auto* node = table.get(key)) {
            auto path = get_path(*node, std::format("[server] {}", key));
            if (!path) {
                return std::unexpected(std::move(path).error());
            }
            *field = std::move(*path);
        }
    }
    if (out.certificate.has_value() != out.private_key.has_value()) {
        const auto* node = out.certificate ? table.get("certificate") : table.get("private_key");
        return error_at(*node, "[server] certificate and private_key go together");
    }
    if (const auto* node = table.get("log_level")) {
        auto level = get_enum<LogLevel>(*node, "[server] log_level",
                                        std::array<std::string_view, 5>{"trace", "debug", "info", "warn", "error"});
        if (!level) {
            return std::unexpected(std::move(level).error());
        }
        out.log_level = *level;
    }
    return {};
}

std::expected<void, ConfigError> parse_auth(const toml::table& table, AuthSection& out)
{
    if (auto ok = check_keys(table, "auth", {"mode", "credential_store", "keytab", "service_principal"}); !ok) {
        return ok;
    }
    if (const auto* node = table.get("mode")) {
        auto mode = get_enum<AuthMode>(*node, "[auth] mode", std::array<std::string_view, 2>{"store", "kerberos"});
        if (!mode) {
            return std::unexpected(std::move(mode).error());
        }
        out.mode = *mode;
    }
    if (const auto* node = table.get("credential_store")) {
        auto path = get_path(*node, "[auth] credential_store");
        if (!path) {
            return std::unexpected(std::move(path).error());
        }
        out.credential_store = std::move(*path);
    }
    if (const auto* node = table.get("keytab")) {
        auto path = get_path(*node, "[auth] keytab");
        if (!path) {
            return std::unexpected(std::move(path).error());
        }
        out.keytab = std::move(*path);
    }
    if (const auto* node = table.get("service_principal")) {
        const auto* value = node->as_string();
        if (value == nullptr) {
            return error_at(*node, "[auth] service_principal must be a string");
        }
        out.service_principal = value->get();
    }
    // Kerberos is the only way in under that mode, so there has to be a
    // keytab to check tickets against.
    if (out.mode == AuthMode::kerberos && !out.keytab) {
        return std::unexpected(ConfigError{"[auth] mode = \"kerberos\" needs a keytab"});
    }
    return {};
}

std::expected<void, ConfigError> parse_session(const toml::table& table, SessionSection& out)
{
    if (auto ok = check_keys(table, "session", {"desktop", "command"}); !ok) {
        return ok;
    }
    if (const auto* node = table.get("desktop")) {
        auto desktop =
            get_enum<DesktopKind>(*node, "[session] desktop",
                                  std::array<std::string_view, 6>{"gnome", "plasma", "sway", "labwc", "cage", "test"});
        if (!desktop) {
            return std::unexpected(std::move(desktop).error());
        }
        out.desktop = *desktop;
    }
    const auto* command = table.get("command");
    if (command != nullptr) {
        if (out.desktop != DesktopKind::cage) {
            return error_at(*command, "[session] command is only for desktop = \"cage\"");
        }
        const auto* array = command->as_array();
        if (array == nullptr || array->empty()) {
            return error_at(*command, "[session] command must be a non-empty array of strings, like [\"firefox\"]");
        }
        for (const auto& element : *array) {
            const auto* argument = element.as_string();
            if (argument == nullptr) {
                return error_at(element, "[session] command must contain only strings");
            }
            out.command.push_back(argument->get());
        }
        if (out.command.front().empty()) {
            return error_at(*command, "[session] command must start with a program");
        }
    } else if (out.desktop == DesktopKind::cage) {
        const auto& where = table.get("desktop")->source();
        return error_at(where, "desktop = \"cage\" needs [session] command, the application to run");
    }
    return {};
}

std::expected<void, ConfigError> parse_policy(const toml::table& table, PolicySection& out)
{
    if (auto ok = check_keys(table, "policy",
                             {"disconnected_timeout", "idle_timeout", "activation_timeout", "max_sessions",
                              "max_sessions_per_user", "on_local_session", "takeover", "takeover_timeout",
                              "takeover_on_timeout", "seat_takeover"});
        !ok) {
        return ok;
    }
    for (auto [key, field] :
         {std::pair{"disconnected_timeout", &out.disconnected_timeout}, std::pair{"idle_timeout", &out.idle_timeout}}) {
        if (const auto* node = table.get(key)) {
            auto duration = get_duration(*node, std::format("[policy] {}", key));
            if (!duration) {
                return std::unexpected(std::move(duration).error());
            }
            *field = *duration;
        }
    }
    if (const auto* node = table.get("activation_timeout")) {
        auto duration = get_duration(*node, "[policy] activation_timeout");
        if (!duration) {
            return std::unexpected(std::move(duration).error());
        }
        if (*duration < min_activation_timeout || *duration > max_activation_timeout) {
            return error_at(*node, "[policy] activation_timeout must be between 5 s and 1 h");
        }
        out.activation_timeout = *duration;
    }
    if (const auto* node = table.get("max_sessions")) {
        auto limit = get_integer(*node, "[policy] max_sessions", 0, max_session_limit);
        if (!limit) {
            return std::unexpected(std::move(limit).error());
        }
        out.max_sessions = static_cast<unsigned>(*limit);
    }
    if (const auto* node = table.get("max_sessions_per_user")) {
        auto limit = get_integer(*node, "[policy] max_sessions_per_user", 1, max_session_limit);
        if (!limit) {
            return std::unexpected(std::move(limit).error());
        }
        if (*limit != 1) {
            // The NLA identity picks the user's one session; choosing among
            // several needs a session picker that does not exist yet.
            return error_at(*node, "[policy] max_sessions_per_user can only be 1 for now");
        }
        out.max_sessions_per_user = static_cast<unsigned>(*limit);
    }
    if (const auto* node = table.get("on_local_session")) {
        auto policy =
            get_enum<LocalSessionPolicy>(*node, "[policy] on_local_session",
                                         std::array<std::string_view, 4>{"refuse", "attach", "replace", "separate"});
        if (!policy) {
            return std::unexpected(std::move(policy).error());
        }
        out.on_local_session = *policy;
    }
    if (const auto* node = table.get("takeover")) {
        auto takeover = get_enum<TakeoverPolicy>(*node, "[policy] takeover",
                                                 std::array<std::string_view, 3>{"ask", "always", "never"});
        if (!takeover) {
            return std::unexpected(std::move(takeover).error());
        }
        out.takeover = *takeover;
    }
    if (const auto* node = table.get("takeover_timeout")) {
        auto duration = get_duration(*node, "[policy] takeover_timeout");
        if (!duration) {
            return std::unexpected(std::move(duration).error());
        }
        if (*duration < min_takeover_timeout || *duration > max_takeover_timeout) {
            return error_at(*node, "[policy] takeover_timeout must be between 5 s and 5 min");
        }
        out.takeover_timeout = *duration;
    }
    if (const auto* node = table.get("seat_takeover")) {
        auto takeover = get_enum<TakeoverPolicy>(*node, "[policy] seat_takeover",
                                                 std::array<std::string_view, 3>{"ask", "always", "never"});
        if (!takeover) {
            return std::unexpected(std::move(takeover).error());
        }
        out.seat_takeover = *takeover;
    }
    if (const auto* node = table.get("takeover_on_timeout")) {
        auto action = get_enum<TakeoverDefault>(*node, "[policy] takeover_on_timeout",
                                                std::array<std::string_view, 2>{"allow", "deny"});
        if (!action) {
            return std::unexpected(std::move(action).error());
        }
        out.takeover_on_timeout = *action;
    }
    return {};
}

std::expected<void, ConfigError> parse_graphics(const toml::table& table, GraphicsSection& out)
{
    if (auto ok = check_keys(table, "graphics",
                             {"gfx_codec", "bitmap_codec", "h264_encoder", "openh264", "render_node", "zero_copy",
                              "clearcodec", "refine", "video_regions", "lossless_still", "frames_per_second",
                              "h264_bitrate", "h264_min_bitrate", "h264_max_bitrate"});
        !ok) {
        return ok;
    }
    if (const auto* node = table.get("gfx_codec")) {
        auto codec = get_enum<GfxCodec>(*node, "[graphics] gfx_codec",
                                        std::array<std::string_view, 4>{"progressive", "planar", "avc420", "avc444"});
        if (!codec) {
            return std::unexpected(std::move(codec).error());
        }
        out.gfx_codec = *codec;
    }
    if (const auto* node = table.get("bitmap_codec")) {
        auto codec =
            get_enum<BitmapCodec>(*node, "[graphics] bitmap_codec", std::array<std::string_view, 2>{"planar", "raw"});
        if (!codec) {
            return std::unexpected(std::move(codec).error());
        }
        out.bitmap_codec = *codec;
    }
    if (const auto* node = table.get("h264_encoder")) {
        auto encoder =
            get_enum<H264Encoder>(*node, "[graphics] h264_encoder",
                                  std::array<std::string_view, 5>{"auto", "nvenc", "vaapi", "openh264", "x264"});
        if (!encoder) {
            return std::unexpected(std::move(encoder).error());
        }
        out.h264_encoder = *encoder;
    }
    if (const auto* node = table.get("openh264")) {
        auto library = get_library(*node, "[graphics] openh264");
        if (!library) {
            return std::unexpected(std::move(library).error());
        }
        out.openh264 = std::move(*library);
    }
    if (const auto* node = table.get("render_node")) {
        auto path = get_path(*node, "[graphics] render_node");
        if (!path) {
            return std::unexpected(std::move(path).error());
        }
        out.render_node = std::move(*path);
    }
    for (auto [key, field] : {std::pair{"zero_copy", &out.zero_copy}, std::pair{"clearcodec", &out.clearcodec},
                              std::pair{"refine", &out.refine}, std::pair{"video_regions", &out.video_regions},
                              std::pair{"lossless_still", &out.lossless_still}}) {
        if (const auto* node = table.get(key)) {
            auto value = get_bool(*node, std::format("[graphics] {}", key));
            if (!value) {
                return std::unexpected(std::move(value).error());
            }
            *field = *value;
        }
    }
    if (const auto* node = table.get("frames_per_second")) {
        auto rate = get_integer(*node, "[graphics] frames_per_second", min_frame_rate, max_frame_rate);
        if (!rate) {
            return std::unexpected(std::move(rate).error());
        }
        out.frames_per_second = static_cast<unsigned>(*rate);
    }
    // What the H.264 ladder may spend. A floor of 0 is meaningful (let the
    // ladder go as low as it likes), so all three take 0.
    const std::array<std::pair<std::string_view, unsigned*>, 3> bitrates{{
        {"h264_bitrate", &out.h264_bitrate_kbps},
        {"h264_min_bitrate", &out.h264_min_bitrate_kbps},
        {"h264_max_bitrate", &out.h264_max_bitrate_kbps},
    }};
    for (const auto& [key, field] : bitrates) {
        if (const auto* node = table.get(key)) {
            auto value = get_integer(*node, std::format("[graphics] {}", key), 0, max_bitrate_kbps);
            if (!value) {
                return std::unexpected(std::move(value).error());
            }
            *field = static_cast<unsigned>(*value);
        }
    }
    if (out.h264_max_bitrate_kbps > 0 && out.h264_max_bitrate_kbps < out.h264_min_bitrate_kbps) {
        return std::unexpected(
            ConfigError{"[graphics] h264_max_bitrate is below h264_min_bitrate, so no tier could be chosen"});
    }
    return {};
}

std::expected<void, ConfigError> parse_network(const toml::table& table, NetworkSection& out)
{
    if (auto ok = check_keys(table, "network", {"autodetect"}); !ok) {
        return ok;
    }
    if (const auto* node = table.get("autodetect")) {
        auto mode = get_enum<AutoDetect>(*node, "[network] autodetect",
                                         std::array<std::string_view, 3>{"off", "continuous", "full"});
        if (!mode) {
            return std::unexpected(std::move(mode).error());
        }
        out.autodetect = *mode;
    }
    return {};
}

std::expected<void, ConfigError> parse_audio(const toml::table& table, AudioSection& out)
{
    if (auto ok = check_keys(table, "audio", {"playback", "microphone"}); !ok) {
        return ok;
    }
    for (auto [key, field] : {std::pair{"playback", &out.playback}, std::pair{"microphone", &out.microphone}}) {
        if (const auto* node = table.get(key)) {
            auto value = get_bool(*node, std::format("[audio] {}", key));
            if (!value) {
                return std::unexpected(std::move(value).error());
            }
            *field = *value;
        }
    }
    return {};
}

std::expected<void, ConfigError> parse_metrics(const toml::table& table, MetricsSection& out)
{
    if (auto ok = check_keys(table, "metrics", {"listen"}); !ok) {
        return ok;
    }
    if (const auto* node = table.get("listen")) {
        const auto* value = node->as_string();
        if (value == nullptr) {
            return std::unexpected(ConfigError{"[metrics] listen must be a string like \"127.0.0.1:9128\""});
        }
        if (auto where = split_listen_address(value->get()); !where) {
            return std::unexpected(ConfigError{std::format("[metrics] listen: {}", where.error().message())});
        }
        out.listen = value->get();
    }
    return {};
}

std::expected<void, ConfigError> parse_camera(const toml::table& table, CameraSection& out)
{
    if (auto ok = check_keys(table, "camera", {"enabled"}); !ok) {
        return ok;
    }
    if (const auto* node = table.get("enabled")) {
        auto value = get_bool(*node, "[camera] enabled");
        if (!value) {
            return std::unexpected(std::move(value).error());
        }
        out.enabled = *value;
    }
    return {};
}

std::expected<void, ConfigError> parse_clipboard(const toml::table& table, ClipboardSection& out)
{
    if (auto ok = check_keys(table, "clipboard", {"enabled"}); !ok) {
        return ok;
    }
    if (const auto* node = table.get("enabled")) {
        auto value = get_bool(*node, "[clipboard] enabled");
        if (!value) {
            return std::unexpected(std::move(value).error());
        }
        out.enabled = *value;
    }
    return {};
}

}  // namespace

std::string ConfigError::describe(std::string_view source) const
{
    if (line == 0) {
        return std::format("{}: {}", source, message);
    }
    return std::format("{}:{}:{}: {}", source, line, column, message);
}

std::expected<Config, ConfigError> parse_config(std::string_view text)
{
    toml::table root;
#if TOML_EXCEPTIONS
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error& e) {
        return error_at(e.source(), std::string(e.description()));
    }
#else
    auto parsed = toml::parse(text);
    if (!parsed) {
        return error_at(parsed.error().source(), std::string(parsed.error().description()));
    }
    root = std::move(parsed).table();
#endif

    using Parser = std::expected<void, ConfigError> (*)(const toml::table&, Config&);
    static constexpr std::array<std::pair<std::string_view, Parser>, 10> sections{{
        {"server", [](const toml::table& t, Config& c) { return parse_server(t, c.server); }},
        {"auth", [](const toml::table& t, Config& c) { return parse_auth(t, c.auth); }},
        {"session", [](const toml::table& t, Config& c) { return parse_session(t, c.session); }},
        {"policy", [](const toml::table& t, Config& c) { return parse_policy(t, c.policy); }},
        {"graphics", [](const toml::table& t, Config& c) { return parse_graphics(t, c.graphics); }},
        {"network", [](const toml::table& t, Config& c) { return parse_network(t, c.network); }},
        {"audio", [](const toml::table& t, Config& c) { return parse_audio(t, c.audio); }},
        {"camera", [](const toml::table& t, Config& c) { return parse_camera(t, c.camera); }},
        {"metrics", [](const toml::table& t, Config& c) { return parse_metrics(t, c.metrics); }},
        {"clipboard", [](const toml::table& t, Config& c) { return parse_clipboard(t, c.clipboard); }},
    }};

    Config config;
    for (const auto& [key, node] : root) {
        const auto* section = std::ranges::find(sections, key.str(), &std::pair<std::string_view, Parser>::first);
        if (section == sections.end()) {
            return error_at(key.source(), std::format("unknown section [{}]", key.str()));
        }
        const auto* table = node.as_table();
        if (table == nullptr) {
            return error_at(node, std::format("{} must be a section, [{}]", key.str(), key.str()));
        }
        if (auto ok = section->second(*table, config); !ok) {
            return std::unexpected(std::move(ok).error());
        }
    }
    return config;
}

std::expected<Config, ConfigError> load_config(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !ec) {
        return Config{};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Error(ConfigError{std::format("cannot read {}", path.string())});
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return parse_config(text);
}

std::string_view to_string(AuthMode mode) noexcept
{
    return mode == AuthMode::store ? "store" : "kerberos";
}

std::string_view to_string(DesktopKind desktop) noexcept
{
    switch (desktop) {
    case DesktopKind::gnome:
        return "gnome";
    case DesktopKind::plasma:
        return "plasma";
    case DesktopKind::sway:
        return "sway";
    case DesktopKind::labwc:
        return "labwc";
    case DesktopKind::cage:
        return "cage";
    case DesktopKind::test:
        return "test";
    }
    return "unknown";
}

std::string_view to_string(LocalSessionPolicy policy) noexcept
{
    switch (policy) {
    case LocalSessionPolicy::refuse:
        return "refuse";
    case LocalSessionPolicy::attach:
        return "attach";
    case LocalSessionPolicy::replace:
        return "replace";
    case LocalSessionPolicy::separate:
        return "separate";
    }
    return "attach";
}

std::string_view to_string(TakeoverPolicy takeover) noexcept
{
    switch (takeover) {
    case TakeoverPolicy::ask:
        return "ask";
    case TakeoverPolicy::always:
        return "always";
    case TakeoverPolicy::never:
        return "never";
    }
    return "ask";
}

std::string_view to_string(TakeoverDefault action) noexcept
{
    return action == TakeoverDefault::allow ? "allow" : "deny";
}

std::string_view to_string(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::trace:
        return "trace";
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warn:
        return "warn";
    case LogLevel::error:
        return "error";
    }
    return "info";
}

std::string_view to_string(GfxCodec codec) noexcept
{
    switch (codec) {
    case GfxCodec::progressive:
        return "progressive";
    case GfxCodec::planar:
        return "planar";
    case GfxCodec::avc420:
        return "avc420";
    case GfxCodec::avc444:
        return "avc444";
    }
    return "progressive";
}

std::string_view to_string(BitmapCodec codec) noexcept
{
    return codec == BitmapCodec::planar ? "planar" : "raw";
}

std::string_view to_string(H264Encoder encoder) noexcept
{
    switch (encoder) {
    case H264Encoder::automatic:
        return "auto";
    case H264Encoder::nvenc:
        return "nvenc";
    case H264Encoder::vaapi:
        return "vaapi";
    case H264Encoder::openh264:
        return "openh264";
    case H264Encoder::x264:
        return "x264";
    }
    return "auto";
}

std::string_view to_string(AutoDetect autodetect) noexcept
{
    switch (autodetect) {
    case AutoDetect::off:
        return "off";
    case AutoDetect::continuous:
        return "continuous";
    case AutoDetect::full:
        return "full";
    }
    return "full";
}

std::string describe(const Config& config)
{
    const auto seconds = [](std::chrono::seconds value) { return std::to_string(value.count()); };
    const auto quoted = [](std::string_view value) { return std::format("\"{}\"", value); };
    const auto optional_path = [&quoted](const std::optional<std::filesystem::path>& value) {
        return value ? quoted(value->string()) : std::string("unset");
    };
    const auto flag = [](bool value) { return value ? "true" : "false"; };
    std::string out;
    const auto line = [&out](std::string_view key, std::string_view value) {
        out += std::format("{} = {}\n", key, value);
    };
    line("server.bind", quoted(config.server.bind));
    line("server.port", std::to_string(config.server.port));
    line("server.certificate", optional_path(config.server.certificate));
    line("server.private_key", optional_path(config.server.private_key));
    line("server.log_level", quoted(to_string(config.server.log_level)));
    line("auth.mode", quoted(to_string(config.auth.mode)));
    line("auth.credential_store", quoted(config.auth.credential_store.string()));
    line("auth.keytab", optional_path(config.auth.keytab));
    line("auth.service_principal",
         config.auth.service_principal.empty() ? std::string("unset") : quoted(config.auth.service_principal));
    line("session.desktop", quoted(to_string(config.session.desktop)));
    std::string command;
    for (const auto& argument : config.session.command) {
        command += command.empty() ? "" : ", ";
        command += quoted(argument);
    }
    line("session.command", std::format("[{}]", command));
    line("policy.disconnected_timeout", seconds(config.policy.disconnected_timeout));
    line("policy.idle_timeout", seconds(config.policy.idle_timeout));
    line("policy.activation_timeout", seconds(config.policy.activation_timeout));
    line("policy.max_sessions", std::to_string(config.policy.max_sessions));
    line("policy.max_sessions_per_user", std::to_string(config.policy.max_sessions_per_user));
    line("policy.on_local_session", quoted(to_string(config.policy.on_local_session)));
    line("policy.takeover", quoted(to_string(config.policy.takeover)));
    line("policy.takeover_timeout", seconds(config.policy.takeover_timeout));
    line("policy.takeover_on_timeout", quoted(to_string(config.policy.takeover_on_timeout)));
    line("policy.seat_takeover", quoted(to_string(config.policy.seat_takeover)));
    line("graphics.gfx_codec", quoted(to_string(config.graphics.gfx_codec)));
    line("graphics.bitmap_codec", quoted(to_string(config.graphics.bitmap_codec)));
    line("graphics.h264_encoder", quoted(to_string(config.graphics.h264_encoder)));
    line("graphics.openh264", config.graphics.openh264 ? quoted(*config.graphics.openh264) : std::string("unset"));
    line("graphics.render_node", optional_path(config.graphics.render_node));
    line("graphics.zero_copy", flag(config.graphics.zero_copy));
    line("graphics.clearcodec", flag(config.graphics.clearcodec));
    line("graphics.refine", flag(config.graphics.refine));
    line("graphics.video_regions", flag(config.graphics.video_regions));
    line("graphics.h264_bitrate", std::to_string(config.graphics.h264_bitrate_kbps));
    line("graphics.h264_min_bitrate", std::to_string(config.graphics.h264_min_bitrate_kbps));
    line("graphics.h264_max_bitrate", std::to_string(config.graphics.h264_max_bitrate_kbps));
    line("graphics.lossless_still", flag(config.graphics.lossless_still));
    line("graphics.frames_per_second", std::to_string(config.graphics.frames_per_second));
    line("network.autodetect", quoted(to_string(config.network.autodetect)));
    line("audio.playback", flag(config.audio.playback));
    line("audio.microphone", flag(config.audio.microphone));
    line("camera.enabled", flag(config.camera.enabled));
    line("metrics.listen", config.metrics.listen ? quoted(*config.metrics.listen) : std::string("unset"));
    line("clipboard.enabled", flag(config.clipboard.enabled));
    return out;
}

}  // namespace farland::daemon
