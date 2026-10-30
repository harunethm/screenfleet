#include "crypto.h"

#include <wincrypt.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace sst::crypto {

namespace {

bool Succeeded(NTSTATUS s) { return s >= 0; }

Bytes FromHex(const char* hex) {
    Bytes out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        out.push_back(uint8_t(nib(hex[i]) << 4 | nib(hex[i + 1])));
    }
    return out;
}

Bytes FromString(const char* s) { return Bytes(s, s + strlen(s)); }

}  // namespace

Bytes Random(size_t n) {
    Bytes out(n);
    if (n && !Succeeded(BCryptGenRandom(nullptr, out.data(), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        throw std::runtime_error("no random numbers");
    return out;
}

Key32 Sha256(const Bytes& data) {
    Key32 out{};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()), ULONG(data.size()), out.data(),
               ULONG(out.size()));
    return out;
}

Key32 HmacSha256(const uint8_t* key, size_t keyLen, const Bytes& data) {
    Key32 out{};
    BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, const_cast<PUCHAR>(key), ULONG(keyLen), const_cast<PUCHAR>(data.data()),
               ULONG(data.size()), out.data(), ULONG(out.size()));
    return out;
}

Bytes Hkdf(const Bytes& salt, const Bytes& ikm, const std::string& info, size_t length) {
    Bytes zeros(32, 0);
    const Bytes& s = salt.empty() ? zeros : salt;
    Key32 prk = HmacSha256(s.data(), s.size(), ikm);
    Bytes out, t;
    for (uint8_t i = 1; out.size() < length; ++i) {
        Bytes block = t;
        block.insert(block.end(), info.begin(), info.end());
        block.push_back(i);
        Key32 h = HmacSha256(prk.data(), prk.size(), block);
        t.assign(h.begin(), h.end());
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(length);
    SecureZeroMemory(prk.data(), prk.size());
    return out;
}

bool Equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= uint8_t(a[i] ^ b[i]);
    return diff == 0;
}

// ---------------------------------------------------------------- ECDH

EcdhKey::EcdhKey() {
    if (!Succeeded(BCryptGenerateKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE, &key_, 256, 0)) ||
        !Succeeded(BCryptFinalizeKeyPair(key_, 0))) {
        if (key_) BCryptDestroyKey(key_);
        key_ = nullptr;
        return;
    }
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    ULONG size = 0;
    if (!Succeeded(BCryptExportKey(key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &size, 0)) ||
        size != sizeof(blob)) {
        BCryptDestroyKey(key_);
        key_ = nullptr;
        return;
    }
    pub_[0] = 4;
    std::copy(blob + sizeof(BCRYPT_ECCKEY_BLOB), blob + sizeof(blob), pub_.begin() + 1);
}

EcdhKey::~EcdhKey() {
    if (key_) BCryptDestroyKey(key_);
}

bool EcdhKey::FromPrivate(const uint8_t d[32], const PublicKey& pub, EcdhKey& out) {
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96];
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    header->dwMagic = BCRYPT_ECDH_PRIVATE_P256_MAGIC;
    header->cbKey = 32;
    std::copy(pub.begin() + 1, pub.end(), blob + sizeof(BCRYPT_ECCKEY_BLOB));
    std::copy(d, d + 32, blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    if (out.key_) BCryptDestroyKey(out.key_);
    out.key_ = nullptr;
    if (!Succeeded(BCryptImportKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPRIVATE_BLOB, &out.key_, blob,
                                sizeof(blob), 0))) {
        out.key_ = nullptr;
        SecureZeroMemory(blob, sizeof(blob));
        return false;
    }
    SecureZeroMemory(blob, sizeof(blob));
    out.pub_ = pub;
    return true;
}

