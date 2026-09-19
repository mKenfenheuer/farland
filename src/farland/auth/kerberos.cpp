// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/kerberos.hpp>
#include <farland/auth/spnego.hpp>
#include <farland/base/log.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#if FARLAND_HAVE_KERBEROS
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_ext.h>
#include <gssapi/gssapi_krb5.h>
#endif

namespace farland::auth::kerberos {

namespace {

constexpr std::string_view log_component = "auth.kerberos";
/// The Wrap token header of RFC 4121 4.2.6.2, ahead of the rotated body.
constexpr std::size_t gss_header_size = 16;

}  // namespace

Identity identity_from_principal(std::string_view principal)
{
    Identity identity;
    const auto at = principal.rfind('@');
    if (at == std::string_view::npos) {
        identity.user = principal;
        return identity;
    }
    identity.user = principal.substr(0, at);
    identity.domain = principal.substr(at + 1);
    return identity;
}

#if !FARLAND_HAVE_KERBEROS

// Without MIT krb5 the whole mechanism is absent rather than failing later:
// SPNEGO is told farland cannot run it, and the client falls back to NTLM.

struct Credential::Impl {};

bool available() noexcept
{
    return false;
}

Credential::Credential() = default;
Credential::Credential(std::unique_ptr<Impl> impl, std::string principal)
    : impl_(std::move(impl)), principal_(std::move(principal))
{
}
Credential::Credential(Credential&&) noexcept = default;
Credential& Credential::operator=(Credential&&) noexcept = default;
Credential::~Credential() = default;

Result<Credential> Credential::acquire(const Config& /*config*/)
{
    return fail(Errc::unsupported, "this build has no Kerberos (configure with -Dkerberos=enabled)");
}

std::unique_ptr<SecurityContext> Credential::accept(std::span<const std::byte> /*mechanism_oid*/) const
{
    return nullptr;
}

#else

namespace {

/// A GSS status pair as text, for the log. GSS splits its errors into a
/// mechanism-independent major code and a mechanism minor code, and both
/// halves matter: "Unspecified GSS failure" alone says nothing, while the
/// minor is what names the missing keytab entry.
std::string gss_text(OM_uint32 major, OM_uint32 minor)
{
    std::string out;
    const auto append = [&out](OM_uint32 status, int type) {
        OM_uint32 context = 0;
        OM_uint32 ignored = 0;
        for (;;) {
            gss_buffer_desc text{0, nullptr};
            const OM_uint32 rc = ::gss_display_status(&ignored, status, type, GSS_C_NO_OID, &context, &text);
            if (rc != GSS_S_COMPLETE) {
                break;
            }
            if (text.value != nullptr && text.length > 0) {
                if (!out.empty()) {
                    out += "; ";
                }
                out.append(static_cast<const char*>(text.value), text.length);
            }
            ::gss_release_buffer(&ignored, &text);
            if (context == 0) {
                break;
            }
        }
    };
    append(major, GSS_C_GSS_CODE);
    append(minor, GSS_C_MECH_CODE);
    return out.empty() ? std::string("no detail") : out;
}

[[nodiscard]] gss_buffer_desc as_buffer(std::span<const std::byte> bytes)
{
    // GSS takes a non-const pointer but does not write through it.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the GSS-API has no const buffers
    return gss_buffer_desc{bytes.size(), const_cast<std::byte*>(bytes.data())};
}

[[nodiscard]] std::vector<std::byte> take(gss_buffer_desc& buffer)
{
    std::vector<std::byte> out;
    if (buffer.value != nullptr && buffer.length > 0) {
        const auto* first = static_cast<const std::byte*>(buffer.value);
        out.assign(first, first + buffer.length);
    }
    OM_uint32 ignored = 0;
    ::gss_release_buffer(&ignored, &buffer);
    return out;
}

/// Both Kerberos OIDs mean the same mechanism. Windows lists the legacy
/// Microsoft one first and expects it echoed ([MS-SPNG] 3.1.5.2), so the
/// acceptor remembers which one was chosen and reports that, while GSS only
/// ever sees the real one.
[[nodiscard]] bool is_kerberos_oid(std::span<const std::byte> oid)
{
    return std::ranges::equal(oid, spnego::kerberos_oid) || std::ranges::equal(oid, spnego::ms_kerberos_oid);
}

class GssAcceptor final : public SecurityContext {
public:
    GssAcceptor(gss_cred_id_t credential, std::vector<std::byte> mechanism_oid)
        : credential_(credential), mechanism_oid_(std::move(mechanism_oid))
    {
    }

