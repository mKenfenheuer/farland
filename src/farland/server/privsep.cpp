// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>
#include <farland/proto/x224.hpp>
#include <farland/server/privsep.hpp>

#include <algorithm>

namespace farland::server::privsep {

namespace {

constexpr std::string_view log_component = "server.privsep";
constexpr std::size_t length_prefix = 4;
constexpr std::size_t max_string = 1024;
constexpr std::size_t max_nt_response = 2048;

enum class Type : std::uint8_t {
    verify_ntlm_request = 1,
    verify_ntlm_response = 2,
    verify_password_request = 3,
    verify_password_response = 4,
    authenticated = 5,
    kerberos_request = 6,
    kerberos_response = 7,
};

/// Largest blob inside a Kerberos message. A ticket with a PAC is the
/// reason the whole message limit is what it is.
constexpr std::size_t max_kerberos_blob = 48000;

void write_blob(Writer& w, std::span<const std::byte> bytes)
{
    FARLAND_ASSERT(bytes.size() <= max_kerberos_blob);
    w.u32le(static_cast<std::uint32_t>(bytes.size()));
    w.bytes(bytes);
}

Result<std::vector<std::byte>> read_blob(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint32_t size, r.u32le());
    if (size > max_kerberos_blob) {
        return fail(Errc::limit_exceeded, "privsep blob too long", start);
    }
    FARLAND_TRY(const auto bytes, r.bytes(size));
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

void write_string(Writer& w, std::string_view text)
{
    FARLAND_ASSERT(text.size() <= max_string);
    w.u16le(static_cast<std::uint16_t>(text.size()));
    w.bytes(std::as_bytes(std::span(text)));
}

Result<std::string> read_string(Reader& r, std::size_t max = max_string)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint16_t size, r.u16le());
    if (size > max) {
        return fail(Errc::limit_exceeded, "privsep string too long", start);
    }
    FARLAND_TRY(const auto bytes, r.bytes(size));
    std::string text(size, '\0');
    std::ranges::transform(bytes, text.begin(), [](std::byte b) { return std::to_integer<char>(b); });
    return text;
}

template <std::size_t N>
Result<std::array<std::byte, N>> read_array(Reader& r)
{
    FARLAND_TRY(const auto bytes, r.bytes(N));
    std::array<std::byte, N> out{};
    std::ranges::copy(bytes, out.begin());
    return out;
}

bool is_nla(std::uint32_t protocol)
{
    return protocol == proto::protocol::hybrid || protocol == proto::protocol::hybrid_ex;
}

}  // namespace

std::vector<std::byte> encode(const Message& message)
{
    Writer w;
    w.u32le(0);  // length, patched below
    std::visit(
        [&w](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, VerifyNtlmRequest>) {
                FARLAND_ASSERT(m.nt_response.size() <= max_nt_response);
                w.u8(static_cast<std::uint8_t>(Type::verify_ntlm_request));
                write_string(w, m.user);
                write_string(w, m.domain);
                w.bytes(m.server_challenge);
                w.u16le(static_cast<std::uint16_t>(m.nt_response.size()));
                w.bytes(m.nt_response);
            } else if constexpr (std::is_same_v<T, VerifyNtlmResponse>) {
                w.u8(static_cast<std::uint8_t>(Type::verify_ntlm_response));
                w.u8(m.session_base_key ? 1 : 0);
                if (m.session_base_key) {
                    w.bytes(*m.session_base_key);
                }
            } else if constexpr (std::is_same_v<T, VerifyPasswordRequest>) {
                w.u8(static_cast<std::uint8_t>(Type::verify_password_request));
                write_string(w, m.user);
                write_string(w, m.domain);
                write_string(w, m.password.view());
            } else if constexpr (std::is_same_v<T, VerifyPasswordResponse>) {
                w.u8(static_cast<std::uint8_t>(Type::verify_password_response));
                w.u8(m.ok ? 1 : 0);
            } else if constexpr (std::is_same_v<T, KerberosRequest>) {
                w.u8(static_cast<std::uint8_t>(Type::kerberos_request));
                w.u8(static_cast<std::uint8_t>(m.op));
                write_blob(w, m.data);
                write_blob(w, m.mic);
                write_blob(w, m.mechanism_oid);
            } else if constexpr (std::is_same_v<T, KerberosResponse>) {
                w.u8(static_cast<std::uint8_t>(Type::kerberos_response));
                w.u8(m.ok ? 1 : 0);
                w.u8(m.complete ? 1 : 0);
                write_blob(w, m.data);
                write_string(w, m.user);
                write_string(w, m.domain);
            } else if constexpr (std::is_same_v<T, Authenticated>) {
                const auto& n = m.negotiation;
                w.u8(static_cast<std::uint8_t>(Type::authenticated));
                write_string(w, n.cookie);
                w.u32le(n.requested_protocols);
                w.u32le(n.selected_protocol);
                w.u8(n.identity ? 1 : 0);
                if (n.identity) {
                    write_string(w, n.identity->user);
                    write_string(w, n.identity->domain);
                }
            }
        },
        message);
    const std::size_t length = w.size() - length_prefix;
    FARLAND_ASSERT(length <= max_message_size);
    w.patch_u32le(0, static_cast<std::uint32_t>(length));
    return std::move(w).take();
}