bool EcdhKey::Agree(const PublicKey& peer, Key32& z) const {
    if (!key_ || peer[0] != 4) return false;
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    header->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::copy(peer.begin() + 1, peer.end(), blob + sizeof(BCRYPT_ECCKEY_BLOB));
    BCRYPT_KEY_HANDLE peerKey = nullptr;
    // Import checks that the point is on the curve.
    if (!Succeeded(BCryptImportKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peerKey, blob,
                                sizeof(blob), 0))) {
        return false;
    }
    BCRYPT_SECRET_HANDLE secret = nullptr;
    bool ok = Succeeded(BCryptSecretAgreement(key_, peerKey, &secret, 0));
    ULONG size = 0;
    if (ok) ok = Succeeded(BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, z.data(), ULONG(z.size()), &size, 0)) &&
                 size == z.size();
    if (secret) BCryptDestroySecret(secret);
    BCryptDestroyKey(peerKey);
    if (!ok) return false;
    std::reverse(z.begin(), z.end());  // CNG gives the raw secret little-endian
    return true;
}

// ---------------------------------------------------------------- AES-GCM

SealKey::SealKey(const uint8_t key[32], uint32_t direction) : direction_(direction) {
    if (!Succeeded(BCryptGenerateSymmetricKey(BCRYPT_AES_GCM_ALG_HANDLE, &key_, nullptr, 0, const_cast<PUCHAR>(key), 32,
                                       0))) {
        key_ = nullptr;
    }
}

SealKey::~SealKey() {
    if (key_) BCryptDestroyKey(key_);
}

void SealKey::Nonce(uint8_t out[12]) {
    for (int i = 0; i < 4; ++i) out[i] = uint8_t(direction_ >> (24 - 8 * i));
    for (int i = 0; i < 8; ++i) out[4 + i] = uint8_t(counter_ >> (56 - 8 * i));
    ++counter_;
}

bool SealKey::Seal(const uint8_t* plain, size_t size, Bytes& out) {
    if (!key_ || counter_ == UINT64_MAX) return false;
    uint8_t nonce[12];
    Nonce(nonce);
    out.resize(size + 16);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = sizeof(nonce);
    info.pbTag = out.data() + size;
    info.cbTag = 16;
    ULONG written = 0;
    return Succeeded(BCryptEncrypt(key_, const_cast<PUCHAR>(plain), ULONG(size), &info, nullptr, 0, out.data(), ULONG(size),
                            &written, 0)) &&
           written == size;
}

bool SealKey::Open(const uint8_t* sealed, size_t size, Bytes& out) {
    if (!key_ || size < 16 || counter_ == UINT64_MAX) return false;
    uint8_t nonce[12];
    Nonce(nonce);
    size_t plainSize = size - 16;
    out.resize(plainSize);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = sizeof(nonce);
    info.pbTag = const_cast<PUCHAR>(sealed + plainSize);
    info.cbTag = 16;
    ULONG written = 0;
    return Succeeded(BCryptDecrypt(key_, const_cast<PUCHAR>(sealed), ULONG(plainSize), &info, nullptr, 0, out.data(),
                            ULONG(plainSize), &written, 0)) &&
           written == plainSize;
}

// ---------------------------------------------------------------- DPAPI

