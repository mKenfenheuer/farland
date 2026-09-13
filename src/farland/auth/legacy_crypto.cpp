// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/legacy_crypto.hpp>
#include <farland/auth/openssl.hpp>
#include <farland/base/assert.hpp>
#include <farland/base/text.hpp>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <algorithm>
#include <bit>
#include <memory>
#include <utility>

namespace farland::auth::legacy {

namespace {

// MD4 (RFC 1320 3.4): one 64-byte block. The three rounds use the auxiliary
// functions F, G and H with the RFC's shift amounts and word orders.
void md4_block(std::array<std::uint32_t, 4>& state, std::span<const std::byte> block) noexcept
{
    FARLAND_ASSERT(block.size() == 64);
    std::array<std::uint32_t, 16> x{};
    for (std::size_t i = 0; i < x.size(); ++i) {
        std::uint32_t word = 0;
        for (std::size_t b = 0; b < 4; ++b) {
            word |= std::to_integer<std::uint32_t>(block[(i * 4) + b]) << (8U * b);
        }
        x[i] = word;
    }

    const auto f = [](std::uint32_t u, std::uint32_t v, std::uint32_t w) { return (u & v) | (~u & w); };
    const auto g = [](std::uint32_t u, std::uint32_t v, std::uint32_t w) { return (u & v) | (u & w) | (v & w); };
    const auto h = [](std::uint32_t u, std::uint32_t v, std::uint32_t w) { return u ^ v ^ w; };
    constexpr std::uint32_t round2 = 0x5a827999U;
    constexpr std::uint32_t round3 = 0x6ed9eba1U;

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];

    for (std::size_t i = 0; i < 16; i += 4) {  // Round 1: [abcd k 3] [dabc k+1 7] [cdab k+2 11] [bcda k+3 19]
        a = std::rotl(a + f(b, c, d) + x[i], 3);
        d = std::rotl(d + f(a, b, c) + x[i + 1], 7);
        c = std::rotl(c + f(d, a, b) + x[i + 2], 11);
        b = std::rotl(b + f(c, d, a) + x[i + 3], 19);
    }
    for (std::size_t i = 0; i < 4; ++i) {  // Round 2: [abcd k 3] [dabc k+4 5] [cdab k+8 9] [bcda k+12 13]
        a = std::rotl(a + g(b, c, d) + x[i] + round2, 3);
        d = std::rotl(d + g(a, b, c) + x[i + 4] + round2, 5);
        c = std::rotl(c + g(d, a, b) + x[i + 8] + round2, 9);
        b = std::rotl(b + g(c, d, a) + x[i + 12] + round2, 13);
    }
    for (const std::size_t i : {0U, 2U, 1U, 3U}) {  // Round 3: [abcd k 3] [dabc k+8 9] [cdab k+4 11] [bcda k+12 15]
        a = std::rotl(a + h(b, c, d) + x[i] + round3, 3);
        d = std::rotl(d + h(a, b, c) + x[i + 8] + round3, 9);
        c = std::rotl(c + h(d, a, b) + x[i + 4] + round3, 11);
        b = std::rotl(b + h(c, d, a) + x[i + 12] + round3, 15);
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    secure_zero(std::as_writable_bytes(std::span(x)));
}

/// OpenSSL takes byte buffers as `unsigned char`; farland spans are `std::byte`.
const unsigned char* as_uchar(std::span<const std::byte> data) noexcept
{
    return reinterpret_cast<const unsigned char*>(data.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

using MdCtx = std::unique_ptr<EVP_MD_CTX, ossl::Deleter<&EVP_MD_CTX_free>>;
using Mac = std::unique_ptr<EVP_MAC, ossl::Deleter<&EVP_MAC_free>>;
using MacCtx = std::unique_ptr<EVP_MAC_CTX, ossl::Deleter<&EVP_MAC_CTX_free>>;

/// Moves a raw OpenSSL digest into a `Digest` and wipes the raw copy.
Digest take_digest(std::array<unsigned char, 16>& raw) noexcept
{
    const auto out = std::bit_cast<Digest>(raw);
    secure_zero(std::as_writable_bytes(std::span(raw)));
    return out;
}

}  // namespace

Digest md4(std::span<const std::byte> data) noexcept
{
    std::array<std::uint32_t, 4> state{0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U};

    std::size_t offset = 0;
    for (; data.size() - offset >= 64; offset += 64) {
        md4_block(state, data.subspan(offset, 64));
    }

    // Padding (RFC 1320 3.1, 3.2): 0x80, zeros to 56 mod 64, then the bit length (LE64).
    const auto tail = data.subspan(offset);
    std::array<std::byte, 128> last{};
    std::ranges::copy(tail, last.begin());
    last[tail.size()] = std::byte{0x80};
    const std::size_t padded = tail.size() < 56 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        last[padded - 8 + i] = static_cast<std::byte>(static_cast<std::uint8_t>(bits >> (8U * i)));
    }
    for (std::size_t block = 0; block < padded; block += 64) {
        md4_block(state, std::span<const std::byte>(last).subspan(block, 64));
    }
    secure_zero(last);

    Digest out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(state[i / 4] >> (8U * (i % 4))));
    }
    secure_zero(std::as_writable_bytes(std::span(state)));
    return out;
}

