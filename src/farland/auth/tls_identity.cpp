// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/tls_identity.hpp>
#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace farland::auth {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view log_component = "auth.tls";
constexpr int rsa_bits = 2048;
constexpr int serial_bits = 128;
constexpr long not_before_offset_seconds = -3600;
constexpr int validity_days = 1826;              // five years, including a leap day
constexpr std::size_t max_hostname_length = 64;  // ub-common-name, RFC 5280 appendix A.1
constexpr std::streamsize max_pem_file_size = 1 << 20;

std::string errno_text()
{
    return std::error_code(errno, std::generic_category()).message();
}

bool valid_hostname(std::string_view hostname)
{
    if (hostname.empty() || hostname.size() > max_hostname_length) {
        return false;
    }
    return std::ranges::all_of(hostname, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
               c == '_';
    });
}

// Key and certificate generation ------------------------------------------

ossl::Pkey generate_rsa_key()
{
    const ossl::PkeyCtx context(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr));
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), rsa_bits) != 1) {
        return nullptr;
    }
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_generate(context.get(), &key) != 1) {
        return nullptr;
    }
    return ossl::Pkey(key);
}

bool set_random_serial(X509& certificate)
{
    // A positive integer of exactly 128 bits (RFC 5280 4.1.2.2 allows up to 20 octets).
    const ossl::Bignum serial(BN_new());
    return serial && BN_rand(serial.get(), serial_bits, BN_RAND_TOP_ONE, BN_RAND_BOTTOM_ANY) == 1 &&
           BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(&certificate)) != nullptr;
}

bool add_extension(X509& certificate, X509V3_CTX& context, int nid, const char* value)
{
    const ossl::X509Extension extension(X509V3_EXT_conf_nid(nullptr, &context, nid, value));
    return extension && X509_add_ext(&certificate, extension.get(), -1) == 1;
}

ossl::X509Ptr make_certificate(EVP_PKEY& key, std::string_view hostname)
{
    ossl::X509Ptr certificate(X509_new());
    if (!certificate || X509_set_version(certificate.get(), X509_VERSION_3) != 1 || !set_random_serial(*certificate) ||
        X509_gmtime_adj(X509_getm_notBefore(certificate.get()), not_before_offset_seconds) == nullptr ||
        X509_time_adj_ex(X509_getm_notAfter(certificate.get()), validity_days, 0, nullptr) == nullptr ||
        X509_set_pubkey(certificate.get(), &key) != 1) {
        return nullptr;
    }

    const std::vector<unsigned char> common_name(hostname.begin(), hostname.end());
    X509_NAME* name = X509_get_subject_name(certificate.get());
    if (X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_UTF8, common_name.data(),
                                   static_cast<int>(common_name.size()), -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), name) != 1) {
        return nullptr;
    }

    // The hostname was validated, so it cannot inject into the config syntax.
    X509V3_CTX context{};
    X509V3_set_ctx(&context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
    const std::string san = std::format("DNS:{}", hostname);
    if (!add_extension(*certificate, context, NID_basic_constraints, "critical,CA:FALSE") ||
        !add_extension(*certificate, context, NID_key_usage, "critical,digitalSignature,keyEncipherment") ||
        !add_extension(*certificate, context, NID_ext_key_usage, "serverAuth") ||
        !add_extension(*certificate, context, NID_subject_alt_name, san.c_str()) ||
        !add_extension(*certificate, context, NID_subject_key_identifier, "hash")) {
        return nullptr;
    }

    if (X509_sign(certificate.get(), &key, EVP_sha256()) <= 0) {
        return nullptr;
    }
    return certificate;
}

ossl::SslCtx make_server_context(X509& certificate, EVP_PKEY& key)
{
    ossl::SslCtx context(SSL_CTX_new(TLS_server_method()));
    if (!context) {
        return nullptr;
    }
    SSL_CTX* ctx = context.get();
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION | SSL_OP_NO_TICKET);
    // RDP clients open one connection per session and never resume it.
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1 || SSL_CTX_set_num_tickets(ctx, 0) != 1 ||
        SSL_CTX_use_certificate(ctx, &certificate) != 1 || SSL_CTX_use_PrivateKey(ctx, &key) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        return nullptr;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    return context;
}

// PEM ----------------------------------------------------------------------

/// Never prompt on the terminal for a passphrase: encrypted keys fail to load.
int no_passphrase(char* /*buffer*/, int /*size*/, int /*rwflag*/, void* /*userdata*/)
{
    return 0;
}

std::string drain_bio(BIO& bio)
{
    std::string text(BIO_ctrl_pending(&bio), '\0');
    std::size_t read = 0;
    if (!text.empty() && BIO_read_ex(&bio, text.data(), text.size(), &read) != 1) {
        read = 0;
    }
    text.resize(read);
    return text;
}