Result<std::optional<std::size_t>> message_length(std::span<const std::byte> buffered)
{
    Reader r(buffered);
    if (r.size() < length_prefix) {
        return std::nullopt;
    }
    FARLAND_TRY(const std::uint32_t length, r.u32le());
    if (length == 0 || length > max_message_size) {
        return fail(Errc::limit_exceeded, "privsep message size out of range", 0);
    }
    const std::size_t total = length_prefix + length;
    if (buffered.size() < total) {
        return std::nullopt;
    }
    return total;
}

Result<Message> decode(std::span<const std::byte> frame)
{
    Reader outer(frame);
    FARLAND_TRY(const std::uint32_t length, outer.u32le());
    if (length != outer.remaining() || length > max_message_size) {
        return fail(Errc::invalid_length, "privsep length does not match the message", 0);
    }
    Reader r = outer;
    FARLAND_TRY(const std::uint8_t type, r.u8());
    Message message;
    switch (static_cast<Type>(type)) {
    case Type::verify_ntlm_request: {
        VerifyNtlmRequest m;
        FARLAND_TRY(m.user, read_string(r));
        FARLAND_TRY(m.domain, read_string(r));
        FARLAND_TRY(m.server_challenge, read_array<8>(r));
        const std::size_t size_offset = r.offset();
        FARLAND_TRY(const std::uint16_t size, r.u16le());
        if (size > max_nt_response) {
            return fail(Errc::limit_exceeded, "NT response too long", size_offset);
        }
        FARLAND_TRY(const auto response, r.bytes(size));
        m.nt_response.assign(response.begin(), response.end());
        message = std::move(m);
        break;
    }
    case Type::verify_ntlm_response: {
        VerifyNtlmResponse m;
        FARLAND_TRY(const std::uint8_t ok, r.u8());
        if (ok != 0) {
            FARLAND_TRY(m.session_base_key, read_array<16>(r));
        }
        message = m;
        break;
    }
    case Type::verify_password_request: {
        VerifyPasswordRequest m;
        FARLAND_TRY(m.user, read_string(r));
        FARLAND_TRY(m.domain, read_string(r));
        FARLAND_TRY(auto password, read_string(r));
        m.password = SecretString(std::move(password));
        message = std::move(m);
        break;
    }
    case Type::verify_password_response: {
        FARLAND_TRY(const std::uint8_t ok, r.u8());
        message = VerifyPasswordResponse{ok != 0};
        break;
    }
    case Type::kerberos_request: {
        KerberosRequest m;
        FARLAND_TRY(const std::uint8_t op, r.u8());
        if (op < static_cast<std::uint8_t>(KerberosOp::step) ||
            op > static_cast<std::uint8_t>(KerberosOp::verify_mic)) {
            return fail(Errc::invalid_value, "unknown Kerberos operation", 0);
        }
        m.op = static_cast<KerberosOp>(op);
        FARLAND_TRY(m.data, read_blob(r));
        FARLAND_TRY(m.mic, read_blob(r));
        FARLAND_TRY(m.mechanism_oid, read_blob(r));
        return Message{std::move(m)};
    }
    case Type::kerberos_response: {
        KerberosResponse m;
        FARLAND_TRY(const std::uint8_t ok, r.u8());
        FARLAND_TRY(const std::uint8_t complete, r.u8());
        m.ok = ok != 0;
        m.complete = complete != 0;
        FARLAND_TRY(m.data, read_blob(r));
        FARLAND_TRY(m.user, read_string(r));
        FARLAND_TRY(m.domain, read_string(r));
        return Message{std::move(m)};
    }
    case Type::authenticated: {
        Authenticated m;
        FARLAND_TRY(m.negotiation.cookie, read_string(r));
        FARLAND_TRY(m.negotiation.requested_protocols, r.u32le());
        FARLAND_TRY(m.negotiation.selected_protocol, r.u32le());
        FARLAND_TRY(const std::uint8_t has_identity, r.u8());
        if (has_identity != 0) {
            auth::Identity identity;
            FARLAND_TRY(identity.user, read_string(r));
            FARLAND_TRY(identity.domain, read_string(r));
            m.negotiation.identity = std::move(identity);
        }
        message = std::move(m);
        break;
    }
    default:
        return fail(Errc::invalid_value, "unknown privsep message type", length_prefix);
    }
    FARLAND_TRY_VOID(r.expect_end("privsep message"));
    return message;
}

