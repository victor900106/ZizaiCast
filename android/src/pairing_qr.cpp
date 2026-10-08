#include "pairing_qr.h"

#include <windows.h>
#include <bcrypt.h>

#include "qrcodegen.hpp"

namespace pm::pairing {

static std::string randomAlnum(size_t n) {
    static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";  // no 0/O/1/l/I
    const size_t k = sizeof(kAlphabet) - 1;
    std::string s;
    while (s.size() < n) {
        uint8_t b[32];
        BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        for (uint8_t x : b) {
            if (x >= 256 - (256 % k)) continue;  // no modulo bias
            s.push_back(kAlphabet[x % k]);
            if (s.size() == n) break;
        }
    }
    return s;
}

Credentials randomCredentials() {
    Credentials c;
    c.service = "zizai-" + randomAlnum(10);
    c.password = randomAlnum(12);
    return c;
}

bool qrModules(const std::string& text, std::vector<std::vector<bool>>& m) {
    try {
        auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        int n = qr.getSize();
        m.assign(size_t(n), std::vector<bool>(size_t(n)));
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) m[size_t(y)][size_t(x)] = qr.getModule(x, y);
        return true;
    } catch (...) {
        return false;
    }
}

bool renderQr(const std::string& text, int scale, int border, std::vector<uint8_t>& bgra, int& size) {
    std::vector<std::vector<bool>> m;
    if (scale < 1 || border < 0 || !qrModules(text, m)) return false;
    int n = int(m.size());
    size = (n + 2 * border) * scale;
    bgra.assign(size_t(size) * size_t(size) * 4, 0xFF);  // white, opaque
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            if (!m[size_t(y)][size_t(x)]) continue;
            for (int dy = 0; dy < scale; ++dy) {
                uint8_t* row = &bgra[(size_t((y + border) * scale + dy) * size_t(size) + size_t((x + border) * scale)) * 4];
                for (int dx = 0; dx < scale; ++dx) {
                    row[dx * 4 + 0] = row[dx * 4 + 1] = row[dx * 4 + 2] = 0;  // black, alpha stays 255
                }
            }
        }
    return true;
}

}  // namespace pm::pairing
