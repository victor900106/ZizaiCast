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
#include <string>
#include <vector>

#include "pm/translate.h"
#include "downloader.h"
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
    // Every missing file at once (downloader.h: parallel Range connections,
    // resumable .part, SHA-256, cancel within ~0.1 s).
    std::vector<const ModelFile*> todo;
    std::vector<dl::Item> items;
    auto itemFor = [](const ModelFile& f, const std::string& loc) {
        const bool ocr = std::string(f.pair) == kOcrPair;
        dl::Item it;
        it.host = toUtf8(ocr ? kOcrHost : kHost);
        it.path = ocr ? loc : kPath + loc;
        it.dest = pairDir(f.pair) + L"\\" + fromUtf8(f.name);
        it.size = f.size;
        it.sha256 = f.sha256;
        return it;
    };
    for (const auto& f : kFiles)
        if (std::find(pairs.begin(), pairs.end(), f.pair) != pairs.end() && !fileOk(f)) {
            makeDirs(pairDir(f.pair));
            todo.push_back(&f);
            items.push_back(itemFor(f, f.location));
        }
    if (todo.empty()) return true;
    dl::Options o;
    o.userAgent = L"ZizaiCast/0.7 (model downloader)";
    auto relay = [&](const dl::Progress& p) {
        if (progress) progress(p.fraction());
    };
    std::vector<dl::Result> res = dl::fetchAll(items, relay, cancel, o);
    if (cancel && cancel->load()) {
        if (err) *err = L"cancelled";
        return false;
    }
    // Republished Bergamot files (404 / 403): same file (same hash), new location.
    std::vector<size_t> again;
    std::vector<dl::Item> moved;
    for (size_t i = 0; i < todo.size(); ++i)
        if (res[i].status == dl::Status::Network && (res[i].http == 404 || res[i].http == 403) &&
            std::string(todo[i]->pair) != kOcrPair) {
            const std::string loc = relocate(*todo[i]);
            if (!loc.empty() && loc != todo[i]->location) {
                again.push_back(i);
                moved.push_back(itemFor(*todo[i], loc));
            }
        }
    if (!moved.empty()) {
        const std::vector<dl::Result> r2 = dl::fetchAll(moved, relay, cancel, o);
        for (size_t k = 0; k < again.size(); ++k) res[again[k]] = r2[k];
        if (cancel && cancel->load()) {
            if (err) *err = L"cancelled";
            return false;
        }
    }
    bool ok = true;
    for (size_t i = 0; i < todo.size(); ++i) {
        if (res[i].status != dl::Status::Ok) {
            // "SHA-256": the caller maps it to TrVerifyFailed.
            if (ok && err) *err = res[i].status == dl::Status::Verify ? L"SHA-256" : res[i].detail;
            ok = false;
            continue;
        }
        // Marker: verified (installed() then only checks sizes, no re-hash).
        std::ofstream(items[i].dest + L".sha256ok") << todo[i]->sha256 << "\n";
    }
    if (progress) progress(1.0);
    return ok;
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

// ---- OCR GPU add-on (ocr_gpu.inc) ----

