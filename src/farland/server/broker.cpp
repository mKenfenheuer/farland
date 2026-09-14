// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/text.hpp>
#include <farland/base/writer.hpp>
#include <farland/proto/x224.hpp>
#include <farland/server/broker.hpp>

#include <algorithm>

namespace farland::server::broker {

namespace {

constexpr std::size_t length_prefix = 4;
constexpr std::size_t max_short_string = 256;  ///< peer, X.224 cookie, client name
constexpr std::size_t max_long_string = 1024;  ///< user, domain, detail

enum class Type : std::uint8_t {
    hello = 1,
    new_connection = 2,
    disconnect = 3,
    session_ended = 4,
    stats = 5,
};

void write_string(Writer& w, std::string_view text, std::size_t max)
{
    FARLAND_ASSERT(text.size() <= max);
    w.u16le(static_cast<std::uint16_t>(text.size()));
    w.bytes(std::as_bytes(std::span(text)));
}

Result<std::string> read_string(Reader& r, std::size_t max)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint16_t size, r.u16le());
    if (size > max) {
        return fail(Errc::limit_exceeded, "broker string too long", start);
    }
    FARLAND_TRY(const auto bytes, r.bytes(size));
    std::string text(size, '\0');
    std::ranges::transform(bytes, text.begin(), [](std::byte b) { return std::to_integer<char>(b); });
    return text;
}

void write_flag(Writer& w, bool value)
{
    w.u8(value ? 1 : 0);
}

/// A presence or boolean byte: 0 or 1, nothing else.
Result<bool> read_flag(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint8_t value, r.u8());
    if (value > 1) {
        return fail(Errc::invalid_value, "broker flag is not 0 or 1", start);
    }
    return value == 1;
}

template <std::size_t N>
Result<std::array<std::byte, N>> read_array(Reader& r)
{
    FARLAND_TRY(const auto bytes, r.bytes(N));
    std::array<std::byte, N> out{};
    std::ranges::copy(bytes, out.begin());
    return out;
}

Result<std::int32_t> read_i32(Reader& r)
{
    FARLAND_TRY(const std::uint32_t value, r.u32le());
    return static_cast<std::int32_t>(value);
}

bool is_nla(std::uint32_t protocol)
{
    return protocol == proto::protocol::hybrid || protocol == proto::protocol::hybrid_ex;
}

bool known_protocol(std::uint32_t protocol)
{
    return protocol == proto::protocol::rdp || protocol == proto::protocol::ssl || is_nla(protocol);
}

void write_negotiation(Writer& w, const Negotiation& n)
{
    write_string(w, n.cookie, max_short_string);
    w.u32le(n.requested_protocols);
    w.u32le(n.selected_protocol);
    write_flag(w, n.identity.has_value());
    if (n.identity) {
        write_string(w, n.identity->user, max_long_string);
        write_string(w, n.identity->domain, max_long_string);
    }
}

Result<Negotiation> read_negotiation(Reader& r)
{
    Negotiation n;
    FARLAND_TRY(n.cookie, read_string(r, max_short_string));
    FARLAND_TRY(n.requested_protocols, r.u32le());
    const std::size_t selected_offset = r.offset();
    FARLAND_TRY(n.selected_protocol, r.u32le());
    if (!known_protocol(n.selected_protocol)) {
        return fail(Errc::invalid_value, "unknown selected protocol", selected_offset);
    }
    const std::size_t identity_offset = r.offset();
    FARLAND_TRY(const bool has_identity, read_flag(r));
    if (has_identity != is_nla(n.selected_protocol)) {
        // As privsep: an identity exactly when NLA authenticated one.
        return fail(Errc::invalid_value, "identity does not match the selected protocol", identity_offset);
    }
    if (has_identity) {
        auth::Identity identity;
        FARLAND_TRY(identity.user, read_string(r, max_long_string));
        FARLAND_TRY(identity.domain, read_string(r, max_long_string));
        if (identity.user.empty()) {
            return fail(Errc::invalid_value, "empty NLA user name", identity_offset);
        }
        n.identity = std::move(identity);
    }
    return n;
}

void write_client(Writer& w, const ClientSummary& c)
{
    FARLAND_ASSERT(c.monitors.size() <= max_monitors);
    w.u16le(c.desktop_width);
    w.u16le(c.desktop_height);
    w.u32le(c.keyboard_layout);
    w.u32le(c.keyboard_type);
    w.u32le(c.keyboard_subtype);
    w.u32le(c.client_build);
    write_string(w, c.client_name, max_short_string);
    write_flag(w, c.desktop_scale_factor.has_value());
    if (c.desktop_scale_factor) {
        w.u32le(*c.desktop_scale_factor);
    }
    w.u8(static_cast<std::uint8_t>(c.monitors.size()));
    for (const auto& m : c.monitors) {
        w.u32le(static_cast<std::uint32_t>(m.left));
        w.u32le(static_cast<std::uint32_t>(m.top));
        w.u32le(static_cast<std::uint32_t>(m.right));
        w.u32le(static_cast<std::uint32_t>(m.bottom));
        write_flag(w, m.primary);
    }
}

