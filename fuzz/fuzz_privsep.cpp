// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Privilege separation: the monitor parses whatever a (possibly compromised)
// network process sends. Every frame must decode or fail cleanly, decoded
// messages must round-trip, and the monitor must never accept an identity it
// did not verify.

#include <farland/base/assert.hpp>
#include <farland/server/privsep.hpp>

#include "fuzz.hpp"

#include <span>

namespace {

using namespace farland;
namespace privsep = server::privsep;

/// Accepts user "alice" when the NT response starts with 0xAA.
class Verifier final : public auth::NtlmVerifier {
public:
    std::optional<std::array<std::byte, 16>> session_base_key(std::string_view user, std::string_view /*domain*/,
                                                              std::span<const std::byte, 8> /*challenge*/,
                                                              std::span<const std::byte> response) override
    {
        if (user != "alice" || response.empty() || response.front() != std::byte{0xAA}) {
            return std::nullopt;
        }
        verified_alice = true;
        return std::array<std::byte, 16>{};
    }
    bool verify_password(std::string_view /*user*/, std::string_view /*domain*/, std::string_view /*password*/) override
    {
        return false;
    }
    bool verified_alice = false;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    auto input = std::as_bytes(std::span(data, size));
    Verifier verifier;
    privsep::MonitorService monitor(verifier);
    while (!input.empty()) {
        const auto length = privsep::message_length(input);
        if (!length || !*length) {
            break;
        }
        const auto frame = input.first(**length);
        input = input.subspan(**length);

        if (const auto message = privsep::decode(frame); message.has_value()) {
            const auto encoded = privsep::encode(*message);
            const auto again = privsep::decode(encoded);
            FARLAND_ASSERT(again.has_value());
            FARLAND_ASSERT(privsep::encode(*again) == encoded);
        }
        if (!monitor.handle(frame).has_value()) {
            break;  // the monitor drops a misbehaving network process
        }
        if (const auto accepted = monitor.take_authenticated(); accepted && accepted->identity) {
            FARLAND_ASSERT(verifier.verified_alice);
            FARLAND_ASSERT(accepted->identity->user == "alice");
        }
    }
    return 0;
}
