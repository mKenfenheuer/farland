// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

/// The obsolete primitives NTLM is built from ([MS-NLMP] 3.3, 3.4). MD4 and
/// RC4 are implemented here because OpenSSL 3 moved them to the legacy
/// provider, which farland does not load; MD5 and HMAC-MD5 wrap OpenSSL's
/// default provider. Nothing but NTLM may use these.
namespace farland::auth::legacy {

using Digest = std::array<std::byte, 16>;

/// MD4 (RFC 1320). Wipes its internal buffers, since the input is a password.
[[nodiscard]] Digest md4(std::span<const std::byte> data) noexcept;

/// MD5 (RFC 1321) of the concatenation of `parts`.
[[nodiscard]] Digest md5(std::initializer_list<std::span<const std::byte>> parts);

/// HMAC-MD5 (RFC 2104) of the concatenation of `parts`.
[[nodiscard]] Digest hmac_md5(std::span<const std::byte> key, std::initializer_list<std::span<const std::byte>> parts);

/// RC4 keystream state. NTLM keeps one handle per direction for the whole
/// session ([MS-NLMP] 3.4.3), so this is stateful; the state is wiped on
/// destruction.
class Rc4 {
public:
    /// `key` must hold 1 to 256 bytes.
    explicit Rc4(std::span<const std::byte> key) noexcept;
    Rc4(const Rc4&) = delete;
    Rc4& operator=(const Rc4&) = delete;
    Rc4(Rc4&&) = delete;
    Rc4& operator=(Rc4&&) = delete;
    ~Rc4();

    /// XORs the next `data.size()` keystream bytes into `data`.
    void apply(std::span<std::byte> data) noexcept;

private:
    std::array<std::uint8_t, 256> s_{};
    std::uint8_t i_ = 0;
    std::uint8_t j_ = 0;
};

}  // namespace farland::auth::legacy
