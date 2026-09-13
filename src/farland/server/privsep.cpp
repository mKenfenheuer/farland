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
};

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

// MonitorService -------------------------------------------------------------------

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
