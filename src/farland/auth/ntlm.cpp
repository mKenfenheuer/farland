// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/legacy_crypto.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/base/assert.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <ratio>

namespace farland::auth::ntlm {

namespace {

using namespace std::string_view_literals;
using legacy::hmac_md5;
using legacy::md5;
using legacy::Rc4;

// NTLMSSP message header ([MS-NLMP] 2.2.1): Signature, then MessageType.
constexpr std::array<std::byte, 8> ntlmssp_signature{
    std::byte{'N'}, std::byte{'T'}, std::byte{'L'}, std::byte{'M'},
    std::byte{'S'}, std::byte{'S'}, std::byte{'P'}, std::byte{0},
};
constexpr std::uint32_t message_negotiate = 1;
constexpr std::uint32_t message_challenge = 2;
constexpr std::uint32_t message_authenticate = 3;

constexpr std::size_t max_name_bytes = max_name_chars * 2;
constexpr std::size_t signature_size = 16;  // NTLMSSP_MESSAGE_SIGNATURE, [MS-NLMP] 2.2.2.9.1
constexpr std::size_t mic_size = 16;

// 1.3.6.1.4.1.311.2.2.10
constexpr std::array<std::byte, 10> ntlm_oid{
    std::byte{0x2b}, std::byte{0x06}, std::byte{0x01}, std::byte{0x04}, std::byte{0x01},
    std::byte{0x82}, std::byte{0x37}, std::byte{0x02}, std::byte{0x02}, std::byte{0x0a},
};

/// What farland offers (acceptor) and asks for (initiator).
constexpr std::uint32_t supported_flags = flags::negotiate_unicode | flags::request_target | flags::negotiate_sign |
                                          flags::negotiate_seal | flags::negotiate_ntlm | flags::negotiate_always_sign |
                                          flags::negotiate_extended_sessionsecurity | flags::negotiate_target_info |
                                          flags::negotiate_version | flags::negotiate_128 | flags::negotiate_key_exch |
                                          flags::negotiate_56;
/// Without these there is no NTLMv2 session security to speak of.
constexpr std::uint32_t required_flags =
    flags::negotiate_unicode | flags::negotiate_ntlm | flags::negotiate_extended_sessionsecurity;

template <class Range>
void wipe(Range& range) noexcept
{
    secure_zero(std::as_writable_bytes(std::span(range)));
}

bool equal_ct(std::span<const std::byte> a, std::span<const std::byte> b) noexcept
{
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

template <std::size_t N>
std::array<std::byte, N> random_bytes()
{
    std::array<unsigned char, N> raw{};
    FARLAND_ASSERT(RAND_bytes(raw.data(), static_cast<int>(raw.size())) == 1);
    const auto out = std::bit_cast<std::array<std::byte, N>>(raw);
    wipe(raw);
    return out;
}

/// Current time as a FILETIME (100 ns ticks since 1601-01-01), for MsvAvTimestamp.
std::uint64_t filetime_now()
{
    using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    constexpr std::uint64_t unix_epoch = 116'444'736'000'000'000ULL;
    const auto since_unix = std::chrono::duration_cast<Ticks>(std::chrono::system_clock::now().time_since_epoch());
    return unix_epoch + static_cast<std::uint64_t>(since_unix.count());
}

std::array<std::byte, 4> le32(std::uint32_t value) noexcept
{
    std::array<std::byte, 4> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8U * i)));
    }
    return out;
}

std::vector<std::byte> le64(std::uint64_t value)
{
    Writer w(8);
    w.u64le(value);
    return std::move(w).take();
}

std::span<const std::byte> ascii_bytes(std::string_view text) noexcept
{
    return std::as_bytes(std::span(text));
}

AvPair av_pair(AvId id, std::vector<std::byte> value)
{
    return AvPair{static_cast<std::uint16_t>(id), std::move(value)};
}

// ---------------------------------------------------------------------------
// Payload fields: Len (2), MaxLen (2), BufferOffset (4) ([MS-NLMP] 2.2.1.1).

struct Field {
    std::uint16_t length = 0;
    std::uint32_t offset = 0;
};

Result<Field> read_field(Reader& r)
{
    FARLAND_TRY(const auto length, r.u16le());
    FARLAND_TRY_VOID(r.skip(2));  // MaxLen: ignored on receipt.
    FARLAND_TRY(const auto offset, r.u32le());
    return Field{length, offset};
}

Result<void> read_header(Reader& r, std::uint32_t type)
{
    FARLAND_TRY(const auto signature, r.bytes(ntlmssp_signature.size()));
    if (!std::ranges::equal(signature, ntlmssp_signature)) {
        return fail(Errc::invalid_value, "not an NTLMSSP message", 0);
    }
    const std::size_t at = r.offset();
    FARLAND_TRY(const auto message_type, r.u32le());
    if (message_type != type) {
        return fail(Errc::invalid_value, "unexpected NTLM message type", at);
    }
    return {};
}

/// Start of the payload: the smallest offset of a non-empty field, or the
/// message size. Optional header fields (Version, MIC) must end before it.
std::size_t payload_begin(std::span<const Field> fields, std::size_t message_size) noexcept
{
    std::size_t begin = message_size;
    for (const Field& field : fields) {
        if (field.length != 0) {
            begin = std::min<std::size_t>(begin, field.offset);
        }
    }
    return begin;
}

/// Every non-empty field must lie in [header_end, message_size) without
/// overlapping another one.
Result<void> check_fields(std::span<const Field> fields, std::size_t header_end, std::size_t message_size)
{
    std::array<Field, 6> used{};
    FARLAND_ASSERT(fields.size() <= used.size());
    std::size_t count = 0;
    for (const Field& field : fields) {
        if (field.length == 0) {
            continue;
        }
        if (field.offset < header_end) {
            return fail(Errc::invalid_length, "NTLM payload field overlaps the header", field.offset);
        }
        if (field.offset > message_size || field.length > message_size - field.offset) {
            return fail(Errc::invalid_length, "NTLM payload field outside the message", field.offset);
        }
        used[count++] = field;
    }
    const auto sorted = std::span(used).first(count);
    std::ranges::sort(sorted, {}, &Field::offset);
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (std::size_t{sorted[i - 1].offset} + sorted[i - 1].length > sorted[i].offset) {
            return fail(Errc::invalid_length, "NTLM payload fields overlap", sorted[i].offset);
        }
    }
    return {};
}

