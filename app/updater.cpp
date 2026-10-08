// 自動更新 helpers (see updater.h).
#include "updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <cstdio>
#include <memory>
#include <vector>

namespace pm::update {
namespace {

std::string winErr(const char* what, DWORD code = GetLastError()) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s failed (error %lu)", what, static_cast<unsigned long>(code));
    return buf;
}

struct HInternet {
    HINTERNET h = nullptr;
    HInternet() = default;
    explicit HInternet(HINTERNET x) : h(x) {}
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
    ~HInternet() {
        if (h) WinHttpCloseHandle(h);
    }
    explicit operator bool() const { return h != nullptr; }
};

// Opens a GET request and waits for the response headers. `sink(data, n)`
// receives the body; returns false to abort.
template <class Sink>
bool request(const std::wstring& url, std::string& error, unsigned long long& total, Sink&& sink) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        error = "bad URL";
        return false;
    }
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    HInternet session(WinHttpOpen(L"ZizaiProjection-Updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = winErr("WinHttpOpen");
        return false;
    }
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    // GitHub release assets: github.com 302 -> release-assets.githubusercontent.com
    // (HTTPS -> HTTPS, other host). Followed automatically; never down to HTTP.
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    // TLS 1.2+ only (WinHTTP's default on Windows 10 includes 1.2).
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS1_3 */;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    HInternet conn(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!conn) {
        error = winErr("WinHttpConnect");
        return false;
    }
    const std::wstring object = std::wstring(path) + extra;
    HInternet req(WinHttpOpenRequest(conn.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!req) {
        error = winErr("WinHttpOpenRequest");
        return false;
    }
    // Never serve the manifest from a cache.
    const wchar_t* hdr = L"Cache-Control: no-cache\r\nPragma: no-cache";
    if (!WinHttpSendRequest(req.h, hdr, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        error = winErr("HTTP request");
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = "HTTP status " + std::to_string(status);
        return false;
    }
    wchar_t clen[32] = {};
    DWORD clenBytes = sizeof(clen);
    total = 0;
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &clenBytes,
                            WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(clen, nullptr, 10);
    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            error = winErr("WinHttpReadData");
            return false;
        }
        if (got == 0) return true;
        if (!sink(buf.data(), static_cast<size_t>(got))) {
            if (error.empty()) error = "aborted";
            return false;
        }
    }
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Parses a JSON string starting at json[i] == '"'. Returns the index after
// the closing quote, or npos on error.
size_t parseString(const std::string& json, size_t i, std::string& out) {
    out.clear();
    if (i >= json.size() || json[i] != '"') return std::string::npos;
    for (++i; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') return i + 1;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= json.size()) return std::string::npos;
        switch (json[i]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            auto hex4 = [&](size_t at, unsigned& v) {
                if (at + 4 > json.size()) return false;
                v = 0;
                for (size_t k = 0; k < 4; ++k) {
                    const int d = hexVal(json[at + k]);
                    if (d < 0) return false;
                    v = v * 16 + static_cast<unsigned>(d);
                }
                return true;
            };
            unsigned cp = 0;
            if (!hex4(i + 1, cp)) return std::string::npos;
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < json.size() && json[i + 1] == '\\' && json[i + 2] == 'u') {
                unsigned lo = 0;
                if (hex4(i + 3, lo) && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            appendUtf8(out, cp);
            break;
        }
        default: return std::string::npos;
        }
    }
    return std::string::npos;
}

}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        auto part = [](const std::string& s, size_t& p) {
            unsigned long long v = 0;
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) v = v * 10 + (s[p++] - '0');
            while (p < s.size() && s[p] != '.') ++p;  // ignore suffixes like "-beta"
            if (p < s.size()) ++p;
            return v;
        };
        const unsigned long long x = part(a, i), y = part(b, j);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::string jsonString(const std::string& json, const std::string& key) {
    // Walk the top-level object: "key" : value pairs; skip non-string values.
    size_t i = json.find('{');
    if (i == std::string::npos) return {};
    ++i;
    int depth = 0;
    std::string k, v;
    while (i < json.size()) {
        const char c = json[i];
        if (c == '"') {
            const size_t after = parseString(json, i, k);
            if (after == std::string::npos) return {};
            i = after;
            if (depth != 0) continue;
            while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
            if (i >= json.size() || json[i] != ':') continue;  // a string value, not a key
            ++i;
            while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
            if (i < json.size() && json[i] == '"') {
                const size_t end = parseString(json, i, v);
                if (end == std::string::npos) return {};
                if (k == key) return v;
                i = end;
            }
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            if (depth == 0) break;
            --depth;
        }
        ++i;
    }
    return {};
}