// RemoteVerifier -------------------------------------------------------------------

std::optional<std::array<std::byte, 16>>
RemoteVerifier::session_base_key(std::string_view user, std::string_view domain,
                                 std::span<const std::byte, 8> server_challenge,
                                 std::span<const std::byte> nt_challenge_response)
{
    if (user.size() > max_string || domain.size() > max_string || nt_challenge_response.size() > max_nt_response) {
        return std::nullopt;
    }
    VerifyNtlmRequest request{std::string(user), std::string(domain), {}, {}};
    std::ranges::copy(server_challenge, request.server_challenge.begin());
    request.nt_response.assign(nt_challenge_response.begin(), nt_challenge_response.end());
    auto reply = call_(encode(request));
    if (!reply) {
        return std::nullopt;
    }
    auto message = decode(*reply);
    secure_zero(*reply);
    if (!message || !std::holds_alternative<VerifyNtlmResponse>(*message)) {
        return std::nullopt;
    }
    return std::get<VerifyNtlmResponse>(*message).session_base_key;
}

bool RemoteVerifier::verify_password(std::string_view user, std::string_view domain, std::string_view password)
{
    if (user.size() > max_string || domain.size() > max_string || password.size() > max_string) {
        return false;
    }
    auto request =
        encode(VerifyPasswordRequest{std::string(user), std::string(domain), SecretString(std::string(password))});
    const auto reply = call_(request);
    secure_zero(request);
    if (!reply) {
        return false;
    }
    const auto message = decode(*reply);
    return message && std::holds_alternative<VerifyPasswordResponse>(*message) &&
           std::get<VerifyPasswordResponse>(*message).ok;
}

// RemoteKerberos -------------------------------------------------------------------

Result<KerberosResponse> RemoteKerberos::ask(KerberosOp op, std::span<const std::byte> data,
                                             std::span<const std::byte> mic)
{
    if (data.size() > max_kerberos_blob || mic.size() > max_kerberos_blob) {
        return fail(Errc::limit_exceeded, "the Kerberos message is too large to hand to the monitor");
    }
    KerberosRequest request;
    request.op = op;
    request.data.assign(data.begin(), data.end());
    request.mic.assign(mic.begin(), mic.end());
    if (op == KerberosOp::step) {
        request.mechanism_oid = mechanism_oid_;
    }
    auto reply = call_(encode(request));
    if (!reply) {
        return fail(Errc::io, "the monitor did not answer a Kerberos request");
    }
    auto message = decode(*reply);
    secure_zero(*reply);
    if (!message || !std::holds_alternative<KerberosResponse>(*message)) {
        return fail(Errc::invalid_value, "the monitor's answer is not a Kerberos response");
    }
    auto response = std::get<KerberosResponse>(std::move(*message));
    if (!response.ok) {
        return fail(Errc::invalid_value, "the monitor refused a Kerberos operation");
    }
    return response;
}

Result<auth::Step> RemoteKerberos::step(std::span<const std::byte> input)
{
    FARLAND_TRY(auto response, ask(KerberosOp::step, input));
    auth::Step step;
    step.token = std::move(response.data);
    step.complete = response.complete;
    if (response.complete) {
        complete_ = true;
        identity_.user = std::move(response.user);
        identity_.domain = std::move(response.domain);
    }
    return step;
}

std::vector<std::byte> RemoteKerberos::wrap(std::span<const std::byte> plaintext)
{
    auto response = ask(KerberosOp::wrap, plaintext);
    // The interface has no way to report a failure here; an empty token is
    // one the peer cannot accept, which ends the handshake.
    return response ? std::move(response->data) : std::vector<std::byte>{};
}

Result<std::vector<std::byte>> RemoteKerberos::unwrap(std::span<const std::byte> wrapped)
{
    FARLAND_TRY(auto response, ask(KerberosOp::unwrap, wrapped));
    return std::move(response.data);
}

std::vector<std::byte> RemoteKerberos::get_mic(std::span<const std::byte> message)
{
    auto response = ask(KerberosOp::get_mic, message);
    return response ? std::move(response->data) : std::vector<std::byte>{};
}

Result<void> RemoteKerberos::verify_mic(std::span<const std::byte> message, std::span<const std::byte> mic)
{
    FARLAND_TRY_VOID(ask(KerberosOp::verify_mic, message, mic));
    return {};
}

// MonitorService -------------------------------------------------------------------

