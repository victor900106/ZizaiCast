// 0.8.4 電腦的檔案: see pc_files.h (the path rules are there).
#include "pc_files.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>

#include "util.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace pm::remote {

namespace {

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

bool sameText(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

bool isDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring leafName(const std::wstring& p) {
    std::wstring s = p;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? s : s.substr(k + 1);
}

bool reservedDeviceName(const std::wstring& part) {
    std::wstring base = part.substr(0, part.find(L'.'));
    while (!base.empty() && base.back() == L' ') base.pop_back();
    for (wchar_t& c : base) c = wchar_t(towupper(c));
    static const wchar_t* const kNames[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$", L"CLOCK$"};
    for (const wchar_t* n : kNames)
        if (base == n) return true;
    if (base.size() == 4 && (base.rfind(L"COM", 0) == 0 || base.rfind(L"LPT", 0) == 0) &&
        ((base[3] >= L'0' && base[3] <= L'9') || base[3] == 0xB9 || base[3] == 0xB2 || base[3] == 0xB3))
        return true;
    return false;
}

int64_t unixSeconds(const FILETIME& ft) {
    const uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t < 116444736000000000ull ? 0 : int64_t((t - 116444736000000000ull) / 10000000ull);
}

}  // namespace

std::wstring finalPath(const std::wstring& path) {
    // FILE_FLAG_BACKUP_SEMANTICS: folders open too. No FILE_FLAG_OPEN_REPARSE_POINT: links are followed,
    // so the answer is where the data really is.
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring out;
    // A device (CON, a named pipe ...) is no file: only disk files / folders.
    if (GetFileType(h) == FILE_TYPE_DISK) {
        std::vector<wchar_t> buf(1024);
        DWORD n = GetFinalPathNameByHandleW(h, buf.data(), DWORD(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n >= buf.size()) {
            buf.resize(size_t(n) + 1);
            n = GetFinalPathNameByHandleW(h, buf.data(), DWORD(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        }
        if (n > 0 && n < buf.size()) out.assign(buf.data(), n);
    }
    CloseHandle(h);
    if (out.rfind(L"\\\\?\\UNC\\", 0) == 0) out = L"\\\\" + out.substr(8);
    else if (out.rfind(L"\\\\?\\", 0) == 0) out = out.substr(4);
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

bool underRoot(const std::wstring& root, const std::wstring& p) {
    if (root.empty() || p.empty()) return false;
    if (sameText(root, p)) return true;
    // "C:\" keeps its backslash; any other root gets one: the next character must be a boundary.
    const std::wstring pre = root.back() == L'\\' ? root : root + L'\\';
    return p.size() > pre.size() && sameText(pre, p.substr(0, pre.size()));
}

bool cleanRelPath(const std::string& rel, std::wstring& out) {
    out.clear();
    if (rel.size() > 1024) return false;
    const std::wstring w = widen(rel);
    if (w.find(0xFFFD) != std::wstring::npos) return false;  // not valid UTF-8
    size_t a = 0, parts = 0;
    while (a <= w.size()) {
        const size_t b = w.find(L'/', a);
        const std::wstring part = w.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        if (part.empty()) {
            if (w.empty()) return true;  // the root itself
            return false;                // "a//b", "/a", "a/"
        }
        for (wchar_t c : part)
            if (c < 32 || c == 0x7F || wcschr(L"<>:\"\\|?*", c)) return false;
        if (part.find_first_not_of(L". ") == std::wstring::npos) return false;  // ".", "..", "...", " "
        if (part.back() == L'.' || part.back() == L' ' || part.front() == L' ') return false;  // Windows would trim them
        if (reservedDeviceName(part)) return false;
        if (++parts > 32) return false;
        if (!out.empty()) out += L'\\';
        out += part;
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    return true;
}

const char* pcPathErrName(PcPathErr e) {
    switch (e) {
    case PcPathErr::Ok: return "ok";
    case PcPathErr::BadPath: return "bad-path";
    case PcPathErr::NoRoot: return "no-root";
    case PcPathErr::NotFound: return "not-found";
    case PcPathErr::Escape: return "escape";
    case PcPathErr::NotFile: return "not-file";
    case PcPathErr::NotDir: return "not-dir";
    }
    return "?";
}

std::vector<PcRoot> pcRoots(const std::wstring& shotDir, const std::wstring& recDir, const std::vector<std::wstring>& extra,
                            const std::vector<std::wstring>& testRoots) {
    std::vector<PcRoot> v;
    auto add = [&](const std::string& id, const std::string& kind, const std::wstring& p) {
        if (p.empty() || !isDir(p)) return;
        const std::wstring c = finalPath(p);
        if (c.empty()) return;
        for (const PcRoot& r : v)
            if (sameText(r.path, c)) return;  // the same folder twice (e.g. 下載 = 文件)
        v.push_back(PcRoot{id, kind, narrow(leafName(c)), c});
    };
    if (!testRoots.empty()) {
        for (const std::wstring& t : testRoots) {
            const size_t eq = t.find(L'=');
            if (eq == std::wstring::npos || eq == 0) continue;
            const std::string id = narrow(t.substr(0, eq));
            add(id, id.rfind("extra", 0) == 0 ? "extra" : id, t.substr(eq + 1));
        }
        return v;
    }
    add("desktop", "desktop", knownFolder(FOLDERID_Desktop));
    add("documents", "documents", knownFolder(FOLDERID_Documents));
    add("downloads", "downloads", knownFolder(FOLDERID_Downloads));
    add("pictures", "pictures", knownFolder(FOLDERID_Pictures));
    add("videos", "videos", knownFolder(FOLDERID_Videos));
    add("shots", "shots", shotDir);
    add("recordings", "recordings", recDir);
    for (size_t i = 0; i < extra.size() && i < 16; ++i) add("extra" + std::to_string(i), "extra", extra[i]);
    return v;
}

PcPathErr resolvePcPath(const std::vector<PcRoot>& roots, const std::string& rootId, const std::string& rel, bool wantDir,
                        std::wstring& canon, const PcRoot** rootOut) {
    canon.clear();
    const PcRoot* root = nullptr;
    for (const PcRoot& r : roots)
        if (r.id == rootId) root = &r;
    if (!root) return PcPathErr::NoRoot;
    if (rootOut) *rootOut = root;
    std::wstring sub;
    if (!cleanRelPath(rel, sub)) return PcPathErr::BadPath;
    // The root as it is now (it may have become a link since the list was made).
    const std::wstring rootNow = finalPath(root->path);
    if (rootNow.empty()) return PcPathErr::NotFound;
    if (!sameText(rootNow, root->path)) return PcPathErr::Escape;
    const std::wstring joined = sub.empty() ? rootNow : (rootNow.back() == L'\\' ? rootNow : rootNow + L'\\') + sub;
    const std::wstring c = finalPath(joined);
    if (c.empty()) return PcPathErr::NotFound;
    if (!underRoot(rootNow, c)) return PcPathErr::Escape;
    const DWORD a = GetFileAttributesW(c.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return PcPathErr::NotFound;
    if (wantDir && !(a & FILE_ATTRIBUTE_DIRECTORY)) return PcPathErr::NotDir;
    if (!wantDir && (a & FILE_ATTRIBUTE_DIRECTORY)) return PcPathErr::NotFile;
    canon = c;
    return PcPathErr::Ok;
}

PcPathErr listPcDir(const std::vector<PcRoot>& roots, const std::string& rootId, const std::string& rel, std::vector<PcEntry>& out,
                    bool& truncated, size_t maxEntries) {
    out.clear();
    truncated = false;
    std::wstring dir;
    const PcRoot* root = nullptr;
    const PcPathErr e = resolvePcPath(roots, rootId, rel, true, dir, &root);
    if (e != PcPathErr::Ok) return e;
    const std::wstring rootNow = root->path;
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return PcPathErr::Ok;  // empty or unreadable: nothing to show
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const DWORD a = fd.dwFileAttributes;
        if ((a & FILE_ATTRIBUTE_HIDDEN) && (a & FILE_ATTRIBUTE_SYSTEM)) continue;  // desktop.ini, $RECYCLE.BIN ...
        if (a & FILE_ATTRIBUTE_DEVICE) continue;
        bool dirEntry = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        uint64_t size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
            // A link: shown only when where it leads is inside this root (what a download would check too).
            const std::wstring target = finalPath(dir + L"\\" + n);
            if (target.empty() || !underRoot(rootNow, target)) continue;
            const DWORD ta = GetFileAttributesW(target.c_str());
            if (ta == INVALID_FILE_ATTRIBUTES) continue;
            dirEntry = (ta & FILE_ATTRIBUTE_DIRECTORY) != 0;
            WIN32_FILE_ATTRIBUTE_DATA td{};
            if (!dirEntry && GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &td)) size = (uint64_t(td.nFileSizeHigh) << 32) | td.nFileSizeLow;
        }
        // A name the phone could not send back (cleanRelPath) is not listed either.
        std::wstring check;
        if (!cleanRelPath(narrow(n), check)) continue;
        if (out.size() >= maxEntries) {
            truncated = true;
            break;
        }
        out.push_back(PcEntry{narrow(n), dirEntry, dirEntry ? 0 : size, unixSeconds(fd.ftLastWriteTime)});
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const PcEntry& x, const PcEntry& y) {
        if (x.dir != y.dir) return x.dir;
        const std::wstring a = widen(x.name), b = widen(y.name);
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(), int(a.size()), b.c_str(), int(b.size()),
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return PcPathErr::Ok;
}

}  // namespace pm::remote