namespace {
// Index of the value of top-level `key` (first non-space after the colon),
// npos if missing. The same walk as jsonString.
size_t findValue(const std::string& json, const std::string& key) {
    size_t i = json.find('{');
    if (i == std::string::npos) return std::string::npos;
    ++i;
    int depth = 0;
    std::string k;
    while (i < json.size()) {
        const char c = json[i];
        if (c == '"') {
            const size_t after = parseString(json, i, k);
            if (after == std::string::npos) return std::string::npos;
            i = after;
            if (depth != 0) continue;
            size_t j = i;
            while (j < json.size() && std::isspace(static_cast<unsigned char>(json[j]))) ++j;
            if (j >= json.size() || json[j] != ':') continue;  // a string value, not a key
            ++j;
            while (j < json.size() && std::isspace(static_cast<unsigned char>(json[j]))) ++j;
            if (k == key) return j < json.size() ? j : std::string::npos;
            i = j;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            if (depth == 0) break;
            --depth;
        }
        ++i;
    }
    return std::string::npos;
}
}  // namespace

std::vector<std::string> jsonStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> out;
    size_t i = findValue(json, key);
    if (i == std::string::npos || json[i] != '[') return out;
    ++i;
    int depth = 0;
    std::string v;
    while (i < json.size()) {
        const char c = json[i];
        if (c == '"') {
            const size_t end = parseString(json, i, v);
            if (end == std::string::npos) break;
            if (depth == 0) out.push_back(v);
            i = end;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            if (depth == 0) break;
            --depth;
        }
        ++i;
    }
    return out;
}

unsigned long long jsonNumber(const std::string& json, const std::string& key) {
    size_t i = findValue(json, key);
    if (i == std::string::npos) return 0;
    if (json[i] == '"') ++i;
    unsigned long long v = 0;
    while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i])) && v < (1ull << 56))
        v = v * 10 + static_cast<unsigned>(json[i++] - '0');
    return v;
}

void parseManifest(std::string body, Manifest& m) {
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF) body.erase(0, 3);  // UTF-8 BOM
    m.version = jsonString(body, "version");
    m.url = jsonString(body, "url");
    m.sha256 = jsonString(body, "sha256");
    m.notes = jsonString(body, "notes");
    m.date = jsonString(body, "date");
    m.size = jsonNumber(body, "size");
    m.changesZh = jsonStringArray(body, "changes_zh");
    m.changesEn = jsonStringArray(body, "changes_en");
    m.changesJa = jsonStringArray(body, "changes_ja");
    m.changesKo = jsonStringArray(body, "changes_ko");
    for (std::vector<std::string>* v : {&m.changesZh, &m.changesEn, &m.changesJa, &m.changesKo}) {
        v->erase(std::remove_if(v->begin(), v->end(),
                                [](const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }),
                 v->end());
        if (v->size() > 40) v->resize(40);
    }
}

bool readManifestFile(const std::wstring& file, Manifest& m) {
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string body(64 * 1024, '\0');
    DWORD got = 0;
    const bool ok = ReadFile(f, body.data(), static_cast<DWORD>(body.size()), &got, nullptr) != FALSE;
    CloseHandle(f);
    if (!ok || got == 0) return false;
    body.resize(got);
    parseManifest(std::move(body), m);
    return true;
}

bool httpGet(const std::wstring& url, std::string& body, std::string& error, size_t maxBytes) {
    body.clear();
    unsigned long long total = 0;
    return request(url, error, total, [&](const char* d, size_t n) {
        if (body.size() + n > maxBytes) {
            error = "response too large";
            return false;
        }
        body.append(d, n);
        return true;
    });
}