Result<std::string> read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        log::error(log_component, "cannot open {}: {}", path.string(), errno_text());
        return fail(Errc::invalid_value, "cannot open TLS identity file");
    }
    std::string text;
    std::array<char, 4096> buffer{};
    while (in.read(buffer.data(), buffer.size()) || in.gcount() > 0) {
        if (static_cast<std::streamsize>(text.size()) + in.gcount() > max_pem_file_size) {
            return fail(Errc::limit_exceeded, "TLS identity file is too large");
        }
        text.append(std::string_view(buffer.data(), static_cast<std::size_t>(in.gcount())));
    }
    if (in.bad()) {
        log::error(log_component, "cannot read {}: {}", path.string(), errno_text());
        return fail(Errc::invalid_value, "cannot read TLS identity file");
    }
    return text;
}

ossl::Bio memory_bio(std::string_view text)
{
    FARLAND_ASSERT(text.size() <= static_cast<std::size_t>(max_pem_file_size));
    return ossl::Bio(BIO_new_mem_buf(text.data(), static_cast<int>(text.size())));
}

// Files ----------------------------------------------------------------------

/// Creates `directory` and its missing parents with mode 0700.
Result<void> create_private_directories(const fs::path& directory)
{
    std::error_code ec;
    if (directory.empty() || fs::is_directory(directory, ec)) {
        return {};
    }
    if (directory.has_parent_path() && directory.parent_path() != directory) {
        FARLAND_TRY_VOID(create_private_directories(directory.parent_path()));
    }
    if (::mkdir(directory.c_str(), S_IRWXU) != 0) {
        if (errno == EEXIST && fs::is_directory(directory, ec)) {
            return {};
        }
        log::error(log_component, "cannot create directory {}: {}", directory.string(), errno_text());
        return fail(Errc::invalid_value, "cannot create TLS identity directory");
    }
    // mkdir() applies the umask; make the mode exact.
    if (::chmod(directory.c_str(), S_IRWXU) != 0) {
        log::error(log_component, "cannot chmod directory {}: {}", directory.string(), errno_text());
        return fail(Errc::invalid_value, "cannot create TLS identity directory");
    }
    return {};
}

