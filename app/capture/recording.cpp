// 自在投影 app: capture/recording.cpp — capture（截圖、錄影）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/share.h"
#include "capture/capture.h"

namespace pm_app {

unsigned long long freeBytes(const fs::path& dir) {
    ULARGE_INTEGER avail{};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &avail, nullptr, nullptr)) return ~0ull;  // unknown: do not block
    return avail.QuadPart;
}

// Detaches the taps and finalizes the MP4. `toast` (if not empty) replaces
// the default 「錄影已儲存：file」.
void stopRecording(const std::wstring& toast) {
    if (!recording()) return;
    KillTimer(g.hwnd, kRecordTimer);
    g.window->setFrameTap(nullptr);
    if (g.audio) g.audio->setPcmMonitor(nullptr);
    g.recorder->stop();
    g.window->setRecording(false);
    const fs::path file = g.recordingFile;
    g.recordingFile.clear();
    g.recordingStoppedAtMs = nowMs();
    trayRetip();  // 0.7.9: the tray tooltip no longer says 錄影中
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    g.log->write("info", "recording stopped: " + toUtf8(file.wstring()) +
                             (ec ? std::string(" (no file)") : " (" + std::to_string(size / 1024) + " KB)"));
    // Nothing arrived (the recorder kept no file): say so, not 「錄影已儲存」.
    g.window->showToast(ec ? std::wstring(tr(S::RecNothing))
                           : toast.empty() ? fmt(S::RecSaved, {file.filename().wstring()}) : toast);
    if (!ec && size > 0) {
        g.lastRec = file;
        if (!g.quitting) offerShare(file);
    }
}

// The picture on screen as the recording's start picture (pm::Recorder::
// setStartPicture): a phone that has paused the stream on a still screen, or a
// still Android screen, sends no new picture, and the recording would keep
// nothing. Via the snapshot PNG (with the view's rotation / mirroring, which
// the frame tap does not have: undone here) -> BT.709 limited-range NV12.
void seedRecording(pm::Recorder* rec) {
    const long long t0 = nowMs();
    const fs::path tmp = fs::temp_directory_path() / (L"zizai-rec-start-" + std::to_wstring(GetCurrentProcessId()) + L".png");
    if (!g.window->saveSnapshot(tmp.wstring())) return;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::vector<uint8_t> bgra;
    UINT sw = 0, sh = 0;
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateDecoderFromFilename(tmp.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&sw, &sh)) && sw >= 2 && sh >= 2 && sw <= 16384 && sh <= 16384) {
        bgra.resize(size_t(sw) * sh * 4);
        if (FAILED(conv->CopyPixels(nullptr, sw * 4, static_cast<UINT>(bgra.size()), bgra.data()))) bgra.clear();
    }
    if (conv) conv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (wic) wic->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    std::error_code ec;
    fs::remove(tmp, ec);
    if (bgra.empty()) return;
    // Snapshot = mirror(rotate(picture, r)); back to the picture's own orientation.
    const int r = g.settings.rotation & 3;
    const bool mir = g.settings.mirrored;
    const int W = static_cast<int>((r & 1) ? sh : sw) & ~1, H = static_cast<int>((r & 1) ? sw : sh) & ~1;
    if (W < 2 || H < 2) return;
    const int rw = static_cast<int>(sw), rh = static_cast<int>(sh);  // snapshot size
    auto at = [&](int x, int y) -> const uint8_t* {
        int xr = x, yr = y;  // picture (x, y) in the rotated view
        if (r == 1) xr = (rh - 1) - y, yr = x;  // 90° clockwise: W x H -> H x W
        else if (r == 2) xr = (rw - 1) - x, yr = (rh - 1) - y;
        else if (r == 3) xr = y, yr = (rh - 1) - x;
        if (mir) xr = (rw - 1) - xr;
        xr = std::clamp(xr, 0, rw - 1);
        yr = std::clamp(yr, 0, rh - 1);
        return &bgra[(size_t(yr) * rw + xr) * 4];
    };
    std::vector<uint8_t> nv12(size_t(W) * H * 3 / 2);
    uint8_t* Y = nv12.data();
    uint8_t* UV = Y + size_t(W) * H;
    auto yOf = [](const uint8_t* p) { return 16 + (47 * p[2] + 157 * p[1] + 16 * p[0] + 128) / 256; };  // 709, 16..235
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) Y[size_t(y) * W + x] = static_cast<uint8_t>(yOf(at(x, y)));
    for (int y = 0; y < H / 2; ++y)
        for (int x = 0; x < W / 2; ++x) {
            int b = 0, gr = 0, rd = 0;
            for (int k = 0; k < 4; ++k) {
                const uint8_t* p = at(2 * x + (k & 1), 2 * y + (k >> 1));
                b += p[0], gr += p[1], rd += p[2];
            }
            b /= 4, gr /= 4, rd /= 4;
            // BT.709: Cb = (B - Y') / 1.8556, Cr = (R - Y') / 1.5748, scaled to 16..240.
            const double yl = 0.2126 * rd + 0.7152 * gr + 0.0722 * b;
            const int cb = static_cast<int>(std::lround(128 + (b - yl) / 1.8556 * 224 / 255));
            const int cr = static_cast<int>(std::lround(128 + (rd - yl) / 1.5748 * 224 / 255));
            UV[size_t(y) * W + 2 * x] = static_cast<uint8_t>(std::clamp(cb, 16, 240));
            UV[size_t(y) * W + 2 * x + 1] = static_cast<uint8_t>(std::clamp(cr, 16, 240));
        }
    rec->setStartPicture(nv12.data(), W, H, W);
    g.log->write("info", "recording: start picture " + std::to_string(W) + "x" + std::to_string(H) + " from the screen (" +
                             std::to_string(nowMs() - t0) + " ms)");
}

