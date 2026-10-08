// pm_miracast_test: checks for the Miracast receiver (pm_miracast).
//
//   pm_miracast_test --status
//       Prints MiracastReceiver::unsupportedReason() and the raw receiver
//       status (WiFiStatus / ListeningStatus / current settings).
//   pm_miracast_test --listen [--name 自在投影] [--seconds N]
//       Starts the receiver with a VideoWindow; cast from a phone.  Logs every
//       status / connection event.  N = 0 (default): until the window closes.
//   pm_miracast_test --file sample.mp4 [--seconds N] [--snapshot out.png] [--no-crop]
//       Plays a local file through the same MediaPlayer frame-server path
//       (FramePump) into a VideoWindow and prints frame statistics each
//       second.  Exit code 0 if frames were delivered.
#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Miracast.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "frame_pump.h"
#include "pm/miracast_receiver.h"
#include "pm/video_window.h"

namespace {

std::string u8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

const auto g_t0 = std::chrono::steady_clock::now();
void logLine(const std::string& s) {
    std::printf("[%7.3f] %s\n", secondsSince(g_t0), s.c_str());
    std::fflush(stdout);
}

const char* statusName(pm::MiracastReceiver::Status s) {
    static const char* n[] = {"Unavailable", "Disabled", "Idle", "Connecting", "Connected"};
    return n[static_cast<int>(s)];
}

int cmdStatus() {
    namespace wmm = winrt::Windows::Media::Miracast;
    const std::wstring reason = pm::MiracastReceiver::unsupportedReason();
    logLine("unsupportedReason: " + (reason.empty() ? std::string("(none: supported)") : u8(reason)));
    try {
        wmm::MiracastReceiver r;
        auto st = r.GetStatus();
        char buf[256];
        std::snprintf(buf, sizeof buf, "WiFiStatus=%d ListeningStatus=%d takeoverSupported=%d maxConnections=%d",
                      static_cast<int>(st.WiFiStatus()), static_cast<int>(st.ListeningStatus()),
                      static_cast<int>(st.IsConnectionTakeoverSupported()), st.MaxSimultaneousConnections());
        logLine(buf);
        auto cur = r.GetCurrentSettings();
        logLine("current settings: name=\"" + u8(std::wstring(cur.FriendlyName())) + "\" model=\"" +
                u8(std::wstring(cur.ModelName())) + "\" auth=" +
                std::to_string(static_cast<int>(cur.AuthorizationMethod())));
    } catch (const winrt::hresult_error& e) {
        logLine("GetStatus failed: " + std::to_string(static_cast<long>(e.code())));
    }
    return reason.empty() ? 0 : 2;
}

int cmdListen(const std::wstring& name, int seconds) {
    pm::VideoWindow win;
    if (!win.create(L"pm_miracast_test (Miracast)")) return 1;
    win.setIdleHints({L"Android：投放 / Smart View → " + name});
    pm::MiracastReceiver rx;
    rx.log = logLine;
    pm::MiracastReceiver::Events ev;
    ev.onStatus = [](pm::MiracastReceiver::Status s, const std::wstring& d) {
        logLine(std::string("EVENT status ") + statusName(s) + " " + u8(d));
    };
    ev.onConnected = [&](const std::wstring& n) {
        logLine("EVENT connected " + u8(n));
        win.showToast(L"已連線：" + n);
    };
    ev.onDisconnected = [] { logLine("EVENT disconnected"); };
    ev.onPin = [&](const std::wstring& pin) {
        logLine("EVENT pin \"" + u8(pin) + "\"");
        win.showPin(pin);
    };
    if (!rx.start(name, &win, ev)) {
        logLine("start() failed; status " + std::string(statusName(rx.status())));
        win.close();
        win.runMessageLoop();
        return 2;
    }
    std::atomic<bool> done{false};
    std::thread timer([&] {
        auto t0 = std::chrono::steady_clock::now();
        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (seconds > 0 && secondsSince(t0) >= seconds) {
                win.close();
                break;
            }
        }
    });
    win.runMessageLoop();
    done = true;
    timer.join();
    rx.stop();
    return 0;
}

