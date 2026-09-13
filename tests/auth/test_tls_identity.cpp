// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/openssl.hpp>
#include <farland/auth/tls_identity.hpp>
#include <farland/base/ber.hpp>
#include <farland/base/reader.hpp>

#include <catch2/catch_test_macros.hpp>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

using farland::auth::TlsIdentity;
namespace fs = std::filesystem;
namespace ossl = farland::auth::ossl;
namespace ber = farland::ber;

namespace {

constexpr std::string_view hostname = "farland-test.example";

/// RSA key generation takes a moment, so tests that only inspect share one.
const TlsIdentity& shared_identity()
{
    static const TlsIdentity identity = TlsIdentity::generate(hostname).value();
    return identity;
}

ossl::X509Ptr parse_certificate(std::span<const std::byte> der)
{
    const auto* p = reinterpret_cast<const unsigned char*>(der.data());
    return ossl::X509Ptr(d2i_X509(nullptr, &p, static_cast<long>(der.size())));
}

class TempDir {
public:
    TempDir()
    {
        std::random_device random;
        path_ = fs::temp_directory_path() / ("farland-test-" + std::to_string(random()) + std::to_string(random()));
        fs::create_directory(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

bool extension_is_critical(const X509* cert, int nid)
{
    const int index = X509_get_ext_by_NID(cert, nid, -1);
    return index >= 0 && X509_EXTENSION_get_critical(X509_get_ext(cert, index)) == 1;
}

}  // namespace

TEST_CASE("A generated certificate meets the RDP client requirements")
{
    const auto cert = parse_certificate(shared_identity().certificate_der());
    REQUIRE(cert);

    CHECK(X509_get_version(cert.get()) == X509_VERSION_3);
    CHECK(X509_get_signature_nid(cert.get()) == NID_sha256WithRSAEncryption);
    CHECK(EVP_PKEY_get_base_id(X509_get0_pubkey(cert.get())) == EVP_PKEY_RSA);
    CHECK(EVP_PKEY_get_bits(X509_get0_pubkey(cert.get())) == 2048);
    CHECK(X509_verify(cert.get(), X509_get0_pubkey(cert.get())) == 1);

    const ossl::Bignum serial(ASN1_INTEGER_to_BN(X509_get0_serialNumber(cert.get()), nullptr));
    REQUIRE(serial);
    CHECK(BN_num_bits(serial.get()) == 128);
    CHECK_FALSE(BN_is_negative(serial.get()));

    std::array<char, 256> cn{};
    REQUIRE(X509_NAME_get_text_by_NID(X509_get_subject_name(cert.get()), NID_commonName, cn.data(),
                                      static_cast<int>(cn.size())) > 0);
    CHECK(std::string(cn.data()) == hostname);
    CHECK(X509_NAME_cmp(X509_get_subject_name(cert.get()), X509_get_issuer_name(cert.get())) == 0);

    // subjectAltName DNS:hostname (X509_check_host only consults the SAN when one exists).
    CHECK(X509_check_host(cert.get(), hostname.data(), hostname.size(), X509_CHECK_FLAG_NEVER_CHECK_SUBJECT, nullptr) ==
          1);

    const std::uint32_t flags = X509_get_extension_flags(cert.get());
    CHECK((flags & EXFLAG_BCONS) != 0);
    CHECK((flags & EXFLAG_CA) == 0);
    CHECK(X509_check_ca(cert.get()) == 0);
    CHECK(extension_is_critical(cert.get(), NID_basic_constraints));

    CHECK((flags & EXFLAG_KUSAGE) != 0);
    CHECK(X509_get_key_usage(cert.get()) == (KU_DIGITAL_SIGNATURE | KU_KEY_ENCIPHERMENT));
    CHECK(extension_is_critical(cert.get(), NID_key_usage));

    CHECK((flags & EXFLAG_XKUSAGE) != 0);
    CHECK(X509_get_extended_key_usage(cert.get()) == XKU_SSL_SERVER);

    CHECK(X509_get0_subject_key_id(cert.get()) != nullptr);

    int days = 0;
    int seconds = 0;
    REQUIRE(ASN1_TIME_diff(&days, &seconds, X509_get0_notBefore(cert.get()), X509_get0_notAfter(cert.get())) == 1);
    CHECK(days >= 5 * 365);
    CHECK(days <= 5 * 365 + 2);

    // notBefore is an hour in the past, to tolerate clients with slow clocks.
    REQUIRE(ASN1_TIME_diff(&days, &seconds, X509_get0_notBefore(cert.get()), nullptr) == 1);
    CHECK(days == 0);
    CHECK(seconds >= 3600);
    CHECK(seconds < 3600 + 300);
}

TEST_CASE("Two generated certificates have different serials")
{
    const auto other = TlsIdentity::generate(hostname).value();
    const auto a = parse_certificate(shared_identity().certificate_der());
    const auto b = parse_certificate(other.certificate_der());
    CHECK(ASN1_INTEGER_cmp(X509_get0_serialNumber(a.get()), X509_get0_serialNumber(b.get())) != 0);
    CHECK(other.sha256_fingerprint() != shared_identity().sha256_fingerprint());
}

TEST_CASE("The fingerprint is the uppercase, colon-separated SHA-256 of the DER certificate")
{
    const auto& fingerprint = shared_identity().sha256_fingerprint();
    REQUIRE(fingerprint.size() == 32 * 3 - 1);

    const auto der = shared_identity().certificate_der();
    std::array<unsigned char, 32> digest{};
    REQUIRE(EVP_Digest(der.data(), der.size(), digest.data(), nullptr, EVP_sha256(), nullptr) == 1);
    std::string expected;
    for (const unsigned char octet : digest) {
        if (!expected.empty()) {
            expected += ':';
        }
        expected += std::format("{:02X}", octet);
    }
    CHECK(fingerprint == expected);

    for (std::size_t i = 0; i < fingerprint.size(); ++i) {
        const char c = fingerprint[i];
        if (i % 3 == 2) {
            CHECK(c == ':');
        } else {
            CHECK(((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')));
        }
    }
}

TEST_CASE("subject_public_key is the certificate's DER RSAPublicKey, not the SubjectPublicKeyInfo")
{
    const auto spk = shared_identity().subject_public_key();
    const auto cert = parse_certificate(shared_identity().certificate_der());
    REQUIRE(cert);

    // RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } (RFC 8017 A.1.1)
    farland::Reader r(spk);
    auto key = ber::read_constructed(r, ber::tags::sequence, ber::Rules::der);
    REQUIRE(key.has_value());
    CHECK(r.empty());
    const auto modulus = ber::expect_tlv(*key, ber::tags::integer, ber::Rules::der);
    REQUIRE(modulus.has_value());
    CHECK(modulus->value.size() == 257);  // leading zero + 2048 bits
    CHECK(ber::read_unsigned(*key, ber::Rules::der).value() == 65537);
    CHECK(key->empty());

    // Same key as the certificate's, and the same bytes OpenSSL encodes as PKCS#1.
    const auto* p = reinterpret_cast<const unsigned char*>(spk.data());
    const ossl::Pkey parsed(d2i_PublicKey(EVP_PKEY_RSA, nullptr, &p, static_cast<long>(spk.size())));
    REQUIRE(parsed);
    CHECK(EVP_PKEY_eq(parsed.get(), X509_get0_pubkey(cert.get())) == 1);

    // The full SPKI is longer: it wraps this in an AlgorithmIdentifier and BIT STRING.
    CHECK(i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert.get()), nullptr) > static_cast<int>(spk.size()));
}

TEST_CASE("generate rejects hostnames that are not usable in a certificate")
{
    CHECK_FALSE(TlsIdentity::generate("").has_value());
    CHECK_FALSE(TlsIdentity::generate(std::string(65, 'a')).has_value());
    CHECK_FALSE(TlsIdentity::generate("host,IP:1.2.3.4").has_value());
    CHECK_FALSE(TlsIdentity::generate("white space").has_value());
}

TEST_CASE("load_or_create creates private files once and reloads them")
{
    const TempDir dir;
    const auto state = dir.path() / "state" / "tls";
    const auto cert_path = state / "cert.pem";
    const auto key_path = state / "key.pem";

    const auto created = TlsIdentity::load_or_create(cert_path, key_path, hostname);
    REQUIRE(created.has_value());

    using fs::perms;
    CHECK(fs::status(dir.path() / "state").permissions() == perms::owner_all);
    CHECK(fs::status(state).permissions() == perms::owner_all);
    CHECK(fs::status(key_path).permissions() == (perms::owner_read | perms::owner_write));
    CHECK(fs::status(cert_path).permissions() ==
          (perms::owner_read | perms::owner_write | perms::group_read | perms::others_read));
    // No temporary files are left behind.
    CHECK(std::distance(fs::directory_iterator(state), fs::directory_iterator()) == 2);

    const auto reloaded = TlsIdentity::load_or_create(cert_path, key_path, "ignored-on-reload");
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->sha256_fingerprint() == created->sha256_fingerprint());
    CHECK(std::ranges::equal(reloaded->subject_public_key(), created->subject_public_key()));
    CHECK(std::ranges::equal(reloaded->certificate_der(), created->certificate_der()));

    const auto loaded = TlsIdentity::load(cert_path, key_path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->sha256_fingerprint() == created->sha256_fingerprint());
}

TEST_CASE("load_or_create refuses a key that does not match the certificate")
{
    const TempDir dir;
    REQUIRE(TlsIdentity::load_or_create(dir.path() / "a.crt", dir.path() / "a.key", hostname).has_value());
    REQUIRE(TlsIdentity::load_or_create(dir.path() / "b.crt", dir.path() / "b.key", hostname).has_value());

    const auto mixed = TlsIdentity::load_or_create(dir.path() / "a.crt", dir.path() / "b.key", hostname);
    REQUIRE_FALSE(mixed.has_value());
    CHECK(mixed.error().code == farland::Errc::invalid_value);
}

TEST_CASE("load_or_create refuses to replace half an identity")
{
    const TempDir dir;
    const auto cert_path = dir.path() / "cert.pem";
    const auto key_path = dir.path() / "key.pem";
    REQUIRE(TlsIdentity::load_or_create(cert_path, key_path, hostname).has_value());

    SECTION("only the certificate")
    {
        fs::remove(key_path);
        CHECK_FALSE(TlsIdentity::load_or_create(cert_path, key_path, hostname).has_value());
        CHECK_FALSE(fs::exists(key_path));
    }
    SECTION("only the key")
    {
        fs::remove(cert_path);
        CHECK_FALSE(TlsIdentity::load_or_create(cert_path, key_path, hostname).has_value());
        CHECK_FALSE(fs::exists(cert_path));
    }
}

TEST_CASE("load rejects files that are not PEM")
{
    const TempDir dir;
    const auto cert_path = dir.path() / "cert.pem";
    const auto key_path = dir.path() / "key.pem";
    REQUIRE(shared_identity().save(cert_path, key_path).has_value());
    REQUIRE(TlsIdentity::load(cert_path, key_path).has_value());

    SECTION("garbage certificate")
    {
        std::ofstream(cert_path) << "not a certificate\n";
        CHECK_FALSE(TlsIdentity::load(cert_path, key_path).has_value());
    }
    SECTION("garbage key")
    {
        std::ofstream(key_path) << "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n";
        CHECK_FALSE(TlsIdentity::load(cert_path, key_path).has_value());
    }
    SECTION("missing file")
    {
        CHECK_FALSE(TlsIdentity::load(dir.path() / "missing.pem", key_path).has_value());
    }
}