void startRecording() {
    if (recording()) return;
    if (!pictureShowing()) {
        g.window->showToast(tr(S::RecNeedPicture));
        return;
    }
    const fs::path dir = recordingDir();
    if (freeBytes(dir) < kMinFreeBytes) {
        g.window->showToast(tr(S::RecNoSpace));
        return;
    }
    const fs::path file = stampedFile(dir, L".mp4");
    if (!g.recorder->start(file.wstring())) {
        g.log->write("error", "recording failed to start: " + toUtf8(file.wstring()));
        g.window->showToast(tr(S::RecStartFail));
        return;
    }
    g.recordingFile = file;
    pm::Recorder* rec = g.recorder;
    g.window->setFrameTap([rec](const uint8_t* nv12, int w, int h, int stride, uint64_t ptsNs) {
        rec->onVideoFrame(nv12, w, h, stride, ptsNs);
    });
    if (g.audio)
        g.audio->setPcmMonitor([rec](const int16_t* pcm, size_t frames, int ch, int rate, uint64_t whenNs) {
            rec->onPcm(pcm, frames, ch, rate, whenNs);
        });
    seedRecording(rec);  // used only if no new picture comes (paused / still phone)
    g.window->setRecording(true);
    SetTimer(g.hwnd, kRecordTimer, 1000, nullptr);
    trayRetip();  // 0.7.9: 「自在投影 — 錄影中」 in the tray tooltip
    g.log->write("info", "recording started: " + toUtf8(file.wstring()));
    g.window->showToast(fmt(S::RecStarted, {file.filename().wstring()}));
}

// Every second while recording: the encoder gave up (write error, disk
// full, encoder failure) or the disk is almost full -> stop with a toast.
void checkRecording() {
    if (!recording()) {
        KillTimer(g.hwnd, kRecordTimer);
        return;
    }
    if (!g.recorder->active()) {
        g.log->write("error", "recorder stopped by itself (encoder / write failure)");
        stopRecording(fmt(S::RecError, {g.recordingFile.filename().wstring()}));
        return;
    }
    if (freeBytes(g.recordingFile.parent_path()) < kMinFreeBytes) {
        g.log->write("warn", "disk almost full: stopping the recording");
        stopRecording(fmt(S::RecDiskFull, {g.recordingFile.filename().wstring()}));
    }
}

void toggleRecording() {
    if (recording()) stopRecording();
    else startRecording();
}

}  // namespace pm_app
