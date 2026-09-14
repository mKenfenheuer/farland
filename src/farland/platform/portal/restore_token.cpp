// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/portal/portal_session.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

namespace farland::platform::portal {

namespace {

constexpr std::string_view log_component = "platform.portal";
constexpr std::size_t max_token_size = 1024;

bool write_all(int fd, std::string_view text)
{
    while (!text.empty()) {
        const auto written = ::write(fd, text.data(), text.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        text.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

}  // namespace

bool valid_restore_token(std::string_view token) noexcept
{
    return !token.empty() && token.size() <= max_token_size &&
           std::ranges::all_of(token, [](char c) { return c > ' ' && c <= '~'; });
}

std::optional<std::filesystem::path> default_restore_token_path()
{
    std::filesystem::path state;
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg == '/') {
        state = xdg;
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        state = std::filesystem::path(home) / ".local" / "state";
    } else {
        return std::nullopt;
    }
    return state / "farland" / "portal-restore-token";
}

Result<std::optional<std::string>> load_restore_token(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return std::optional<std::string>{};
    }
    if (ec || status.type() != std::filesystem::file_type::regular) {
        log::error(log_component, "cannot read {}", path.string());
        return fail(Errc::io, "cannot read the restore token");
    }
    const auto group_or_others = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    if ((status.permissions() & group_or_others) != std::filesystem::perms::none) {
        log::error(log_component, "{} is accessible by group or others; run chmod 600 on it", path.string());
        return fail(Errc::io, "the restore token file has unsafe permissions");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        log::error(log_component, "cannot open {}", path.string());
        return fail(Errc::io, "cannot read the restore token");
    }
    // At most one token plus a line break; anything longer is rejected below.
    std::string token;
    char c = 0;
    while (token.size() < max_token_size + 2 && in.get(c)) {
        token += c;
    }
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
        token.pop_back();
    }
    if (!valid_restore_token(token)) {
        log::error(log_component, "{} does not hold a restore token", path.string());
        return fail(Errc::invalid_value, "malformed restore token");
    }
    return std::optional<std::string>(std::move(token));
}

Result<void> save_restore_token(const std::filesystem::path& path, std::string_view token)
{
    if (!valid_restore_token(token)) {
        return fail(Errc::invalid_value, "malformed restore token");
    }
    std::error_code ec;
    const auto dir = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    if (!std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all, ec);
        if (ec) {
            log::error(log_component, "cannot create {}: {}", dir.string(), ec.message());
            return fail(Errc::io, "cannot create the restore token directory");
        }
    }
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    std::filesystem::remove(temporary, ec);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is the only way to create with a mode atomically
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        log::error(log_component, "cannot create {}: {}", temporary.string(), std::strerror(errno));
        return fail(Errc::io, "cannot write the restore token");
    }
    const std::string text = std::string(token) + '\n';
    const bool synced = write_all(fd, text) && ::fsync(fd) == 0;
    ::close(fd);
    if (!synced || ::rename(temporary.c_str(), path.c_str()) != 0) {
        std::filesystem::remove(temporary, ec);
        log::error(log_component, "cannot replace {}", path.string());
        return fail(Errc::io, "cannot write the restore token");
    }
    return {};
}

Result<void> remove_restore_token(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        log::error(log_component, "cannot remove {}: {}", path.string(), ec.message());
        return fail(Errc::io, "cannot remove the restore token");
    }
    return {};
}

}  // namespace farland::platform::portal