    ~GssAcceptor() override
    {
        OM_uint32 ignored = 0;
        if (context_ != GSS_C_NO_CONTEXT) {
            ::gss_delete_sec_context(&ignored, &context_, GSS_C_NO_BUFFER);
        }
    }

    GssAcceptor(const GssAcceptor&) = delete;
    GssAcceptor& operator=(const GssAcceptor&) = delete;
    GssAcceptor(GssAcceptor&&) = delete;
    GssAcceptor& operator=(GssAcceptor&&) = delete;

    [[nodiscard]] std::span<const std::byte> mechanism() const noexcept override { return mechanism_oid_; }

    [[nodiscard]] Result<Step> step(std::span<const std::byte> input) override
    {
        if (complete_) {
            return fail(Errc::invalid_value, "a Kerberos context that is established took another token");
        }
        if (input.empty()) {
            return fail(Errc::truncated, "the Kerberos acceptor was given no token");
        }
        if (input.size() > spnego::max_token_size) {
            return fail(Errc::limit_exceeded, "the Kerberos token is larger than MaxTokenSize");
        }
        gss_buffer_desc in = as_buffer(input);
        gss_buffer_desc out{0, nullptr};
        gss_name_t client = GSS_C_NO_NAME;
        OM_uint32 minor = 0;
        OM_uint32 flags = 0;
        const OM_uint32 major = ::gss_accept_sec_context(&minor, &context_, credential_, &in, GSS_C_NO_CHANNEL_BINDINGS,
                                                         &client, nullptr, &out, &flags, nullptr, nullptr);
        Step step;
        step.token = take(out);
        if (GSS_ERROR(major) != 0) {
            log::warn(log_component, "the client's Kerberos ticket was refused: {}", gss_text(major, minor));
            release(client);
            return fail(Errc::invalid_value, "the client's Kerberos ticket was refused");
        }
        if ((major & GSS_S_CONTINUE_NEEDED) != 0) {
            release(client);
            return step;
        }

        // CredSSP seals the public key and the delegated credentials with
        // this context, so a context that cannot do confidentiality is of no
        // use and must not be mistaken for a successful login.
        if ((flags & GSS_C_CONF_FLAG) == 0 || (flags & GSS_C_INTEG_FLAG) == 0) {
            log::warn(log_component, "the Kerberos context offers no confidentiality or integrity");
            release(client);
            return fail(Errc::unsupported, "the Kerberos context offers no confidentiality or integrity");
        }
        if (!read_identity(client)) {
            release(client);
            return fail(Errc::invalid_value, "the Kerberos client has no readable name");
        }
        release(client);
        complete_ = true;
        step.complete = true;
        return step;
    }

    [[nodiscard]] bool complete() const noexcept override { return complete_; }

