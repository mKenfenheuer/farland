// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The first byte selects a target, the rest is its input:
//   0 mod 4: TSRequest (and framing), 1: SPNEGO token, 2: TSCredentials. A
//            value that decodes must re-encode and decode to the same value.
//   3 mod 4: two CredSSP Acceptors with max_version 2 + (byte / 4) % 5, over
//            a mechanism that accepts anything. One gets the input at once,
//            the other byte by byte; both must end in the same state with the
//            same output.

#include <farland/auth/credssp.hpp>
#include <farland/auth/spnego.hpp>
#include <farland/base/assert.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace {

using namespace farland;
namespace credssp = farland::auth::credssp;
namespace spnego = farland::auth::spnego;
using Bytes = std::vector<std::byte>;

/// The key the Acceptor binds to. Short, so that a v2-4 pubKeyAuth (the raw
/// key, under the identity wrap below) is within the fuzzer's reach.
constexpr std::array public_key{std::byte{'K'}, std::byte{'E'}, std::byte{'Y'}};
constexpr std::array valid_mic{std::byte{'M'}, std::byte{'I'}, std::byte{'C'}};
/// Largest input also fed byte by byte.
constexpr std::size_t max_bytewise = 4096;

/// Accepts every token and completes on its second step. wrap and unwrap are
/// the identity and the only valid MIC is "MIC", so the fuzzer reaches
/// pubKeyAuth and TSCredentials. A token starting with 0xFF is rejected.
class PassThrough final : public auth::SecurityContext {
public:
    [[nodiscard]] std::span<const std::byte> mechanism() const noexcept override { return spnego::ntlm_oid; }

    [[nodiscard]] Result<auth::Step> step(std::span<const std::byte> input) override
    {
        if (!input.empty() && input.front() == std::byte{0xFF}) {
            return fail(Errc::invalid_value, "fuzz mechanism rejects the token");
        }
        if (++steps_ == 1) {
            return auth::Step{Bytes{std::byte{'C'}}, false};
        }
        complete_ = true;
        return auth::Step{{}, true};
    }

    [[nodiscard]] bool complete() const noexcept override { return complete_; }
    [[nodiscard]] Bytes wrap(std::span<const std::byte> plaintext) override
    {
        return {plaintext.begin(), plaintext.end()};
    }
    [[nodiscard]] Result<Bytes> unwrap(std::span<const std::byte> wrapped) override
    {
        return Bytes(wrapped.begin(), wrapped.end());
    }
    [[nodiscard]] Bytes get_mic(std::span<const std::byte> /*message*/) override
    {
        return {valid_mic.begin(), valid_mic.end()};
    }
    [[nodiscard]] Result<void> verify_mic(std::span<const std::byte> /*message*/,
                                          std::span<const std::byte> mic) override
    {
        if (!std::ranges::equal(mic, valid_mic)) {
            return fail(Errc::invalid_value, "fuzz mechanism rejects the MIC");
        }
        return {};
    }
    [[nodiscard]] const auth::Identity& identity() const noexcept override { return identity_; }

private:
    int steps_ = 0;
    bool complete_ = false;
    auth::Identity identity_{"fuzz", "FUZZ"};
};

credssp::AcceptorConfig acceptor_config(std::uint32_t max_version)
{
    credssp::AcceptorConfig config;
    config.server_public_key = public_key;
    config.max_version = max_version;
    config.make_mechanism = [](std::span<const std::byte> oid) -> std::unique_ptr<auth::SecurityContext> {
        if (!std::ranges::equal(oid, spnego::ntlm_oid)) {
            return nullptr;
        }
        return std::make_unique<PassThrough>();
    };
    return config;
}

void check_ts_request(std::span<const std::byte> input)
{
    const auto frame = credssp::frame_ts_request(input);
    const auto decoded = credssp::decode_ts_request(input);
    if (!decoded.has_value()) {
        return;
    }
    FARLAND_ASSERT(frame.has_value() && frame->has_value() && **frame == input.size());
    const auto again = credssp::decode_ts_request(credssp::encode(*decoded));
    FARLAND_ASSERT(again.has_value() && *again == *decoded);
}

void check_spnego(std::span<const std::byte> input)
{
    const auto decoded = spnego::decode(input);
    if (!decoded.has_value()) {
        return;
    }
    const Bytes encoded = std::visit([](const auto& token) { return spnego::encode(token); }, *decoded);
    const auto again = spnego::decode(encoded);
    FARLAND_ASSERT(again.has_value() && *again == *decoded);
    static_cast<void>(spnego::is_raw_ntlm(input));
}

void check_ts_credentials(std::span<const std::byte> input)
{
    const auto decoded = credssp::decode_ts_credentials(input);
    if (!decoded.has_value()) {
        return;
    }
    const auto again = credssp::decode_ts_credentials(credssp::encode(*decoded));
    FARLAND_ASSERT(again.has_value() && *again == *decoded);
    if (const auto* password = std::get_if<credssp::TsPasswordCreds>(&decoded->credentials)) {
        static_cast<void>(credssp::to_password_credentials(*password));
    }
}

void run_acceptors(std::span<const std::byte> input, std::uint32_t max_version)
{
    credssp::Acceptor whole(acceptor_config(max_version));
    whole.receive(input);
    const Bytes whole_output = whole.take_output();
    const Bytes whole_rest = whole.take_remaining_input();
    static_cast<void>(whole.failure_reason());
    static_cast<void>(whole.take_credentials());
    if (input.size() > max_bytewise) {
        return;
    }

    credssp::Acceptor bytewise(acceptor_config(max_version));
    for (std::size_t i = 0; i < input.size(); ++i) {
        bytewise.receive(input.subspan(i, 1));
    }
    FARLAND_ASSERT(bytewise.status() == whole.status());
    FARLAND_ASSERT(bytewise.take_output() == whole_output);
    FARLAND_ASSERT(bytewise.take_remaining_input() == whole_rest);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span(data, size));
    if (input.empty()) {
        return 0;
    }
    const auto selector = std::to_integer<unsigned>(input.front());
    const auto rest = input.subspan(1);
    switch (selector % 4U) {
    case 0:
        check_ts_request(rest);
        break;
    case 1:
        check_spnego(rest);
        break;
    case 2:
        check_ts_credentials(rest);
        break;
    default:
        run_acceptors(rest, credssp::min_version + (selector / 4U) % 5U);
        break;
    }
    return 0;
}
