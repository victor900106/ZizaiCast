// pm_scroll_test: offline test of the 即時翻譯 scroll tracker (live_scroll_tracker).
//
//   pm_scroll_test [--dir launch/_work/eval_web/png] [--verbose]
//
// Builds a long "web page" from screenshots of the evaluation set (scaled
// to a 1080-wide phone), shows it through a 1080 x 2400 phone screen with a
// static header (200 px) and footer (140 px), and scrolls it frame by frame
// (still, flings, drags, a page change).  Each frame gets +-2 levels of noise
// (video compression) and is reduced like the renderer does on the GPU
// (bilinear, 256 columns x 1024 rows).  Text boxes are the ground-truth
// boxes of the screenshots (*.gt.tsv).  The anchor (the picture the shown
// boxes were recognised on) follows the live-mode cadence: a run when the
// picture has been still for 300 ms (2 s apart), its boxes 0.7 s later.
//
// Reports: box offset error (px, phone pixels) of the boxes that are on
// screen, boxes shown where they are not / hidden where they are, header
// boxes moved, frames wrongly hidden, a page change not noticed, and the
// tracker's time per frame (signature + match; the 256 x 1024 copy itself is
// made by the GPU in the app).  Exit code 1 if a limit is exceeded.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "../src/live_scroll_tracker.h"

using pm::video::ScrollSig;
using pm::video::ScrollTracker;

namespace {

constexpr int W = 1080, H = 2400, kHeader = 200, kFooter = 140, kViewH = H - kHeader - kFooter;
constexpr int SW = 256, SH = 1024;  // the renderer's signature copy

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // BGRA
};
struct Box {
    float x0, y0, x1, y1;  // phone px (page coords for page boxes)
};

bool readImage(const std::wstring& path, Image& im) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->GetSize(&w, &h)))
        return false;
    im.w = static_cast<int>(w);
    im.h = static_cast<int>(h);
    im.px.resize(static_cast<size_t>(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(im.px.size()), im.px.data()));
}

// Area-average resize (the phone rendering a page at its width).
Image resize(const Image& s, int w, int h) {
    Image d;
    d.w = w;
    d.h = h;
    d.px.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const int sy0 = y * s.h / h, sy1 = std::max(sy0 + 1, (y + 1) * s.h / h);
        for (int x = 0; x < w; ++x) {
            const int sx0 = x * s.w / w, sx1 = std::max(sx0 + 1, (x + 1) * s.w / w);
            int acc[3] = {};
            for (int yy = sy0; yy < sy1; ++yy)
                for (int xx = sx0; xx < sx1; ++xx)
                    for (int c = 0; c < 3; ++c) acc[c] += s.px[(static_cast<size_t>(yy) * s.w + xx) * 4 + c];
            const int n = (sy1 - sy0) * (sx1 - sx0);
            uint8_t* o = &d.px[(static_cast<size_t>(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c) o[c] = static_cast<uint8_t>(acc[c] / n);
            o[3] = 255;
        }
    }
    return d;
}

// GPU-like bilinear reduction to dw x dh (texel centres, clamped edges).
void bilinear(const uint8_t* src, int sw, int sh, std::vector<uint8_t>& out, int dw, int dh) {
    out.resize(static_cast<size_t>(dw) * dh * 4);
    std::vector<int> xi(dw);
    std::vector<float> xf(dw);
    for (int i = 0; i < dw; ++i) {
        const float u = std::clamp((i + 0.5f) * sw / dw - 0.5f, 0.f, sw - 1.001f);
        xi[i] = static_cast<int>(u);
        xf[i] = u - xi[i];
    }
    for (int j = 0; j < dh; ++j) {
        const float v = std::clamp((j + 0.5f) * sh / dh - 0.5f, 0.f, sh - 1.001f);
        const int y0 = static_cast<int>(v);
        const float fy = v - y0;
        const uint8_t *r0 = src + static_cast<size_t>(y0) * sw * 4, *r1 = r0 + static_cast<size_t>(sw) * 4;
        uint8_t* o = &out[static_cast<size_t>(j) * dw * 4];
        for (int i = 0; i < dw; ++i) {
            const int x = xi[i];
            const float fx = xf[i];
            for (int c = 0; c < 4; ++c) {
                const float a = r0[x * 4 + c] * (1 - fx) + r0[x * 4 + 4 + c] * fx;
                const float b = r1[x * 4 + c] * (1 - fx) + r1[x * 4 + 4 + c] * fx;
                o[i * 4 + c] = static_cast<uint8_t>(a * (1 - fy) + b * fy + 0.5f);
            }
        }
    }
}

struct Page {
    Image im;
    std::vector<Box> boxes;
};

bool buildPage(const std::wstring& dir, const std::vector<std::wstring>& names, Page& page) {
    std::vector<Image> parts;
    int total = 0;
    for (const auto& n : names) {
        Image src;
        if (!readImage(dir + L"\\" + n + L".png", src)) {
            std::fprintf(stderr, "cannot read %ls\\%ls.png\n", dir.c_str(), n.c_str());
            return false;
        }
        const int h = static_cast<int>(std::lround(static_cast<double>(src.h) * W / src.w));
        parts.push_back(resize(src, W, h));
        std::ifstream f(dir + L"\\" + n + L".gt.tsv");
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            float x0, y0, x1, y1;
            if (!(ss >> x0 >> y0 >> x1 >> y1)) continue;
            page.boxes.push_back({x0 * W, total + y0 * h, x1 * W, total + y1 * h});
        }
        total += h;
    }
    page.im.w = W;
    page.im.h = total;
    page.im.px.reserve(static_cast<size_t>(W) * total * 4);
    for (const auto& p : parts) page.im.px.insert(page.im.px.end(), p.px.begin(), p.px.end());
    return true;
}

