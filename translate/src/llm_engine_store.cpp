// Downloads of the local LLM engine (llm_engine.h): the pinned llama.cpp CPU
// runtime and the GGUF model, same flow as ModelStore (consent by the caller,
// HTTPS, resumable .part files, SHA-256, .part -> final name, a .sha256ok
// marker so later checks only compare sizes).
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>
#include <dxgi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

#include "downloader.h"
#include "llm_engine.h"
#include "text_util.h"

namespace pm::translate::llm {

namespace {

struct RuntimeArchive {
    const char* name;
    uint64_t size;
    const char* sha256;
    const char* host;
    const char* path;
};
struct RuntimeFile {
    const char* name;
    uint64_t size;
    const char* sha256;
};
// Where a file's zip record lies in the archive (PM_LLM_RUNTIME_ENTRY).
struct RuntimeEntry {
    const char* name;
    uint64_t offset, length;
    const char* sha256;  // of exactly those bytes
};

#define PM_LLM_RUNTIME(name, size, sha, host, path) constexpr RuntimeArchive kArchive{name, size, sha, host, path};
#define PM_LLM_RUNTIME_FILE(name, size, sha)
#define PM_LLM_RUNTIME_ENTRY(name, off, len, sha)
#define PM_LLM_GPU(name, size, sha, host, path)
#define PM_LLM_GPU_FILE(name, size, sha)
#define PM_LLM_MODEL(id, family, file, size, sha, host, path, license, name, ram)
#include "llm_engine_models.inc"
#undef PM_LLM_RUNTIME
#undef PM_LLM_RUNTIME_FILE
#undef PM_LLM_RUNTIME_ENTRY
#undef PM_LLM_GPU
#undef PM_LLM_GPU_FILE
#undef PM_LLM_MODEL

constexpr RuntimeFile kRuntimeFiles[] = {
#define PM_LLM_RUNTIME(name, size, sha, host, path)
#define PM_LLM_RUNTIME_FILE(name, size, sha) {name, size, sha},
#define PM_LLM_RUNTIME_ENTRY(name, off, len, sha)
#define PM_LLM_GPU(name, size, sha, host, path)
#define PM_LLM_GPU_FILE(name, size, sha)
#define PM_LLM_MODEL(id, family, file, size, sha, host, path, license, name, ram)
#include "llm_engine_models.inc"
#undef PM_LLM_RUNTIME
#undef PM_LLM_RUNTIME_FILE
#undef PM_LLM_RUNTIME_ENTRY
#undef PM_LLM_GPU
#undef PM_LLM_GPU_FILE
#undef PM_LLM_MODEL
};

constexpr RuntimeEntry kRuntimeEntries[] = {
#define PM_LLM_RUNTIME(name, size, sha, host, path)
#define PM_LLM_RUNTIME_FILE(name, size, sha)
#define PM_LLM_RUNTIME_ENTRY(name, off, len, sha) {name, off, len, sha},
#define PM_LLM_GPU(name, size, sha, host, path)
#define PM_LLM_GPU_FILE(name, size, sha)
#define PM_LLM_MODEL(id, family, file, size, sha, host, path, license, name, ram)
#include "llm_engine_models.inc"
#undef PM_LLM_RUNTIME
#undef PM_LLM_RUNTIME_FILE
#undef PM_LLM_RUNTIME_ENTRY
#undef PM_LLM_GPU
#undef PM_LLM_GPU_FILE
#undef PM_LLM_MODEL
};

#define PM_LLM_RUNTIME(name, size, sha, host, path)
#define PM_LLM_RUNTIME_FILE(name, size, sha)
#define PM_LLM_RUNTIME_ENTRY(name, off, len, sha)
#define PM_LLM_GPU(name, size, sha, host, path) constexpr RuntimeArchive kGpuArchive{name, size, sha, host, path};
#define PM_LLM_GPU_FILE(name, size, sha) constexpr RuntimeFile kGpuFile{name, size, sha};
#define PM_LLM_MODEL(id, family, file, size, sha, host, path, license, name, ram)
#include "llm_engine_models.inc"
#undef PM_LLM_RUNTIME
#undef PM_LLM_RUNTIME_FILE
#undef PM_LLM_RUNTIME_ENTRY
#undef PM_LLM_GPU
#undef PM_LLM_GPU_FILE
#undef PM_LLM_MODEL

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring llmDir() { return ModelStore::root() + L"\\llm"; }

// "b11514" from the archive name: the runtime directory is per release, so
// a new pinned release never mixes with the old DLLs.
std::wstring runtimeTag() {
    std::string n = kArchive.name;  // llama-b11514-bin-win-cpu-x64.zip
    const size_t a = n.find('-'), b = n.find('-', a + 1);
    return fromUtf8(a != std::string::npos && b != std::string::npos ? n.substr(a + 1, b - a - 1) : "runtime");
}

bool fileSize(const std::wstring& path, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return true;
}
bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The ggml-cpu variant to fetch (test builds: PM_LLM_TEST_VARIANT overrides).
std::string wantedVariant() {
#ifdef PM_DL_TEST_HTTP
    const std::wstring e = envVar(L"PM_LLM_TEST_VARIANT");
    if (!e.empty()) return toUtf8(e);
#endif
    return cpuVariant();
}
const std::string& thisVariant() {
    static const std::string v = wantedVariant();
    return v;
}
bool isCpuVariant(const char* name) { return std::strncmp(name, "ggml-cpu-", 9) == 0; }
// A runtime file a PC with `variant` needs: everything that is not a
// ggml-cpu-<variant>.dll, the variant ggml picks there, and ggml-cpu-x64.dll
// (its fallback: ggml skips a backend that cannot load and takes the next best).
bool neededFor(const char* name, const std::string& variant) {
    if (!isCpuVariant(name)) return true;
    return "ggml-cpu-" + variant + ".dll" == name || std::strcmp(name, "ggml-cpu-x64.dll") == 0;
}
bool neededFile(const char* name) { return neededFor(name, thisVariant()); }

const RuntimeEntry* entryFor(const char* name) {
    for (const auto& e : kRuntimeEntries)
        if (std::strcmp(e.name, name) == 0) return &e;
    return nullptr;
}

// The owner's mirror of the GitHub release zips (GitHub Pages, same bytes,
// same pins): tried first, GitHub (throttled per connection) after it.  Until
// the files are published there the mirror answers 404 and the original is
// used at once.  Test builds: PM_LLM_TEST_MIRROR / PM_LLM_TEST_ORIGIN
// ("127.0.0.1:PORT") replace the hosts.
constexpr char kMirrorHost[] = "victor900106.github.io";
constexpr char kMirrorDir[] = "/ZizaiCast/addons/";
std::string mirrorHost() {
#ifdef PM_DL_TEST_HTTP
    const std::wstring e = envVar(L"PM_LLM_TEST_MIRROR");
    if (!e.empty()) return toUtf8(e);
#endif
    return kMirrorHost;
}
std::string originHost(const char* host) {
#ifdef PM_DL_TEST_HTTP
    const std::wstring e = envVar(L"PM_LLM_TEST_ORIGIN");
    if (!e.empty()) return toUtf8(e);
#endif
    return host;
}

std::mutex gInfoMu;
RuntimeFetchInfo gInfo;

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
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void reset() {
        if (hash_) BCryptDestroyHash(hash_);
        hash_ = nullptr;
        if (alg_) BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
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

std::string sha256File(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    Sha256 sha;
    std::vector<char> buf(1 << 20);
    DWORD got = 0;
    while (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got) sha.add(buf.data(), got);
    CloseHandle(h);
    return sha.hex();
}

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

// GET https://host/path (redirects followed: GitHub release assets and
// Hugging Face LFS files are served from their CDNs) -> file `part`,
// hashed on the way.  Resumable: a `part` left by an interrupted download
// (closed app, lost connection, cancel) is hashed and continued with a
// Range request; a server that answers 200 instead of 206 starts it over.
bool fetch(const char* host, const char* path, const std::wstring& part, uint64_t size, const char* sha256,
           const std::function<void(uint64_t)>& got, const std::atomic<bool>* cancel, std::wstring* err) {
    Sha256 sha;
    uint64_t have = 0;
    if (fileSize(part, have) && have == size && sha256File(part) == sha256) {
        if (got) got(size);  // finished before, not moved into place yet
        return true;
    }
    if (fileSize(part, have) && have > 0 && have < size) {
        HANDLE r = CreateFileW(part.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        uint64_t read = 0;
        if (r != INVALID_HANDLE_VALUE) {
            std::vector<char> b(1 << 20);
            DWORD n = 0;
            while (read < have && ReadFile(r, b.data(), static_cast<DWORD>(b.size()), &n, nullptr) && n) {
                sha.add(b.data(), n);
                read += n;
            }
            CloseHandle(r);
        }
        if (read != have) {
            DeleteFileW(part.c_str());
            sha.reset();
            have = 0;
        }
    } else {
        DeleteFileW(part.c_str());
        have = 0;
    }
    HInternet session(WinHttpOpen(L"ZizaiCast-ModelDownloader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        if (err) *err = L"WinHttpOpen failed";
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    const std::wstring whost = fromUtf8(host), wpath = fromUtf8(path);
    HInternet conn(WinHttpConnect(session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    HInternet req(conn ? WinHttpOpenRequest(conn, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                       : nullptr);
    if (req) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    }
    const std::wstring range = have ? L"Range: bytes=" + std::to_wstring(have) + L"-\r\n" : std::wstring();
    if (!req ||
        !WinHttpSendRequest(req, range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(),
                            range.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        if (err) *err = L"cannot reach " + whost + L" (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    DWORD code = 0, len = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                        &len, WINHTTP_NO_HEADER_INDEX);
    if (have && code == 200) {  // no resume: from the start
        sha.reset();
        have = 0;
    }
    if (code != (have ? 206u : 200u)) {
        if (err) *err = L"HTTP " + std::to_wstring(code) + L" from " + whost;
        return false;
    }
    HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, have ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"cannot write " + part;
        return false;
    }
    if (have) SetFilePointerEx(h, LARGE_INTEGER{}, nullptr, FILE_END);
    uint64_t total = have;
    if (got) got(std::min(total, size));
    std::vector<char> buf(256 * 1024);
    bool ok = true, keepPart = false;
    for (;;) {
        DWORD n = 0;
        if (!WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &n)) {
            if (err) *err = L"download interrupted (error " + std::to_wstring(GetLastError()) + L")";
            ok = false;
            keepPart = true;  // continued next time
            break;
        }
        if (n == 0) break;
        DWORD w = 0;
        if (!WriteFile(h, buf.data(), n, &w, nullptr) || w != n) {
            if (err) *err = L"cannot write " + part + L" (disk full?)";
            ok = false;
            break;
        }
        sha.add(buf.data(), n);
        total += n;
        if (got) got(std::min(total, size));
        if (cancel && cancel->load()) {
            if (err) *err = L"cancelled";
            ok = false;
            keepPart = true;
            break;
        }
        if (total > size) break;
    }
    CloseHandle(h);
    if (ok && total < size) {  // the connection closed early
        if (err) *err = L"download interrupted";
        ok = false;
        keepPart = true;
    } else if (ok && (total != size || sha.hex() != sha256)) {
        if (err) *err = L"SHA-256";  // same wording as ModelStore (caller maps it to TrVerifyFailed)
        ok = false;
    }
    if (!ok && !keepPart) DeleteFileW(part.c_str());
    return ok;
}

bool markOk(const std::wstring& file) { return exists(file + L".sha256ok"); }
void writeMark(const std::wstring& file, const char* sha) { std::ofstream(file + L".sha256ok") << sha << "\n"; }

// Windows' own bsdtar (System32\tar.exe, Windows 10 1803 and later) reads
// zip archives; no window, no shell.
// Unpacks the files `names` of zip into dir.
bool unzip(const std::wstring& zip, const std::wstring& dir, std::wstring* err, const std::vector<std::string>& names) {
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tar = std::wstring(sys, n) + L"\\tar.exe";
    if (!exists(tar)) {
        if (err) *err = L"tar.exe not found (Windows 10 1803 or later is needed)";
        return false;
    }
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dir + L"\"";
    for (const auto& f : names) cmd += L" \"" + fromUtf8(f) + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(tar.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, dir.c_str(), &si, &pi)) {
        if (err) *err = L"cannot run tar.exe (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        if (err) *err = L"cannot unpack " + zip + L" (tar " + std::to_wstring(code) + L")";
        return false;
    }
    return true;
}

uint32_t le(const std::string& s, size_t at, int bytes) {
    uint32_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | static_cast<uint8_t>(s[at + i]);
    return v;
}
void put(std::string& s, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) s += static_cast<char>((v >> (8 * i)) & 255);
}

// The fetched zip records (each a local header + its data, already checked
// against its pin) joined into a zip with a central directory, so tar.exe can
// unpack them.  False when a record is not what PM_LLM_RUNTIME_ENTRY says.
bool joinRecords(const std::vector<std::pair<std::string, std::wstring>>& recs, const std::wstring& zipPath) {
    std::string zip, cd;
    for (const auto& [name, part] : recs) {
        std::ifstream in(part, std::ios::binary);
        std::string r((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (r.size() < 30 || le(r, 0, 4) != 0x04034b50) return false;
        const uint32_t nl = le(r, 26, 2), el = le(r, 28, 2), flags = le(r, 6, 2);
        if (r.size() != 30ull + nl + el + le(r, 18, 4) || (flags & 8) || r.compare(30, nl, name) != 0) return false;
        const uint32_t at = static_cast<uint32_t>(zip.size());
        cd.append("PK\x01\x02", 4);
        put(cd, 20, 2);                    // made by
        cd.append(r, 4, 26 - 4);           // needed, flags, method, time, date, crc, sizes (as in the record)
        put(cd, nl, 2);
        put(cd, 0, 2);                     // extra
        put(cd, 0, 2);                     // comment
        put(cd, 0, 2);                     // disk
        put(cd, 0, 2);                     // internal attributes
        put(cd, 0, 4);                     // external attributes
        put(cd, at, 4);
        cd += name;
        zip += r;
    }
    const uint32_t cdAt = static_cast<uint32_t>(zip.size()), count = static_cast<uint32_t>(recs.size());
    zip += cd;
    zip.append("PK\x05\x06", 4);
    put(zip, 0, 4);  // disk numbers
    put(zip, count, 2);
    put(zip, count, 2);
    put(zip, static_cast<uint32_t>(cd.size()), 4);
    put(zip, cdAt, 4);
    put(zip, 0, 2);
    std::ofstream out(zipPath, std::ios::binary | std::ios::trunc);
    out << zip;
    return static_cast<bool>(out);
}

std::mutex& storeMutex() {
    static std::mutex m;
    return m;
}

}  // namespace

const std::vector<ModelInfo>& models() {
    static const std::vector<ModelInfo> v = {
#define PM_LLM_RUNTIME(name, size, sha, host, path)
#define PM_LLM_RUNTIME_FILE(name, size, sha)
#define PM_LLM_RUNTIME_ENTRY(name, off, len, sha)
#define PM_LLM_GPU(name, size, sha, host, path)
#define PM_LLM_GPU_FILE(name, size, sha)
#define PM_LLM_MODEL(id, family, file, size, sha, host, path, license, name, ram) {id, family, file, size, sha, host, path, license, name, ram},
#include "llm_engine_models.inc"
#undef PM_LLM_RUNTIME
#undef PM_LLM_RUNTIME_FILE
#undef PM_LLM_RUNTIME_ENTRY
#undef PM_LLM_GPU
#undef PM_LLM_GPU_FILE
#undef PM_LLM_MODEL
    };
    return v;
}

const ModelInfo* findModel(const std::string& id) {
    for (const auto& m : models())
        if (id == m.id) return &m;
    return nullptr;
}

std::wstring settingsPath() { return llmDir() + L"\\settings.ini"; }

std::string settingValue(const wchar_t* key) {
    wchar_t buf[256] = {};
    GetPrivateProfileStringW(L"llm", key, L"", buf, 256, settingsPath().c_str());
    return toUtf8(buf);
}

// A discrete GPU the Vulkan add-on can use with the 2B model: NVIDIA / AMD /
// Intel (Arc) hardware adapter with >= 3 GB of its own video memory (the
// model + its context take ~2.2 GB; integrated GPUs share system memory and
// measured ~10x slower than needed).  DXGI, loaded at run time (no import);
// computed once per process.  PM_LLM_GPU=off: none.
bool discreteGpu() {
    if (envVar(L"PM_LLM_GPU") == L"off") return false;
    static const bool has = [] {
        HMODULE dx = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dx) return false;
        using Create = HRESULT(WINAPI*)(REFIID, void**);
        auto create = reinterpret_cast<Create>(GetProcAddress(dx, "CreateDXGIFactory1"));
        IDXGIFactory1* f = nullptr;
        bool found = false;
        if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) && f) {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    (d.VendorId == 0x10DE || d.VendorId == 0x1002 || d.VendorId == 0x8086) &&
                    d.DedicatedVideoMemory >= (3ull << 30))
                    found = true;
                a->Release();
            }
            f->Release();
        }
        return found;
    }();
    return has;
}

const ModelInfo& defaultModel() {
    // 2B with a usable discrete GPU, the 0.8B one on the CPU (the 2B model
    // takes 4-14 s a picture there; 0.8B fits ~2.5 s).
    const ModelInfo* m = findModel(gpuEnabled() && gpuPossible() && discreteGpu() ? "qwen3.5-2b-q4km" : "qwen3.5-0.8b-q4");
    return m ? *m : models().front();
}

const ModelInfo& activeModel() {
    const std::wstring e = envVar(L"PM_LLM_MODEL");
    if (!e.empty())
        if (const ModelInfo* m = findModel(toUtf8(e))) return *m;
    if (const ModelInfo* m = findModel(settingValue(L"model"))) return *m;
    return defaultModel();
}

bool enabled() { return settingValue(L"enabled") != "0"; }
bool gpuEnabled() { return settingValue(L"gpu") != "0"; }

// A GPU the add-on can use: the Vulkan loader (installed with every current
// NVIDIA / AMD / Intel driver) and a hardware display adapter.
bool gpuPossible() {
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!exists(std::wstring(sys, n) + L"\\vulkan-1.dll")) return false;
    DISPLAY_DEVICEW dd{sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        const std::wstring id = dd.DeviceID;
        if (id.find(L"VEN_10DE") != std::wstring::npos || id.find(L"VEN_1002") != std::wstring::npos ||
            id.find(L"VEN_8086") != std::wstring::npos)
            return true;
    }
    return false;
}

bool gpuInstalled() {
    uint64_t sz = 0;
    const std::wstring p = runtimeDir() + L"\\" + fromUtf8(kGpuFile.name);
    if (!envVar(L"PM_LLAMA_DIR").empty()) return exists(p);
    return fileSize(p, sz) && sz == kGpuFile.size && markOk(p);
}

// The add-on is part of the download when the PC has a GPU and the user did not turn it off.
bool gpuWanted() { return gpuEnabled() && gpuPossible() && discreteGpu(); }

namespace {

// What download() fetches for m on this PC (the runtime, the GPU backend
// when wanted, the model), as downloader items.
struct Need {
    bool rt = false, gpu = false, model = false;
};
Need needFor(const ModelInfo& m) {
    Need n;
    n.rt = !runtimeInstalled();
    n.gpu = gpuWanted() && !gpuInstalled();
    n.model = !modelInstalled(m);
    return n;
}

dl::Item zipItem(const RuntimeArchive& a, const char* label) {
    dl::Item it;
    it.host = originHost(a.host);
    it.path = a.path;
    it.dest = llmDir() + L"\\" + fromUtf8(a.name);
    it.size = a.size;
    it.sha256 = a.sha256;
    it.mirrors.push_back({mirrorHost(), std::string(kMirrorDir) + a.name});
    it.label = label;
    it.priority = 1;  // small, slow host: first, with most connections
    return it;
}
dl::Item rtItem() { return zipItem(kArchive, "runtime"); }
dl::Item gpuItem() { return zipItem(kGpuArchive, "gpu"); }
dl::Item modelItem(const ModelInfo& m) {
    dl::Item it;
    it.host = m.host;
    it.path = m.path;
    it.dest = modelPath(m);
    it.size = m.size;
    it.sha256 = m.sha256;
    it.label = "model";
    return it;
}

// The runtime file f is in place: right size, right SHA-256.
bool runtimeFileOk(const RuntimeFile& f) {
    const std::wstring p = runtimeDir() + L"\\" + fromUtf8(f.name);
    uint64_t sz = 0;
    return fileSize(p, sz) && sz == f.size && sha256File(p) == f.sha256;
}

// The ranged runtime download: one item per zip record this PC still needs
// (the bytes [offset, offset + length) of the same archive, on the mirror and
// on GitHub; pinned by the record's SHA-256).  `names`: the files they hold.
std::vector<dl::Item> rtRecordItems(std::vector<std::string>* names, bool skipPresent) {
    std::vector<dl::Item> v;
    for (const auto& f : kRuntimeFiles) {
        if (!neededFile(f.name) || (skipPresent && runtimeFileOk(f))) continue;
        const RuntimeEntry* e = entryFor(f.name);
        if (!e) continue;
        dl::Item it = rtItem();
        it.dest = llmDir() + L"\\" + runtimeTag() + L"-" + fromUtf8(f.name) + L".rec";
        it.size = e->length;
        it.sha256 = e->sha256;
        it.offset = e->offset;
        v.push_back(std::move(it));
        if (names) names->push_back(f.name);
    }
    return v;
}
uint64_t rtPartialBytes() {
    uint64_t n = 0;
    for (const auto& it : rtRecordItems(nullptr, false)) n += dl::partialBytes(it);
    return n;
}

void clearItem(const dl::Item& it) {
    DeleteFileW(it.dest.c_str());
    DeleteFileW((it.dest + L".part").c_str());
    DeleteFileW((it.dest + L".part.seg").c_str());
}

// Checks the unpacked runtime files (only those this PC needs when
// `onlyNeeded`) against their pins and writes the marker; a mismatch deletes them.
bool verifyRuntime(bool onlyNeeded, std::wstring* err) {
    const std::wstring rt = runtimeDir();
    for (const auto& f : kRuntimeFiles) {
        if (onlyNeeded && !neededFile(f.name)) continue;
        if (!runtimeFileOk(f)) {
            if (err) *err = L"SHA-256";
            for (const auto& g : kRuntimeFiles) DeleteFileW((rt + L"\\" + fromUtf8(g.name)).c_str());
            return false;
        }
    }
    writeMark(rt + L"\\llama.dll", kArchive.sha256);
    return true;
}

// The ranged records are in: joined into a small zip, unpacked, verified.
bool installRecords(const std::vector<dl::Item>& recs, const std::vector<std::string>& names, std::wstring* err) {
    const std::wstring rt = runtimeDir(), zip = llmDir() + L"\\" + runtimeTag() + L"-records.zip";
    SHCreateDirectoryExW(nullptr, rt.c_str(), nullptr);
    std::vector<std::pair<std::string, std::wstring>> parts;
    for (size_t i = 0; i < recs.size(); ++i) parts.emplace_back(names[i], recs[i].dest);
    const bool ok = recs.empty() || (joinRecords(parts, zip) && unzip(zip, rt, err, names));
    DeleteFileW(zip.c_str());
    for (const auto& it : recs) clearItem(it);
    return ok && verifyRuntime(true, err);
}

// The whole archive is in: unpacked (every file), verified.
bool installArchive(const dl::Item& it, std::wstring* err) {
    const std::wstring rt = runtimeDir();
    SHCreateDirectoryExW(nullptr, rt.c_str(), nullptr);
    std::vector<std::string> names;
    for (const auto& f : kRuntimeFiles) names.push_back(f.name);
    const bool unz = unzip(it.dest, rt, err, names);
    DeleteFileW(it.dest.c_str());
    return unz && verifyRuntime(false, err);
}

// m == nullptr: the runtime alone (tests).
bool downloadImpl(const ModelInfo* m, const std::function<void(const dl::Progress&)>& progress,
                  const std::atomic<bool>* cancel, std::wstring* err) {
    std::lock_guard<std::mutex> lock(storeMutex());
    Need n;
    if (m) n = needFor(*m);
    else n.rt = !runtimeInstalled();
    const std::wstring dir = llmDir(), rt = runtimeDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    // All at once: the big model (Hugging Face, fast) moves the bar from the
    // start while the small runtime records / GPU zip (GitHub, throttled per
    // connection) come over several connections.
    std::vector<dl::Item> items;
    std::vector<std::string> recNames;
    size_t recN = 0;
    int iGpu = -1, iModel = -1;
    if (n.rt) {
        items = rtRecordItems(&recNames, true);
        recN = items.size();
        std::lock_guard<std::mutex> l(gInfoMu);
        gInfo = RuntimeFetchInfo{};
        gInfo.variant = thisVariant();
    }
    if (n.gpu) iGpu = static_cast<int>(items.size()), items.push_back(gpuItem());
    if (n.model) iModel = static_cast<int>(items.size()), items.push_back(modelItem(*m));
    dl::Options o;
    o.userAgent = L"ZizaiCast/0.7 (model downloader)";
    std::vector<dl::Result> res;
    if (!items.empty()) res = dl::fetchAll(items, progress, cancel, o);
    auto fail = [&](int i) {
        if (err) *err = res[i].status == dl::Status::Cancelled ? L"cancelled" : res[i].detail;
        return false;
    };
    // Cancelled: everything stops (partial files kept).
    for (size_t i = 0; i < res.size(); ++i)
        if (res[i].status == dl::Status::Cancelled) return fail(static_cast<int>(i));
    bool ok = true;
    if (n.rt) {
        bool recsOk = true;
        uint64_t got = 0;
        for (size_t i = 0; i < recN; ++i) {
            recsOk = recsOk && res[i].status == dl::Status::Ok;
            got += items[i].size;
        }
        const std::vector<dl::Item> recs(items.begin(), items.begin() + recN);
        std::wstring rerr;
        if (recsOk && installRecords(recs, recNames, &rerr)) {
            clearItem(rtItem());  // a whole-archive .part an older version left
            std::lock_guard<std::mutex> l(gInfoMu);
            gInfo.ranged = true;
            gInfo.bytes = got;
        } else {
            // No Range (a server answering 200), a record that does not match
            // its pin, a failed unpack: the whole archive, as before.
            for (const auto& it : recs) clearItem(it);
            const dl::Item whole = rtItem();
            const std::vector<dl::Result> r = dl::fetchAll({whole}, progress, cancel, o);
            {
                std::lock_guard<std::mutex> l(gInfoMu);
                gInfo.fellBack = true;
                gInfo.bytes = whole.size;
            }
            if (r[0].status != dl::Status::Ok) {
                if (err) *err = r[0].status == dl::Status::Cancelled ? L"cancelled" : r[0].detail;
                if (r[0].status == dl::Status::Cancelled) return false;
                ok = false;
            } else if (!installArchive(whole, err)) {
                ok = false;
            }
        }
    }
    if (iGpu >= 0 && res[iGpu].status == dl::Status::Ok) {
        // Optional: without it the CPU is used, so a failure here is not fatal.
        SHCreateDirectoryExW(nullptr, rt.c_str(), nullptr);
        const std::wstring zip = items[iGpu].dest, p = rt + L"\\" + fromUtf8(kGpuFile.name);
        std::wstring gerr;
        uint64_t sz = 0;
        if (unzip(zip, rt, &gerr, {kGpuFile.name}) && fileSize(p, sz) && sz == kGpuFile.size && sha256File(p) == kGpuFile.sha256)
            writeMark(p, kGpuFile.sha256);
        else
            DeleteFileW(p.c_str());
        DeleteFileW(zip.c_str());
    }
    if (iModel >= 0) {
        if (res[iModel].status != dl::Status::Ok) ok = ok && fail(iModel);
        else writeMark(items[iModel].dest, m->sha256);
    }
    return ok;
}

}  // namespace

uint64_t partialBytes(const ModelInfo& m) {
    const Need n = needFor(m);
    return (n.rt ? rtPartialBytes() : 0) + (n.gpu ? dl::partialBytes(gpuItem()) : 0) +
           (n.model ? dl::partialBytes(modelItem(m)) : 0);
}

std::wstring modelPath(const ModelInfo& m) { return llmDir() + L"\\" + fromUtf8(m.file); }

std::wstring runtimeDir() {
    const std::wstring e = envVar(L"PM_LLAMA_DIR");
    return !e.empty() ? e : llmDir() + L"\\runtime-" + runtimeTag();
}

// Installed: the marker + every file this PC needs at its size.  A runtime
// unpacked from the whole archive (0.7.8 and earlier: all 14 variants) has
// them too, so it is not fetched again.
bool runtimeInstalled() {
    const std::wstring dir = runtimeDir();
    if (!envVar(L"PM_LLAMA_DIR").empty()) return exists(dir + L"\\llama.dll");
    if (!markOk(dir + L"\\llama.dll")) return false;
    for (const auto& f : kRuntimeFiles) {
        if (!neededFile(f.name)) continue;
        uint64_t sz = 0;
        if (!fileSize(dir + L"\\" + fromUtf8(f.name), sz) || sz != f.size) return false;
    }
    return true;
}

std::vector<std::string> runtimeFilesFor(const std::string& variant) {
    std::vector<std::string> v;
    for (const auto& f : kRuntimeFiles)
        if (neededFor(f.name, variant)) v.push_back(f.name);
    return v;
}

uint64_t runtimeRangedBytes(const std::string& variant) {
    uint64_t n = 0;
    for (const auto& f : kRuntimeFiles)
        if (neededFor(f.name, variant))
            if (const RuntimeEntry* e = entryFor(f.name)) n += e->length;
    return n;
}

RuntimeFetchInfo lastRuntimeFetch() {
    std::lock_guard<std::mutex> l(gInfoMu);
    return gInfo;
}

bool modelInstalled(const ModelInfo& m) {
    const std::wstring p = modelPath(m);
    uint64_t sz = 0;
    return fileSize(p, sz) && sz == m.size && markOk(p);
}

bool installed(const ModelInfo& m) { return runtimeInstalled() && modelInstalled(m); }

uint64_t missingBytes(const ModelInfo& m) {
    // The runtime: its records for this PC (the whole archive only when the
    // server cannot do Range).  Minus what an interrupted download left.
    const Need n = needFor(m);
    const uint64_t all = (n.rt ? runtimeRangedBytes(thisVariant()) : 0) + (n.gpu ? kGpuArchive.size : 0) + (n.model ? m.size : 0);
    return all - std::min(all, partialBytes(m));
}

bool memoryOk(const ModelInfo& m) {
    MEMORYSTATUSEX ms{sizeof(ms)};
    if (!GlobalMemoryStatusEx(&ms)) return true;
    return ms.ullAvailPhys / (1024 * 1024) >= static_cast<uint64_t>(m.ramMB) + 1024;
}

std::wstring describeDownload(const ModelInfo& m) {
    std::wstring s;
    if (!runtimeInstalled())
        s += L"llama.cpp " + runtimeTag() + L" (MIT) - https://" + fromUtf8(kArchive.host) + fromUtf8(kArchive.path) + L"\n";
    if (gpuWanted() && !gpuInstalled())
        s += L"llama.cpp " + runtimeTag() + L" Vulkan GPU backend (MIT) - https://" + fromUtf8(kGpuArchive.host) +
             fromUtf8(kGpuArchive.path) + L"\n";
    if (!modelInstalled(m))
        s += fromUtf8(m.name) + L" (" + fromUtf8(m.license) + L") - https://" + fromUtf8(m.host) + fromUtf8(m.path) + L"\n";
    return s;
}

// The runtime comes as the zip records of the files this PC needs (HTTP Range
// into the pinned archive: llama.dll, ggml*.dll, libomp.dll, the ggml-cpu
// variant for this CPU + x64; 2.2-2.9 of 19.5 MB), each checked against its pin,
// then the unpacked files against theirs.  A server without Range or a record
// that fails: the whole archive, as before.
bool download(const ModelInfo& m, const std::function<void(const dl::Progress&)>& progress, const std::atomic<bool>* cancel,
              std::wstring* err) {
    return downloadImpl(&m, progress, cancel, err);
}

bool downloadRuntime(const std::function<void(const dl::Progress&)>& progress, const std::atomic<bool>* cancel,
                     std::wstring* err) {
    return downloadImpl(nullptr, progress, cancel, err);
}

bool download(const ModelInfo& m, const std::function<void(double)>& progress, const std::atomic<bool>* cancel,
              std::wstring* err) {
    return download(m, [&](const dl::Progress& p) {
        if (progress) progress(p.fraction());
    }, cancel, err);
}

bool remove(const ModelInfo& m) {
    std::lock_guard<std::mutex> lock(storeMutex());
    const std::wstring p = modelPath(m);
    DeleteFileW((p + L".sha256ok").c_str());
    bool ok = !exists(p) || DeleteFileW(p.c_str());
    bool anyLeft = false;
    for (const auto& o : models())
        if (exists(modelPath(o))) anyLeft = true;
    if (!anyLeft && envVar(L"PM_LLAMA_DIR").empty()) {
        // The DLLs stay loaded in this process until exit: deletion may fail now (next start: removed by the check).
        const std::wstring rt = runtimeDir();
        DeleteFileW((rt + L"\\llama.dll.sha256ok").c_str());
        for (const auto& f : kRuntimeFiles) DeleteFileW((rt + L"\\" + fromUtf8(f.name)).c_str());
        DeleteFileW((rt + L"\\" + fromUtf8(kGpuFile.name) + L".sha256ok").c_str());
        DeleteFileW((rt + L"\\" + fromUtf8(kGpuFile.name)).c_str());
        RemoveDirectoryW(rt.c_str());
    }
    return ok;
}

}  // namespace pm::translate::llm
