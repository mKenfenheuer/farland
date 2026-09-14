// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/auto_reconnect.hpp>
#include <farland/auth/legacy_crypto.hpp>
#include <farland/base/text.hpp>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <array>
#include <bit>

namespace farland::auth::arc {

Verifier security_verifier(const Random& random, std::span<const std::byte, client_random_size> client_random)
{
    return legacy::hmac_md5(random, {client_random});
}

Secret::~Secret()
{
    forget();
}

void Secret::forget() noexcept
{
    if (random_) {
        secure_zero(*random_);
        random_.reset();
    }
}

Result<Random> Secret::rotate()
{
    forget();
    std::array<unsigned char, random_size> raw{};
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        return fail(Errc::io, "no randomness for the auto-reconnect cookie");
    }
    const auto random = std::bit_cast<Random>(raw);
    secure_zero(std::as_writable_bytes(std::span(raw)));
    random_ = random;
    return random;
}

bool Secret::verify(std::uint32_t logon_id, std::span<const std::byte, 16> verifier,
                    std::span<const std::byte, client_random_size> client_random)
{
    if (!random_) {
        return false;
    }
    auto expected = security_verifier(*random_, client_random);
    forget();
    const bool match = CRYPTO_memcmp(expected.data(), verifier.data(), expected.size()) == 0;
    secure_zero(expected);
    return match && logon_id == logon_id_;
}

}  // namespace farland::auth::arc