// The phone screen: header, page rows [off, off + kViewH), footer; noise.
void compose(const Page& page, const Image& header, const Image& footer, int off, std::mt19937& rng, std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(W) * H * 4);
    std::memcpy(out.data(), header.px.data(), static_cast<size_t>(W) * kHeader * 4);
    for (int y = 0; y < kViewH; ++y) {
        const int py = off + y;
        uint8_t* o = &out[static_cast<size_t>(kHeader + y) * W * 4];
        if (py >= 0 && py < page.im.h) std::memcpy(o, &page.im.px[static_cast<size_t>(py) * W * 4], static_cast<size_t>(W) * 4);
        else std::memset(o, 255, static_cast<size_t>(W) * 4);
    }
    std::memcpy(&out[static_cast<size_t>(H - kFooter) * W * 4], footer.px.data(), static_cast<size_t>(W) * kFooter * 4);
    std::uniform_int_distribution<int> nd(-2, 2);
    for (size_t i = 0; i < out.size(); i += 4)
        for (int c = 0; c < 3; ++c) out[i + c] = static_cast<uint8_t>(std::clamp(out[i + c] + nd(rng), 0, 255));
}

// This thread's CPU cycles per ms (the busiest of a few 20 ms spins: the
// least preempted); per-frame cost is measured in cycles of this thread, so
// other programs on a busy PC do not count.
double cyclesPerMs() {
    double best = 0;
    for (int k = 0; k < 8; ++k) {
        ULONG64 c0 = 0, c1 = 0;
        QueryThreadCycleTime(GetCurrentThread(), &c0);
        const auto t0 = std::chrono::steady_clock::now();
        volatile double x = 0;
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(20)) x = x + 1;
        QueryThreadCycleTime(GetCurrentThread(), &c1);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        best = std::max(best, (c1 - c0) / ms);
    }
    return best;
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5))];
}

}  // namespace

