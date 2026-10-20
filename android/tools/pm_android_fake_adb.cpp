// Stand-in for adb.exe in pm_android_test (AndroidSource without a phone or
// a real adb server).  No phone is ever contacted.  One paired phone
// 10.9.8.7:5555 "Fake Phone" is "connected"; `push` takes 4 s (the slow
// start-up step a takeover can interrupt; PM_FAKE_PUSH_MS overrides);
// anything unknown fails.
//
// With PM_FAKE_ADB_DIR set, the directory scripts a phone (files the test
// creates / removes while AndroidSource runs):
//   calls.log       every invocation is appended: "<ms>\t<args…>"
//   devices.txt     replaces the `devices -l` list (USB tests)
//   offline         the Wi-Fi phone dropped off: get-state / shell fail,
//                   connect fails, the scrcpy-server shell exits
//   exists.txt      phone paths that already exist (`ls -d`), one per line;
//                   every push appends its target
//   stat.txt        size `stat -c %s` prints (push writes its progress here)
//   push_fail       push prints this file to stderr and fails
//   install_result  what `install` prints (exit 0 only if it says Success)
//   server_error    the scrcpy-server shell prints this line and exits
//                   (the file is consumed: one shot)
// PM_FAKE_SCRCPY_PORT: the port `forward tcp:0` prints (the test's fake
// scrcpy-server listens there).
#include <windows.h>

#include <share.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string g_dir;

static std::string path(const char* name) { return g_dir + "\\" + name; }
static bool exists(const char* name) {
    return !g_dir.empty() && GetFileAttributesA(path(name).c_str()) != INVALID_FILE_ATTRIBUTES;
}
static std::string readFile(const char* name) {
    std::string s;
    if (g_dir.empty()) return s;
    for (int i = 0; i < 400; ++i) {  // an appendFile() may hold it exclusively for a moment
        if (FILE* f = fopen(path(name).c_str(), "rb")) {
            char b[4096];
            size_t n;
            while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
            fclose(f);
            return s;
        }
        if (GetFileAttributesA(path(name).c_str()) == INVALID_FILE_ATTRIBUTES) return s;
        Sleep(5);
    }
    return s;
}
static void appendFile(const char* name, const std::string& line) {
    if (g_dir.empty()) return;
    // Several fake adb processes run at once (push + progress polls): an
    // exclusive open serialises them (shared appends can overwrite each other).
    for (int i = 0; i < 400; ++i) {
        if (FILE* f = _fsopen(path(name).c_str(), "ab", _SH_DENYRW)) {
            fwrite(line.data(), 1, line.size(), f);
            fclose(f);
            return;
        }
        Sleep(5);
    }
}
static void writeFile(const char* name, const std::string& s) {
    if (g_dir.empty()) return;
    for (int i = 0; i < 50; ++i) {
        if (FILE* f = fopen(path(name).c_str(), "wb")) {
            fwrite(s.data(), 1, s.size(), f);
            fclose(f);
            return;
        }
        Sleep(5);
    }
}

static std::string envVar(const char* n) {
    char b[1024];
    DWORD r = GetEnvironmentVariableA(n, b, sizeof(b));
    return r > 0 && r < sizeof(b) ? std::string(b, r) : std::string();
}

