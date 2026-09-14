// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/auto_reconnect.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace arc = farland::auth::arc;

namespace {

template <std::size_t N>
std::array<std::byte, N> hex(std::string_view text)
{
    std::array<std::byte, N> out{};
    REQUIRE(text.size() == 2 * N);
    for (std::size_t i = 0; i < N; ++i) {
        out.at(i) = static_cast<std::byte>(std::stoul(std::string(text.substr(2 * i, 2)), nullptr, 16));
    }
    return out;
}

arc::Random counting_random()
{
    arc::Random random{};
    for (std::size_t i = 0; i < random.size(); ++i) {
        random.at(i) = static_cast<std::byte>(i);
    }
    return random;
}

}  // namespace

TEST_CASE("The ARC security verifier is HMAC-MD5 over the client random, [MS-RDPBCGR] 5.5")
{
    // Enhanced RDP Security: the client random is 32 zero bytes (5.5 step 4;
    // FreeRDP's rdp_compute_client_auto_reconnect_cookie does the same). The
    // expected values are HMAC-MD5 computed independently (Python's hmac).
    CHECK(arc::security_verifier(counting_random()) == hex<16>("b639c8731638618b707972aa6e96cf90"));
    const auto random = hex<16>("a1b2c3d4e5f60718293a4b5c6d7e8f90");
    CHECK(arc::security_verifier(random) == hex<16>("f0f38a408823d9a746057bac2de8bee7"));
    CHECK(arc::security_verifier(random, arc::enhanced_security_client_random) == arc::security_verifier(random));

    // Standard RDP Security, with a real client random 20 21 .. 3f.
    std::array<std::byte, arc::client_random_size> client_random{};
    for (std::size_t i = 0; i < client_random.size(); ++i) {
        client_random.at(i) = static_cast<std::byte>(0x20 + i);
    }
    CHECK(arc::security_verifier(random, client_random) == hex<16>("d38db46df3e780d1b40e073ffe3dffc9"));
}

TEST_CASE("An auto-reconnect secret is good for one check and rotates with every connection")
{
    arc::Secret secret(5);
    CHECK(secret.logon_id() == 5);
    CHECK_FALSE(secret.verify(5, arc::security_verifier(arc::Random{})));  // nothing issued yet

    const auto first = secret.rotate().value();
    const auto first_verifier = arc::security_verifier(first);
    CHECK(secret.verify(5, first_verifier));
    CHECK_FALSE(secret.verify(5, first_verifier));  // spent

    const auto second = secret.rotate().value();
    CHECK(second != first);
    CHECK_FALSE(secret.verify(5, first_verifier));  // an old cookie never works again

    const auto third = secret.rotate().value();
    CHECK_FALSE(secret.verify(6, arc::security_verifier(third)));  // another session's logon id

    const auto fourth = secret.rotate().value();
    auto tampered = arc::security_verifier(fourth);
    tampered.back() ^= std::byte{1};
    CHECK_FALSE(secret.verify(5, tampered));
}
