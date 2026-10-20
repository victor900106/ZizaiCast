// pm_tr_bench: whole-picture translation time (0.8.3), cold and warm, with
// the local LLM as the app uses it; process RAM and video memory; the idle
// release.  Off-screen (PM_VIDEO_OFFSCREEN), no sound, no input.
//
//   pm_tr_bench [--when command|connect] [--connect-wait S] [--repeat N] [--idle S] [--target T] PNG [PNG...]
//
//   --when command   (default) as 翻譯整個畫面 in a fresh app: local_llm::warmUp()
//                    and translateScreen() at the same moment (cold)
//   --when connect   as a phone connecting first: ScreenTranslator::prewarm(ja)
//                    + local_llm::warmUp(), --connect-wait seconds (default 8),
//                    then translateScreen()
//   --repeat N       warm runs of every picture after the first (default 3)
//   --command-warm 0 as the 0.8.3 app: no local_llm::warmUp() at the command
//                    (the translator loads it after a picture that wanted it)
//   --idle S         then wait S seconds without translating and print the
//                    memory every 10 s (the idle release)
// Lines: "RUN <name> cold|warm total <ms> ..." and "MEM <tag> ...".
// Models: PM_MODELS_DIR (the tests' copy).
#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pm/llm_translate.h"
#include "pm/translate.h"
#include "pm/video_window.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")

using namespace pm::translate;
using Microsoft::WRL::ComPtr;

namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool readPng(const std::wstring& path, std::vector<uint8_t>& bgra, int& w, int& h) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    UINT uw = 0, uh = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->GetSize(&uw, &uh)))
        return false;
    w = static_cast<int>(uw);
    h = static_cast<int>(uh);
    bgra.resize(static_cast<size_t>(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, static_cast<UINT>(bgra.size()), bgra.data()));
}

std::wstring baseName(const std::wstring& p) {
    std::wstring n = p.substr(p.find_last_of(L"\\/") + 1);
    return n.substr(0, n.find_last_of(L'.'));
}

