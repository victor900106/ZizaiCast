// Firefox Translations model files: pinned manifest (models.inc), download
// over HTTPS (WinHTTP) with SHA-256 verification (BCrypt), Bergamot configs.
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>

#include "pm/translate.h"
#include "text_util.h"

namespace pm::translate {

namespace {

constexpr char kOcrPair[] = "ocr";  // PaddleOCR text recognition models (ocr_models.inc)
constexpr wchar_t kOcrHost[] = L"www.modelscope.cn";

struct ModelFile {
    const char* pair;
    const char* type;  // model, lex, vocab, srcvocab, trgvocab
    const char* name;
    uint64_t size;
    const char* sha256;
    const char* location;  // under kCdn (Bergamot) / full path on kOcrHost (pair "ocr")
};
constexpr ModelFile kFiles[] = {
#define PM_MODEL_FILE(pair, type, name, size, sha, loc) {pair, type, name, size, sha, loc},
#include "models.inc"
#undef PM_MODEL_FILE
#define PM_OCR_FILE(name, size, sha, path) {kOcrPair, "ocr", name, size, sha, path},
#include "ocr_models.inc"
#undef PM_OCR_FILE
};
constexpr wchar_t kHost[] = L"firefox-settings-attachments.cdn.mozilla.net";
constexpr char kPath[] = "/main-workspace/translations-models/";
constexpr wchar_t kRecordsHost[] = L"firefox.settings.services.mozilla.com";

std::wstring pairDir(const std::string& pair) {
    if (pair == kOcrPair) return ModelStore::root() + L"\\ocr";
    return ModelStore::root() + L"\\bergamot\\" + fromUtf8(pair);
}

bool fileSize(const std::wstring& path, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return true;
}

void makeDirs(const std::wstring& dir) { SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr); }

// Incremental SHA-256 (BCrypt).
class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
            BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    void add(const void* p, size_t n) {
        if (hash_) BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(p)), static_cast<ULONG>(n), 0);
    }
    std::string hex() {
        unsigned char d[32] = {};
        if (!hash_ || BCryptFinishHash(hash_, d, sizeof(d), 0) != 0) return {};
        char out[65];
        for (int i = 0; i < 32; ++i) std::snprintf(out + i * 2, 3, "%02x", d[i]);
        return std::string(out, 64);
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

struct HInternet {
    HINTERNET h = nullptr;
    HInternet(HINTERNET x = nullptr) : h(x) {}
    ~HInternet() {
        if (h) WinHttpCloseHandle(h);
    }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
    operator HINTERNET() const { return h; }
};

// GET https://host/path; body chunks to sink (return false to stop).  HTTP status in *status.
bool httpGet(const wchar_t* host, const std::wstring& path, const std::function<bool(const char*, size_t)>& sink,
             DWORD* status, std::wstring* err) {
    HInternet session(WinHttpOpen(L"ZizaiCast-ModelDownloader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        if (err) *err = L"WinHttpOpen failed";
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    HInternet conn(WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0));
    HInternet req(conn ? WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                       : nullptr);
    if (!req || !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        if (err) *err = L"cannot reach " + std::wstring(host) + L" (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    DWORD code = 0, len = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                        &len, WINHTTP_NO_HEADER_INDEX);
    if (status) *status = code;
    if (code != 200) {
        if (err) *err = L"HTTP " + std::to_wstring(code);
        return false;
    }
    std::vector<char> buf(256 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            if (err) *err = L"download interrupted (error " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        if (got == 0) return true;
        if (!sink(buf.data(), got)) {
            if (err && err->empty()) *err = L"cancelled";
            return false;
        }
    }
}

// The file was republished under a new location: find it in the Remote
// Settings records by its (unchanged) hash.
std::string relocate(const ModelFile& f) {
    std::string json;
    const std::wstring path = L"/v1/buckets/main/collections/translations-models/records";
    if (!httpGet(kRecordsHost, path, [&](const char* p, size_t n) {
            json.append(p, n);
            return json.size() < 64u * 1024 * 1024;
        }, nullptr, nullptr))
        return {};
    const size_t at = json.find(std::string("\"") + f.sha256 + "\"");
    if (at == std::string::npos) return {};
    // The attachment object around the hash holds "location": "main-workspace/...".
    const size_t objStart = json.rfind('{', at), objEnd = json.find('}', at);
    if (objStart == std::string::npos || objEnd == std::string::npos) return {};
    const std::string obj = json.substr(objStart, objEnd - objStart);
    const size_t k = obj.find("\"location\"");
    if (k == std::string::npos) return {};
    const size_t q0 = obj.find('"', obj.find(':', k) + 1), q1 = q0 == std::string::npos ? q0 : obj.find('"', q0 + 1);
    if (q1 == std::string::npos) return {};
    std::string loc = obj.substr(q0 + 1, q1 - q0 - 1);
    const std::string prefix = "main-workspace/translations-models/";
    if (loc.rfind(prefix, 0) == 0) loc = loc.substr(prefix.size());
    if (loc.find('/') != std::string::npos || loc.find("..") != std::string::npos) return {};
    return loc;
}

bool fileOk(const ModelFile& f) {
    uint64_t sz = 0;
    return fileSize(pairDir(f.pair) + L"\\" + fromUtf8(f.name), sz) && sz == f.size &&
           GetFileAttributesW((pairDir(f.pair) + L"\\" + fromUtf8(f.name) + L".sha256ok").c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

std::wstring ModelStore::root() {
    wchar_t env[MAX_PATH];
    if (DWORD n = GetEnvironmentVariableW(L"PM_MODELS_DIR", env, MAX_PATH); n > 0 && n < MAX_PATH) return env;
    PWSTR local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = local;
    CoTaskMemFree(local);
    return dir + L"\\PhoneMirror\\models";
}

std::vector<std::string> ModelStore::pairsFor(Lang src, Lang tgt, bool* supported) {
    if (supported) *supported = true;
    if (src == tgt || src == Lang::Unknown) return {};
    if (src == Lang::ZhHans && tgt == Lang::ZhHant) return {};  // character conversion
    auto toEn = [](Lang l) -> std::string {
        switch (l) {
        case Lang::Ja: return "ja-en";
        case Lang::Ko: return "ko-en";
        case Lang::ZhHans: return "zhHans-en";
        case Lang::ZhHant: return "zhHant-en";
        default: return {};
        }
    };
    if (tgt == Lang::En) {
        std::string p = toEn(src);
        if (p.empty() && supported) *supported = false;
        return p.empty() ? std::vector<std::string>{} : std::vector<std::string>{p};
    }
    // en -> X models; other sources pivot through English (X -> en -> Y), as
    // Firefox does.  ja / ko targets: the 日本語 / 한국어 UI (0.7.0).
    const char* fromEn = tgt == Lang::ZhHant ? "en-zhHant" : tgt == Lang::Ja ? "en-ja" : tgt == Lang::Ko ? "en-ko" : nullptr;
    if (fromEn) {
        if (src == Lang::En) return {fromEn};
        std::string p = toEn(src);
        if (!p.empty()) return {p, fromEn};
    }
    if (supported) *supported = false;
    return {};
}

bool ModelStore::installed(const std::string& pair) {
    bool any = false;
    for (const auto& f : kFiles) {
        if (pair != f.pair) continue;
        any = true;
        if (!fileOk(f)) return false;
    }
    return any;
}

uint64_t ModelStore::missingBytes(const std::vector<std::string>& pairs) {
    uint64_t n = 0;
    for (const auto& f : kFiles)
        if (std::find(pairs.begin(), pairs.end(), f.pair) != pairs.end() && !fileOk(f)) n += f.size;
    return n;
}

bool ModelStore::download(const std::vector<std::string>& pairs, const std::function<void(double)>& progress,
                          const std::atomic<bool>* cancel, std::wstring* err) {
    std::vector<const ModelFile*> todo;
    uint64_t total = 0, done = 0;
    for (const auto& f : kFiles)
        if (std::find(pairs.begin(), pairs.end(), f.pair) != pairs.end() && !fileOk(f)) {
            todo.push_back(&f);
            total += f.size;
        }
    for (const ModelFile* f : todo) {
        const std::wstring dir = pairDir(f->pair);
        makeDirs(dir);
        const std::wstring final = dir + L"\\" + fromUtf8(f->name), part = final + L".part";
        auto fetch = [&](const std::string& loc, std::wstring* e, DWORD* status) {
            HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                if (e) *e = L"cannot write " + part;
                return false;
            }
            Sha256 sha;
            uint64_t got = 0;
            const bool ocr = std::string(f->pair) == kOcrPair;
            const bool ok = httpGet(ocr ? kOcrHost : kHost, fromUtf8(ocr ? loc : kPath + loc), [&](const char* p, size_t n) {
                DWORD w = 0;
                if (!WriteFile(h, p, static_cast<DWORD>(n), &w, nullptr) || w != n) return false;
                sha.add(p, n);
                got += n;
                if (progress && total) progress(static_cast<double>(done + std::min<uint64_t>(got, f->size)) / total);
                return !(cancel && cancel->load()) && got <= f->size;
            }, status, e);
            CloseHandle(h);
            if (!ok) {
                DeleteFileW(part.c_str());
                return false;
            }
            if (got != f->size || sha.hex() != f->sha256) {
                DeleteFileW(part.c_str());
                if (e) *e = L"SHA-256";  // caller maps to TrVerifyFailed
                return false;
            }
            return true;
        };
        std::wstring e;
        DWORD status = 0;
        bool ok = fetch(f->location, &e, &status);
        if (!ok && (status == 404 || status == 403) && !(cancel && cancel->load()) && std::string(f->pair) != kOcrPair) {
            // Republished: same file (same hash), new location.
            const std::string loc = relocate(*f);
            if (!loc.empty() && loc != f->location) {
                e.clear();
                ok = fetch(loc, &e, &status);
            }
        }
        if (!ok) {
            if (err) *err = (cancel && cancel->load()) ? L"cancelled" : e;
            return false;
        }
        DeleteFileW(final.c_str());
        if (!MoveFileExW(part.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (err) *err = L"cannot move " + part;
            return false;
        }
        // Marker: verified (installed() then only checks sizes, no re-hash).
        std::ofstream(final + L".sha256ok") << f->sha256 << "\n";
        done += f->size;
        if (progress && total) progress(static_cast<double>(done) / total);
    }
    return true;
}

std::wstring ModelStore::ocrFile(const char* name) {
    for (const auto& f : kFiles)
        if (std::string(f.pair) == kOcrPair && !strcmp(f.name, name)) return fileOk(f) ? pairDir(kOcrPair) + L"\\" + fromUtf8(f.name) : L"";
    return {};
}

std::wstring ModelStore::configPath(const std::string& pair) {
    std::string model, lex, vocab, src, trg;
    for (const auto& f : kFiles) {
        if (pair != f.pair) continue;
        const std::string t = f.type;
        if (t == "model") model = f.name;
        else if (t == "lex") lex = f.name;
        else if (t == "vocab") vocab = f.name;
        else if (t == "srcvocab") src = f.name;
        else if (t == "trgvocab") trg = f.name;
    }
    if (model.empty()) return {};
    if (src.empty()) src = vocab;
    if (trg.empty()) trg = vocab;
    const std::wstring path = pairDir(pair) + L"\\config.yml";
    // Same settings as Firefox: greedy decoding, int8 "alphas" GEMM, lexical shortlist.
    const std::string yml = "relative-paths: true\nmodels:\n- " + model + "\nvocabs:\n- " + src + "\n- " + trg +
                            "\nshortlist:\n- " + lex +
                            "\n- false\nbeam-size: 1\nnormalize: 1.0\nword-penalty: 0\nmax-length-break: 128\n"
                            "mini-batch-words: 1024\nworkspace: 128\nmax-length-factor: 2.0\nskip-cost: true\n"
                            "cpu-threads: 0\nquiet: true\nquiet-translation: true\ngemm-precision: int8shiftAlphaAll\n";
    std::ifstream in(path, std::ios::binary);
    const std::string cur((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (cur != yml) std::ofstream(path, std::ios::binary) << yml;
    return path;
}

}  // namespace pm::translate
