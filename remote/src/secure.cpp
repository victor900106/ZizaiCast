// Password hashing (PBKDF2-HMAC-SHA256) and HMAC tokens for the fixed link
// (常駐外網, 0.8.2). Windows CNG (bcrypt), no extra library.
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <string>
#include <vector>

#include "pm/remote.h"
#include "util.h"

namespace pm::remote {

namespace {

std::string hex(const unsigned char* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15];
    return s;
}

bool unhex(const std::string& s, std::vector<unsigned char>& out) {
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        const int a = v(s[i]), b = v(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<unsigned char>(a * 16 + b));
    }
    return true;
}

bool pbkdf2(const std::string& pw, const std::vector<unsigned char>& salt, ULONGLONG iter, unsigned char* out, ULONG outLen) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return false;
    const NTSTATUS st = BCryptDeriveKeyPBKDF2(alg, reinterpret_cast<PUCHAR>(const_cast<char*>(pw.data())), ULONG(pw.size()),
                                              const_cast<PUCHAR>(salt.data()), ULONG(salt.size()), iter, out, outLen, 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    return st == 0;
}

}  // namespace

std::string hmacSha256Hex(const std::string& key, const std::string& msg) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    unsigned char mac[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return {};
    BCryptHash(alg, reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), ULONG(key.size()), reinterpret_cast<PUCHAR>(const_cast<char*>(msg.data())),
               ULONG(msg.size()), mac, sizeof(mac));
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex(mac, sizeof(mac));
}

std::string randomHex(size_t bytes) {
    std::vector<unsigned char> b(bytes);
    BCryptGenRandom(nullptr, b.data(), ULONG(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return hex(b.data(), b.size());
}

std::string hashPassword(const std::string& password, int iterations) {
    std::vector<unsigned char> salt(16);
    BCryptGenRandom(nullptr, salt.data(), ULONG(salt.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    unsigned char dk[32];
    if (!pbkdf2(password, salt, ULONGLONG(iterations), dk, sizeof(dk))) return {};
    return "pbkdf2-sha256$" + std::to_string(iterations) + "$" + hex(salt.data(), salt.size()) + "$" + hex(dk, sizeof(dk));
}

bool verifyPassword(const std::string& password, const std::string& stored) {
    // pbkdf2-sha256$<iterations>$<salt hex>$<key hex>
    const size_t a = stored.find('$'), b = stored.find('$', a + 1), c = stored.find('$', b + 1);
    if (a == std::string::npos || b == std::string::npos || c == std::string::npos || stored.substr(0, a) != "pbkdf2-sha256") return false;
    const long long iter = atoll(stored.substr(a + 1, b - a - 1).c_str());
    std::vector<unsigned char> salt, want;
    if (iter < 1000 || iter > 10000000 || !unhex(stored.substr(b + 1, c - b - 1), salt) || !unhex(stored.substr(c + 1), want) || want.size() != 32) return false;
    unsigned char dk[32];
    if (!pbkdf2(password, salt, ULONGLONG(iter), dk, sizeof(dk))) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < 32; ++i) d |= static_cast<unsigned char>(dk[i] ^ want[i]);
    return d == 0;
}

}  // namespace pm::remote