/// The bytes of a field already checked by check_fields().
Result<std::span<const std::byte>> payload(std::span<const std::byte> message, Field field)
{
    if (field.length == 0) {
        return std::span<const std::byte>{};
    }
    Reader r(message);
    FARLAND_TRY_VOID(r.skip(field.offset));
    return r.bytes(field.length);
}

std::size_t field_placeholder(Writer& w)
{
    const std::size_t position = w.size();
    w.zeros(8);
    return position;
}

void append_payload(Writer& w, std::size_t field, std::span<const std::byte> data)
{
    FARLAND_ASSERT(data.size() <= 0xffffU && w.size() <= 0xffffffffU);
    const auto length = static_cast<std::uint16_t>(data.size());
    w.patch_u16le(field, length);
    w.patch_u16le(field + 2, length);
    w.patch_u32le(field + 4, static_cast<std::uint32_t>(w.size()));
    w.bytes(data);
}

// VERSION ([MS-NLMP] 2.2.2.10): major, minor, build, 3 reserved, revision.
Result<Version> read_version(Reader& r)
{
    Version version;
    FARLAND_TRY(version.major, r.u8());
    FARLAND_TRY(version.minor, r.u8());
    FARLAND_TRY(version.build, r.u16le());
    FARLAND_TRY_VOID(r.skip(3));
    FARLAND_TRY(version.revision, r.u8());
    return version;
}

void write_version(Writer& w, const Version& version)
{
    w.u8(version.major);
    w.u8(version.minor);
    w.u16le(version.build);
    w.zeros(3);
    w.u8(version.revision);
}

/// A UTF-16LE name from an AUTHENTICATE_MESSAGE or AV pair.
Result<std::string> decode_name(std::span<const std::byte> bytes, std::size_t at)
{
    if (bytes.size() % 2 != 0) {
        return fail(Errc::invalid_length, "odd-length UTF-16 name", at);
    }
    if (bytes.size() > max_name_bytes) {
        return fail(Errc::limit_exceeded, "NTLM name too long", at);
    }
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        if (bytes[i] == std::byte{0} && bytes[i + 1] == std::byte{0}) {
            return fail(Errc::invalid_value, "NUL in NTLM name", at + i);
        }
    }
    return utf16le_to_utf8(bytes);
}

std::optional<std::size_t> av_pair_length(std::uint16_t id) noexcept
{
    switch (static_cast<AvId>(id)) {
    case AvId::flags:
        return 4;
    case AvId::timestamp:
        return 8;
    case AvId::channel_bindings:
        return 16;
    default:
        return std::nullopt;
    }
}

std::uint32_t read_u32(std::span<const std::byte> value) noexcept
{
    Reader r(value);
    return r.u32le().value_or(0);
}

std::uint64_t read_u64(std::span<const std::byte> value) noexcept
{
    Reader r(value);
    return r.u64le().value_or(0);
}

// ---------------------------------------------------------------------------
// NTLMv2 helpers

/// Simple uppercase mapping of one UTF-16 code unit, for NTOWFv2's
/// Uppercase(User) ([MS-NLMP] 3.3.2). Covers ASCII, Latin-1, Latin
/// Extended-A, Greek and Cyrillic; other code units are unchanged.
std::uint16_t upcase(std::uint16_t c) noexcept
{
    const auto minus = [c](unsigned delta) { return static_cast<std::uint16_t>(c - delta); };
    if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) {
        return minus(0x20);
    }
    if (c == 0xff) {
        return 0x178;
    }
    const bool odd = (c & 1U) != 0;
    if (((c >= 0x100 && c <= 0x12f) || (c >= 0x132 && c <= 0x137) || (c >= 0x14a && c <= 0x177)) && odd) {
        return minus(1);
    }
    if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) && !odd) {
        return minus(1);
    }
    if (c == 0x3c2) {  // final sigma
        return 0x3a3;
    }
    if ((c >= 0x3b1 && c <= 0x3cb) || (c >= 0x430 && c <= 0x44f)) {
        return minus(0x20);
    }
    if (c >= 0x450 && c <= 0x45f) {
        return minus(0x50);
    }
    return c;
}

std::vector<std::byte> upper_utf16le(std::string_view text)
{
    auto units = utf8_to_utf16le(text);
    for (std::size_t i = 0; i + 1 < units.size(); i += 2) {
        const auto unit = static_cast<std::uint16_t>(std::to_integer<unsigned>(units[i]) |
                                                     (std::to_integer<unsigned>(units[i + 1]) << 8U));
        const std::uint16_t upper = upcase(unit);
        units[i] = static_cast<std::byte>(upper & 0xffU);
        units[i + 1] = static_cast<std::byte>(upper >> 8U);
    }
    return units;
}

/// The parts of an NTLMv2_RESPONSE ([MS-NLMP] 2.2.2.8, 2.2.2.7) the acceptor looks at.
struct Ntlmv2Blob {
    std::uint64_t timestamp = 0;
    std::vector<AvPair> av_pairs;
};

Result<Ntlmv2Blob> parse_ntlmv2_response(std::span<const std::byte> response)
{
    Reader r(response);
    FARLAND_TRY_VOID(r.skip(16));  // NTProofStr
    const std::size_t at = r.offset();
    FARLAND_TRY(const auto resp_type, r.u8());
    FARLAND_TRY(const auto hi_resp_type, r.u8());
    if (resp_type != 1 || hi_resp_type != 1) {
        return fail(Errc::unsupported, "unknown NTLMv2 response version", at);
    }
    FARLAND_TRY_VOID(r.skip(6));  // Reserved1, Reserved2
    Ntlmv2Blob blob;
    FARLAND_TRY(blob.timestamp, r.u64le());
    FARLAND_TRY_VOID(r.skip(8 + 4));  // ChallengeFromClient, Reserved3
    FARLAND_TRY(blob.av_pairs, decode_av_pairs(r));
    return blob;  // Anything after MsvAvEOL is padding.
}

using Signature = std::array<std::byte, signature_size>;

}  // namespace

// ---------------------------------------------------------------------------
// AV pairs ([MS-NLMP] 2.2.2.1)