    // CredSSP's GSS_WrapEx is the layout Windows' SSPI produces, and a
    // client cuts the field in two at a fixed offset: everything but the
    // sealed data first, then the sealed data. RFC 4121 provides for that
    // with RRC, the right rotation count, but MIT emits RRC = 0 -- a
    // perfectly good token that gss_unwrap reads back, and one no peer
    // splitting at an offset can use, because the confounder and the
    // ciphertext come first. So: wrap through the IOV interface, which
    // keeps the pieces separate, and rotate them as RRC says.
    //
    //   MIT gives   confounder | data | padding | E"header" | checksum
    //   the wire is E"header" | padding | checksum | confounder | data
    //
    // and RRC is how many bytes moved to the front. A client rotating back
    // gets what MIT produced.
    [[nodiscard]] std::vector<std::byte> wrap(std::span<const std::byte> plaintext) override
    {
        std::vector<std::byte> data(plaintext.begin(), plaintext.end());  // sealed in place
        std::array<gss_iov_buffer_desc, 4> iov{};
        iov[0].type = GSS_IOV_BUFFER_TYPE_HEADER | GSS_IOV_BUFFER_FLAG_ALLOCATE;
        iov[1].type = GSS_IOV_BUFFER_TYPE_DATA;
        iov[1].buffer = gss_buffer_desc{data.size(), data.data()};
        iov[2].type = GSS_IOV_BUFFER_TYPE_PADDING | GSS_IOV_BUFFER_FLAG_ALLOCATE;
        iov[3].type = GSS_IOV_BUFFER_TYPE_TRAILER | GSS_IOV_BUFFER_FLAG_ALLOCATE;

        OM_uint32 minor = 0;
        int sealed = 0;
        const OM_uint32 major =
            ::gss_wrap_iov(&minor, context_, 1, GSS_C_QOP_DEFAULT, &sealed, iov.data(), static_cast<int>(iov.size()));
        if (GSS_ERROR(major) != 0 || sealed == 0) {
            log::warn(log_component, "cannot seal with the Kerberos context: {}", gss_text(major, minor));
            OM_uint32 ignored = 0;
            ::gss_release_iov_buffer(&ignored, iov.data(), static_cast<int>(iov.size()));
            secure_zero(data);
            return {};  // an empty token fails the handshake at the peer
        }
        std::vector<std::byte> token;
        for (const auto& piece : iov) {
            const auto* first = static_cast<const std::byte*>(piece.buffer.value);
            if (first != nullptr) {
                token.insert(token.end(), first, first + piece.buffer.length);
            }
        }
        const std::size_t rotate = iov[2].buffer.length + iov[3].buffer.length;  // padding and trailer
        OM_uint32 ignored = 0;
        ::gss_release_iov_buffer(&ignored, iov.data(), static_cast<int>(iov.size()));
        secure_zero(data);

        if (token.size() < gss_header_size + rotate) {
            log::warn(log_component, "the Kerberos wrap token is shorter than its own header");
            return {};
        }
        std::vector<std::byte> out;
        out.reserve(token.size());
        const auto body = std::span(token).subspan(gss_header_size);
        out.insert(out.end(), token.begin(), token.begin() + gss_header_size);
        out.insert(out.end(), body.end() - static_cast<std::ptrdiff_t>(rotate), body.end());
        out.insert(out.end(), body.begin(), body.end() - static_cast<std::ptrdiff_t>(rotate));
        // RRC lives at offset 6 of the header, big endian (RFC 4121 4.2.6.2).
        out[6] = static_cast<std::byte>((rotate >> 8) & 0xff);
        out[7] = static_cast<std::byte>(rotate & 0xff);
        secure_zero(token);
        return out;
    }

    [[nodiscard]] Result<std::vector<std::byte>> unwrap(std::span<const std::byte> wrapped) override
    {
        if (wrapped.size() > spnego::max_token_size) {
            return fail(Errc::limit_exceeded, "the sealed Kerberos message is larger than MaxTokenSize");
        }
        // A STREAM buffer reads either layout -- the contiguous token and
        // the split one a Windows client sends -- and unseals within the
        // copy, so the caller's bytes are untouched.
        std::vector<std::byte> stream(wrapped.begin(), wrapped.end());
        std::array<gss_iov_buffer_desc, 2> iov{};
        iov[0].type = GSS_IOV_BUFFER_TYPE_STREAM;
        iov[0].buffer = gss_buffer_desc{stream.size(), stream.data()};
        iov[1].type = GSS_IOV_BUFFER_TYPE_DATA;

        OM_uint32 minor = 0;
        int sealed = 0;
        const OM_uint32 major =
            ::gss_unwrap_iov(&minor, context_, &sealed, nullptr, iov.data(), static_cast<int>(iov.size()));
        if (GSS_ERROR(major) != 0) {
            log::warn(log_component, "cannot unseal a Kerberos message: {}", gss_text(major, minor));
            secure_zero(stream);
            return fail(Errc::invalid_value, "cannot unseal a Kerberos message");
        }
        if (sealed == 0) {
            // Signed but not sealed: CredSSP's pubKeyAuth and authInfo are
            // both confidential, so this is not the message it claims to be.
            secure_zero(stream);
            return fail(Errc::invalid_value, "a Kerberos message that should be sealed was only signed");
        }
        const auto* first = static_cast<const std::byte*>(iov[1].buffer.value);
        std::vector<std::byte> plaintext;
        if (first != nullptr) {
            plaintext.assign(first, first + iov[1].buffer.length);
        }
        secure_zero(stream);
        return plaintext;
    }

