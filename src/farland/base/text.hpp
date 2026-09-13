// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace farland {

/// Decodes UTF-16LE up to the first NUL code unit or the end of the input.
/// Invalid sequences (unpaired surrogates, an odd trailing byte) become U+FFFD,
/// so a peer can never make text decoding fail.
[[nodiscard]] std::string utf16le_to_utf8(std::span<const std::byte> bytes);

/// Encodes UTF-8 as UTF-16LE without a terminator. Invalid UTF-8 becomes U+FFFD.
[[nodiscard]] std::vector<std::byte> utf8_to_utf16le(std::string_view text);

/// Overwrites memory in a way the optimiser does not remove. For passwords and keys.
void secure_zero(std::span<std::byte> data) noexcept;

/// A string that wipes its storage when destroyed or reassigned. For passwords
/// received from the peer (Client Info, CredSSP).
class SecretString {
public:
    SecretString() = default;
    explicit SecretString(std::string value) : value_(std::move(value)) {}
    SecretString(const SecretString&) = delete;
    SecretString& operator=(const SecretString&) = delete;
    SecretString(SecretString&& other) noexcept : value_(std::move(other.value_)) { other.wipe(); }
    SecretString& operator=(SecretString&& other) noexcept
    {
        if (this != &other) {
            wipe();
            value_ = std::move(other.value_);
            other.wipe();
        }
        return *this;
    }
    ~SecretString() { wipe(); }

    [[nodiscard]] std::string_view view() const noexcept { return value_; }
    [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

private:
    void wipe() noexcept
    {
        secure_zero(std::as_writable_bytes(std::span(value_)));
        value_.clear();
    }

    std::string value_;
};

}  // namespace farland
