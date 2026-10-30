#pragma once

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The few primitives pairing and encrypted sessions need (protocol/PROTOCOL.md),
// all from Windows itself (CNG and DPAPI), no third-party code.
namespace sst::crypto {

using Bytes = std::vector<uint8_t>;
using Key32 = std::array<uint8_t, 32>;
using PublicKey = std::array<uint8_t, 65>;  // 04 || X || Y

Bytes Random(size_t n);
Key32 Sha256(const Bytes& data);
Key32 HmacSha256(const uint8_t* key, size_t keyLen, const Bytes& data);
Bytes Hkdf(const Bytes& salt, const Bytes& ikm, const std::string& info, size_t length);
// Compares without leaking where the first difference is.
bool Equal(const uint8_t* a, const uint8_t* b, size_t n);

// An ECDH P-256 key pair.
class EcdhKey {
public:
    EcdhKey();  // a fresh random key
    ~EcdhKey();
    EcdhKey(const EcdhKey&) = delete;
    EcdhKey& operator=(const EcdhKey&) = delete;

    // For tests: a known private key (32 bytes, big-endian) and its public key.
    static bool FromPrivate(const uint8_t d[32], const PublicKey& pub, EcdhKey& out);

    bool Ok() const { return key_ != nullptr; }
    const PublicKey& Public() const { return pub_; }
    // The shared secret Z (X coordinate, big-endian); false if `peer` isn't a valid P-256 point.
    bool Agree(const PublicKey& peer, Key32& z) const;

private:
    BCRYPT_KEY_HANDLE key_ = nullptr;
    PublicKey pub_{};
};

// AES-256-GCM for one direction of a session: the nonce is `direction`
// followed by a message counter, so it never repeats and is never sent.
class SealKey {
public:
    SealKey(const uint8_t key[32], uint32_t direction);
    ~SealKey();
    SealKey(const SealKey&) = delete;
    SealKey& operator=(const SealKey&) = delete;

    bool Ok() const { return key_ != nullptr; }
    // ciphertext || 16-byte tag
    bool Seal(const uint8_t* plain, size_t size, Bytes& out);
    bool Open(const uint8_t* sealed, size_t size, Bytes& out);

private:
    void Nonce(uint8_t out[12]);
    BCRYPT_KEY_HANDLE key_ = nullptr;
    uint32_t direction_;
    uint64_t counter_ = 0;
};

// Encrypts data so only this Windows user can read it back (DPAPI).
std::string Protect(const Bytes& data);  // base64
bool Unprotect(const std::string& base64, Bytes& data);

std::string Hex(const uint8_t* data, size_t n);

// Known-answer tests (RFC 4231, RFC 5869, NIST P-256 CDH) and round trips.
bool SelfTest(std::string& report);

}  // namespace sst::crypto