Result<std::vector<AvPair>> decode_av_pairs(Reader& r)
{
    std::vector<AvPair> pairs;
    while (true) {
        const std::size_t at = r.offset();
        FARLAND_TRY(const auto id, r.u16le());
        FARLAND_TRY(const auto length, r.u16le());
        FARLAND_TRY(const auto value, r.bytes(length));
        if (id == static_cast<std::uint16_t>(AvId::eol)) {
            if (length != 0) {
                return fail(Errc::invalid_length, "MsvAvEOL has a value", at);
            }
            return pairs;
        }
        if (pairs.size() == max_av_pairs) {
            return fail(Errc::limit_exceeded, "too many AV pairs", at);
        }
        if (std::ranges::any_of(pairs, [id](const AvPair& pair) { return pair.id == id; })) {
            return fail(Errc::invalid_value, "duplicate AV pair", at);
        }
        if (const auto expected = av_pair_length(id); expected.has_value() && *expected != length) {
            return fail(Errc::invalid_length, "AV pair has the wrong length", at);
        }
        pairs.push_back(AvPair{id, {value.begin(), value.end()}});
    }
}

void encode_av_pairs(Writer& w, std::span<const AvPair> pairs)
{
    for (const AvPair& pair : pairs) {
        FARLAND_ASSERT(pair.id != static_cast<std::uint16_t>(AvId::eol) && pair.value.size() <= 0xffffU);
        w.u16le(pair.id);
        w.u16le(static_cast<std::uint16_t>(pair.value.size()));
        w.bytes(pair.value);
    }
    w.u16le(static_cast<std::uint16_t>(AvId::eol));
    w.u16le(0);
}

std::optional<std::span<const std::byte>> find_av_pair(std::span<const AvPair> pairs, AvId id) noexcept
{
    const auto it = std::ranges::find(pairs, static_cast<std::uint16_t>(id), &AvPair::id);
    if (it == pairs.end()) {
        return std::nullopt;
    }
    return std::span<const std::byte>(it->value);
}

// ---------------------------------------------------------------------------
// NEGOTIATE_MESSAGE ([MS-NLMP] 2.2.1.1)

std::vector<std::byte> encode(const NegotiateMessage& message)
{
    Writer w(64);
    w.bytes(ntlmssp_signature);
    w.u32le(message_negotiate);
    w.u32le(message.flags);
    const std::size_t domain = field_placeholder(w);
    const std::size_t workstation = field_placeholder(w);
    if (message.version.has_value()) {
        write_version(w, *message.version);
    }
    append_payload(w, domain, message.domain);
    append_payload(w, workstation, message.workstation);
    return std::move(w).take();
}

Result<NegotiateMessage> decode_negotiate(std::span<const std::byte> message)
{
    Reader r(message);
    FARLAND_TRY_VOID(read_header(r, message_negotiate));
    NegotiateMessage out;
    FARLAND_TRY(out.flags, r.u32le());
    FARLAND_TRY(const Field domain, read_field(r));
    FARLAND_TRY(const Field workstation, read_field(r));
    const std::array fields{domain, workstation};

    std::size_t header_end = r.position();
    if ((out.flags & flags::negotiate_version) != 0 && payload_begin(fields, message.size()) >= header_end + 8) {
        FARLAND_TRY(out.version, read_version(r));
        header_end = r.position();
    }
    FARLAND_TRY_VOID(check_fields(fields, header_end, message.size()));
    FARLAND_TRY(const auto domain_bytes, payload(message, domain));
    FARLAND_TRY(const auto workstation_bytes, payload(message, workstation));
    if (domain_bytes.size() > max_name_bytes || workstation_bytes.size() > max_name_bytes) {
        return fail(Errc::limit_exceeded, "NTLM name too long", header_end);
    }
    out.domain.assign(domain_bytes.begin(), domain_bytes.end());
    out.workstation.assign(workstation_bytes.begin(), workstation_bytes.end());
    return out;
}

// ---------------------------------------------------------------------------
// CHALLENGE_MESSAGE ([MS-NLMP] 2.2.1.2)

std::vector<std::byte> encode(const ChallengeMessage& message)
{
    Writer w(128);
    w.bytes(ntlmssp_signature);
    w.u32le(message_challenge);
    const std::size_t target_name = field_placeholder(w);
    w.u32le(message.flags);
    w.bytes(message.server_challenge);
    w.zeros(8);  // Reserved
    const std::size_t target_info = field_placeholder(w);
    if (message.version.has_value()) {
        write_version(w, *message.version);
    }
    append_payload(w, target_name, message.target_name);
    if (!message.target_info.empty() || (message.flags & flags::negotiate_target_info) != 0) {
        Writer pairs;
        encode_av_pairs(pairs, message.target_info);
        append_payload(w, target_info, pairs.view());
    }
    return std::move(w).take();
}

Result<ChallengeMessage> decode_challenge(std::span<const std::byte> message)
{
    Reader r(message);
    FARLAND_TRY_VOID(read_header(r, message_challenge));
    ChallengeMessage out;
    FARLAND_TRY(const Field target_name, read_field(r));
    FARLAND_TRY(out.flags, r.u32le());
    FARLAND_TRY(const auto challenge, r.bytes(out.server_challenge.size()));
    std::ranges::copy(challenge, out.server_challenge.begin());
    FARLAND_TRY_VOID(r.skip(8));  // Reserved
    FARLAND_TRY(const Field target_info, read_field(r));
    const std::array fields{target_name, target_info};

    std::size_t header_end = r.position();
    if ((out.flags & flags::negotiate_version) != 0 && payload_begin(fields, message.size()) >= header_end + 8) {
        FARLAND_TRY(out.version, read_version(r));
        header_end = r.position();
    }
    FARLAND_TRY_VOID(check_fields(fields, header_end, message.size()));
    FARLAND_TRY(const auto name, payload(message, target_name));
    out.target_name.assign(name.begin(), name.end());
    if (target_info.length != 0) {
        FARLAND_TRY(const auto info, payload(message, target_info));
        Reader pairs(info, target_info.offset);
        FARLAND_TRY(out.target_info, decode_av_pairs(pairs));
    }
    return out;
}

