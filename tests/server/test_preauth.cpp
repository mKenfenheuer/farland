// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Pre-authentication: protocol selection, the TLS switch and the NLA
// handshake, driven through a scripted NlaAcceptor.

#include <farland/base/hexdump.hpp>
#include <farland/proto/x224.hpp>
#include <farland/server/preauth.hpp>

#include "support/bytes.hpp"
#include "support/client_pdus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <memory>
#include <vector>

namespace proto = farland::proto;
namespace ev = farland::server::preauth_event;
using farland::Reader;
using farland::to_hex;
using farland::server::PreAuth;
using farland::server::PreAuthConfig;
using farland::server::PreAuthEvent;
using Bytes = std::vector<std::byte>;
using Status = farland::auth::NlaAcceptor::Status;

namespace {

/// An NlaAcceptor that answers "ok" to "hello" and then succeeds, or fails on
/// anything else. Stands in for CredSSP here; CredSSP has its own tests.
class ScriptedNla final : public farland::auth::NlaAcceptor {
public:
    void receive(std::span<const std::byte> bytes) override
    {
        input_.insert(input_.end(), bytes.begin(), bytes.end());
        const Bytes hello = farland::test::hex("68 65 6c 6c 6f");
        if (status_ != Status::in_progress || input_.size() < hello.size()) {
            return;
        }
        if (!std::equal(hello.begin(), hello.end(), input_.begin())) {
            status_ = Status::failed;
            output_ = farland::test::hex("ee");  // stands in for the CredSSP error TSRequest
            return;
        }
        output_ = farland::test::hex("6f 6b");
        remaining_.assign(input_.begin() + static_cast<std::ptrdiff_t>(hello.size()), input_.end());
        status_ = Status::succeeded;
    }
    std::vector<std::byte> take_output() override { return std::exchange(output_, {}); }
    Status status() const noexcept override { return status_; }
    std::vector<std::byte> take_remaining_input() override { return std::exchange(remaining_, {}); }
    const farland::auth::Identity& identity() const noexcept override { return identity_; }
    std::optional<farland::auth::PasswordCredentials> take_credentials() override
    {
        return farland::auth::PasswordCredentials{"LAB", "alice", farland::SecretString("pw")};
    }
    std::string_view failure_reason() const noexcept override { return "scripted failure"; }

private:
    Bytes input_;
    Bytes output_;
    Bytes remaining_;
    Status status_ = Status::in_progress;
    farland::auth::Identity identity_{"alice", "LAB"};
};

PreAuth::NlaFactory scripted()
{
    return [] { return std::make_unique<ScriptedNla>(); };
}

std::vector<PreAuthEvent> events(PreAuth& p)
{
    std::vector<PreAuthEvent> out;
    while (auto e = p.poll_event()) {
        out.push_back(std::move(*e));
    }
    return out;
}

/// The Connection Confirm in `bytes`, decoded.
proto::ConnectionConfirm confirm_of(const Bytes& bytes)
{
    Reader r(bytes);
    Reader tpdu = proto::read_tpkt(r).value();
    return proto::decode_connection_confirm(tpdu).value();
}

std::uint32_t selected(PreAuth& p, std::uint32_t requested)
{
    p.receive(farland::test::client::connection_request(requested));
    const auto confirm = confirm_of(p.take_output());
    return std::get<proto::NegotiationResponse>(confirm.result).selected_protocol;
}

}  // namespace

TEST_CASE("Protocol selection prefers HYBRID_EX, then HYBRID, then TLS when allowed")
{
    const auto all = proto::protocol::ssl | proto::protocol::hybrid | proto::protocol::hybrid_ex;
    {
        PreAuth p({}, scripted());
        CHECK(selected(p, all) == proto::protocol::hybrid_ex);
    }
    {
        PreAuth p({}, scripted());
        CHECK(selected(p, proto::protocol::ssl | proto::protocol::hybrid) == proto::protocol::hybrid);
    }
    {
        PreAuth p(PreAuthConfig{all, false, false}, scripted());
        CHECK(selected(p, proto::protocol::ssl) == proto::protocol::ssl);
    }
    {
        // Without an NLA acceptor, TLS is all there is.
        PreAuth p(PreAuthConfig{all, false, false});
        CHECK(selected(p, all) == proto::protocol::ssl);
        CHECK(p.negotiation().requested_protocols == all);
    }
}

TEST_CASE("Clients that cannot do what the server requires get RDP_NEG_FAILURE")
{
    {
        PreAuth p({}, scripted());  // NLA required
        p.receive(farland::test::client::connection_request(proto::protocol::ssl));
        const auto confirm = confirm_of(p.take_output());
        CHECK(std::get<proto::NegotiationFailureCode>(confirm.result) ==
              proto::NegotiationFailureCode::hybrid_required_by_server);
        CHECK(p.state() == PreAuth::State::failed);
    }
    {
        PreAuth p(PreAuthConfig{proto::protocol::ssl, false, false});
        p.receive(farland::test::client::connection_request(proto::protocol::rdp));
        const auto confirm = confirm_of(p.take_output());
        CHECK(std::get<proto::NegotiationFailureCode>(confirm.result) ==
              proto::NegotiationFailureCode::ssl_required_by_server);
        const auto evs = events(p);
        REQUIRE(evs.size() == 1);
        CHECK(std::holds_alternative<ev::Failed>(evs[0]));
    }
}

