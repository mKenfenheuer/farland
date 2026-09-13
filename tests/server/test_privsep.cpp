// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/proto/x224.hpp>
#include <farland/server/privsep.hpp>

#include <catch2/catch_test_macros.hpp>

namespace privsep = farland::server::privsep;
namespace proto = farland::proto;
using farland::Errc;

namespace {

/// Accepts user "alice" (any domain) when the response starts with 0xAA, and
/// the password "pw".
class FakeVerifier final : public farland::auth::NtlmVerifier {
public:
    std::optional<std::array<std::byte, 16>> session_base_key(std::string_view user, std::string_view /*domain*/,
                                                              std::span<const std::byte, 8> /*challenge*/,
                                                              std::span<const std::byte> response) override
    {
        ++calls;
        if (user != "alice" || response.empty() || response[0] != std::byte{0xAA}) {
            return std::nullopt;
        }
        std::array<std::byte, 16> key{};
        key.fill(std::byte{0x5C});
        return key;
    }
    bool verify_password(std::string_view user, std::string_view /*domain*/, std::string_view password) override
    {
        ++calls;
        return user == "alice" && password == "pw";
    }
    int calls = 0;
};

/// A RemoteVerifier talking to a MonitorService directly, as over a socketpair.
privsep::RemoteVerifier::Call direct(privsep::MonitorService& monitor)
{
    return [&monitor](std::span<const std::byte> request) -> farland::Result<std::vector<std::byte>> {
        auto reply = monitor.handle(request);
        if (!reply) {
            return std::unexpected(reply.error());
        }
        return reply->value_or(std::vector<std::byte>{});
    };
}

farland::server::Negotiation nla_negotiation(std::string user, std::string domain)
{
    return {"alice", proto::protocol::hybrid_ex | proto::protocol::ssl, proto::protocol::hybrid_ex,
            farland::auth::Identity{std::move(user), std::move(domain)}};
}

}  // namespace

TEST_CASE("Privsep messages round-trip")
{
    const std::array challenge{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                               std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    privsep::VerifyNtlmRequest request{"alice", "LAB", challenge, {std::byte{0xAA}, std::byte{0xBB}}};
    const auto frame = privsep::encode(request);
    CHECK(privsep::message_length(frame).value() == frame.size());
    CHECK_FALSE(privsep::message_length(std::span(frame).first(frame.size() - 1)).value().has_value());
    const auto decoded = std::get<privsep::VerifyNtlmRequest>(privsep::decode(frame).value());
    CHECK(decoded.user == "alice");
    CHECK(decoded.domain == "LAB");
    CHECK(decoded.server_challenge == challenge);
    CHECK(decoded.nt_response == request.nt_response);

    const auto notice = privsep::encode(privsep::Authenticated{nla_negotiation("alice", "LAB")});
    const auto authenticated = std::get<privsep::Authenticated>(privsep::decode(notice).value());
    CHECK(authenticated.negotiation.selected_protocol == proto::protocol::hybrid_ex);
    CHECK(authenticated.negotiation.identity->domain == "LAB");
}

TEST_CASE("Malformed privsep messages are rejected")
{
    auto frame = privsep::encode(privsep::VerifyPasswordResponse{true});
    frame.push_back(std::byte{0});  // trailing byte, length now wrong
    CHECK(privsep::decode(frame).error().code == Errc::invalid_length);

    const std::array oversized{std::byte{0xFF}, std::byte{0xFF}, std::byte{0}, std::byte{0}};
    CHECK(privsep::message_length(oversized).error().code == Errc::limit_exceeded);

    const std::array unknown{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{99}};
    CHECK(privsep::decode(unknown).error().code == Errc::invalid_value);
}

TEST_CASE("The network process verifies through the monitor without seeing hashes")
{
    FakeVerifier real;
    privsep::MonitorService monitor(real);
    privsep::RemoteVerifier remote(direct(monitor));

    const std::array<std::byte, 8> challenge{};
    const std::array good{std::byte{0xAA}, std::byte{0x01}};
    const std::array bad{std::byte{0x00}};
    const auto key = remote.session_base_key("alice", "LAB", challenge, good);
    REQUIRE(key.has_value());
    CHECK((*key)[0] == std::byte{0x5C});
    CHECK_FALSE(remote.session_base_key("alice", "LAB", challenge, bad).has_value());
    CHECK(remote.verify_password("alice", "LAB", "pw"));
    CHECK_FALSE(remote.verify_password("alice", "LAB", "nope"));
    CHECK(real.calls == 4);
}

TEST_CASE("The monitor accepts only identities it verified")
{
    FakeVerifier real;
    {
        privsep::MonitorService monitor(real);
        privsep::RemoteVerifier remote(direct(monitor));
        const std::array<std::byte, 8> challenge{};
        const std::array good{std::byte{0xAA}};
        REQUIRE(remote.session_base_key("alice", "LAB", challenge, good).has_value());
        const auto ok = monitor.handle(privsep::encode(privsep::Authenticated{nla_negotiation("alice", "LAB")}));
        REQUIRE(ok.has_value());
        CHECK_FALSE(ok->has_value());  // notices get no reply
        CHECK(monitor.take_authenticated()->identity->user == "alice");
        // A second report is a protocol violation.
        CHECK_FALSE(
            monitor.handle(privsep::encode(privsep::Authenticated{nla_negotiation("alice", "LAB")})).has_value());
    }
    {
        privsep::MonitorService monitor(real);
        // Claiming alice without any verification: refused.
        CHECK_FALSE(
            monitor.handle(privsep::encode(privsep::Authenticated{nla_negotiation("alice", "LAB")})).has_value());
        CHECK_FALSE(monitor.take_authenticated().has_value());
    }
    {
        privsep::MonitorService monitor(real);
        // TLS-only connections carry no identity.
        farland::server::Negotiation tls{"", proto::protocol::ssl, proto::protocol::ssl, std::nullopt};
        REQUIRE(monitor.handle(privsep::encode(privsep::Authenticated{tls})).has_value());
        CHECK(monitor.take_authenticated().has_value());
        privsep::MonitorService other(real);
        tls.identity = farland::auth::Identity{"alice", ""};
        CHECK_FALSE(other.handle(privsep::encode(privsep::Authenticated{tls})).has_value());
    }
}

TEST_CASE("The monitor limits verification attempts per connection")
{
    FakeVerifier real;
    privsep::MonitorService monitor(real, 3);
    privsep::RemoteVerifier remote(direct(monitor));
    const std::array<std::byte, 8> challenge{};
    const std::array good{std::byte{0xAA}};
    const std::array bad{std::byte{0x00}};
    CHECK_FALSE(remote.session_base_key("alice", "", challenge, bad).has_value());
    CHECK_FALSE(remote.session_base_key("alice", "", challenge, bad).has_value());
    CHECK(remote.session_base_key("alice", "", challenge, good).has_value());
    CHECK_FALSE(remote.session_base_key("alice", "", challenge, good).has_value());  // fourth attempt refused
    CHECK(real.calls == 3);
}
