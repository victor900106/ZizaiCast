// 自在投影 app: cloudflared add-on download (see cloudflared_addon.h).
#include "remote/cloudflared_addon.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "downloader.h"  // translate/src (the same downloader as the llama / OCR add-ons)

namespace pm_app::cfaddon {
namespace dl = pm::translate::dl;
namespace {

constexpr char kMirrorHost[] = "victor900106.github.io";
constexpr char kMirrorDir[] = "/ZizaiCast/addons/";

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string sha256File(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD got = 0;
        while (ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) BCryptHashData(hash, buf.data(), got, 0);
        unsigned char d[32] = {};
        if (BCryptFinishHash(hash, d, sizeof d, 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", d[i]);
            out.assign(hex, 64);
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return out;
}

// Test builds (PM_DL_TEST_HTTP): hosts "127.0.0.1:PORT" and, for a small stand-in file, the exe pin.
#ifdef PM_DL_TEST_HTTP
std::string envNarrow(const wchar_t* name) {
    wchar_t b[300] = {};
    const DWORD n = GetEnvironmentVariableW(name, b, 300);
    return n > 0 && n < 300 ? narrow(b) : std::string();
}
#endif
std::string mirrorHost() {
#ifdef PM_DL_TEST_HTTP
    if (auto e = envNarrow(L"PM_CF_TEST_MIRROR"); !e.empty()) return e;
#endif
    return kMirrorHost;
}
std::string originHost(const char* host) {
#ifdef PM_DL_TEST_HTTP
    if (auto e = envNarrow(L"PM_CF_TEST_ORIGIN"); !e.empty()) return e;
#endif
    return host;
}
struct Pin {
    uint64_t size;
    std::string sha;
};
Pin exePin() {
    Pin p{kExeSize, kExeSha256};
#ifdef PM_DL_TEST_HTTP
    if (auto e = envNarrow(L"PM_CF_TEST_PIN"); !e.empty()) {  // "<size>:<sha256>"
        const size_t c = e.find(':');
        if (c != std::string::npos) p = {std::strtoull(e.c_str(), nullptr, 10), e.substr(c + 1)};
    }
#endif
    return p;
}

std::mutex gMu;
std::wstring gHashedPath;
uint64_t gHashedSize = 0;
FILETIME gHashedTime{};
bool gHashedOk = false;

}  // namespace

std::wstring dirIn(const std::wstring& root) { return root + L"\\cloudflared"; }

std::wstring installedExe(const std::wstring& root) {
    const std::wstring exe = dirIn(root) + L"\\cloudflared.exe";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &a)) return {};
    const uint64_t size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    const Pin pin = exePin();
    if (size != pin.size) return {};
    std::lock_guard<std::mutex> lk(gMu);
    if (gHashedPath == exe && gHashedSize == size && CompareFileTime(&gHashedTime, &a.ftLastWriteTime) == 0)
        return gHashedOk ? exe : std::wstring();
    gHashedOk = sha256File(exe) == pin.sha;
    gHashedPath = exe, gHashedSize = size, gHashedTime = a.ftLastWriteTime;
    return gHashedOk ? exe : std::wstring();
}

bool download(const std::wstring& root, const std::function<void(const Progress&)>& progress, const std::atomic<bool>* cancel,
              std::wstring* err) {
    const std::wstring dir = dirIn(root);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const Pin pin = exePin();
    std::vector<dl::Item> items;
    {
        dl::Item it;
        it.host = originHost("github.com");
        it.path = std::string("/cloudflare/cloudflared/releases/download/") + kVersion + "/cloudflared-windows-amd64.exe";
        it.dest = dir + L"\\cloudflared.exe";
        it.size = pin.size;
        it.sha256 = pin.sha;
        it.mirrors.push_back({mirrorHost(), std::string(kMirrorDir) + kMirrorExeName});
        it.label = "cloudflared";
        it.priority = 1;
        items.push_back(it);
    }
    {
        dl::Item it;
        it.host = originHost("raw.githubusercontent.com");
        it.path = std::string("/cloudflare/cloudflared/") + kVersion + "/LICENSE";
        it.dest = dir + L"\\LICENSE";
        it.size = kLicenseSize;
        it.sha256 = kLicenseSha256;
        it.mirrors.push_back({mirrorHost(), std::string(kMirrorDir) + kMirrorLicenseName});
        it.label = "license";
        it.priority = 1;
        items.push_back(it);
    }
    dl::Options o;
    o.userAgent = L"ZizaiCast/0.8 (add-on downloader)";
    const auto res = dl::fetchAll(
        items,
        [&](const dl::Progress& p) {
            if (!progress) return;
            Progress q;
            q.done = p.done, q.total = p.total, q.bytesPerSec = p.bytesPerSec, q.secondsLeft = p.secondsLeft, q.verifying = p.verifying;
            progress(q);
        },
        cancel, o);
    for (const auto& r : res) {
        if (r.status == dl::Status::Ok) continue;
        if (err) *err = r.status == dl::Status::Cancelled ? L"cancelled"
                        : r.status == dl::Status::Verify  ? L"SHA-256 mismatch"
                        : r.status == dl::Status::Disk    ? L"disk: " + r.detail
                                                          : (r.http ? L"HTTP " + std::to_wstring(r.http) : r.detail);
        return false;
    }
    std::ofstream v(dir + L"\\VERSION.txt", std::ios::binary | std::ios::trunc);
    v << "cloudflared " << kVersion << " (cloudflared-windows-amd64.exe, SHA-256 " << pin.sha << ")\r\n"
      << "Source: https://github.com/cloudflare/cloudflared/tree/" << kVersion << "\r\n";
    return true;
}

}  // namespace pm_app::cfaddon