Result<ClientSummary> read_client(Reader& r)
{
    ClientSummary c;
    const std::size_t size_offset = r.offset();
    FARLAND_TRY(c.desktop_width, r.u16le());
    FARLAND_TRY(c.desktop_height, r.u16le());
    if (c.desktop_width == 0 || c.desktop_height == 0) {
        return fail(Errc::invalid_value, "empty desktop size", size_offset);
    }
    FARLAND_TRY(c.keyboard_layout, r.u32le());
    FARLAND_TRY(c.keyboard_type, r.u32le());
    FARLAND_TRY(c.keyboard_subtype, r.u32le());
    FARLAND_TRY(c.client_build, r.u32le());
    FARLAND_TRY(c.client_name, read_string(r, max_short_string));
    FARLAND_TRY(const bool has_scale, read_flag(r));
    if (has_scale) {
        FARLAND_TRY(c.desktop_scale_factor, r.u32le());
    }
    const std::size_t count_offset = r.offset();
    FARLAND_TRY(const std::uint8_t count, r.u8());
    if (count > max_monitors) {
        return fail(Errc::limit_exceeded, "too many monitors", count_offset);
    }
    c.monitors.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i) {
        const std::size_t monitor_offset = r.offset();
        Monitor m;
        FARLAND_TRY(m.left, read_i32(r));
        FARLAND_TRY(m.top, read_i32(r));
        FARLAND_TRY(m.right, read_i32(r));
        FARLAND_TRY(m.bottom, read_i32(r));
        FARLAND_TRY(m.primary, read_flag(r));
        if (m.left > m.right || m.top > m.bottom) {
            return fail(Errc::invalid_value, "monitor rectangle is inverted", monitor_offset);
        }
        c.monitors.push_back(m);
    }
    return c;
}

void encode_body(Writer& w, const Hello& m)
{
    w.u8(static_cast<std::uint8_t>(Type::hello));
    w.u16le(m.version);
    w.bytes(m.token);
}

void encode_body(Writer& w, const NewConnection& m)
{
    FARLAND_ASSERT(m.connection_id != 0);
    FARLAND_ASSERT(m.pending_input.size() <= max_pending_input);
    w.u8(static_cast<std::uint8_t>(Type::new_connection));
    w.u64le(m.connection_id);
    write_negotiation(w, m.negotiation);
    write_string(w, m.peer, max_short_string);
    w.u32le(m.elapsed_ms);
    write_flag(w, m.client.has_value());
    if (m.client) {
        write_client(w, *m.client);
    }
    write_flag(w, m.auto_reconnect.has_value());
    if (m.auto_reconnect) {
        w.u32le(m.auto_reconnect->logon_id);
        w.bytes(m.auto_reconnect->security_verifier);
    }
    w.u32le(static_cast<std::uint32_t>(m.pending_input.size()));
    w.bytes(m.pending_input);
}

void encode_body(Writer& w, const Disconnect& m)
{
    FARLAND_ASSERT(m.connection_id != 0);
    w.u8(static_cast<std::uint8_t>(Type::disconnect));
    w.u64le(m.connection_id);
    w.u32le(m.error_info);
}

void encode_body(Writer& w, const SessionEnded& m)
{
    w.u8(static_cast<std::uint8_t>(Type::session_ended));
    w.u8(static_cast<std::uint8_t>(m.reason));
    write_string(w, m.detail, max_long_string);
}

void encode_body(Writer& w, const Stats& m)
{
    w.u8(static_cast<std::uint8_t>(Type::stats));
    w.u64le(m.connection_id);
    w.u32le(m.session_seconds);
    w.u32le(m.idle_seconds);
    w.u16le(m.desktop_width);
    w.u16le(m.desktop_height);
    w.u64le(m.frames_sent);
    w.u64le(m.bytes_sent);
    w.u64le(m.bytes_received);
    w.u32le(m.rtt_ms);
    w.u32le(m.bandwidth_kbps);
}

Result<Message> decode_new_connection(Reader& r)
{
    NewConnection m;
    const std::size_t id_offset = r.offset();
    FARLAND_TRY(m.connection_id, r.u64le());
    if (m.connection_id == 0) {
        return fail(Errc::invalid_value, "connection id 0", id_offset);
    }
    FARLAND_TRY(m.negotiation, read_negotiation(r));
    FARLAND_TRY(m.peer, read_string(r, max_short_string));
    FARLAND_TRY(m.elapsed_ms, r.u32le());
    FARLAND_TRY(const bool has_client, read_flag(r));
    if (has_client) {
        FARLAND_TRY(m.client, read_client(r));
    }
    FARLAND_TRY(const bool has_cookie, read_flag(r));
    if (has_cookie) {
        ReconnectCookie cookie;
        FARLAND_TRY(cookie.logon_id, r.u32le());
        FARLAND_TRY(cookie.security_verifier, read_array<16>(r));
        m.auto_reconnect = cookie;
    }
    const std::size_t input_offset = r.offset();
    FARLAND_TRY(const std::uint32_t input_size, r.u32le());
    if (input_size > max_pending_input) {
        return fail(Errc::limit_exceeded, "too much pending input", input_offset);
    }
    FARLAND_TRY(const auto input, r.bytes(input_size));
    m.pending_input.assign(input.begin(), input.end());
    return m;
}