bool fetchManifest(const std::wstring& url, Manifest& m, std::string& error) {
    std::string body;
    if (!httpGet(url, body, error, 64 * 1024)) return false;
    m = {};
    parseManifest(std::move(body), m);
    if (m.version.empty() || m.url.empty() || m.sha256.size() != 64) {
        error = "manifest incomplete (version / url / sha256)";
        return false;
    }
    for (char& c : m.sha256) {
        if (hexVal(c) < 0) {
            error = "manifest sha256 is not hex";
            return false;
        }
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return true;
}

bool download(const std::wstring& url, const std::wstring& file, std::string& error,
              void (*progress)(unsigned long long, unsigned long long, void*), void* ctx) {
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = winErr("CreateFile");
        return false;
    }
    unsigned long long total = 0, done = 0;
    constexpr unsigned long long kMax = 512ull << 20;  // an installer is ~5 MB; refuse anything absurd
    const bool ok = request(url, error, total, [&](const char* d, size_t n) {
        DWORD wrote = 0;
        if (!WriteFile(f, d, static_cast<DWORD>(n), &wrote, nullptr) || wrote != n) {
            error = winErr("WriteFile");
            return false;
        }
        done += n;
        if (done > kMax) {
            error = "download too large";
            return false;
        }
        if (progress) progress(done, total, ctx);
        return true;
    });
    CloseHandle(f);
    if (ok && total && done != total) {
        error = "download truncated";
        DeleteFileW(file.c_str());
        return false;
    }
    if (!ok) DeleteFileW(file.c_str());
    return ok;
}

bool installerVersion(const std::wstring& file, std::string& version) {
    version.clear();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (size == 0) return false;
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return false;
    struct LangCp {
        WORD lang, cp;
    };
    std::vector<LangCp> langs;
    LangCp* tr = nullptr;
    UINT trLen = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &trLen) && tr)
        langs.assign(tr, tr + trLen / sizeof(LangCp));
    langs.push_back({0x0000, 0x04B0});  // Inno Setup writes "000004b0"
    langs.push_back({0x0409, 0x04B0});
    langs.push_back({0x0404, 0x04B0});
    auto value = [&](const wchar_t* key) -> std::wstring {
        for (const LangCp& l : langs) {
            wchar_t path[96];
            swprintf_s(path, L"\\StringFileInfo\\%04x%04x\\%s", l.lang, l.cp, key);
            wchar_t* v = nullptr;
            UINT len = 0;
            if (VerQueryValueW(data.data(), path, reinterpret_cast<void**>(&v), &len) && v && len) {
                std::wstring s(v);  // Inno Setup pads its values with spaces
                while (!s.empty() && (s.back() == L' ' || s.back() == L'\0')) s.pop_back();
                return s;
            }
        }
        return {};
    };
    // The installer's VERSIONINFO product name (not a UI string): the release
    // installers say 自在投影 in every language; "Zizai Cast" is accepted too.
    const std::wstring product = value(L"ProductName");
    if (product != L"自在投影" && product != L"Zizai Cast") return false;
    std::wstring v = value(L"ProductVersion");
    if (v.empty()) v = value(L"FileVersion");
    std::string out;
    for (wchar_t c : v) {
        if ((c >= L'0' && c <= L'9') || c == L'.') out += static_cast<char>(c);
        else break;  // "0.5.3 (beta)" -> "0.5.3"
    }
    if (out.empty() || out.front() == '.') {
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi) return false;
        out = std::to_string(HIWORD(fi->dwFileVersionMS)) + "." + std::to_string(LOWORD(fi->dwFileVersionMS)) + "." +
              std::to_string(HIWORD(fi->dwFileVersionLS)) + "." + std::to_string(LOWORD(fi->dwFileVersionLS));
    }
    while (!out.empty() && out.back() == '.') out.pop_back();
    // "0.5.3.0" -> "0.5.3" (keep at least three parts)
    while (std::count(out.begin(), out.end(), '.') > 2 && out.size() > 2 && out.compare(out.size() - 2, 2, ".0") == 0)
        out.resize(out.size() - 2);
    version = out;
    return !version.empty();
}

std::string sha256File(const std::wstring& file) {
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(256 * 1024);
        DWORD got = 0;
        bool ok = true;
        while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0) {
            if (BCryptHashData(hash, buf.data(), got, 0) != 0) {
                ok = false;
                break;
            }
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
            static const char* d = "0123456789abcdef";
            for (unsigned char b : digest) {
                hex += d[b >> 4];
                hex += d[b & 15];
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

}  // namespace pm::update
