// 自在投影 app: core/app/dev_harness.cpp — core/app（視窗程序、命令分派、--dev 測試台）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "share/share.h"
#include "capture/capture.h"
#include "translate/translate.h"
#include "connect/connect.h"
#include "view/view.h"
#include "update/update.h"
#include "menus/menus.h"
#include "core/app/core_app.h"

namespace pm_app {

// Fake Miracast cast: arbitration like MiracastReceiver's onStatus(Connecting),
// then ~30 fps BGRA pictures (gradient, moving band, colour blocks).
void runTestMiracast(TestFeed tf, pm::VideoWindow* win, HWND h, Log* log, std::atomic<bool>* stop) {
    Sleep(tf.delayMs);
    const std::wstring name = L"Galaxy S24";
    const int a = g_active.load();
    if (a != SrcNone && a != SrcMiracast && !g_takeoverNew.load()) {
        postSource(h, MiraRefused, name);
        return;
    }
    claimSource(SrcMiracast, true);
    postSource(h, MiraConnecting, name);
    if (tf.startFail) {  // --test-start-fail: ConnectionPending, then back to Listening (no cast, no Disconnected)
        Sleep(1500);
        log->write("info", "test miracast: pending connection went back to listening");
        postSource(h, MiraStatus, std::to_wstring(static_cast<int>(pm::MiracastReceiver::Status::Idle)));
        return;
    }
    Sleep(400);
    postSource(h, MiraConnected, name);
    log->write("info", "test miracast: casting");
    const int W = 540, H = 1170;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
    const auto t0 = std::chrono::steady_clock::now();
    for (long long n = 0; !*stop; ++n) {
        if (g_active.load() != SrcMiracast) return;  // taken over
        if (tf.seconds > 0 && std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(tf.seconds)) break;
        const int band = static_cast<int>((n * 9) % (H + 120)) - 60;
        for (int y = 0; y < H; ++y) {
            uint8_t* row = px.data() + static_cast<size_t>(y) * W * 4;
            const bool inBand = y >= band && y < band + 60;
            for (int x = 0; x < W; ++x) {
                uint8_t r = static_cast<uint8_t>(40 + 160 * y / H), gg = static_cast<uint8_t>(60 + 120 * x / W), b = 170;
                if (y > 120 && y < 300 && x > 40 && x < W - 40) r = 250, gg = 250, b = 250;  // "status card"
                for (int k = 0; k < 4; ++k)
                    if (y > 360 + k * 180 && y < 500 + k * 180 && x > 40 + (k % 2) * 250 && x < 250 + (k % 2) * 250)
                        r = static_cast<uint8_t>(60 * k), gg = 200, b = static_cast<uint8_t>(255 - 50 * k);
                if (inBand) r = 255, gg = 160, b = 170;
                row[x * 4 + 0] = b;
                row[x * 4 + 1] = gg;
                row[x * 4 + 2] = r;
                row[x * 4 + 3] = 255;
            }
        }
        win->submitBgraFrame(px.data(), W, H, W * 4, 0);
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(33 * (n + 1)));
    }
    if (*stop || g_active.load() != SrcMiracast) return;
    log->write("info", "test miracast: cast ended");
    win->onReset();
    postSource(h, MiraDisconnected);
}

// Access units of an Annex-B H.264 stream: a new AU starts at an AUD / SPS /
// PPS / SEI after a slice, or at a slice with first_mb_in_slice == 0.
std::vector<std::pair<size_t, size_t>> splitH264(const std::string& d) {
    std::vector<size_t> starts;  // offsets of start codes (00 00 01 / 00 00 00 01)
    for (size_t i = 0; i + 3 < d.size(); ++i)
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
            starts.push_back(i);
            i += d[i + 2] == 1 ? 2 : 3;
        }
    std::vector<std::pair<size_t, size_t>> aus;
    size_t auStart = std::string::npos;
    bool haveSlice = false;
    for (size_t k = 0; k < starts.size(); ++k) {
        const size_t s = starts[k];
        const size_t hdr = s + (d[s + 2] == 1 ? 3 : 4);
        if (hdr >= d.size()) break;
        const int type = d[hdr] & 0x1F;
        const bool slice = type == 1 || type == 5;
        const bool firstMb = slice && hdr + 1 < d.size() && (static_cast<unsigned char>(d[hdr + 1]) & 0x80);
        const bool boundary = haveSlice && ((slice && firstMb) || type == 9 || type == 7 || type == 8 || type == 6);
        if (auStart == std::string::npos) auStart = s;
        if (boundary) {
            aus.push_back({auStart, s - auStart});
            auStart = s;
            haveSlice = false;
        }
        haveSlice |= slice;
    }
    if (auStart != std::string::npos && haveSlice) aus.push_back({auStart, d.size() - auStart});
    return aus;
}

