// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "config.hpp"

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
    if (auto ok = check_keys(table, "server", {"bind", "port", "certificate", "private_key"}); !ok) {
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
    return {};
}

std::expected<void, ConfigError> parse_auth(const toml::table& table, AuthSection& out)
{
    if (auto ok = check_keys(table, "auth", {"mode", "credential_store", "keytab"}); !ok) {
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
        if (out.mode != AuthMode::kerberos) {
            return error_at(*node, "[auth] keytab needs mode = \"kerberos\"");
        }
        auto path = get_path(*node, "[auth] keytab");
        if (!path) {
            return std::unexpected(std::move(path).error());
        }
        out.keytab = std::move(*path);
    }
    return {};
}

std::expected<void, ConfigError> parse_session(const toml::table& table, SessionSection& out)
{
    if (auto ok = check_keys(table, "session", {"desktop", "command"}); !ok) {
        return ok;
    }
    if (const auto* node = table.get("desktop")) {
        auto desktop = get_enum<DesktopKind>(
            *node, "[session] desktop", std::array<std::string_view, 5>{"gnome", "plasma", "sway", "labwc", "cage"});
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
    if (auto ok = check_keys(
            table, "policy",
            {"disconnected_timeout", "idle_timeout", "max_sessions", "max_sessions_per_user", "on_local_session"});
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
        auto policy = get_enum<LocalSessionPolicy>(*node, "[policy] on_local_session",
                                                   std::array<std::string_view, 2>{"refuse", "attach"});
        if (!policy) {
            return std::unexpected(std::move(policy).error());
        }
        out.on_local_session = *policy;
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
    static constexpr std::array<std::pair<std::string_view, Parser>, 4> sections{{
        {"server", [](const toml::table& t, Config& c) { return parse_server(t, c.server); }},
        {"auth", [](const toml::table& t, Config& c) { return parse_auth(t, c.auth); }},
        {"session", [](const toml::table& t, Config& c) { return parse_session(t, c.session); }},
        {"policy", [](const toml::table& t, Config& c) { return parse_policy(t, c.policy); }},
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
    }
    return "unknown";
}

std::string_view to_string(LocalSessionPolicy policy) noexcept
{
    return policy == LocalSessionPolicy::refuse ? "refuse" : "attach";
}

}  // namespace farland::daemon