std::string Protect(const Bytes& data) {
    DATA_BLOB in{DWORD(data.size()), const_cast<BYTE*>(data.data())}, out{};
    if (!CryptProtectData(&in, L"ScreenFleet pairing", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    DWORD len = 0;
    CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::string b64(len, '\0');
    CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &len);
    b64.resize(len);
    LocalFree(out.pbData);
    return b64;
}

bool Unprotect(const std::string& base64, Bytes& data) {
    DWORD len = 0;
    if (base64.empty() || !CryptStringToBinaryA(base64.c_str(), DWORD(base64.size()), CRYPT_STRING_BASE64, nullptr,
                                                &len, nullptr, nullptr)) {
        return false;
    }
    Bytes blob(len);
    CryptStringToBinaryA(base64.c_str(), DWORD(base64.size()), CRYPT_STRING_BASE64, blob.data(), &len, nullptr,
                         nullptr);
    DATA_BLOB in{len, blob.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    data.assign(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

std::string Hex(const uint8_t* data, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 15]);
    }
    return s;
}

// ---------------------------------------------------------------- self-test

bool SelfTest(std::string& report) {
    bool all = true;
    auto check = [&](const char* name, bool ok) {
        report += std::string(ok ? "PASS  " : "FAIL  ") + name + "\n";
        all = all && ok;
    };

    Key32 h = Sha256(FromString("abc"));
    check("SHA-256 (FIPS 180-2 \"abc\")",
          Hex(h.data(), 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    Bytes jefe = FromString("Jefe");
    h = HmacSha256(jefe.data(), jefe.size(), FromString("what do ya want for nothing?"));
    check("HMAC-SHA256 (RFC 4231 case 2)",
          Hex(h.data(), 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    Bytes okm = Hkdf(FromHex("000102030405060708090a0b0c"), Bytes(22, 0x0b),
                     std::string("\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9"), 42);
    check("HKDF-SHA256 (RFC 5869 case 1)",
          Hex(okm.data(), okm.size()) ==
              "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

    // NIST CAVS ECC CDH, P-256, first vector (also checked independently).
    Bytes d = FromHex("7d7dc5f71eb29ddaf80d6214632eeae03d9058af1fb6d22ed80badb62bc1a534");
    PublicKey mine{}, theirs{};
    Bytes mineB = FromHex("04ead218590119e8876b29146ff89ca61770c4edbbf97d38ce385ed281d8a6b230"
                          "28af61281fd35e2fa7002523acc85a429cb06ee6648325389f59edfce1405141");
    Bytes theirsB = FromHex("04700c48f77f56584c5cc632ca65640db91b6bacce3a4df6b42ce7cc838833d287"
                            "db71e509e3fd9b060ddb20ba5c51dcc5948d46fbf640dfe0441782cab85fa4ac");
    std::copy(mineB.begin(), mineB.end(), mine.begin());
    std::copy(theirsB.begin(), theirsB.end(), theirs.begin());
    EcdhKey known;
    Key32 z{};
    bool ecdh = EcdhKey::FromPrivate(d.data(), mine, known) && known.Agree(theirs, z);
    check("ECDH P-256 (NIST CAVS CDH)",
          ecdh && Hex(z.data(), 32) == "46fc62106420ff012e54a434fbdd2d25ccc5852060561e68040dd7778997bd7b");

    EcdhKey a, b;
    Key32 za{}, zb{};
    check("ECDH P-256 fresh keys agree", a.Ok() && b.Ok() && a.Agree(b.Public(), za) && b.Agree(a.Public(), zb) &&
                                             Equal(za.data(), zb.data(), 32));
    PublicKey bad = theirs;
    bad[64] ^= 1;  // no longer on the curve
    check("ECDH rejects a point off the curve", !a.Agree(bad, za));

    // GCM spec (McGrew & Viega) test cases 13 and 14: AES-256, zero key and nonce.
    uint8_t zeroKey[32] = {};
    Bytes out;
    SealKey tc13(zeroKey, 0);
    check("AES-256-GCM (GCM spec case 13)",
          tc13.Seal(nullptr, 0, out) && Hex(out.data(), out.size()) == "530f8afbc74536b9a963b4f1c4cb738b");
    SealKey tc14(zeroKey, 0);
    uint8_t zeros16[16] = {};
    check("AES-256-GCM (GCM spec case 14)",
          tc14.Seal(zeros16, 16, out) &&
              Hex(out.data(), out.size()) == "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919");

    Bytes key = Random(32), msg = FromString("a frame"), sealed, opened;
    SealKey tx(key.data(), 1), rx(key.data(), 1);
    bool round = tx.Seal(msg.data(), msg.size(), sealed) && rx.Open(sealed.data(), sealed.size(), opened) &&
                 opened == msg;
    check("AES-256-GCM round trip", round);
    tx.Seal(msg.data(), msg.size(), sealed);
    sealed[0] ^= 1;
    check("AES-256-GCM rejects a changed message", !rx.Open(sealed.data(), sealed.size(), opened));

    Bytes secret = Random(32), back;
    check("DPAPI round trip", Unprotect(Protect(secret), back) && back == secret);
    return all;
}

}  // namespace sst::crypto
