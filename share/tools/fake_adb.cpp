// Fake adb.exe for pm_share_test: answers the commands AndroidSource uses
// (start-server, devices -l, connect, shell ..., push, mdns services) like a
// phone 「Pixel 8 測試」 at 192.168.50.7:41234 would, and appends every
// invocation (arguments after `-P port`, joined by " | ") to %FAKE_ADB_LOG%.
// FAKE_ADB_MODE: ok (default) | lateindex (MediaStore lists the file only
// after scan_volume) | never (never listed) | pushfail. FAKE_ADB_PUSH_MS: each
// push takes this long (queueing tests).
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

static std::string u8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring env(const wchar_t* k) {
    wchar_t b[1024];
    const DWORD n = GetEnvironmentVariableW(k, b, 1024);
    return n && n < 1024 ? std::wstring(b, n) : std::wstring();
}

static void out(const std::string& s) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), DWORD(s.size()), &w, nullptr);
}

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> a;
    for (int i = 1; i < argc; ++i) a.push_back(argv[i]);
    if (a.size() >= 2 && a[0] == L"-P") a.erase(a.begin(), a.begin() + 2);
    const std::wstring logPath = env(L"FAKE_ADB_LOG"), mode = env(L"FAKE_ADB_MODE");
    if (!logPath.empty()) {
        std::string line;
        for (size_t i = 0; i < a.size(); ++i) line += (i ? " | " : "") + u8(a[i]);
        line += "\n";
        HANDLE f = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(f, line.data(), DWORD(line.size()), &w, nullptr);
            CloseHandle(f);
        }
    }
    const std::wstring marker = logPath + L".scanned";
    if (a.empty()) return 1;
    if (a[0] == L"start-server" || a[0] == L"kill-server") return 0;
    if (a[0] == L"devices") {
        out("List of devices attached\n192.168.50.7:41234      device product:husky model:Pixel_8 device:husky transport_id:1\n\n");
        return 0;
    }
    if (a[0] == L"connect" && a.size() > 1) {
        out("already connected to " + u8(a[1]) + "\n");
        return 0;
    }
    if (a[0] == L"mdns") {
        out("List of discovered mdns services\n");
        return 0;
    }
    if (a[0] == L"-s" && a.size() >= 3) {
        const std::wstring& cmd = a[2];
        if (cmd == L"push" && a.size() >= 5) {
            if (mode == L"pushfail") {
                out("adb: error: failed to get feature set: device '192.168.50.7:41234' not found\n");
                return 1;
            }
            if (const std::wstring ms = env(L"FAKE_ADB_PUSH_MS"); !ms.empty()) Sleep(DWORD(_wtoi(ms.c_str())));  // a slow push
            // Unicode local paths must arrive intact (adb itself is Unicode-aware).
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (!GetFileAttributesExW(a[3].c_str(), GetFileExInfoStandard, &fa)) {
                out("adb: error: cannot stat '" + u8(a[3]) + "': No such file or directory\n");
                return 1;
            }
            out(u8(a[3]) + ": 1 file pushed, 0 skipped. 31.4 MB/s (" + std::to_string(fa.nFileSizeLow) +
                " bytes in 0.001s)\n");
            return 0;
        }
        if (cmd == L"shell") {
            std::wstring s;
            for (size_t i = 3; i < a.size(); ++i) s += (i > 3 ? L" " : L"") + a[i];
            if (s.rfind(L"settings get global device_name", 0) == 0) {
                out(u8(L"Pixel 8 測試") + "\n");
                return 0;
            }
            if (s.rfind(L"content query", 0) == 0) {
                const bool listed = mode == L"ok" || mode.empty() ||
                                    (mode == L"lateindex" && GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES);
                out(listed ? "Row: 0 _id=1000042\n" : "No result found.\n");
                return 0;
            }
            if (s.find(L"scan_volume") != std::wstring::npos) {
                HANDLE f = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
                out("Result: null\n");
                return 0;
            }
            if (s.find(L"scan_file") != std::wstring::npos) {
                out("Result: Bundle[{android.intent.extra.STREAM=content://media/external_primary/images/media/1000042}]\n");
                return 0;
            }
            if (s.rfind(L"am broadcast", 0) == 0) {
                out("Broadcasting: Intent { act=android.intent.action.MEDIA_SCANNER_SCAN_FILE flg=0x400000 }\n"
                    "Broadcast completed: result=0\n");
                return 0;
            }
            return 0;  // mkdir -p etc.
        }
    }
    out("fake adb: unknown command\n");
    return 1;
}