// ---------------------------------------------------------------------------
// AUTHENTICATE_MESSAGE ([MS-NLMP] 2.2.1.3)

std::vector<std::byte> encode(const AuthenticateMessage& message)
{
    const auto domain = utf8_to_utf16le(message.domain);
    const auto user = utf8_to_utf16le(message.user);
    const auto workstation = utf8_to_utf16le(message.workstation);

    Writer w(512);
    w.bytes(ntlmssp_signature);
    w.u32le(message_authenticate);
    const std::size_t lm = field_placeholder(w);
    const std::size_t nt = field_placeholder(w);
    const std::size_t domain_field = field_placeholder(w);
    const std::size_t user_field = field_placeholder(w);
    const std::size_t workstation_field = field_placeholder(w);
    const std::size_t session_key = field_placeholder(w);
    w.u32le(message.flags);
    const bool version_flag = (message.flags & flags::negotiate_version) != 0;
    if (message.version.has_value() || (message.mic.has_value() && version_flag)) {
        write_version(w, message.version.value_or(Version{}));
    }
    if (message.mic.has_value()) {
        FARLAND_ASSERT(w.size() == authenticate_mic_offset(message.flags));
        w.bytes(*message.mic);
    }
    append_payload(w, domain_field, domain);
    append_payload(w, user_field, user);
    append_payload(w, workstation_field, workstation);
    append_payload(w, lm, message.lm_challenge_response);
    append_payload(w, nt, message.nt_challenge_response);
    append_payload(w, session_key, message.encrypted_random_session_key);
    return std::move(w).take();
}

Result<AuthenticateMessage> decode_authenticate(std::span<const std::byte> message)
{
    Reader r(message);
    FARLAND_TRY_VOID(read_header(r, message_authenticate));
    FARLAND_TRY(const Field lm, read_field(r));
    FARLAND_TRY(const Field nt, read_field(r));
    FARLAND_TRY(const Field domain, read_field(r));
    FARLAND_TRY(const Field user, read_field(r));
    FARLAND_TRY(const Field workstation, read_field(r));
    FARLAND_TRY(const Field session_key, read_field(r));
    AuthenticateMessage out;
    const std::size_t flags_at = r.offset();
    FARLAND_TRY(out.flags, r.u32le());
    if ((out.flags & flags::negotiate_unicode) == 0) {
        return fail(Errc::unsupported, "OEM AUTHENTICATE_MESSAGE", flags_at);
    }
    const std::array fields{lm, nt, domain, user, workstation, session_key};
    const std::size_t begin = payload_begin(fields, message.size());

    // Version and MIC are only there if the payload leaves room for them.
    std::size_t header_end = r.position();
    if ((out.flags & flags::negotiate_version) != 0 && begin >= header_end + 8) {
        FARLAND_TRY(out.version, read_version(r));
        header_end = r.position();
    }
    if (header_end == authenticate_mic_offset(out.flags) && begin >= header_end + mic_size) {
        FARLAND_TRY(const auto mic, r.bytes(mic_size));
        out.mic.emplace();
        std::ranges::copy(mic, out.mic->begin());
        header_end = r.position();
    }
    FARLAND_TRY_VOID(check_fields(fields, header_end, message.size()));

    FARLAND_TRY(const auto lm_bytes, payload(message, lm));
    FARLAND_TRY(const auto nt_bytes, payload(message, nt));
    FARLAND_TRY(const auto key_bytes, payload(message, session_key));
    FARLAND_TRY(const auto domain_bytes, payload(message, domain));
    FARLAND_TRY(const auto user_bytes, payload(message, user));
    FARLAND_TRY(const auto workstation_bytes, payload(message, workstation));
    out.lm_challenge_response.assign(lm_bytes.begin(), lm_bytes.end());
    out.nt_challenge_response.assign(nt_bytes.begin(), nt_bytes.end());
    out.encrypted_random_session_key.assign(key_bytes.begin(), key_bytes.end());
    FARLAND_TRY(out.domain, decode_name(domain_bytes, domain.offset));
    FARLAND_TRY(out.user, decode_name(user_bytes, user.offset));
    FARLAND_TRY(out.workstation, decode_name(workstation_bytes, workstation.offset));
    return out;
}

// ---------------------------------------------------------------------------
// Key derivation ([MS-NLMP] 3.3.1, 3.3.2, 3.4.5)

NtHash nt_hash(std::string_view password)
{
    auto unicode = utf8_to_utf16le(password);
    const NtHash hash = legacy::md4(unicode);
    wipe(unicode);
    return hash;
}

Key ntowf_v2(const NtHash& hash, std::string_view user, std::string_view domain)
{
    const auto upper_user = upper_utf16le(user);
    const auto unicode_domain = utf8_to_utf16le(domain);
    return hmac_md5(hash, {upper_user, unicode_domain});
}

std::vector<std::byte> ntlmv2_temp(std::uint64_t timestamp, std::span<const std::byte, 8> client_challenge,
                                   std::span<const AvPair> av_pairs)
{
    Writer w(64);
    w.u8(1);     // RespType
    w.u8(1);     // HiRespType
    w.zeros(6);  // Reserved1, Reserved2
    w.u64le(timestamp);
    w.bytes(client_challenge);
    w.zeros(4);  // Reserved3
    encode_av_pairs(w, av_pairs);
    w.zeros(4);  // Z(4) after the AV pairs (3.3.2)
    return std::move(w).take();
}

Key nt_proof_str(const Key& response_key, std::span<const std::byte, 8> server_challenge,
                 std::span<const std::byte> temp)
{
    return hmac_md5(response_key, {server_challenge, temp});
}

Key session_base_key(const Key& response_key, std::span<const std::byte, 16> nt_proof)
{
    return hmac_md5(response_key, {nt_proof});
}

std::array<std::byte, 24> lmv2_response(const Key& response_key, std::span<const std::byte, 8> server_challenge,
                                        std::span<const std::byte, 8> client_challenge)
{
    auto proof = hmac_md5(response_key, {server_challenge, client_challenge});
    std::array<std::byte, 24> out{};
    std::ranges::copy(proof, out.begin());
    std::ranges::copy(client_challenge, std::span(out).subspan(16).begin());
    wipe(proof);
    return out;
}

