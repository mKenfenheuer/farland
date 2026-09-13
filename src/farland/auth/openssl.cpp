// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/openssl.hpp>
#include <farland/base/assert.hpp>
#include <farland/base/ber.hpp>
#include <farland/base/reader.hpp>

#include <openssl/err.h>

#include <array>
#include <span>

namespace farland::auth::ossl {

namespace {

/// Encodes an ASN.1 object with its `i2d_*` function into a buffer we own.
template <class T>
std::vector<unsigned char> to_der(int (*encode)(const T*, unsigned char**), const T* object)
{
    const int length = encode(object, nullptr);
    FARLAND_ASSERT(length > 0);
    std::vector<unsigned char> der(static_cast<std::size_t>(length));
    unsigned char* out = der.data();
    const int written = encode(object, &out);
    FARLAND_ASSERT(written == length);
    return der;
}

}  // namespace

std::string take_errors()
{
    std::string text;
    for (;;) {
        const char* data = nullptr;
        int flags = 0;
        const unsigned long code = ERR_get_error_all(nullptr, nullptr, nullptr, &data, &flags);
        if (code == 0) {
            break;
        }
        std::array<char, 256> buffer{};
        ERR_error_string_n(code, buffer.data(), buffer.size());
        if (!text.empty()) {
            text += "; ";
        }
        text += buffer.data();
        if ((flags & ERR_TXT_STRING) != 0 && data != nullptr && *data != '\0') {
            text += " (";
            text += data;
            text += ')';
        }
    }
    return text;
}

std::vector<std::byte> certificate_der(const X509& certificate)
{
    const auto der = to_der(&i2d_X509, &certificate);
    const auto bytes = std::as_bytes(std::span(der));
    return {bytes.begin(), bytes.end()};
}

Result<std::vector<std::byte>> subject_public_key(const X509& certificate)
{
    // SubjectPublicKeyInfo ::= SEQUENCE { algorithm AlgorithmIdentifier, subjectPublicKey BIT STRING }
    // (RFC 5280 4.1). OpenSSL has no accessor for the raw BIT STRING that
    // stays within safe buffers, so re-encode the SPKI and take it apart.
    const auto spki = to_der(&i2d_X509_PUBKEY, static_cast<const X509_PUBKEY*>(X509_get_X509_PUBKEY(&certificate)));
    Reader r(std::as_bytes(std::span(spki)));
    constexpr auto der = ber::Rules::der;
    FARLAND_TRY(auto info, ber::read_constructed(r, ber::tags::sequence, der));
    FARLAND_TRY_VOID(r.expect_end("SubjectPublicKeyInfo"));
    FARLAND_TRY_VOID(ber::expect_tlv(info, ber::tags::sequence, der));
    FARLAND_TRY(const auto bits, ber::expect_tlv(info, ber::tags::bit_string, der));
    FARLAND_TRY_VOID(info.expect_end("SubjectPublicKeyInfo"));

    auto contents = bits.reader();
    FARLAND_TRY(const auto unused_bits, contents.u8());
    if (unused_bits != 0) {
        return fail(Errc::invalid_value, "subjectPublicKey BIT STRING has unused bits", bits.value_offset);
    }
    const auto key = contents.rest();
    return std::vector<std::byte>(key.begin(), key.end());
}

}  // namespace farland::auth::ossl