Result<KerberosResponse> MonitorService::handle_kerberos(const KerberosRequest& request)
{
    if (!kerberos_) {
        log::warn(log_component, "the network process asked for Kerberos, which this host does not accept");
        return KerberosResponse{};
    }
    if (request.op == KerberosOp::step && kerberos_context_ == nullptr) {
        kerberos_context_ = kerberos_(request.mechanism_oid);
        if (kerberos_context_ == nullptr) {
            log::warn(log_component, "the network process asked for a mechanism that is not Kerberos");
            return KerberosResponse{};
        }
    }
    if (kerberos_context_ == nullptr) {
        return fail(Errc::invalid_value, "a Kerberos operation before the context exists", 0);
    }

    KerberosResponse response;
    switch (request.op) {
    case KerberosOp::step: {
        auto step = kerberos_context_->step(request.data);
        if (!step) {
            return KerberosResponse{};
        }
        response.ok = true;
        response.data = std::move(step->token);
        response.complete = step->complete;
        if (step->complete) {
            const auto& identity = kerberos_context_->identity();
            response.user = identity.user;
            response.domain = identity.domain;
            // The same rule as NTLM: the network process may later claim
            // only an identity the monitor established itself.
            verified_.emplace(identity.user, identity.domain);
        }
        break;
    }
    case KerberosOp::wrap: {
        auto sealed = kerberos_context_->wrap(request.data);
        if (sealed.empty()) {
            return KerberosResponse{};
        }
        response.ok = true;
        response.data = std::move(sealed);
        break;
    }
    case KerberosOp::unwrap: {
        auto plaintext = kerberos_context_->unwrap(request.data);
        if (!plaintext) {
            return KerberosResponse{};
        }
        response.ok = true;
        response.data = std::move(*plaintext);
        break;
    }
    case KerberosOp::get_mic: {
        auto mic = kerberos_context_->get_mic(request.data);
        if (mic.empty()) {
            return KerberosResponse{};
        }
        response.ok = true;
        response.data = std::move(mic);
        break;
    }
    case KerberosOp::verify_mic:
        response.ok = kerberos_context_->verify_mic(request.data, request.mic).has_value();
        break;
    }
    return response;
}

Result<std::optional<std::vector<std::byte>>> MonitorService::handle(std::span<const std::byte> frame)
{
    FARLAND_TRY(auto message, decode(frame));

    if (auto* request = std::get_if<VerifyNtlmRequest>(&message)) {
        if (++attempts_ > max_attempts_) {
            log::warn(log_component, "too many verification attempts on one connection");
            return encode(VerifyNtlmResponse{});
        }
        auto key = verifier_->session_base_key(request->user, request->domain, request->server_challenge,
                                               request->nt_response);
        if (key) {
            verified_.emplace(request->user, request->domain);
        }
        auto reply = encode(VerifyNtlmResponse{key});
        if (key) {
            secure_zero(*key);
        }
        return reply;
    }
    if (auto* request = std::get_if<VerifyPasswordRequest>(&message)) {
        if (++attempts_ > max_attempts_) {
            log::warn(log_component, "too many verification attempts on one connection");
            return encode(VerifyPasswordResponse{false});
        }
        return encode(VerifyPasswordResponse{
            verifier_->verify_password(request->user, request->domain, request->password.view())});
    }
    if (auto* request = std::get_if<KerberosRequest>(&message)) {
        // A ticket is checked once, like a password: the step count is the
        // same budget the NTLM verifications spend from.
        if (request->op == KerberosOp::step && ++attempts_ > max_attempts_) {
            log::warn(log_component, "too many verification attempts on one connection");
            return encode(KerberosResponse{});
        }
        FARLAND_TRY(auto response, handle_kerberos(*request));
        auto reply = encode(response);
        secure_zero(response.data);
        return reply;
    }
    if (auto* notice = std::get_if<Authenticated>(&message)) {
        if (reported_) {
            return fail(Errc::invalid_value, "pre-authentication reported twice", 0);
        }
        reported_ = true;
        const auto& negotiation = notice->negotiation;
        if (is_nla(negotiation.selected_protocol)) {
            // The network process may only claim an identity the monitor verified.
            if (!negotiation.identity ||
                !verified_.contains({negotiation.identity->user, negotiation.identity->domain})) {
                return fail(Errc::invalid_value, "network process claims an identity the monitor did not verify", 0);
            }
        } else if (negotiation.identity) {
            return fail(Errc::invalid_value, "identity reported for a connection without NLA", 0);
        }
        authenticated_ = negotiation;
        return std::optional<std::vector<std::byte>>{};
    }
    return fail(Errc::invalid_value, "unexpected privsep message from the network process", 0);
}

}  // namespace farland::server::privsep
