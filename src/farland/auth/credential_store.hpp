// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/gss.hpp>
#include <farland/base/error.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace farland::auth {

/// The users who may log in with NLA, kept as NT hashes: NTLMv2 verification
/// needs the hash, never the password ([MS-NLMP] 3.3.1).
///
/// On disk: one user per line, `user:domain:hash` with the hash as 32 hex
/// digits; `#` starts a comment. An empty domain matches whatever domain the
/// client names. The file holds password equivalents, so it must be
/// readable by its owner only.
class CredentialStore {
public:
    struct Entry {
        std::string user;
        std::string domain;
        NtHash hash{};
    };

    CredentialStore() = default;
    CredentialStore(const CredentialStore&) = delete;
    CredentialStore& operator=(const CredentialStore&) = delete;
    CredentialStore(CredentialStore&&) noexcept = default;
    CredentialStore& operator=(CredentialStore&&) noexcept = default;
    ~CredentialStore();

    /// Loads the store; a missing file is an empty store. Refuses files that
    /// group or others can access.
    [[nodiscard]] static Result<CredentialStore> load(const std::filesystem::path& path);
    /// Parses the text form. For errors, `Error::offset` is the line number.
    [[nodiscard]] static Result<CredentialStore> parse(std::string_view text);
    [[nodiscard]] std::string serialize() const;
    /// Writes atomically (temporary file and rename) with mode 0600,
    /// creating the directory with mode 0700 if needed.
    [[nodiscard]] Result<void> save(const std::filesystem::path& path) const;

    /// Case-insensitive match; an entry for exactly this domain wins over an
    /// entry without a domain.
    [[nodiscard]] std::optional<NtHash> lookup(std::string_view user, std::string_view domain) const;
    /// Adds or replaces the entry for `user` in `domain`. Asserts that the
    /// names are valid (see `valid_name`).
    void set(std::string_view user, std::string_view domain, const NtHash& hash);
    /// Removes the entry; false if there was none.
    bool remove(std::string_view user, std::string_view domain);
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

    /// Names may not contain ':' or control characters and are at most 256 bytes.
    [[nodiscard]] static bool valid_name(std::string_view name, bool allow_empty);

private:
    std::vector<Entry> entries_;
};

}  // namespace farland::auth