Result<Message> decode_body(Reader& r)
{
    FARLAND_TRY(const std::uint8_t type, r.u8());
    switch (static_cast<Type>(type)) {
    case Type::hello: {
        Hello m;
        FARLAND_TRY(m.version, r.u16le());
        FARLAND_TRY(m.token, read_array<std::tuple_size_v<Token>>(r));
        return m;
    }
    case Type::new_connection:
        return decode_new_connection(r);
    case Type::disconnect: {
        Disconnect m;
        const std::size_t id_offset = r.offset();
        FARLAND_TRY(m.connection_id, r.u64le());
        if (m.connection_id == 0) {
            return fail(Errc::invalid_value, "connection id 0", id_offset);
        }
        FARLAND_TRY(m.error_info, r.u32le());
        return m;
    }
    case Type::session_ended: {
        SessionEnded m;
        const std::size_t reason_offset = r.offset();
        FARLAND_TRY(const std::uint8_t reason, r.u8());
        if (reason < static_cast<std::uint8_t>(EndReason::logout) ||
            reason > static_cast<std::uint8_t>(EndReason::error)) {
            return fail(Errc::invalid_value, "unknown session end reason", reason_offset);
        }
        m.reason = static_cast<EndReason>(reason);
        FARLAND_TRY(m.detail, read_string(r, max_long_string));
        return m;
    }
    case Type::stats: {
        Stats m;
        FARLAND_TRY(m.connection_id, r.u64le());
        FARLAND_TRY(m.session_seconds, r.u32le());
        FARLAND_TRY(m.idle_seconds, r.u32le());
        FARLAND_TRY(m.desktop_width, r.u16le());
        FARLAND_TRY(m.desktop_height, r.u16le());
        FARLAND_TRY(m.frames_sent, r.u64le());
        FARLAND_TRY(m.bytes_sent, r.u64le());
        FARLAND_TRY(m.bytes_received, r.u64le());
        FARLAND_TRY(m.rtt_ms, r.u32le());
        FARLAND_TRY(m.bandwidth_kbps, r.u32le());
        return m;
    }
    default:
        return fail(Errc::invalid_value, "unknown broker message type", r.offset() - 1);
    }
}

/// Compares in time independent of where the tokens differ.
bool tokens_equal(const Token& a, const Token& b) noexcept
{
    unsigned difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= std::to_integer<unsigned>(a.at(i) ^ b.at(i));
    }
    return difference == 0;
}

}  // namespace

bool may_send(Sender sender, const Message& message) noexcept
{
    if (std::holds_alternative<Disconnect>(message)) {
        return true;
    }
    if (sender == Sender::daemon) {
        return std::holds_alternative<NewConnection>(message);
    }
    return !std::holds_alternative<NewConnection>(message);
}

bool carries_fd(const Message& message) noexcept
{
    return std::holds_alternative<NewConnection>(message);
}

std::vector<std::byte> encode(const Message& message)
{
    Writer w;
    w.u32le(0);  // length, patched below
    std::visit([&w](const auto& m) { encode_body(w, m); }, message);
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
        return fail(Errc::limit_exceeded, "broker message size out of range", 0);
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
    if (length != outer.remaining() || length == 0 || length > max_message_size) {
        return fail(Errc::invalid_length, "broker length does not match the message", 0);
    }
    Reader r = outer;
    FARLAND_TRY(auto message, decode_body(r));
    FARLAND_TRY_VOID(r.expect_end("broker message"));
    return message;
}

Result<Message> decode_from(Sender sender, std::span<const std::byte> frame, bool with_fd)
{
    FARLAND_TRY(auto message, decode(frame));
    if (!may_send(sender, message)) {
        return fail(Errc::invalid_value, "broker message from the wrong side", length_prefix);
    }
    if (carries_fd(message) != with_fd) {
        return fail(Errc::invalid_value, "broker descriptor missing or unexpected", length_prefix);
    }
    return message;
}

AgentLink::~AgentLink()
{
    secure_zero(expected_);
}

Result<Message> AgentLink::receive(std::span<const std::byte> frame, bool with_fd)
{
    if (ended_) {
        return fail(Errc::invalid_value, "agent sent a message after SessionEnded", 0);
    }
    FARLAND_TRY(auto message, decode_from(Sender::agent, frame, with_fd));
    if (auto* hello = std::get_if<Hello>(&message)) {
        const bool valid = !greeted_ && hello->version == protocol_version && tokens_equal(hello->token, expected_);
        secure_zero(hello->token);
        if (!valid) {
            return fail(Errc::invalid_value, "agent hello repeated, of another version or with a wrong token", 0);
        }
        greeted_ = true;
        return message;
    }
    if (!greeted_) {
        return fail(Errc::invalid_value, "agent did not start with a hello", 0);
    }
    ended_ = std::holds_alternative<SessionEnded>(message);
    return message;
}

}  // namespace farland::server::broker