TEST_CASE("TLS-only pre-authentication is ready right after the handshake")
{
    PreAuth p(PreAuthConfig{proto::protocol::ssl, false, false});
    p.receive(farland::test::client::connection_request(proto::protocol::ssl));
    static_cast<void>(p.take_output());
    auto evs = events(p);
    REQUIRE(evs.size() == 1);
    CHECK(std::holds_alternative<ev::StartTls>(evs[0]));
    CHECK(p.state() == PreAuth::State::wait_tls);

    p.tls_established();
    evs = events(p);
    REQUIRE(evs.size() == 1);
    CHECK(std::holds_alternative<ev::Ready>(evs[0]));
    CHECK(p.negotiation().selected_protocol == proto::protocol::ssl);
    CHECK_FALSE(p.negotiation().identity.has_value());
    CHECK(p.negotiation().cookie == "tester");
}

TEST_CASE("HYBRID_EX: NLA, then the Early User Authorization Result, then the MCS stream")
{
    PreAuth p({}, scripted());
    p.receive(farland::test::client::connection_request(proto::protocol::ssl | proto::protocol::hybrid_ex));
    static_cast<void>(p.take_output());
    static_cast<void>(events(p));
    p.tls_established();
    CHECK(p.state() == PreAuth::State::nla);

    // "hello" completes the scripted NLA; the bytes after it belong to MCS.
    p.receive(farland::test::hex("68 65 6c"));
    CHECK(p.take_output().empty());
    p.receive(farland::test::hex("6c 6f 03 00"));
    CHECK(to_hex(p.take_output()) == "6f 6b 00 00 00 00");  // NLA reply, then AUTHZ_SUCCESS
    auto evs = events(p);
    REQUIRE(evs.size() == 2);
    const auto& auth = std::get<ev::Authenticated>(evs[0]);
    CHECK(auth.identity.user == "alice");
    REQUIRE(auth.credentials.has_value());
    CHECK(auth.credentials->password.view() == "pw");
    CHECK(std::holds_alternative<ev::Ready>(evs[1]));
    CHECK(p.negotiation().identity->user == "alice");

    p.receive(farland::test::hex("00 13"));  // more MCS bytes before the caller switched over
    CHECK(to_hex(p.take_remaining_input()) == "03 00 00 13");
}

TEST_CASE("HYBRID sends no Early User Authorization Result")
{
    PreAuth p({}, scripted());
    p.receive(farland::test::client::connection_request(proto::protocol::hybrid));
    static_cast<void>(p.take_output());
    p.tls_established();
    p.receive(farland::test::hex("68 65 6c 6c 6f"));
    CHECK(to_hex(p.take_output()) == "6f 6b");
    CHECK(p.state() == PreAuth::State::ready);
}

TEST_CASE("A failed NLA handshake flushes the acceptor's error and fails")
{
    PreAuth p({}, scripted());
    p.receive(farland::test::client::connection_request(proto::protocol::hybrid));
    static_cast<void>(p.take_output());
    static_cast<void>(events(p));
    p.tls_established();
    p.receive(farland::test::hex("62 61 64 21 21"));
    CHECK(to_hex(p.take_output()) == "ee");
    const auto evs = events(p);
    REQUIRE(evs.size() == 1);
    CHECK(std::holds_alternative<ev::Failed>(evs[0]));
    CHECK(p.state() == PreAuth::State::failed);
}

TEST_CASE("Protocol violations before TLS fail the connection")
{
    {
        PreAuth p({}, scripted());
        auto request = farland::test::client::connection_request(proto::protocol::hybrid);
        request.push_back(std::byte{0x16});  // a TLS record before the Connection Confirm
        p.receive(request);
        CHECK(p.state() == PreAuth::State::failed);
    }
    {
        PreAuth p({}, scripted());
        p.receive(farland::test::client::connection_request(proto::protocol::hybrid));
        p.receive(farland::test::hex("16 03 01"));  // plaintext after the confirm, not through TLS
        CHECK(p.state() == PreAuth::State::failed);
    }
    {
        PreAuth p({}, scripted());
        p.receive(farland::test::hex("00 08 00 00 00 00 00 00"));  // fast-path, not a TPKT
        CHECK(p.state() == PreAuth::State::failed);
    }
    {
        PreAuth p({}, scripted());
        p.receive(farland::test::hex("ff ff"));
        CHECK(p.state() == PreAuth::State::failed);
    }
}

TEST_CASE("The negotiation response advertises GFX only when configured")
{
    const auto flags_for = [](bool gfx) {
        PreAuth p(PreAuthConfig{proto::protocol::ssl, false, gfx});
        p.receive(farland::test::client::connection_request(proto::protocol::ssl));
        return std::get<proto::NegotiationResponse>(confirm_of(p.take_output()).result).flags;
    };
    CHECK(flags_for(false) == proto::neg_rsp_flags::extended_client_data_supported);
    CHECK(flags_for(true) ==
          (proto::neg_rsp_flags::extended_client_data_supported | proto::neg_rsp_flags::dynvc_gfx_protocol_supported));
}
