// Owner test with a real Android phone (see docs/android.md):
//   pm_android_demo              reconnect a paired phone, else show the QR
//   pm_android_demo --qr         always pair with a new QR code
//   pm_android_demo --code 192.168.1.23:37011 123456   pair with a 6-digit code
// The QR is printed in the console and opened as an image; the phone is
// mirrored in a VideoWindow (+ audio). Mouse/keyboard control works when the
// video module implements VideoWindow::setPointerHandler/setKeyHandler.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "pm/android_source.h"
#include "pm/audio_player.h"
#include "pm/video_window.h"

static std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L'\\'));
}

static std::string u8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

static void printQr(const std::vector<uint8_t>& bgra, int size, int moduleScale) {
    // One console row per module, two spaces per module, black on white (ANSI).
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(out, &mode);
    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    std::string s;
    for (int y = moduleScale / 2; y < size; y += moduleScale) {
        for (int x = moduleScale / 2; x < size; x += moduleScale) {
            bool dark = bgra[(size_t(y) * size_t(size) + size_t(x)) * 4] < 128;
            s += dark ? "\x1b[40m  " : "\x1b[107m  ";
        }
        s += "\x1b[0m\n";
    }
    fputs(s.c_str(), stdout);
    fflush(stdout);
}

static void openQrImage(const std::vector<uint8_t>& bgra, int size) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"自在投影_配對QR.bmp";
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + DWORD(size) * DWORD(size) * 4;
    ih.biSize = sizeof(ih);
    ih.biWidth = size;
    ih.biHeight = -size;  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
    f.write(reinterpret_cast<const char*>(bgra.data()), std::streamsize(bgra.size()));
    f.close();
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool forceQr = false;
    std::wstring codeHost, code;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--qr") forceQr = true;
        else if (a == L"--code" && i + 2 < argc) codeHost = argv[++i], code = argv[++i];
    }

    pm::VideoWindow win;
    if (!win.create(L"自在投影 Android 測試")) return 1;
    pm::AudioPlayer audio;
    audio.start();

    pm::AndroidSource src;
    src.log = [](const std::string& s) { printf("%s\n", s.c_str()); fflush(stdout); };
    auto showQr = [&] {
        std::vector<uint8_t> bgra;
        int size = 0;
        std::wstring text;
        if (!src.beginQrPairing(bgra, size, text)) { printf("QR pairing failed to start\n"); return; }
        printf("\n手機：設定 → 開發人員選項 → 無線偵錯 → 使用 QR 圖碼配對裝置，掃描下面的 QR 圖碼\n%s\n\n", u8(text).c_str());
        printQr(bgra, size, 8);
        openQrImage(bgra, size);
    };
    src.events.onState = [&](pm::AndroidSource::State s, const std::wstring& d) {
        printf(">> [%d] %s\n", int(s), u8(d).c_str());
        fflush(stdout);
        static bool qrShown = false;
        // Reconnect found nothing → fall back to the QR code (once).
        if (s == pm::AndroidSource::State::Idle && !qrShown && codeHost.empty() &&
            (d.find(L"沒有找到") != std::wstring::npos || d.find(L"尚未配對") != std::wstring::npos)) {
            qrShown = true;
            showQr();
        }
    };
    src.events.onConnected = [&](const std::wstring& name) {
        printf(">> connected: %s → start mirroring\n", u8(name).c_str());
        win.setConnecting(name);
        src.start(&win, &audio);
    };
    src.events.onDisconnected = [&] {
        printf(">> disconnected; trying to reconnect\n");
        src.connectKnownDevices();
    };
    if (!src.init(exeDir() + L"\\android-tools")) {
        printf("android-tools missing next to the exe (run android/third_party/fetch_tools.ps1 and rebuild)\n");
        return 1;
    }
#ifdef PM_HAVE_POINTER_HANDLER
    win.setPointerHandler([&](const pm::VideoWindow::PointerEvent& e) { src.sendPointer(e); });
    win.setKeyHandler([&](unsigned vk, bool down, wchar_t ch) { src.sendKey(vk, down, ch); });
    printf("control: mouse = touch, right-click = back, middle-click = home, wheel = scroll, keyboard = text/keys\n");
#else
    printf("control: VideoWindow pointer/key handlers not implemented in this build (view only)\n");
#endif

    if (!codeHost.empty()) {
        if (!src.pairWithCode(codeHost, code)) printf("bad --code arguments (IP:port 6-digit-code)\n");
    } else if (forceQr) {
        showQr();
    } else {
        src.connectKnownDevices();
    }
    int rc = win.runMessageLoop();
    src.stop();
    audio.stop();
    return rc;
}