Key signing_key(const Key& exported_session_key, Direction direction)
{
    // SIGNKEY: MD5(ExportedSessionKey + magic constant, NUL included).
    const auto magic = direction == Direction::client_to_server
                           ? "session key to client-to-server signing key magic constant\0"sv
                           : "session key to server-to-client signing key magic constant\0"sv;
    return md5({exported_session_key, ascii_bytes(magic)});
}

Key sealing_key(const Key& exported_session_key, std::uint32_t negotiate_flags, Direction direction)
{
    // SEALKEY with extended session security: the key is weakened first
    // unless 128-bit was negotiated.
    std::size_t length = 5;
    if ((negotiate_flags & flags::negotiate_128) != 0) {
        length = 16;
    } else if ((negotiate_flags & flags::negotiate_56) != 0) {
        length = 7;
    }
    const auto magic = direction == Direction::client_to_server
                           ? "session key to client-to-server sealing key magic constant\0"sv
                           : "session key to server-to-client sealing key magic constant\0"sv;
    return md5({std::span<const std::byte>(exported_session_key).first(length), ascii_bytes(magic)});
}

std::array<std::byte, 16> channel_bindings_hash(std::span<const std::byte> certificate_der)
{
    std::array<unsigned char, 32> digest{};
    unsigned int digest_size = 0;
    FARLAND_ASSERT(EVP_Digest(certificate_der.data(), certificate_der.size(), digest.data(), &digest_size, EVP_sha256(),
                              nullptr) == 1 &&
                   digest_size == digest.size());
    constexpr auto prefix = "tls-server-end-point:"sv;

    // gss_channel_bindings_struct as SSPI hashes it: initiator_addrtype,
    // initiator_address.length, acceptor_addrtype, acceptor_address.length,
    // application_data.length (all 32-bit little-endian), application_data.
    Writer header(20);
    header.zeros(16);
    header.u32le(static_cast<std::uint32_t>(prefix.size() + digest.size()));
    return md5({header.view(), ascii_bytes(prefix), std::as_bytes(std::span(digest))});
}

std::span<const std::byte> mechanism_oid() noexcept
{
    return ntlm_oid;
}

// ---------------------------------------------------------------------------
// Session security ([MS-NLMP] 3.4): per-direction keys, RC4 handles and
// sequence numbers, with extended session security.