    [[nodiscard]] std::vector<std::byte> get_mic(std::span<const std::byte> message) override
    {
        gss_buffer_desc in = as_buffer(message);
        gss_buffer_desc out{0, nullptr};
        OM_uint32 minor = 0;
        const OM_uint32 major = ::gss_get_mic(&minor, context_, GSS_C_QOP_DEFAULT, &in, &out);
        if (GSS_ERROR(major) != 0) {
            log::warn(log_component, "cannot sign with the Kerberos context: {}", gss_text(major, minor));
            OM_uint32 ignored = 0;
            ::gss_release_buffer(&ignored, &out);
            return {};
        }
        return take(out);
    }

    [[nodiscard]] Result<void> verify_mic(std::span<const std::byte> message, std::span<const std::byte> mic) override
    {
        if (mic.size() > spnego::max_mic_size) {
            return fail(Errc::limit_exceeded, "the Kerberos mechListMIC is too large");
        }
        gss_buffer_desc in = as_buffer(message);
        gss_buffer_desc signature = as_buffer(mic);
        OM_uint32 minor = 0;
        const OM_uint32 major = ::gss_verify_mic(&minor, context_, &in, &signature, nullptr);
        if (GSS_ERROR(major) != 0) {
            log::warn(log_component, "a Kerberos signature does not verify: {}", gss_text(major, minor));
            return fail(Errc::invalid_value, "a Kerberos signature does not verify");
        }
        return {};
    }

    [[nodiscard]] const Identity& identity() const noexcept override { return identity_; }

private:
    static void release(gss_name_t& name)
    {
        if (name != GSS_C_NO_NAME) {
            OM_uint32 ignored = 0;
            ::gss_release_name(&ignored, &name);
        }
    }

    bool read_identity(gss_name_t client)
    {
        gss_buffer_desc text{0, nullptr};
        OM_uint32 minor = 0;
        const OM_uint32 major = ::gss_display_name(&minor, client, &text, nullptr);
        if (GSS_ERROR(major) != 0) {
            log::warn(log_component, "cannot read the Kerberos client's name: {}", gss_text(major, minor));
            return false;
        }
        const std::string principal(static_cast<const char*>(text.value), text.length);
        OM_uint32 ignored = 0;
        ::gss_release_buffer(&ignored, &text);
        if (principal.empty()) {
            return false;
        }
        identity_ = identity_from_principal(principal);
        log::info(log_component, "Kerberos authenticated {}", principal);
        return true;
    }