int cmdFile(const std::wstring& file, int seconds, const std::wstring& snapshot, bool crop) {
    namespace wmc = winrt::Windows::Media::Core;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const std::wstring abs = std::filesystem::absolute(file).wstring();
    if (!std::filesystem::exists(abs)) {
        logLine("file not found: " + u8(abs));
        return 1;
    }
    pm::VideoWindow win;
    if (!win.create(L"pm_miracast_test (file)")) return 1;
    pm::FramePump pump(&win, logLine);
    pump.setAutoCrop(crop);
    std::atomic<bool> ended{false};
    pump.onEnded = [&] { ended = true; };
    pump.onFailed = [&](const std::wstring& e) {
        logLine("FAILED: " + u8(e));
        ended = true;
    };
    pump.onFirstFrame = [](int w, int h) { logLine("first frame " + std::to_string(w) + "x" + std::to_string(h)); };
    if (!pump.init()) return 1;
    wmc::MediaSource src{nullptr};
    try {
        src = wmc::MediaSource::CreateFromUri(winrt::Windows::Foundation::Uri(abs));
    } catch (const winrt::hresult_error& e) {
        logLine("CreateFromUri failed " + std::to_string(static_cast<long>(e.code())));
        return 1;
    }
    if (!pump.play(src, false)) return 1;

    std::atomic<bool> done{false};
    long long delivered = 0;
    std::thread timer([&] {
        const auto t0 = std::chrono::steady_clock::now();
        long long last = 0;
        while (!done) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const auto s = pump.stats();
            const auto v = win.stats();
            char buf[400];
            std::snprintf(buf, sizeof buf,
                          "avail=%lld copied=%lld delivered=%lld (+%lld/s) dropped=%lld copyErr=%lld size=%dx%d "
                          "crop=%d,%d %dx%d copy=%.2fms readback=%.2fms | window presented=%lld %dx%d",
                          s.framesAvailable, s.framesCopied, s.framesDelivered, s.framesDelivered - last,
                          s.framesDropped, s.copyErrors, s.width, s.height, s.cropX, s.cropY, s.cropW, s.cropH,
                          s.copyAvgMs, s.readbackAvgMs, v.framesPresented, v.width, v.height);
            logLine(buf);
            last = s.framesDelivered;
            delivered = s.framesDelivered;
            if (ended || (seconds > 0 && secondsSince(t0) >= seconds)) {
                if (!snapshot.empty())
                    logLine(std::string("snapshot ") + (win.saveSnapshot(snapshot) ? "saved " : "FAILED ") + u8(snapshot));
                pump.stop();
                win.close();
                break;
            }
        }
    });
    win.runMessageLoop();
    done = true;
    timer.join();
    pump.stop();
    logLine(delivered > 0 ? "RESULT ok" : "RESULT no frames delivered");
    return delivered > 0 ? 0 : 3;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::wstring mode, file, name = L"自在投影", snapshot;
    int seconds = 0;
    bool crop = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--status" || a == L"--listen") mode = a;
        else if (a == L"--file" && i + 1 < argc) mode = a, file = argv[++i];
        else if (a == L"--name" && i + 1 < argc) name = argv[++i];
        else if (a == L"--seconds" && i + 1 < argc) seconds = _wtoi(argv[++i]);
        else if (a == L"--snapshot" && i + 1 < argc) snapshot = argv[++i];
        else if (a == L"--no-crop") crop = false;
    }
    if (mode == L"--status") return cmdStatus();
    if (mode == L"--listen") return cmdListen(name, seconds);
    if (mode == L"--file") return cmdFile(file, seconds, snapshot, crop);
    std::printf("usage: pm_miracast_test --status | --listen [--name N] [--seconds S] | --file F [--seconds S] "
                "[--snapshot out.png] [--no-crop]\n");
    return 1;
}