void runTestFeed(TestFeed tf, pm::VideoSink* sink, HWND h, Log* log, std::atomic<bool>* stop) {
    std::ifstream in(fs::path(tf.file), std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto aus = splitH264(data);
    log->write("info", "test feed: " + std::to_string(aus.size()) + " access units from " + toUtf8(tf.file));
    if (aus.empty()) return;
    Sleep(tf.delayMs);
    const bool android = tf.source == SrcAndroid;
    if (android) {
        postSource(h, AndTestConnected, L"Galaxy S24");  // a model name, not translated
        for (int i = 0; i < 60 && g_active.load() != SrcAndroid && !*stop; ++i) Sleep(50);
        if (g_active.load() != SrcAndroid) {
            log->write("info", "test feed: android phone refused");
            return;
        }
        if (tf.startFail) {  // what AndroidSource reports when scrcpy-server does not come up
            Sleep(1500);
            log->write("info", "test feed: android start-up fails");
            postSource(h, AndState, std::to_wstring(static_cast<int>(pm::AndroidSource::State::Error)) + L"投影啟動失敗：test");
            return;
        }
    } else if (tf.startFail) {  // --test-start-fail: a PIN that is cancelled on the iPhone (no SETUP)
        postText(h, EvPin, L"1234");
        Sleep(2500);
        log->write("info", "test feed: PIN cancelled on the phone");
        postText(h, EvPin, {});
        return;
    } else {
        postText(h, EvConnecting, tr(S::TestPhone));
    }
    if (tf.noPicture) {
        log->write("info", "test feed: connected, no picture");
        while (!*stop) Sleep(100);
        return;
    }
    Sleep(300);
    sink->onCodec(pm::VideoCodec::H264);
    const auto t0 = std::chrono::steady_clock::now();
    bool tookOver = false;
    size_t idx = 0;
    const int kick = g_testKick.load();
    for (long long n = 0; !*stop; ++n) {
        if (android && g_active.load() != SrcAndroid) return;  // taken over / stopped
        if (g_testKick.load() != kick) {  // 中斷連線 by the user: the phone stops sending
            log->write("info", "test feed: disconnected by the user");
            return;
        }
        if (tf.stallAt > 0 && std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(tf.stallAt)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // silent, no reset: Wi-Fi gone
            continue;
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (tf.takeoverAt > 0 && !tookOver && elapsed >= std::chrono::seconds(tf.takeoverAt)) {
            tookOver = true;
            sink->onReset();  // what the core does to the old session's sinks
            postText(h, EvTakeover, tr(S::TestPhone2));
            Sleep(200);
            postText(h, EvConnecting, tr(S::TestPhone2));
            sink->onCodec(pm::VideoCodec::H264);
            idx = 0;  // restart at the IDR
        }
        if (tf.seconds > 0 && elapsed >= std::chrono::seconds(tf.seconds)) break;
        if (idx == aus.size()) {  // loop: the decoder restarts at the IDR
            idx = 0;
            sink->onCodec(pm::VideoCodec::H264);
        }
        const auto& au = aus[idx++];
        sink->onFrame(reinterpret_cast<const uint8_t*>(data.data() + au.first), au.second, 0);
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(33 * (n + 1)));
    }
    if (*stop) return;
    log->write("info", "test feed: phone gone");
    if (android) {
        sink->onReset();  // AndroidSource resets the sinks before onDisconnected
        postSource(h, AndDisconnected);
        return;
    }
    postText(h, EvDisconnected, {});
}

}  // namespace pm_app