// This process' memory: private bytes, working set, and the video memory it
// uses on the hardware adapters (DXGI: per-process, every API incl. Vulkan).
void mem(const char* tag) {
    PROCESS_MEMORY_COUNTERS_EX pm{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm));
    double vram = 0;
    ComPtr<IDXGIFactory4> f;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            ComPtr<IDXGIAdapter3> a3;
            if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || FAILED(a.As(&a3))) continue;
            DXGI_QUERY_VIDEO_MEMORY_INFO vi{};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi))) vram += vi.CurrentUsage / 1048576.0;
        }
    }
    const local_llm::Status st = local_llm::status();
    std::printf("MEM %s: private %.0f MB, working set %.0f MB, VRAM %.0f MB, llm %s\n", tag, pm.PrivateUsage / 1048576.0,
                pm.WorkingSetSize / 1048576.0, vram, st.device.empty() ? "not loaded" : st.device.c_str());
    std::fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetEnvironmentVariableW(L"PM_VIDEO_OFFSCREEN", L"1");
    std::wstring when = L"command";
    int connectWait = 8, repeat = 3, idle = 0;
    bool commandWarm = true;
    Lang tgt = Lang::ZhHant;
    std::vector<std::wstring> files;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool v = i + 1 < argc;
        if (a == L"--when" && v) when = argv[++i];
        else if (a == L"--connect-wait" && v) connectWait = _wtoi(argv[++i]);
        else if (a == L"--repeat" && v) repeat = _wtoi(argv[++i]);
        else if (a == L"--idle" && v) idle = _wtoi(argv[++i]);
        else if (a == L"--command-warm" && v) commandWarm = _wtoi(argv[++i]) != 0;
        else if (a == L"--target" && v) tgt = !_wcsicmp(argv[++i], L"en") ? Lang::En : Lang::ZhHant;
        else files.push_back(a);
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: see the header of translate/tools/pm_tr_bench.cpp\n");
        return 2;
    }
    pm::VideoWindow win;
    if (!win.create(L"pm_tr_bench", 540, 960)) return 1;
    int fails = 0;
    std::thread t([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::atomic<int> done{0};
        std::mutex lm;
        ScreenTranslator::Timing last;
        bool lastOk = false;
        ScreenTranslator::Callbacks cb;
        cb.askDownload = [](Lang, Lang, double, std::function<void(bool)> answer) { answer(false); };  // never downloads
        cb.notify = [](const std::wstring&, const std::wstring& text, bool) {
            std::fprintf(stderr, "[bench] notify: %ls\n", text.c_str());
        };
        cb.finished = [&](bool ok, const ScreenTranslator::Timing& tm) {
            std::lock_guard lk(lm);
            last = tm;
            lastOk = ok;
            ++done;
        };
        auto onUi = [&](std::function<void()> fn) {
            HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            win.post([&] {
                fn();
                SetEvent(ev);
            });
            WaitForSingleObject(ev, 5000);
            CloseHandle(ev);
        };
        ScreenTranslator tr(win, cb);
        tr.setTarget(tgt);
        mem("start");
        int runs = 0;
        bool first = true;
        for (const auto& png : files) {
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            if (!readPng(png, px, w, h)) {
                std::printf("cannot read %ls\n", png.c_str());
                ++fails;
                continue;
            }
            const std::string n = [&] {
                const std::wstring b = baseName(png);
                char buf[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, b.c_str(), -1, buf, sizeof buf - 1, nullptr, nullptr);
                return std::string(buf);
            }();
            for (int i = 0; i < 8; ++i) {
                win.submitBgraFrame(px.data(), w, h, w * 4, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            if (first && when == L"connect") {  // a phone connected: the app's prewarmTranslation()
                onUi([&] { tr.prewarm(Lang::Ja); });
                local_llm::warmUp();
                std::this_thread::sleep_for(std::chrono::seconds(connectWait));
                mem("after connect wait");
            }
            for (int r = 0; r <= repeat; ++r) {
                const bool cold = first && r == 0;
                // The app up to 0.8.2: 翻譯整個畫面 also started loading the local AI at that moment.
                if (commandWarm) local_llm::warmUp();
                onUi([&] { tr.translateScreen(); });
                const double t0 = nowMs();
                while (done < runs + 1 && nowMs() - t0 < 120000) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (done < ++runs) {
                    std::printf("RUN %s FAIL: no result within 120 s\n", n.c_str());
                    ++fails;
                    break;
                }
                {
                    std::lock_guard lk(lm);
                    std::printf("RUN %s %s total %.0f ms (OCR %.0f, translate %.0f, first shown %.0f): %d lines, %d blocks, %d translated%s\n",
                                n.c_str(), cold ? "cold" : (r == 0 ? "first" : "warm"), last.totalMs, last.ocrMs, last.translateMs,
                                last.firstMs > 0 ? last.firstMs : last.totalMs, last.lines, last.blocks, last.translated,
                                lastOk ? "" : " FAILED");
                    if (!lastOk) ++fails;
                }
                std::fflush(stdout);
                if (cold) mem("after the cold run");
                onUi([&] { tr.close(); });
                // As a user between two pictures (the LLM loaded meanwhile, by the
                // escalator's warm-up after the result): 1.5 s.
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            }
            first = false;
            win.onReset();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        mem("after the warm runs");
        for (int s = 10; s <= idle; s += 10) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            char tag[48];
            std::snprintf(tag, sizeof tag, "idle %d s", s);
            mem(tag);
        }
        onUi([&] { tr.close(); });
        CoUninitialize();
        win.close();
    });
    win.runMessageLoop();
    t.join();
    std::printf("%d failure(s)\n", fails);
    std::fflush(stdout);
    // Do not let llama.dll's own threads / the loader thread delay the exit.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(fails));
    return fails;
}