/// A temporary file next to its destination, removed unless committed.
class TempFile {
public:
    explicit TempFile(const fs::path& destination)
        : path_(destination.string() + ".tmp.XXXXXX"), fd_(::mkostemp(path_.data(), O_CLOEXEC)), created_(fd_ >= 0)
    {
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
    ~TempFile()
    {
        close();
        if (created_) {
            ::unlink(path_.c_str());
        }
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    bool close() noexcept
    {
        const bool ok = fd_ < 0 || ::close(fd_) == 0;
        fd_ = -1;
        return ok;
    }
    void committed() noexcept { created_ = false; }

private:
    std::string path_;
    int fd_ = -1;
    bool created_ = false;
};

Result<void> write_file_atomically(const fs::path& path, std::string_view contents, ::mode_t mode)
{
    TempFile file(path);
    if (file.fd() < 0) {
        log::error(log_component, "cannot create a temporary file for {}: {}", path.string(), errno_text());
        return fail(Errc::invalid_value, "cannot write TLS identity file");
    }
    // fchmod() rather than relying on the umask, so the mode is exact.
    bool ok = ::fchmod(file.fd(), mode) == 0;
    for (std::string_view rest = contents; ok && !rest.empty();) {
        const ::ssize_t written = ::write(file.fd(), rest.data(), rest.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        ok = written > 0;
        if (ok) {
            rest.remove_prefix(static_cast<std::size_t>(written));
        }
    }
    ok = ok && ::fsync(file.fd()) == 0;
    ok = file.close() && ok;
    ok = ok && ::rename(file.path().c_str(), path.c_str()) == 0;
    if (!ok) {
        log::error(log_component, "cannot write {}: {}", path.string(), errno_text());
        return fail(Errc::invalid_value, "cannot write TLS identity file");
    }
    file.committed();

    // Make the rename itself durable. Failure here leaves a valid file.
    const fs::path directory = path.has_parent_path() ? path.parent_path() : fs::path(".");
    // open(2) is variadic in POSIX; there is no other way to get a directory fd.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (const int dir_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dir_fd >= 0) {
        static_cast<void>(::fsync(dir_fd));
        static_cast<void>(::close(dir_fd));
    }
    return {};
}

}  // namespace

// TlsIdentity ----------------------------------------------------------------

Result<TlsIdentity> TlsIdentity::from_parts(ossl::X509Ptr certificate, ossl::Pkey key)
{
    FARLAND_ASSERT(certificate && key);
    if (X509_check_private_key(certificate.get(), key.get()) != 1) {
        log::error(log_component, "private key does not match certificate: {}", ossl::take_errors());
        return fail(Errc::invalid_value, "TLS private key does not match the certificate");
    }

    TlsIdentity identity;
    FARLAND_TRY(identity.subject_public_key_, ossl::subject_public_key(*certificate));
    identity.certificate_der_ = ossl::certificate_der(*certificate);
    identity.server_context_ = make_server_context(*certificate, *key);
    if (!identity.server_context_) {
        log::error(log_component, "cannot configure SSL_CTX: {}", ossl::take_errors());
        return fail(Errc::invalid_value, "cannot configure TLS server context");
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    FARLAND_ASSERT(X509_digest(certificate.get(), EVP_sha256(), digest.data(), &digest_size) == 1);
    for (const unsigned char octet : std::span(digest).first(digest_size)) {
        if (!identity.fingerprint_.empty()) {
            identity.fingerprint_ += ':';
        }
        std::format_to(std::back_inserter(identity.fingerprint_), "{:02X}", octet);
    }

    identity.certificate_ = std::move(certificate);
    identity.key_ = std::move(key);
    return identity;
}

Result<TlsIdentity> TlsIdentity::generate(std::string_view hostname)
{
    if (!valid_hostname(hostname)) {
        return fail(Errc::invalid_value, "invalid hostname for the TLS certificate");
    }
    ERR_clear_error();
    ossl::Pkey key = generate_rsa_key();
    ossl::X509Ptr certificate = key ? make_certificate(*key, hostname) : nullptr;
    if (!certificate) {
        log::error(log_component, "certificate generation failed: {}", ossl::take_errors());
        return fail(Errc::invalid_value, "TLS certificate generation failed");
    }
    return from_parts(std::move(certificate), std::move(key));
}

Result<TlsIdentity> TlsIdentity::load(const fs::path& cert_pem, const fs::path& key_pem)
{
    FARLAND_TRY(const auto cert_text, read_file(cert_pem));
    FARLAND_TRY(const auto key_text, read_file(key_pem));

    ERR_clear_error();
    const ossl::Bio cert_bio = memory_bio(cert_text);
    ossl::X509Ptr certificate(cert_bio ? PEM_read_bio_X509(cert_bio.get(), nullptr, &no_passphrase, nullptr) : nullptr);
    if (!certificate) {
        log::error(log_component, "cannot parse certificate {}: {}", cert_pem.string(), ossl::take_errors());
        return fail(Errc::invalid_value, "invalid PEM certificate");
    }
    const ossl::Bio key_bio = memory_bio(key_text);
    ossl::Pkey key(key_bio ? PEM_read_bio_PrivateKey(key_bio.get(), nullptr, &no_passphrase, nullptr) : nullptr);
    if (!key) {
        log::error(log_component, "cannot parse private key {}: {}", key_pem.string(), ossl::take_errors());
        return fail(Errc::invalid_value, "invalid or encrypted PEM private key");
    }
    return from_parts(std::move(certificate), std::move(key));
}

Result<TlsIdentity> TlsIdentity::load_or_create(const fs::path& cert_pem, const fs::path& key_pem,
                                                std::string_view hostname)
{
    std::error_code cert_error;
    std::error_code key_error;
    const bool have_cert = fs::exists(cert_pem, cert_error);
    const bool have_key = fs::exists(key_pem, key_error);
    if (cert_error || key_error) {
        return fail(Errc::invalid_value, "cannot access TLS identity files");
    }
    if (have_cert && have_key) {
        return load(cert_pem, key_pem);
    }
    if (have_cert || have_key) {
        log::error(log_component, "{} exists but {} does not", (have_cert ? cert_pem : key_pem).string(),
                   (have_cert ? key_pem : cert_pem).string());
        return fail(Errc::invalid_value, "only one of the TLS certificate and key files exists");
    }

    FARLAND_TRY(auto identity, generate(hostname));
    FARLAND_TRY_VOID(identity.save(cert_pem, key_pem));
    log::info(log_component, "generated TLS certificate {} for {}, SHA-256 {}", cert_pem.string(), hostname,
              identity.sha256_fingerprint());
    return identity;
}

Result<void> TlsIdentity::save(const fs::path& cert_pem, const fs::path& key_pem) const
{
    ERR_clear_error();
    const ossl::Bio key_bio(BIO_new(BIO_s_mem()));
    const ossl::Bio cert_bio(BIO_new(BIO_s_mem()));
    if (!key_bio || !cert_bio ||
        PEM_write_bio_PrivateKey(key_bio.get(), key_.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1 ||
        PEM_write_bio_X509(cert_bio.get(), certificate_.get()) != 1) {
        log::error(log_component, "cannot encode PEM: {}", ossl::take_errors());
        return fail(Errc::invalid_value, "cannot encode TLS identity");
    }

    FARLAND_TRY_VOID(create_private_directories(key_pem.parent_path()));
    FARLAND_TRY_VOID(create_private_directories(cert_pem.parent_path()));
    // Key first: a crash in between leaves only the key, which load_or_create
    // reports instead of silently generating a new certificate.
    FARLAND_TRY_VOID(write_file_atomically(key_pem, drain_bio(*key_bio), S_IRUSR | S_IWUSR));
    FARLAND_TRY_VOID(write_file_atomically(cert_pem, drain_bio(*cert_bio), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));
    return {};
}

}  // namespace farland::auth