// UTF-8 argv (adb paths may be Chinese).
static std::string arg(wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

int wmain(int argc, wchar_t** wargv) {
    g_dir = envVar("PM_FAKE_ADB_DIR");
    std::string* argv = new std::string[size_t(argc)];
    for (int k = 0; k < argc; ++k) argv[k] = arg(wargv[k]);
    {
        std::string line = std::to_string(GetTickCount64()) + "\t";
        for (int k = 1; k < argc; ++k) line += (k > 1 ? " " : "") + argv[k];
        appendFile("calls.log", line + "\n");
    }
    int i = 1;
    if (i + 1 < argc && argv[i] == "-P") i += 2;  // private server port
    std::string serial;
    if (i + 1 < argc && argv[i] == "-s") serial = argv[i + 1], i += 2;  // device serial
    if (i >= argc) return 1;
    const std::string cmd = argv[i];
    const bool offline = exists("offline") && (serial.empty() || serial.find(':') != std::string::npos);
    if (cmd == "start-server" || cmd == "kill-server") return 0;
    if (cmd == "devices") {
        if (exists("devices.txt")) {
            std::printf("List of devices attached\n%s\n", readFile("devices.txt").c_str());
            return 0;
        }
        std::printf("List of devices attached\n10.9.8.7:5555          %s product:fake model:Fake_Phone device:fake\n\n",
                    offline ? "offline" : "device");
        return 0;
    }
    if (cmd == "get-state") {
        if (offline) {
            std::fprintf(stderr, "error: device offline\n");
            return 1;
        }
        std::printf("device\n");
        return 0;
    }
    if (cmd == "connect" && i + 1 < argc) {
        if (offline) {
            std::printf("failed to connect to '%s': Connection timed out\n", argv[i + 1].c_str());
            return 1;
        }
        std::printf("already connected to %s\n", argv[i + 1].c_str());
        return 0;
    }
    if (cmd == "disconnect") {
        std::printf("disconnected %s\n", i + 1 < argc ? argv[i + 1].c_str() : "everything");
        return 0;
    }
    if (cmd == "shell") {
        if (offline) {
            std::fprintf(stderr, "error: closed\n");
            return 1;
        }
        std::string rest;
        for (int k = i + 1; k < argc; ++k) rest += (k > i + 1 ? " " : "") + argv[k];
        if (rest.find("app_process") != std::string::npos) {  // the scrcpy-server
            std::printf("[server] INFO: Device: [fake] Fake Phone (Android 14)\n");
            std::fflush(stdout);
            for (;;) {
                Sleep(50);
                if (exists("offline")) return 255;  // transport gone
                if (exists("server_error")) {
                    std::string l = readFile("server_error");
                    DeleteFileA(path("server_error").c_str());
                    std::printf("%s\n", l.c_str());
                    std::fflush(stdout);
                    return 1;
                }
            }
        }
        if (i + 1 < argc && argv[i + 1] == "echo") {
            std::string out;
            for (int k = i + 2; k < argc; ++k) out += (k > i + 2 ? " " : "") + argv[k];
            std::printf("%s\n", out.c_str());
            return 0;
        }
        if (rest.rfind("ls -d ", 0) == 0) {
            // ls -d '<path>' 2>/dev/null
            size_t a = rest.find('\''), b = rest.rfind('\'');
            std::string p = a != std::string::npos && b > a ? rest.substr(a + 1, b - a - 1) : std::string();
            std::string list = "\n" + readFile("exists.txt");
            if (!p.empty() && list.find("\n" + p + "\n") != std::string::npos) {
                std::printf("%s\n", p.c_str());
                return 0;
            }
            return 1;
        }
        if (rest.rfind("stat -c %s ", 0) == 0) {
            std::string s = readFile("stat.txt");
            if (s.empty()) return 1;
            std::printf("%s\n", s.c_str());
            return 0;
        }
        std::printf("Fake Phone\n");
        return 0;
    }
    if (cmd == "push") {
        int ms = 4000;
        std::string e = envVar("PM_FAKE_PUSH_MS");
        if (!e.empty()) ms = atoi(e.c_str());
        if (exists("push_fail")) {
            Sleep(DWORD(ms / 4));
            std::fprintf(stderr, "%s\n", readFile("push_fail").c_str());
            return 1;
        }
        // Progress the way the phone sees it: the target grows.
        unsigned long long size = 0;
        if (i + 1 < argc) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(wargv[i + 1], GetFileExInfoStandard, &fa))
                size = (unsigned long long)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
        }
        const int steps = 4;
        for (int k = 1; k <= steps; ++k) {
            Sleep(DWORD(ms / steps));
            writeFile("stat.txt", std::to_string(size * unsigned(k) / steps));
        }
        if (i + 2 < argc) appendFile("exists.txt", argv[i + 2] + "\n");
        std::printf("%s: 1 file pushed, 0 skipped.\n", i + 1 < argc ? argv[i + 1].c_str() : "?");
        return 0;
    }
    if (cmd == "install") {
        Sleep(200);
        if (exists("install_result")) {
            std::string r = readFile("install_result");
            std::printf("Performing Streamed Install\n%s\n", r.c_str());
            return r.find("Success") != std::string::npos ? 0 : 1;
        }
        std::printf("Performing Streamed Install\nSuccess\n");
        return 0;
    }
    if (cmd == "forward") {
        if (i + 1 < argc && argv[i + 1] == "--remove") return 0;
        std::string port = envVar("PM_FAKE_SCRCPY_PORT");
        if (!port.empty()) {
            std::printf("%s\n", port.c_str());
            return 0;
        }
    }
    if (cmd == "mdns") {
        std::printf("List of discovered mdns services\n");
        return 0;
    }
    std::fprintf(stderr, "fake adb: unsupported command %s\n", cmd.c_str());
    return 1;
}