Digest md5(std::initializer_list<std::span<const std::byte>> parts)
{
    const MdCtx context(EVP_MD_CTX_new());
    FARLAND_ASSERT(context != nullptr);
    FARLAND_ASSERT(EVP_DigestInit_ex(context.get(), EVP_md5(), nullptr) == 1);
    for (const auto part : parts) {
        FARLAND_ASSERT(EVP_DigestUpdate(context.get(), part.data(), part.size()) == 1);
    }
    std::array<unsigned char, 16> raw{};
    unsigned int length = 0;
    FARLAND_ASSERT(EVP_DigestFinal_ex(context.get(), raw.data(), &length) == 1 && length == raw.size());
    return take_digest(raw);
}

Digest hmac_md5(std::span<const std::byte> key, std::initializer_list<std::span<const std::byte>> parts)
{
    static const Mac hmac(EVP_MAC_fetch(nullptr, OSSL_MAC_NAME_HMAC, nullptr));
    FARLAND_ASSERT(hmac != nullptr);
    const MacCtx context(EVP_MAC_CTX_new(hmac.get()));
    FARLAND_ASSERT(context != nullptr);

    std::array<char, 4> digest_name{'M', 'D', '5', '\0'};
    const std::array params{
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name.data(), 0),
        OSSL_PARAM_construct_end(),
    };
    FARLAND_ASSERT(EVP_MAC_init(context.get(), as_uchar(key), key.size(), params.data()) == 1);
    for (const auto part : parts) {
        FARLAND_ASSERT(EVP_MAC_update(context.get(), as_uchar(part), part.size()) == 1);
    }
    std::array<unsigned char, 16> raw{};
    std::size_t length = 0;
    FARLAND_ASSERT(EVP_MAC_final(context.get(), raw.data(), &length, raw.size()) == 1 && length == raw.size());
    return take_digest(raw);
}

// RC4 key scheduling (KSA) and keystream generation (PRGA).
Rc4::Rc4(std::span<const std::byte> key) noexcept
{
    FARLAND_ASSERT(!key.empty() && key.size() <= s_.size());
    for (std::size_t i = 0; i < s_.size(); ++i) {
        s_[i] = static_cast<std::uint8_t>(i);
    }
    std::uint8_t j = 0;
    for (std::size_t i = 0; i < s_.size(); ++i) {
        j = static_cast<std::uint8_t>(j + s_[i] + std::to_integer<std::uint8_t>(key[i % key.size()]));
        std::swap(s_[i], s_[j]);
    }
}

Rc4::~Rc4()
{
    secure_zero(std::as_writable_bytes(std::span(s_)));
    i_ = 0;
    j_ = 0;
}

void Rc4::apply(std::span<std::byte> data) noexcept
{
    for (std::byte& out : data) {
        i_ = static_cast<std::uint8_t>(i_ + 1U);
        j_ = static_cast<std::uint8_t>(j_ + s_[i_]);
        std::swap(s_[i_], s_[j_]);
        out ^= std::byte{s_[static_cast<std::uint8_t>(s_[i_] + s_[j_])]};
    }
}

}  // namespace farland::auth::legacy
