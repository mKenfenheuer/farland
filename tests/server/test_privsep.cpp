// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/spnego.hpp>
#include <farland/proto/x224.hpp>
#include <farland/server/privsep.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>

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

/// A Kerberos context the monitor would hold: it takes one token, reports
/// `alice@EXAMPLE.COM`, and seals by flipping every byte -- enough to tell
/// whether the proxy carried the bytes both ways.
class FakeKerberos final : public farland::auth::SecurityContext {
public:
    [[nodiscard]] std::span<const std::byte> mechanism() const noexcept override
    {
        return farland::auth::spnego::kerberos_oid;
    }

    [[nodiscard]] farland::Result<farland::auth::Step> step(std::span<const std::byte> input) override
    {
        ++steps;
        if (input.empty() || input[0] != std::byte{0x60}) {
            return farland::fail(Errc::invalid_value, "not a ticket");
        }
        identity_ = {"alice", "EXAMPLE.COM"};
        complete_ = true;
        return farland::auth::Step{.token = {std::byte{0xA0}}, .complete = true};
    }

    [[nodiscard]] bool complete() const noexcept override { return complete_; }

    [[nodiscard]] std::vector<std::byte> wrap(std::span<const std::byte> plaintext) override { return flip(plaintext); }

    [[nodiscard]] farland::Result<std::vector<std::byte>> unwrap(std::span<const std::byte> wrapped) override
    {
        if (wrapped.empty()) {
            return farland::fail(Errc::truncated, "nothing to unseal");
        }
        return flip(wrapped);
    }

    [[nodiscard]] std::vector<std::byte> get_mic(std::span<const std::byte> message) override
    {
        return {std::byte{static_cast<unsigned char>(message.size())}};
    }

    [[nodiscard]] farland::Result<void> verify_mic(std::span<const std::byte> message,
                                                   std::span<const std::byte> mic) override
    {
        if (mic.size() != 1 || mic[0] != std::byte{static_cast<unsigned char>(message.size())}) {
            return farland::fail(Errc::invalid_value, "wrong signature");
        }
        return {};
    }

    [[nodiscard]] const farland::auth::Identity& identity() const noexcept override { return identity_; }

    int steps = 0;

private:
    static std::vector<std::byte> flip(std::span<const std::byte> bytes)
    {
        std::vector<std::byte> out(bytes.begin(), bytes.end());
        for (auto& b : out) {
            b = static_cast<std::byte>(std::to_integer<unsigned>(b) ^ 0xFFU);
        }
        return out;
    }

    bool complete_ = false;
    farland::auth::Identity identity_;
};

farland::server::Negotiation nla_negotiation(std::string user, std::string domain)
{
    return {"alice", proto::protocol::hybrid_ex | proto::protocol::ssl, proto::protocol::hybrid_ex,
            farland::auth::Identity{std::move(user), std::move(domain)}};
}

/// The Call a RemoteKerberos uses, straight into a MonitorService.
privsep::Call direct_call(privsep::MonitorService& monitor)
{
    return [&monitor](std::span<const std::byte> request) -> farland::Result<std::vector<std::byte>> {
        auto reply = monitor.handle(request);
        if (!reply) {
            return std::unexpected(reply.error());
        }
        return reply->value_or(std::vector<std::byte>{});
    };
}

}  // namespace