    gss_cred_id_t credential_;  ///< owned by the Credential, which outlives this
    std::vector<std::byte> mechanism_oid_;
    gss_ctx_id_t context_ = GSS_C_NO_CONTEXT;
    bool complete_ = false;
    Identity identity_;
};

}  // namespace

struct Credential::Impl {
    gss_cred_id_t credential = GSS_C_NO_CREDENTIAL;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl()
    {
        if (credential != GSS_C_NO_CREDENTIAL) {
            OM_uint32 ignored = 0;
            ::gss_release_cred(&ignored, &credential);
        }
    }
};

bool available() noexcept
{
    return true;
}

Credential::Credential() = default;
Credential::Credential(std::unique_ptr<Impl> impl, std::string principal)
    : impl_(std::move(impl)), principal_(std::move(principal))
{
}
Credential::Credential(Credential&&) noexcept = default;
Credential& Credential::operator=(Credential&&) noexcept = default;
Credential::~Credential() = default;

Result<Credential> Credential::acquire(const Config& config)
{
    auto impl = std::make_unique<Impl>();

    // The principal to accept as. GSS_C_NO_NAME accepts every key in the
    // keytab, which is what a host serving one service wants; a named
    // principal is for a host whose keytab holds several.
    gss_name_t desired = GSS_C_NO_NAME;
    OM_uint32 minor = 0;
    if (!config.service_principal.empty()) {
        gss_buffer_desc text = as_buffer(std::as_bytes(std::span(config.service_principal)));
        const OM_uint32 major = ::gss_import_name(&minor, &text, GSS_KRB5_NT_PRINCIPAL_NAME, &desired);
        if (GSS_ERROR(major) != 0) {
            log::error(log_component, "'{}' is not a Kerberos principal: {}", config.service_principal,
                       gss_text(major, minor));
            return fail(Errc::invalid_value, "the Kerberos service principal cannot be read");
        }
    }
    const auto release_desired = [&desired] {
        if (desired != GSS_C_NO_NAME) {
            OM_uint32 ignored = 0;
            ::gss_release_name(&ignored, &desired);
        }
    };

    // The keytab goes through the credential store rather than KRB5_KTNAME,
    // which is process-wide and would also reach anything else in this
    // process that speaks Kerberos.
    gss_key_value_element_desc element{"keytab", config.keytab.c_str()};
    gss_key_value_set_desc store{1, &element};
    const gss_key_value_set_desc* store_ptr = config.keytab.empty() ? nullptr : &store;

    OM_uint32 major = ::gss_acquire_cred_from(&minor, desired, GSS_C_INDEFINITE, GSS_C_NO_OID_SET, GSS_C_ACCEPT,
                                              store_ptr, &impl->credential, nullptr, nullptr);
    if (GSS_ERROR(major) != 0) {
        log::error(log_component, "cannot use the keytab {}: {}",
                   config.keytab.empty() ? "(the system default)" : config.keytab, gss_text(major, minor));
        release_desired();
        return fail(Errc::io, "cannot acquire the Kerberos acceptor credential");
    }

    // What it ended up accepting as, so the log names it and a wrong keytab
    // is obvious before a client ever tries.
    std::string principal = config.service_principal;
    gss_name_t name = GSS_C_NO_NAME;
    if (::gss_inquire_cred(&minor, impl->credential, &name, nullptr, nullptr, nullptr) == GSS_S_COMPLETE &&
        name != GSS_C_NO_NAME) {
        gss_buffer_desc text{0, nullptr};
        if (::gss_display_name(&minor, name, &text, nullptr) == GSS_S_COMPLETE && text.value != nullptr) {
            principal.assign(static_cast<const char*>(text.value), text.length);
        }
        OM_uint32 ignored = 0;
        ::gss_release_buffer(&ignored, &text);
        ::gss_release_name(&ignored, &name);
    }
    if (principal.empty()) {
        principal = "any principal in the keytab";
    }
    release_desired();
    return Credential(std::move(impl), std::move(principal));
}

std::unique_ptr<SecurityContext> Credential::accept(std::span<const std::byte> mechanism_oid) const
{
    if (impl_ == nullptr || impl_->credential == GSS_C_NO_CREDENTIAL || !is_kerberos_oid(mechanism_oid)) {
        return nullptr;
    }
    return std::make_unique<GssAcceptor>(impl_->credential,
                                         std::vector<std::byte>(mechanism_oid.begin(), mechanism_oid.end()));
}

#endif  // FARLAND_HAVE_KERBEROS

}  // namespace farland::auth::kerberos