namespace {

struct GpuFile {
    const char* name;
    const char* path;   // on api.nuget.org
    const char* entry;  // zip entry in the package
    uint64_t offset, length;
    const char* rangeSha;
    uint32_t crc, csize;
    uint64_t size;
    const char* sha;
    uint16_t time, date;
};
constexpr GpuFile kGpuFiles[] = {
#define PM_OCR_GPU_FILE(name, path, entry, off, len, rsha, crc, cs, sz, sha, t, d) {name, path, entry, off, len, rsha, crc, cs, sz, sha, t, d},
#include "ocr_gpu.inc"
#undef PM_OCR_GPU_FILE
};
constexpr wchar_t kNugetHost[] = L"api.nuget.org";

bool gpuFileOk(const GpuFile& f) {
    uint64_t sz = 0;
    const std::wstring p = ModelStore::ocrGpuDir() + L"\\" + fromUtf8(f.name);
    return fileSize(p, sz) && sz == f.size && GetFileAttributesW((p + L".sha256ok").c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string sha256Of(const std::wstring& path) {
    Sha256 sha;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::vector<char> b(1 << 20);
    DWORD n = 0;
    while (ReadFile(h, b.data(), static_cast<DWORD>(b.size()), &n, nullptr) && n) sha.add(b.data(), n);
    CloseHandle(h);
    return sha.hex();
}

// The zip entry's bytes [offset, offset + length) of https://api.nuget.org<path>,
// as a downloader item (resumable, verified against rangeSha).
dl::Item gpuItem(const GpuFile& f) {
    dl::Item it;
    it.host = toUtf8(kNugetHost);
    it.path = f.path;
    it.dest = ModelStore::ocrGpuDir() + L"\\" + fromUtf8(f.name) + L".entry";
    it.size = f.length;
    it.sha256 = f.rangeSha;
    it.offset = f.offset;
    return it;
}

void put16(std::string& s, uint32_t v) {
    s += static_cast<char>(v & 255);
    s += static_cast<char>((v >> 8) & 255);
}
void put32(std::string& s, uint32_t v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

// The fetched zip entry as a one-file zip (its own name, a central
// directory), unpacked by Windows' tar.exe into dir.
bool unpackEntry(const GpuFile& f, const std::wstring& part, const std::wstring& dir, std::wstring* err) {
    std::ifstream in(part, std::ios::binary);
    std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (blob.size() != f.length || blob.size() < 30 || blob.compare(0, 4, std::string("PK\x03\x04", 4)) != 0) {
        if (err) *err = L"SHA-256";
        return false;
    }
    const size_t nl = static_cast<uint8_t>(blob[26]) | (static_cast<size_t>(static_cast<uint8_t>(blob[27])) << 8);
    const size_t el = static_cast<uint8_t>(blob[28]) | (static_cast<size_t>(static_cast<uint8_t>(blob[29])) << 8);
    const std::string name = f.name;
    std::string zip("PK\x03\x04", 4);
    put16(zip, 20);
    put16(zip, 0);
    put16(zip, 8);
    put16(zip, f.time);
    put16(zip, f.date);
    put32(zip, f.crc);
    put32(zip, f.csize);
    put32(zip, static_cast<uint32_t>(f.size));
    put16(zip, static_cast<uint32_t>(name.size()));
    put16(zip, 0);
    zip += name;
    zip.append(blob, 30 + nl + el, std::string::npos);
    const uint32_t cdAt = static_cast<uint32_t>(zip.size());
    std::string cd("PK\x01\x02", 4);
    put16(cd, 20);
    put16(cd, 20);
    put16(cd, 0);
    put16(cd, 8);
    put16(cd, f.time);
    put16(cd, f.date);
    put32(cd, f.crc);
    put32(cd, f.csize);
    put32(cd, static_cast<uint32_t>(f.size));
    put16(cd, static_cast<uint32_t>(name.size()));
    for (int k = 0; k < 4; ++k) put16(cd, 0);  // extra, comment, disk, internal attributes
    put32(cd, 0);                              // external attributes
    put32(cd, 0);                              // local header offset
    cd += name;
    zip += cd;
    zip.append("PK\x05\x06", 4);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, 1);
    put16(zip, 1);
    put32(zip, static_cast<uint32_t>(cd.size()));
    put32(zip, cdAt);
    put16(zip, 0);
    const std::wstring zipPath = dir + L"\\" + fromUtf8(f.name) + L".zip";
    std::ofstream(zipPath, std::ios::binary) << zip;
    wchar_t sys[MAX_PATH];
    const UINT sn = GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tar = std::wstring(sys, sn) + L"\\tar.exe";
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zipPath + L"\" -C \"" + dir + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    DWORD exitCode = 1;
    if (CreateProcessW(tar.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                       dir.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 120000);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    DeleteFileW(zipPath.c_str());
    if (exitCode != 0) {
        if (err) *err = L"tar.exe could not unpack " + fromUtf8(f.name) + L" (Windows 10 1803 or later is needed)";
        return false;
    }
    return true;
}

}  // namespace

std::wstring ModelStore::ocrGpuDir() { return root() + L"\\ocr-gpu"; }

bool ModelStore::ocrGpuInstalled() {
    for (const auto& f : kGpuFiles)
        if (!gpuFileOk(f)) return false;
    return true;
}

uint64_t ModelStore::ocrGpuMissingBytes() {
    uint64_t n = 0;
    for (const auto& f : kGpuFiles)
        if (!gpuFileOk(f)) {
            const dl::Item it = gpuItem(f);
            uint64_t have = 0;
            if (fileSize(it.dest, have) && have == f.length) continue;  // fetched, not unpacked yet
            n += f.length - std::min<uint64_t>(dl::partialBytes(it), f.length);
        }
    return n;
}

bool ModelStore::downloadOcrGpu(const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::wstring* err) {
    const std::wstring dir = ocrGpuDir();
    makeDirs(dir);
    std::vector<const GpuFile*> todo;
    std::vector<dl::Item> items;
    for (const auto& f : kGpuFiles)
        if (!gpuFileOk(f)) {
            todo.push_back(&f);
            items.push_back(gpuItem(f));
        }
    if (todo.empty()) return true;
    dl::Options o;
    o.userAgent = L"ZizaiCast/0.7 (model downloader)";
    const std::vector<dl::Result> res = dl::fetchAll(items, [&](const dl::Progress& p) {
        if (progress) progress(p.fraction());
    }, cancel, o);
    if (cancel && cancel->load()) {
        if (err) *err = L"cancelled";
        return false;
    }
    for (size_t i = 0; i < todo.size(); ++i) {
        const GpuFile& f = *todo[i];
        if (res[i].status != dl::Status::Ok) {
            if (err) *err = res[i].status == dl::Status::Verify ? L"SHA-256" : res[i].detail;
            return false;
        }
        const std::wstring final = dir + L"\\" + fromUtf8(f.name), entry = items[i].dest;
        DeleteFileW(final.c_str());
        if (!unpackEntry(f, entry, dir, err)) return false;
        uint64_t sz = 0;
        if (!fileSize(final, sz) || sz != f.size || sha256Of(final) != f.sha) {
            DeleteFileW(final.c_str());
            DeleteFileW(entry.c_str());
            if (err) *err = L"SHA-256";
            return false;
        }
        std::ofstream(final + L".sha256ok") << f.sha << "\n";
        DeleteFileW(entry.c_str());
    }
    if (progress) progress(1.0);
    return true;
}

}  // namespace pm::translate