TEST_CASE("Privsep: a Kerberos context is driven from the sandbox", "[privsep][kerberos]")
{
    FakeVerifier real;
    FakeKerberos* context = nullptr;
    privsep::MonitorService monitor(real);
    monitor.serve_kerberos(
        [&context](std::span<const std::byte> oid) -> std::unique_ptr<farland::auth::SecurityContext> {
            if (!std::ranges::equal(oid, farland::auth::spnego::kerberos_oid)) {
                return nullptr;
            }
            auto made = std::make_unique<FakeKerberos>();
            context = made.get();
            return made;
        });

    privsep::RemoteKerberos remote(direct_call(monitor), farland::auth::spnego::kerberos_oid);
    CHECK_FALSE(remote.complete());

    const std::array ticket{std::byte{0x60}, std::byte{0x01}};
    const auto step = remote.step(ticket);
    REQUIRE(step.has_value());
    CHECK(step->complete);
    CHECK(step->token == std::vector{std::byte{0xA0}});
    CHECK(remote.complete());
    // The identity crossed the channel: the sandbox never ran Kerberos.
    CHECK(remote.identity().user == "alice");
    CHECK(remote.identity().domain == "EXAMPLE.COM");
    REQUIRE(context != nullptr);
    CHECK(context->steps == 1);

    const std::array secret{std::byte{0x11}, std::byte{0x22}};
    const auto sealed = remote.wrap(secret);
    CHECK(sealed == std::vector{std::byte{0xEE}, std::byte{0xDD}});
    const auto opened = remote.unwrap(sealed);
    REQUIRE(opened.has_value());
    CHECK(*opened == std::vector(secret.begin(), secret.end()));

    const auto mic = remote.get_mic(secret);
    CHECK(remote.verify_mic(secret, mic).has_value());
    CHECK_FALSE(remote.verify_mic(secret, std::array{std::byte{0x00}}).has_value());

    // Having established the identity itself, the monitor now lets the
    // network process claim it -- and nothing else.
    CHECK(monitor.handle(privsep::encode(privsep::Authenticated{nla_negotiation("alice", "EXAMPLE.COM")})).has_value());
}

TEST_CASE("Privsep: Kerberos is refused where the host does not serve it", "[privsep][kerberos]")
{
    FakeVerifier real;
    privsep::MonitorService monitor(real);  // no serve_kerberos
    privsep::RemoteKerberos remote(direct_call(monitor), farland::auth::spnego::kerberos_oid);

    const std::array ticket{std::byte{0x60}, std::byte{0x01}};
    CHECK_FALSE(remote.step(ticket).has_value());
    CHECK_FALSE(remote.complete());
    // Nothing was authenticated, so nothing may be claimed.
    CHECK_FALSE(
        monitor.handle(privsep::encode(privsep::Authenticated{nla_negotiation("alice", "EXAMPLE.COM")})).has_value());
}

TEST_CASE("Privsep: a mechanism that is not Kerberos gets no context", "[privsep][kerberos]")
{
    FakeVerifier real;
    privsep::MonitorService monitor(real);
    monitor.serve_kerberos([](std::span<const std::byte> oid) -> std::unique_ptr<farland::auth::SecurityContext> {
        return std::ranges::equal(oid, farland::auth::spnego::kerberos_oid) ? std::make_unique<FakeKerberos>()
                                                                            : nullptr;
    });
    // The network process asks with the NTLM OID: the monitor makes nothing.
    privsep::RemoteKerberos remote(direct_call(monitor), farland::auth::spnego::ntlm_oid);
    CHECK_FALSE(remote.step(std::array{std::byte{0x60}}).has_value());
}

TEST_CASE("Privsep: a bad ticket is refused, and costs an attempt", "[privsep][kerberos]")
{
    FakeVerifier real;
    privsep::MonitorService monitor(real, 2);
    monitor.serve_kerberos([](std::span<const std::byte>) -> std::unique_ptr<farland::auth::SecurityContext> {
        return std::make_unique<FakeKerberos>();
    });
    privsep::RemoteKerberos remote(direct_call(monitor), farland::auth::spnego::kerberos_oid);

    // Not a ticket: the context refuses it and the answer says only that.
    CHECK_FALSE(remote.step(std::array{std::byte{0x00}}).has_value());
    CHECK_FALSE(remote.step(std::array{std::byte{0x00}}).has_value());
    // The third step is over the budget and refused without asking.
    CHECK_FALSE(remote.step(std::array{std::byte{0x60}}).has_value());
}

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

    // One byte past max_message_size, little endian. The limit is 64 KiB
    // because a Kerberos ticket with a PAC goes through this channel.
    const std::array oversized{std::byte{0x01}, std::byte{0x00}, std::byte{0x01}, std::byte{0}};
    CHECK(privsep::message_length(oversized).error().code == Errc::limit_exceeded);
    // The largest that is allowed is not an error, only not here yet.
    const std::array at_limit{std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0}};
    const auto length = privsep::message_length(at_limit);
    REQUIRE(length.has_value());
    CHECK_FALSE(length->has_value());

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
