// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/credential_store.hpp>
#include <farland/base/assert.hpp>
#include <farland/base/hexdump.hpp>
#include <farland/base/log.hpp>
#include <farland/base/text.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

namespace farland::auth {

namespace {

constexpr std::string_view log_component = "auth.credentials";
constexpr std::size_t max_name_size = 256;

char ascii_lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

std::string_view trim(std::string_view text) noexcept
{
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    text.remove_prefix(first);
    text.remove_suffix(text.size() - text.find_last_not_of(" \t\r") - 1);
    return text;
}

Result<void> write_all(int fd, std::string_view data, const std::filesystem::path& path)
{
    auto bytes = std::as_bytes(std::span(data));
    while (!bytes.empty()) {
        const ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            log::error(log_component, "cannot write {}: {}", path.string(), std::strerror(errno));
            return fail(Errc::io, "cannot write the credential store");
        }
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    return {};
}

}  // namespace

CredentialStore::~CredentialStore()
{
    for (auto& entry : entries_) {
        secure_zero(entry.hash);
    }
}

bool CredentialStore::valid_name(std::string_view name, bool allow_empty)
{
    if (name.empty()) {
        return allow_empty;
    }
    return name.size() <= max_name_size && std::ranges::none_of(name, [](char c) {
               return c == ':' || static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
           });
}

Result<CredentialStore> CredentialStore::parse(std::string_view text)
{
    CredentialStore store;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t end = text.find('\n', pos);
        auto line = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++line_no;
        if (const auto hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        const auto first = line.find(':');
        const auto second = first == std::string_view::npos ? first : line.find(':', first + 1);
        if (second == std::string_view::npos) {
            return fail(Errc::invalid_value, "credential line is not user:domain:hash", line_no);
        }
        const auto user = line.substr(0, first);
        const auto domain = line.substr(first + 1, second - first - 1);
        if (!valid_name(user, false) || !valid_name(domain, true)) {
            return fail(Errc::invalid_value, "invalid user or domain name", line_no);
        }
        auto digits = from_hex(line.substr(second + 1));
        if (!digits || digits->size() != NtHash{}.size()) {
            return fail(Errc::invalid_value, "NT hash is not 32 hexadecimal digits", line_no);
        }
        Entry entry{std::string(user), std::string(domain), {}};
        std::ranges::copy(*digits, entry.hash.begin());
        secure_zero(*digits);
        const bool duplicate = std::ranges::any_of(store.entries_, [&entry](const Entry& e) {
            return iequals(e.user, entry.user) && iequals(e.domain, entry.domain);
        });
        if (duplicate) {
            return fail(Errc::invalid_value, "duplicate credential entry", line_no);
        }
        store.entries_.push_back(std::move(entry));
    }
    return store;
}

std::string CredentialStore::serialize() const
{
    std::string out = "# farland NLA users: user:domain:NT hash (an empty domain matches any)\n";
    for (const auto& entry : entries_) {
        std::string digits = to_hex(entry.hash);
        std::erase(digits, ' ');
        out += std::format("{}:{}:{}\n", entry.user, entry.domain, digits);
        secure_zero(std::as_writable_bytes(std::span(digits)));
    }
    return out;
}

Result<CredentialStore> CredentialStore::load(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec || status.type() == std::filesystem::file_type::not_found) {
        return CredentialStore{};
    }
    const auto group_or_others = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    if ((status.permissions() & group_or_others) != std::filesystem::perms::none) {
        log::error(log_component, "{} is accessible by group or others; run chmod 600 on it", path.string());
        return fail(Errc::io, "credential store has unsafe permissions");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        log::error(log_component, "cannot open {}", path.string());
        return fail(Errc::io, "cannot read the credential store");
    }
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto store = parse(text);
    secure_zero(std::as_writable_bytes(std::span(text)));
    if (!store) {
        log::error(log_component, "{} line {}: {}", path.string(), store.error().offset, store.error().what);
    }
    return store;
}

Result<void> CredentialStore::save(const std::filesystem::path& path) const
{
    std::error_code ec;
    const auto dir = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    if (!std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all, ec);
        if (ec) {
            log::error(log_component, "cannot create {}: {}", dir.string(), ec.message());
            return fail(Errc::io, "cannot create the credential store directory");
        }
    }
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    std::filesystem::remove(temporary, ec);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is the only way to create with a mode atomically
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        log::error(log_component, "cannot create {}: {}", temporary.string(), std::strerror(errno));
        return fail(Errc::io, "cannot write the credential store");
    }
    std::string text = serialize();
    const auto written = write_all(fd, text, temporary);
    secure_zero(std::as_writable_bytes(std::span(text)));
    const bool synced = written.has_value() && ::fsync(fd) == 0;
    ::close(fd);
    if (!synced || ::rename(temporary.c_str(), path.c_str()) != 0) {
        std::filesystem::remove(temporary, ec);
        log::error(log_component, "cannot replace {}", path.string());
        return fail(Errc::io, "cannot write the credential store");
    }
    return {};
}

std::optional<NtHash> CredentialStore::lookup(std::string_view user, std::string_view domain) const
{
    const Entry* wildcard = nullptr;
    for (const auto& entry : entries_) {
        if (!iequals(entry.user, user)) {
            continue;
        }
        if (!entry.domain.empty() && iequals(entry.domain, domain)) {
            return entry.hash;
        }
        if (entry.domain.empty()) {
            wildcard = &entry;
        }
    }
    if (wildcard != nullptr) {
        return wildcard->hash;
    }
    return std::nullopt;
}

void CredentialStore::set(std::string_view user, std::string_view domain, const NtHash& hash)
{
    FARLAND_ASSERT(valid_name(user, false) && valid_name(domain, true));
    for (auto& entry : entries_) {
        if (iequals(entry.user, user) && iequals(entry.domain, domain)) {
            entry.hash = hash;
            return;
        }
    }
    entries_.push_back(Entry{std::string(user), std::string(domain), hash});
}

bool CredentialStore::remove(std::string_view user, std::string_view domain)
{
    const auto it = std::ranges::find_if(
        entries_, [&](const Entry& e) { return iequals(e.user, user) && iequals(e.domain, domain); });
    if (it == entries_.end()) {
        return false;
    }
    secure_zero(it->hash);
    entries_.erase(it);
    return true;
}

}  // namespace farland::auth
