// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Runs the NTLM message and AV pair decoders on the input; every message
// that decodes must re-encode and decode back to the same value. Then offers
// the input as the AUTHENTICATE_MESSAGE of a fixed NEGOTIATE/CHALLENGE
// exchange (the seeds in corpus/ntlm/ are valid ones for that exchange) and
// as the CHALLENGE_MESSAGE for an initiator.

#include <farland/auth/ntlm.hpp>
#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include "fuzz.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace {

using namespace farland;
namespace ntlm = farland::auth::ntlm;

template <class Decode>
void round_trip(std::span<const std::byte> input, Decode decode)
{
    const auto decoded = decode(input);
    if (!decoded.has_value()) {
        return;
    }
    const auto again = decode(ntlm::encode(*decoded));
    FARLAND_ASSERT(again.has_value() && *again == *decoded);
}

void av_pairs_round_trip(std::span<const std::byte> input)
{
    Reader r(input);
    const auto pairs = ntlm::decode_av_pairs(r);
    if (!pairs.has_value()) {
        return;
    }
    Writer w;
    ntlm::encode_av_pairs(w, *pairs);
    Reader back(w.view());
    const auto again = ntlm::decode_av_pairs(back);
    FARLAND_ASSERT(again.has_value() && *again == *pairs && back.empty());
}

// The fixed exchange. The seeds were made with exactly these values.
constexpr std::array<std::byte, 8> server_challenge{std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
                                                    std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef}};
constexpr std::array<std::byte, 8> client_challenge{std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa},
                                                    std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa}};
constexpr std::uint64_t timestamp = 0x01d9'0000'0000'0000ULL;

std::array<std::byte, 16> filled(std::uint8_t value)
{
    std::array<std::byte, 16> out{};
    out.fill(std::byte{value});
    return out;
}

ntlm::InitiatorConfig initiator_config()
{
    return ntlm::InitiatorConfig{
        .user = "User",
        .domain = "Domain",
        .password = SecretString("Password"),
        .workstation = "COMPUTER",
        .channel_bindings = filled(0x42),
        .target_name = "TERMSRV/farland.example",
        .client_challenge = client_challenge,
        .timestamp = timestamp,
        .random_session_key = filled(0x55),
    };
}

void as_authenticate(std::span<const std::byte> input)
{
    ntlm::LocalNtlmVerifier verifier([](std::string_view user, std::string_view) -> std::optional<auth::NtHash> {
        if (user == "User") {
            return ntlm::nt_hash("Password");
        }
        return std::nullopt;
    });
    ntlm::Initiator initiator(initiator_config());
    const auto negotiate = initiator.step({});
    FARLAND_ASSERT(negotiate.has_value());

    ntlm::Acceptor acceptor(ntlm::AcceptorConfig{
        .verifier = verifier,
        .netbios_domain = "WORKGROUP",
        .netbios_computer = "FARLAND",
        .dns_domain = "workgroup.example",
        .dns_computer = "farland.workgroup.example",
        .channel_bindings = filled(0x42),
        .channel_binding_policy = ntlm::ChannelBindingPolicy::require,
        .server_challenge = server_challenge,
        .timestamp = timestamp,
    });
    const auto challenge = acceptor.step(negotiate->token);
    FARLAND_ASSERT(challenge.has_value());
    if (!acceptor.step(input).has_value()) {
        return;
    }
    // Established: exercise the session with the input as well.
    FARLAND_ASSERT(acceptor.complete());
    static_cast<void>(acceptor.unwrap(input));
    static_cast<void>(acceptor.verify_mic(input, input));
    static_cast<void>(acceptor.wrap(input));
}

void as_challenge(std::span<const std::byte> input)
{
    ntlm::Initiator initiator(initiator_config());
    FARLAND_ASSERT(initiator.step({}).has_value());
    if (initiator.step(input).has_value()) {
        static_cast<void>(initiator.unwrap(input));
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span(data, size));
    round_trip(input, ntlm::decode_negotiate);
    round_trip(input, ntlm::decode_challenge);
    round_trip(input, ntlm::decode_authenticate);
    av_pairs_round_trip(input);
    as_authenticate(input);
    as_challenge(input);
    return 0;
}