namespace detail {

class Session {
public:
    Session(const Key& exported_session_key, std::uint32_t negotiate_flags, bool initiator)
        : key_exch_((negotiate_flags & flags::negotiate_key_exch) != 0)
    {
        const Direction send = initiator ? Direction::client_to_server : Direction::server_to_client;
        const Direction receive = initiator ? Direction::server_to_client : Direction::client_to_server;
        send_signing_ = signing_key(exported_session_key, send);
        receive_signing_ = signing_key(exported_session_key, receive);
        send_sealing_ = sealing_key(exported_session_key, negotiate_flags, send);
        receive_sealing_ = sealing_key(exported_session_key, negotiate_flags, receive);
        reset();
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session()
    {
        wipe(send_signing_);
        wipe(receive_signing_);
        wipe(send_sealing_);
        wipe(receive_sealing_);
    }

    /// Connection-oriented RC4 handles are initialised once (3.4.5.3) and
    /// sequence numbers start at zero (3.4.4).
    void reset()
    {
        send_rc4_ = std::make_unique<Rc4>(send_sealing_);
        receive_rc4_ = std::make_unique<Rc4>(receive_sealing_);
        send_sequence_ = 0;
        receive_sequence_ = 0;
    }

    // SEAL (3.4.3): checksum over the plaintext, then the message is sealed,
    // then the checksum is encrypted with the same RC4 handle.
    std::vector<std::byte> wrap(std::span<const std::byte> plaintext)
    {
        std::vector<std::byte> out(signature_size + plaintext.size());
        const auto body = std::span(out).subspan(signature_size);
        std::ranges::copy(plaintext, body.begin());
        const std::uint32_t sequence = send_sequence_++;
        auto sum = checksum(send_signing_, sequence, plaintext);
        send_rc4_->apply(body);
        const Signature signature = finish(sum, sequence, *send_rc4_);
        std::ranges::copy(signature, out.begin());
        return out;
    }

    Result<std::vector<std::byte>> unwrap(std::span<const std::byte> wrapped)
    {
        Reader r(wrapped);
        FARLAND_TRY(const auto received, r.bytes(signature_size));
        const auto sealed = r.rest();
        std::vector<std::byte> plaintext(sealed.begin(), sealed.end());
        receive_rc4_->apply(plaintext);
        const std::uint32_t sequence = receive_sequence_++;
        auto sum = checksum(receive_signing_, sequence, plaintext);
        const Signature expected = finish(sum, sequence, *receive_rc4_);
        if (!equal_ct(expected, received)) {
            wipe(plaintext);
            return fail(Errc::invalid_value, "NTLM message signature does not verify");
        }
        return plaintext;
    }

    // MAC (3.4.4.2): the same signature without sealing the message.
    std::vector<std::byte> get_mic(std::span<const std::byte> message)
    {
        const std::uint32_t sequence = send_sequence_++;
        auto sum = checksum(send_signing_, sequence, message);
        const Signature signature = finish(sum, sequence, *send_rc4_);
        return {signature.begin(), signature.end()};
    }

    Result<void> verify_mic(std::span<const std::byte> message, std::span<const std::byte> mic)
    {
        const std::uint32_t sequence = receive_sequence_++;
        auto sum = checksum(receive_signing_, sequence, message);
        const Signature expected = finish(sum, sequence, *receive_rc4_);
        if (!equal_ct(expected, mic)) {
            return fail(Errc::invalid_value, "NTLM MIC does not verify");
        }
        return {};
    }

private:
    /// HMAC_MD5(SigningKey, SeqNum + Message)[0..7].
    static std::array<std::byte, 8> checksum(const Key& key, std::uint32_t sequence, std::span<const std::byte> message)
    {
        const auto sequence_le = le32(sequence);
        auto mac = hmac_md5(key, {sequence_le, message});
        std::array<std::byte, 8> out{};
        std::ranges::copy(std::span(mac).first(out.size()), out.begin());
        wipe(mac);
        return out;
    }

    /// NTLMSSP_MESSAGE_SIGNATURE with extended session security (2.2.2.9.1):
    /// Version 1, Checksum (RC4-encrypted with key exchange), SeqNum.
    [[nodiscard]] Signature finish(std::array<std::byte, 8>& sum, std::uint32_t sequence, Rc4& rc4) const
    {
        if (key_exch_) {
            rc4.apply(sum);
        }
        Signature signature{};
        const auto version = le32(1);
        const auto sequence_le = le32(sequence);
        std::ranges::copy(version, signature.begin());
        std::ranges::copy(sum, std::span(signature).subspan(4).begin());
        std::ranges::copy(sequence_le, std::span(signature).subspan(12).begin());
        wipe(sum);
        return signature;
    }

    bool key_exch_;
    Key send_signing_{};
    Key receive_signing_{};
    Key send_sealing_{};
    Key receive_sealing_{};
    std::unique_ptr<Rc4> send_rc4_;
    std::unique_ptr<Rc4> receive_rc4_;
    std::uint32_t send_sequence_ = 0;
    std::uint32_t receive_sequence_ = 0;
};

}  // namespace detail

// ---------------------------------------------------------------------------
// Acceptor ([MS-NLMP] 3.2.5)

Acceptor::Acceptor(AcceptorConfig config) : config_(std::move(config))
{
    FARLAND_ASSERT(config_.channel_binding_policy != ChannelBindingPolicy::require ||
                   config_.channel_bindings.has_value());
}

Acceptor::~Acceptor() = default;

std::span<const std::byte> Acceptor::mechanism() const noexcept
{
    return ntlm_oid;
}

bool Acceptor::complete() const noexcept
{
    return state_ == State::complete;
}

const Identity& Acceptor::identity() const noexcept
{
    return identity_;
}

Result<Step> Acceptor::step(std::span<const std::byte> input)
{
    if (state_ == State::complete || state_ == State::failed) {
        return fail(Errc::invalid_value, "NTLM acceptor is not expecting a token");
    }
    auto result = state_ == State::expect_negotiate ? on_negotiate(input) : on_authenticate(input);
    if (!result.has_value()) {
        state_ = State::failed;
        session_.reset();
    }
    return result;
}

Result<Step> Acceptor::on_negotiate(std::span<const std::byte> input)
{
    FARLAND_TRY(const auto negotiate, decode_negotiate(input));
    if ((negotiate.flags & required_flags) != required_flags) {
        return fail(Errc::unsupported, "NTLM client lacks Unicode, NTLM or extended session security");
    }

    // 3.2.5.1.1: answer with the flags both sides support.
    challenge_flags_ = (negotiate.flags & supported_flags) | flags::negotiate_target_info | flags::target_type_domain;
    server_challenge_ = config_.server_challenge.has_value() ? *config_.server_challenge : random_bytes<8>();
    const std::uint64_t timestamp = config_.timestamp.has_value() ? *config_.timestamp : filetime_now();
    const auto& dns_domain = config_.dns_domain.empty() ? config_.netbios_domain : config_.dns_domain;
    const auto& dns_computer = config_.dns_computer.empty() ? config_.netbios_computer : config_.dns_computer;

    ChallengeMessage challenge;
    challenge.flags = challenge_flags_;
    challenge.server_challenge = server_challenge_;
    challenge.target_name = utf8_to_utf16le(config_.netbios_domain);
    challenge.target_info = {
        av_pair(AvId::nb_domain_name, utf8_to_utf16le(config_.netbios_domain)),
        av_pair(AvId::nb_computer_name, utf8_to_utf16le(config_.netbios_computer)),
        av_pair(AvId::dns_domain_name, utf8_to_utf16le(dns_domain)),
        av_pair(AvId::dns_computer_name, utf8_to_utf16le(dns_computer)),
        av_pair(AvId::timestamp, le64(timestamp)),
    };
    if ((challenge_flags_ & flags::negotiate_version) != 0) {
        challenge.version = Version{};
    }

    negotiate_message_.assign(input.begin(), input.end());
    challenge_message_ = encode(challenge);
    state_ = State::expect_authenticate;
    return Step{challenge_message_, false};
}

Result<Step> Acceptor::on_authenticate(std::span<const std::byte> input)
{
    FARLAND_TRY(const auto authenticate, decode_authenticate(input));
    if ((authenticate.flags & flags::anonymous) != 0 || authenticate.user.empty()) {
        return fail(Errc::unsupported, "anonymous NTLM logon");
    }
    if (authenticate.nt_challenge_response.size() <= 24) {
        return fail(Errc::unsupported, "NTLMv1 response");
    }
    const std::uint32_t negotiated = authenticate.flags & challenge_flags_;
    if ((negotiated & required_flags) != required_flags) {
        return fail(Errc::unsupported, "AUTHENTICATE_MESSAGE drops required NTLM flags");
    }
    FARLAND_TRY(const auto blob, parse_ntlmv2_response(authenticate.nt_challenge_response));

    // Channel bindings (3.2.5.1.2, [MS-NLMP] 2.2.2.1 MsvAvChannelBindings):
    // all-zero counts as absent. Checked before the verifier is bothered.
    const auto bindings = find_av_pair(blob.av_pairs, AvId::channel_bindings);
    const bool present =
        bindings.has_value() && std::ranges::any_of(*bindings, [](std::byte b) { return b != std::byte{0}; });
    const bool matches =
        present && config_.channel_bindings.has_value() && equal_ct(*bindings, *config_.channel_bindings);
    switch (config_.channel_binding_policy) {
    case ChannelBindingPolicy::ignore:
        break;
    case ChannelBindingPolicy::verify_if_present:
        if (present && config_.channel_bindings.has_value() && !matches) {
            return fail(Errc::invalid_value, "NTLM channel bindings do not match");
        }
        break;
    case ChannelBindingPolicy::require:
        if (!present) {
            return fail(Errc::invalid_value, "NTLM channel bindings missing");
        }
        if (!matches) {
            return fail(Errc::invalid_value, "NTLM channel bindings do not match");
        }
        break;
    }

    // NTProofStr and SessionBaseKey (3.3.2), done by the verifier.
    auto base = config_.verifier.session_base_key(authenticate.user, authenticate.domain, server_challenge_,
                                                  authenticate.nt_challenge_response);
    if (!base.has_value()) {
        return fail(Errc::invalid_value, "NTLMv2 response does not verify");
    }

    // KXKEY = SessionBaseKey for NTLMv2 (3.4.5.1); the ExportedSessionKey is
    // RC4(KXKEY, EncryptedRandomSessionKey) with key exchange (3.2.5.1.2).
    Key exported = *base;
    wipe(*base);
    if ((negotiated & flags::negotiate_key_exch) != 0) {
        if (authenticate.encrypted_random_session_key.size() != exported.size()) {
            wipe(exported);
            return fail(Errc::invalid_length, "EncryptedRandomSessionKey is not 16 bytes");
        }
        Rc4 rc4(exported);
        std::ranges::copy(authenticate.encrypted_random_session_key, exported.begin());
        rc4.apply(exported);
    }

    // MIC (3.2.5.1.2): HMAC_MD5(ExportedSessionKey, NEGOTIATE + CHALLENGE +
    // AUTHENTICATE with the MIC zeroed), when MsvAvFlags announces one.
    const auto av_flags = find_av_pair(blob.av_pairs, AvId::flags);
    if (av_flags.has_value() && (read_u32(*av_flags) & av_flag_mic_present) != 0) {
        if (!authenticate.mic.has_value()) {
            wipe(exported);
            return fail(Errc::invalid_value, "NTLM MIC announced but missing");
        }
        std::vector<std::byte> zeroed(input.begin(), input.end());
        const auto mic_field = std::span(zeroed).subspan(authenticate_mic_offset(authenticate.flags), mic_size);
        std::ranges::fill(mic_field, std::byte{0});
        auto expected = hmac_md5(exported, {negotiate_message_, challenge_message_, zeroed});
        const bool mic_ok = equal_ct(expected, *authenticate.mic);
        wipe(expected);
        if (!mic_ok) {
            wipe(exported);
            return fail(Errc::invalid_value, "NTLM MIC does not verify");
        }
    }

    if (const auto target = find_av_pair(blob.av_pairs, AvId::target_name); target.has_value()) {
        auto name = decode_name(*target, 0);
        if (!name.has_value()) {
            wipe(exported);
            return std::unexpected(name.error());
        }
        target_name_ = std::move(*name);
    }

    session_ = std::make_unique<detail::Session>(exported, negotiated, false);
    wipe(exported);
    negotiated_flags_ = negotiated;
    channel_bound_ = matches && config_.channel_binding_policy != ChannelBindingPolicy::ignore;
    identity_ = Identity{authenticate.user, authenticate.domain};
    state_ = State::complete;
    return Step{{}, true};
}

std::vector<std::byte> Acceptor::wrap(std::span<const std::byte> plaintext)
{
    FARLAND_ASSERT(session_ != nullptr);
    return session_->wrap(plaintext);
}

Result<std::vector<std::byte>> Acceptor::unwrap(std::span<const std::byte> wrapped)
{
    FARLAND_ASSERT(session_ != nullptr);
    return session_->unwrap(wrapped);
}

std::vector<std::byte> Acceptor::get_mic(std::span<const std::byte> message)
{
    FARLAND_ASSERT(session_ != nullptr);
    return session_->get_mic(message);
}

Result<void> Acceptor::verify_mic(std::span<const std::byte> message, std::span<const std::byte> mic)
{
    FARLAND_ASSERT(session_ != nullptr);
    return session_->verify_mic(message, mic);
}

void Acceptor::reset_cipher_state()
{
    FARLAND_ASSERT(session_ != nullptr);
    session_->reset();
}

// ---------------------------------------------------------------------------
// Initiator ([MS-NLMP] 3.1.5)

Initiator::Initiator(InitiatorConfig config) : config_(std::move(config)), nt_hash_(nt_hash(config_.password.view()))
{
    config_.password = SecretString();
}

Initiator::~Initiator()
{
    wipe(nt_hash_);
    if (config_.random_session_key.has_value()) {
        wipe(*config_.random_session_key);
    }
}

std::span<const std::byte> Initiator::mechanism() const noexcept
{
    return ntlm_oid;
}

bool Initiator::complete() const noexcept
{
    return state_ == State::complete;
}

const Identity& Initiator::identity() const noexcept
{
    return identity_;
}

Result<Step> Initiator::step(std::span<const std::byte> input)
{
    switch (state_) {
    case State::initial: {
        if (!input.empty()) {
            state_ = State::failed;
            return fail(Errc::invalid_value, "NTLM initiator expects no input token first");
        }
        NegotiateMessage negotiate;
        negotiate.flags = supported_flags;
        negotiate.version = Version{};
        negotiate_message_ = encode(negotiate);
        state_ = State::expect_challenge;
        return Step{negotiate_message_, false};
    }
    case State::expect_challenge: {
        auto result = on_challenge(input);
        if (!result.has_value()) {
            state_ = State::failed;
            session_.reset();
        }
        return result;
    }
    case State::complete:
    case State::failed:
        break;
    }
    return fail(Errc::invalid_value, "NTLM initiator is not expecting a token");
}

Result<Step> Initiator::on_challenge(std::span<const std::byte> input)
{
    if (config_.user.empty()) {
        return fail(Errc::invalid_value, "anonymous NTLM logon is not supported");
    }
    for (const std::string& name : {config_.user, config_.domain, config_.workstation}) {
        if (utf8_to_utf16le(name).size() > max_name_bytes) {
            return fail(Errc::limit_exceeded, "NTLM name too long");
        }
    }
    FARLAND_TRY(const auto challenge, decode_challenge(input));
    constexpr std::uint32_t needed = required_flags | flags::negotiate_key_exch | flags::negotiate_target_info;
    if ((challenge.flags & needed) != needed) {
        return fail(Errc::unsupported, "NTLM server does not offer NTLMv2 with key exchange");
    }
    negotiated_flags_ = challenge.flags & supported_flags;

    // 3.1.5.1.2: echo the server's MsvAvTimestamp when there is one.
    const auto server_timestamp = find_av_pair(challenge.target_info, AvId::timestamp);
    std::uint64_t timestamp = 0;
    if (server_timestamp.has_value()) {
        timestamp = read_u64(*server_timestamp);
    } else {
        timestamp = config_.timestamp.has_value() ? *config_.timestamp : filetime_now();
    }
    const auto client_challenge = config_.client_challenge.has_value() ? *config_.client_challenge : random_bytes<8>();

    // The server's target info plus MsvAvFlags (MIC present),
    // MsvAvChannelBindings and MsvAvTargetName.
    std::vector<AvPair> pairs;
    std::uint32_t av_flags = av_flag_mic_present;
    for (const AvPair& pair : challenge.target_info) {
        switch (static_cast<AvId>(pair.id)) {
        case AvId::flags:
            av_flags |= read_u32(pair.value);
            break;
        case AvId::channel_bindings:
        case AvId::target_name:
            break;
        default:
            pairs.push_back(pair);
            break;
        }
    }
    const auto av_flags_le = le32(av_flags);
    pairs.push_back(av_pair(AvId::flags, {av_flags_le.begin(), av_flags_le.end()}));
    const auto bindings = config_.channel_bindings.value_or(std::array<std::byte, 16>{});
    pairs.push_back(av_pair(AvId::channel_bindings, {bindings.begin(), bindings.end()}));
    if (config_.target_name.has_value()) {
        pairs.push_back(av_pair(AvId::target_name, utf8_to_utf16le(*config_.target_name)));
    }
    if (pairs.size() > max_av_pairs) {
        return fail(Errc::limit_exceeded, "too many AV pairs");
    }

    // NTLMv2 response (3.3.2).
    const auto temp = ntlmv2_temp(timestamp, client_challenge, pairs);
    auto response_key = ntowf_v2(nt_hash_, config_.user, config_.domain);
    auto proof = nt_proof_str(response_key, challenge.server_challenge, temp);
    auto base = session_base_key(response_key, proof);

    AuthenticateMessage authenticate;
    authenticate.flags = negotiated_flags_;
    if (server_timestamp.has_value()) {
        authenticate.lm_challenge_response.assign(24, std::byte{0});  // Z(24), 3.1.5.1.2
    } else {
        const auto lm = lmv2_response(response_key, challenge.server_challenge, client_challenge);
        authenticate.lm_challenge_response.assign(lm.begin(), lm.end());
    }
    authenticate.nt_challenge_response.assign(proof.begin(), proof.end());
    authenticate.nt_challenge_response.insert(authenticate.nt_challenge_response.end(), temp.begin(), temp.end());
    authenticate.domain = config_.domain;
    authenticate.user = config_.user;
    authenticate.workstation = config_.workstation;

    // Key exchange (3.1.5.1.2): EncryptedRandomSessionKey = RC4(KXKEY, ExportedSessionKey).
    Key exported = config_.random_session_key.has_value() ? *config_.random_session_key : random_bytes<16>();
    Key encrypted = exported;
    Rc4(base).apply(encrypted);
    authenticate.encrypted_random_session_key.assign(encrypted.begin(), encrypted.end());
    if ((negotiated_flags_ & flags::negotiate_version) != 0) {
        authenticate.version = Version{};
    }
    authenticate.mic.emplace();

    auto message = encode(authenticate);
    auto mic = hmac_md5(exported, {negotiate_message_, input, message});
    std::ranges::copy(mic, std::span(message).subspan(authenticate_mic_offset(authenticate.flags)).begin());

    session_ = std::make_unique<detail::Session>(exported, negotiated_flags_, true);
    identity_ = Identity{config_.user, config_.domain};
    for (auto* key : {&response_key, &proof, &base, &exported, &encrypted, &mic}) {
        wipe(*key);
    }
    state_ = State::complete;
    return Step{std::move(message), true};
}

std::vector<std::byte> Initiator::wrap(std::span<const std::byte> plaintext)
{
    FARLAND_ASSERT(session_ != nullptr);
    return session_->wrap(plaintext);
}

Result<std::vector<std::byte>> Initiator::unwrap(std::span<const std::byte> wrapped)
{
    FARLAND_ASSERT(session_ != nullptr);
    return session_->unwrap(wrapped);
}

std::vector<std::byte> Initiator::get_mic(std::span<const std::byte> message)
{
    FARLAND_ASSERT(session_ != nullptr);
    return session_->get_mic(message);
}

Result<void> Initiator::verify_mic(std::span<const std::byte> message, std::span<const std::byte> mic)
{
    FARLAND_ASSERT(session_ != nullptr);
    return session_->verify_mic(message, mic);
}

void Initiator::reset_cipher_state()
{
    FARLAND_ASSERT(session_ != nullptr);
    session_->reset();
}

// ---------------------------------------------------------------------------
// LocalNtlmVerifier

std::optional<std::array<std::byte, 16>>
LocalNtlmVerifier::session_base_key(std::string_view user, std::string_view domain,
                                    std::span<const std::byte, 8> server_challenge,
                                    std::span<const std::byte> nt_challenge_response)
{
    constexpr std::size_t min_response = 16 + 28;  // NTProofStr + NTLMv2_CLIENT_CHALLENGE header
    if (nt_challenge_response.size() < min_response) {
        return std::nullopt;
    }
    // An unknown user costs the same work as a wrong password.
    auto stored = lookup_(user, domain);
    const bool known = stored.has_value();
    NtHash hash = stored.value_or(NtHash{});
    if (stored.has_value()) {
        wipe(*stored);
    }
    auto response_key = ntowf_v2(hash, user, domain);
    wipe(hash);
    const auto received = nt_challenge_response.first<16>();
    auto proof = nt_proof_str(response_key, server_challenge, nt_challenge_response.subspan(16));
    const bool valid = equal_ct(proof, received) && known;
    wipe(proof);
    std::optional<std::array<std::byte, 16>> result;
    if (valid) {
        result = ntlm::session_base_key(response_key, received);
    }
    wipe(response_key);
    return result;
}

bool LocalNtlmVerifier::verify_password(std::string_view user, std::string_view domain, std::string_view password)
{
    auto stored = lookup_(user, domain);
    auto candidate = nt_hash(password);
    const bool valid = stored.has_value() && equal_ct(*stored, candidate);
    if (stored.has_value()) {
        wipe(*stored);
    }
    wipe(candidate);
    return valid;
}

}  // namespace farland::auth::ntlm