int main(int argc, char** argv) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring dir = L"launch\\_work\\eval_web\\png";
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) {
            const std::string d = argv[++i];
            dir.assign(d.begin(), d.end());
        } else if (a == "--verbose") {
            verbose = true;
        }
    }
    Page page, other;
    if (!buildPage(dir, {L"ja_ui_01", L"ja_receipt_04", L"ko_ui_01", L"en_menu_01", L"ja_ui_02", L"en_manual_03"}, page) ||
        !buildPage(dir, {L"ko_menu_02", L"ja_station_02"}, other))
        return 2;
    // Header: a coloured app bar with a title (text from another screenshot); footer: a tab bar.
    Image header, footer;
    {
        Image src;
        if (!readImage(dir + L"\\ja_error_01.png", src)) return 2;
        const Image s = resize(src, W, static_cast<int>(std::lround(static_cast<double>(src.h) * W / src.w)));
        header.w = footer.w = W;
        header.h = kHeader;
        footer.h = kFooter;
        header.px.assign(s.px.begin() + static_cast<size_t>(s.h / 3) * W * 4,
                         s.px.begin() + static_cast<size_t>(s.h / 3 + kHeader) * W * 4);
        footer.px.resize(static_cast<size_t>(W) * kFooter * 4);
        for (int y = 0; y < kFooter; ++y)
            for (int x = 0; x < W; ++x) {
                uint8_t* o = &footer.px[(static_cast<size_t>(y) * W + x) * 4];
                const bool icon = (x % 270) > 100 && (x % 270) < 170 && y > 30 && y < 100;
                o[0] = o[1] = o[2] = icon ? 90 : 245;
                o[3] = 255;
            }
    }
    const Box headerBox{80, 50, 700, 150};
    const int maxOff = page.im.h - kViewH;
    std::printf("page %dx%d, %zu text boxes, screen %dx%d (header %d, footer %d), signature %dx%d\n", W, page.im.h,
                page.boxes.size(), W, H, kHeader, kFooter, SW, SH);

    // Scroll script: page offset per frame (60 fps); -1 = the other page.
    std::vector<int> offs;
    {
        double off = 0;
        auto still = [&](int n) {
            for (int i = 0; i < n; ++i) offs.push_back(static_cast<int>(std::lround(off)));
        };
        auto fling = [&](double v, double decay) {
            while (std::fabs(v) > 0.6) {
                off = std::clamp(off + v, 0.0, static_cast<double>(maxOff));
                offs.push_back(static_cast<int>(std::lround(off)));
                v *= decay;
            }
        };
        auto drag = [&](double v, int n) {
            for (int i = 0; i < n; ++i) {
                off = std::clamp(off + v, 0.0, static_cast<double>(maxOff));
                offs.push_back(static_cast<int>(std::lround(off)));
            }
        };
        still(30);
        drag(3, 40);         // slow reading scroll
        still(60);
        fling(90, 0.95);     // a fling (~1.7 screens)
        still(150);
        drag(-14.5, 50);     // dragged back up, fractional speed
        still(60);
        fling(-60, 0.9);
        still(150);
        fling(180, 0.93);    // a hard fling (several screens)
        still(150);
        drag(7, 120);
        still(40);
        for (int i = 0; i < 20; ++i) offs.push_back(-1);  // another page
        still(20);           // and back
    }

    std::mt19937 rng(1234);
    ScrollTracker tr;
    std::vector<uint8_t> frame, thumb;
    std::vector<double> errs, times, cpu;
    const double cpms = cyclesPerMs();
    int anchorOff = 0, anchorFrame = -1;
    int pendingAt = -1, pendingOff = 0;  // a run's grab: its boxes arrive kLatency frames later
    ScrollSig pendingSig;
    constexpr int kLatency = 42, kSettle = 18, kGap = 120;
    int lastMove = -1000, lastRun = -1000;
    int wrongShown = 0, wrongHidden = 0, headerMoved = 0, framesHiddenWrongly = 0, pageChangeMissed = 0, okFrames = 0;
    int boxFrames = 0;
    double maxErr = 0;
    for (size_t f = 0; f < offs.size(); ++f) {
        const bool otherPage = offs[f] < 0;
        compose(otherPage ? other : page, header, footer, otherPage ? 0 : offs[f], rng, frame);
        bilinear(frame.data(), W, H, thumb, SW, SH);
        const auto t0 = std::chrono::steady_clock::now();
        ULONG64 k0 = 0, k1 = 0;
        QueryThreadCycleTime(GetCurrentThread(), &k0);
        const ScrollSig sig = pm::video::makeScrollSig(thumb.data(), SW, SH, SW * 4);
        if (anchorFrame < 0) {  // first run: boxes of frame 0
            tr.setAnchor(sig);
            anchorFrame = 0;
            anchorOff = offs[0];
        }
        tr.update(sig);
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        QueryThreadCycleTime(GetCurrentThread(), &k1);
        cpu.push_back((k1 - k0) / cpms);
        // Live-mode cadence (screen_translator livePoll): run when still, 2 s apart.
        if (f > 0 && offs[f] != offs[f - 1]) lastMove = static_cast<int>(f);
        if (pendingAt < 0 && !otherPage && static_cast<int>(f) - lastMove >= kSettle && static_cast<int>(f) - lastRun >= kGap &&
            offs[f] != anchorOff) {
            pendingAt = static_cast<int>(f);
            pendingOff = offs[f];
            pendingSig = sig;
            lastRun = static_cast<int>(f);
        }
        if (pendingAt >= 0 && static_cast<int>(f) - pendingAt >= kLatency) {
            tr.setAnchor(pendingSig);
            anchorOff = pendingOff;
            anchorFrame = pendingAt;
            pendingAt = -1;
        }
        const auto& res = tr.last();
        // Truth.
        if (otherPage) {
            pageChangeMissed += res.ok ? 1 : 0;
            if (verbose) std::printf("f%4zu other page: ok %d cost %.3f changed %d\n", f, res.ok, res.cost, res.changedRows);
            continue;
        }
        const int trueShift = anchorOff - offs[f];  // screen px: anchor y -> y + shift
        int visibleTruth = 0;
        double frameMax = 0;
        if (res.ok) ++okFrames;
        if (tr.boxMoves(headerBox.x0 / W, headerBox.y0 / H, headerBox.x1 / W, headerBox.y1 / H)) ++headerMoved;
        const float regTop = res.moved ? res.top * H : 0, regBot = res.moved ? res.bottom * H : H;
        for (const Box& b : page.boxes) {
            // On screen in the anchor picture (what the shown boxes are).
            const float ay0 = b.y0 - anchorOff + kHeader, ay1 = b.y1 - anchorOff + kHeader;
            if (ay0 < kHeader || ay1 > H - kFooter) continue;
            const float ty0 = ay0 + trueShift, ty1 = ay1 + trueShift;
            const bool trueVis = ty1 > kHeader + 2 && ty0 < H - kFooter - 2;
            const bool trueFull = ty0 >= kHeader && ty1 <= H - kFooter;
            visibleTruth += trueVis;
            if (!res.ok) continue;
            const bool moves = tr.boxMoves(b.x0 / W, ay0 / H, b.x1 / W, ay1 / H);
            const float py0 = ay0 + (moves ? res.dy * H : 0), py1 = ay1 + (moves ? res.dy * H : 0);
            const bool predVis = py1 > regTop + 2 && py0 < regBot - 2 && py1 > 2 && py0 < H - 2;
            const bool trueGone = ty1 <= kHeader || ty0 >= H - kFooter;
            if (trueFull && !predVis) ++wrongHidden;
            if (trueGone && predVis && std::min(py1, regBot) - std::max(py0, regTop) > 3) ++wrongShown;
            if (trueVis && predVis) {
                const double e = std::fabs(py0 - ty0);
                errs.push_back(e);
                frameMax = std::max(frameMax, e);
                ++boxFrames;
            }
        }
        maxErr = std::max(maxErr, frameMax);
        if (!res.ok && visibleTruth > 0) ++framesHiddenWrongly;
        if (verbose)
            std::printf("f%4zu off %5d anchor %5d true %5d est %8.1f ok %d cost %.3f changed %4d top %4.0f bot %4.0f maxErr %.1f %.2fms\n", f,
                        offs[f], anchorOff, trueShift, res.dy * H, res.ok, res.cost, res.changedRows, regTop, regBot, frameMax, times.back());
    }
    double mean = 0;
    for (double e : errs) mean += e;
    mean = errs.empty() ? 0 : mean / errs.size();
    const int pageFrames = static_cast<int>(offs.size()) - 20;
    std::printf("frames %zu (page %d, ok %d), box-frames on screen %d\n", offs.size(), pageFrames, okFrames, boxFrames);
    std::printf("box offset error px: mean %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f\n", mean, pct(errs, 0.5), pct(errs, 0.95),
                pct(errs, 0.99), maxErr);
    std::printf("frames hidden while boxes were on screen: %d   page change not noticed: %d/20\n", framesHiddenWrongly, pageChangeMissed);
    std::printf("boxes hidden but on screen: %d   shown but scrolled off: %d   header box moved: %d frames\n", wrongHidden, wrongShown,
                headerMoved);
    std::printf("tracker time per frame ms (wall clock, busy PC): median %.3f  mean %.3f  p95 %.3f  max %.3f\n", pct(times, 0.5),
                [&] {
                    double s = 0;
                    for (double t : times) s += t;
                    return s / std::max<size_t>(1, times.size());
                }(),
                pct(times, 0.95), pct(times, 1.0));
    double cpuMean = 0;
    for (double t : cpu) cpuMean += t;
    cpuMean /= std::max<size_t>(1, cpu.size());
    std::printf("tracker CPU time per frame ms (this thread's cycles): median %.3f  mean %.3f  p95 %.3f  max %.3f\n", pct(cpu, 0.5), cpuMean,
                pct(cpu, 0.95), pct(cpu, 1.0));
    const bool pass = pct(errs, 0.95) <= 3.0 && maxErr <= 12 && pageChangeMissed == 0 && headerMoved == 0 && pct(cpu, 0.95) <= 3.0 && pct(cpu, 1.0) <= 6.0 &&
                      framesHiddenWrongly <= pageFrames / 50 && wrongShown <= boxFrames / 200 && wrongHidden <= boxFrames / 200;
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
