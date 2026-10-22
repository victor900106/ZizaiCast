// 翻譯 ▸ 本機 AI 翻譯… (see tr_settings.h, docs/app.md
// "Translation settings").  Blocking engine calls (download) run
// on worker threads; their results come back to the UI thread through a
// message-only window (a closed panel just ignores them).
#include "tr_settings.h"

#ifdef PM_APP_TR_ENGINES  // app/CMakeLists.txt: pm_translate has the LLM engine

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "ask_panel.h"
#include "pm/i18n.h"
#include "pm/llm_translate.h"
#include "pm/translate.h"  // ModelStore / PaddleOcr: the OCR GPU add-on
#include "settings_panel.h"

using pm::i18n::fmt;
using pm::i18n::S;
using pm::i18n::tr;

namespace pm::ui::trset {
namespace {

namespace llm = pm::translate::local_llm;
using Item = SettingsPanel::Item;
using Kind = SettingsPanel::Item::Kind;
using Btn = SettingsPanel::Button;

constexpr wchar_t kIcoLlm = 0xE7F8;     // DeviceLaptopNoPic (this PC)
constexpr const char* kModelBest = "qwen3.5-2b-q4km";
constexpr const char* kModelSmall = "qwen3.5-0.8b-q4";

Host host;
HWND msgWnd = nullptr;
constexpr UINT kMsgRun = WM_APP + 1;

void logLine(const char* level, const std::string& s) {
    if (host.log) host.log(level, s);
}
std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring wide(const std::string& s) {  // UTF-8 (a GPU's name)
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Runs fn on the UI thread (from any thread); dropped once shut down.
void postUi(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    if (!msgWnd || !PostMessageW(msgWnd, kMsgRun, 0, reinterpret_cast<LPARAM>(p))) delete p;
}
LRESULT CALLBACK msgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == kMsgRun) {
        std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lp));
        if (*fn) (*fn)();
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

std::wstring sizeText(uint64_t bytes) {
    wchar_t b[32];
    if (bytes >= 1000ull * 1024 * 1024) swprintf_s(b, L"%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else swprintf_s(b, L"%.0f MB", (std::max)(1.0, bytes / (1024.0 * 1024)));
    return b;
}

AskPanel ask;  // the panel's own question dialog (consent, remove)

// ===================================================================== 本機 AI 翻譯
enum LlmId { LEnable = 1, LModelBest, LModelSmall, LGpu, LDownload = 10, LCancel, LRemove, LDetails, LOcrGpu, LOcrCancel, LOcrDetails, LClose = 20 };

struct LlmUi {
    SettingsPanel panel;
    llm::Status st;
    llm::Settings set;
    std::vector<llm::Model> models;
    bool downloading = false;
    std::atomic<double> progress{0};
    std::atomic<bool> progressPosted{false};
    uint64_t totalBytes = 0;
    std::atomic<uint64_t> doneBytes{0}, progressTotal{0};
    std::atomic<double> bytesPerSec{0}, secondsLeft{-1};
    bool cancelling = false;  // 取消中…: the button off, repeat clicks ignored
    std::atomic<bool> verifying{false};  // 驗證中…: the final SHA-256 check
    std::mutex pendingMu;
    std::vector<std::string> pending;    // files still downloading ("runtime", "gpu", "model")
    bool ocrCancelling = false;
    std::atomic<bool> cancel{false};
    std::thread worker;
    int result = -1;  // llm::Result of the last download (-1 none)
    std::wstring detail;
    bool showDetail = false, saveFailed = false, removeFailed = false;
    // The OCR GPU add-on (ModelStore::downloadOcrGpu, ~15.4 MB) for users whose
    // OCR models came before it: its own download, progress and result.
    bool ocrWanted = false, ocrDownloading = false, ocrShowDetail = false;
    uint64_t ocrMissing = 0, ocrTotal = 0;
    std::atomic<double> ocrProgress{0};
    std::atomic<bool> ocrPosted{false}, ocrCancel{false};
    std::thread ocrWorker;
    int ocrResult = -1;  // llm::Result (same messages)
    std::wstring ocrDetail;
} Ai;

const llm::Model* findModel(const char* id) {
    for (const auto& m : Ai.models)
        if (m.id == id) return &m;
    return nullptr;
}

void llmRead() {
    Ai.st = llm::status();
    Ai.set = llm::loadSettings();
    Ai.models = llm::models();
    // OCR GPU add-on: for users who already have the OCR models (new users get
    // it with the first OCR download, ScreenTranslator).
    Ai.ocrWanted = pm::translate::PaddleOcr::gpuWanted() && pm::translate::PaddleOcr::modelsInstalled();
    // --dev --test-no-network + PM_TEST_OCR_GPU_OFFER=1: as if the OCR models were there (screenshots).
    if (host.noNetwork && std::getenv("PM_TEST_OCR_GPU_OFFER")) Ai.ocrWanted = pm::translate::PaddleOcr::gpuWanted();
    Ai.ocrMissing = Ai.ocrWanted ? pm::translate::ModelStore::ocrGpuMissingBytes() : 0;
}

std::wstring llmResultText(int r) {
    switch (static_cast<llm::Result>(r)) {
    case llm::Result::Cancelled: return tr(S::TrLlmErrCancelled);
    case llm::Result::Network: return tr(S::TrLlmErrNetwork);
    case llm::Result::Verify: return tr(S::TrLlmErrVerify);
    case llm::Result::Disk: return tr(S::TrLlmErrDisk);
    case llm::Result::Unpack: return tr(S::TrLlmErrUnpack);
    case llm::Result::UnknownModel: return tr(S::TrLlmErrModel);
    default: return {};
    }
}

// The small parts still downloading when the model is already in ("" otherwise).
std::wstring pendingParts() {
    std::lock_guard<std::mutex> l(Ai.pendingMu);
    std::wstring s;
    for (const auto& p : Ai.pending) {
        if (p == "model") return {};
        const wchar_t* name = p == "runtime" ? tr(S::TrDlPartRuntime) : p == "gpu" ? tr(S::TrDlPartGpu) : nullptr;
        if (!name) continue;
        if (!s.empty()) s += L"、";
        s += name;
    }
    return s;
}

std::vector<Item> llmItems() {
    std::vector<Item> v;
    auto add = [&](Kind k, std::wstring text, int id = 0) -> Item& {
        Item it;
        it.kind = k;
        it.text = std::move(text);
        it.id = id;
        v.push_back(std::move(it));
        return v.back();
    };
    const bool busy = Ai.downloading;
    add(Kind::Heading, tr(S::TrLlmHeading));
    add(Kind::Note, tr(S::TrLlmNote));
    add(Kind::Text, tr(S::TrLlmIntro));
    Item& en = add(Kind::Toggle, tr(S::TrLlmEnable), LEnable);
    en.on = Ai.set.enabled;
    if (Ai.set.enabled && !Ai.st.memoryOk) add(Kind::Status, tr(S::TrLlmMemory)).tone = 2;
    if (Ai.saveFailed) add(Kind::Status, tr(S::TrLlmSaveFailed)).tone = 2;
    add(Kind::Rule, L"");
    // Model: 建議（較準確） = 2B, 較小、較快 = 0.8B.
    add(Kind::Heading, tr(S::TrLlmModel));
    const struct {
        const char* id;
        S label;
        int hit;
    } picks[2] = {{kModelBest, S::TrLlmModelBest, LModelBest}, {kModelSmall, S::TrLlmModelSmall, LModelSmall}};
    for (const auto& p : picks) {
        const llm::Model* m = findModel(p.id);
        if (!m) continue;
        Item& r = add(Kind::Radio, tr(p.label), p.hit);
        r.on = Ai.st.model == m->id;
        r.sub = m->installed ? fmt(S::TrLlmModelSubReady, {m->name, sizeText(m->fileBytes)})
                             : fmt(S::TrLlmModelSub, {m->name, sizeText(m->fileBytes)});
        r.enabled = !busy;
    }
    // GPU: only with a discrete GPU (>= 3 GB); greyed (with why) otherwise.
    Item& gpu = add(Kind::Toggle, tr(S::TrLlmGpu), LGpu);
    gpu.on = Ai.set.gpu && Ai.st.discreteGpu;
    gpu.enabled = Ai.st.discreteGpu && !busy;
    gpu.sub = tr(S::TrLlmGpuHint);
    if (!Ai.st.discreteGpu) gpu.sub += L"\n" + std::wstring(tr(S::TrLlmGpuNone));
    else gpu.sub += L"\n" + std::wstring(tr(S::TrLlmGpuShared));  // PaddleOcr::gpuWanted reads the same setting
    // 文字辨識的顯示卡加速: only when wanted (discrete GPU, setting on) and missing.
    if (Ai.ocrDownloading) {
        const double p = Ai.ocrProgress.load();
        Item& pr = add(Kind::Progress, L"");
        pr.value = static_cast<float>(p);
        const double totalMB = Ai.ocrTotal / 1e6;  // decimal MB, like "15.4 MB" in the add-on notes
        wchar_t pct[16], done[24], total[24];
        swprintf_s(pct, L"%d", static_cast<int>(p * 100));
        swprintf_s(done, L"%.1f", totalMB * p);
        swprintf_s(total, L"%.1f", totalMB);
        add(Kind::Status, Ai.ocrCancelling ? std::wstring(tr(S::TrDlCancelling)) : fmt(S::TrOcrGpuProgress, {pct, done, total}));
        add(Kind::Buttons, L"").buttons.push_back({LOcrCancel, tr(S::Cancel)});
        v.back().buttons.back().enabled = !Ai.ocrCancelling;
    } else if (Ai.ocrWanted && Ai.ocrMissing > 0) {
        wchar_t mb[24];
        swprintf_s(mb, L"%.1f MB", Ai.ocrMissing / 1e6);
        // After an interrupted / cancelled try (the .part is kept): 繼續下載, as its message says.
        const bool resume = Ai.ocrResult == static_cast<int>(llm::Result::Network) ||
                            Ai.ocrResult == static_cast<int>(llm::Result::Cancelled);
        add(Kind::Buttons, L"").buttons.push_back({LOcrGpu, fmt(resume ? S::TrLlmResume : S::TrOcrGpuDownload, {mb})});
    }
    if (!Ai.ocrDownloading && Ai.ocrResult > 0) {
        add(Kind::Status, llmResultText(Ai.ocrResult)).tone = Ai.ocrResult == static_cast<int>(llm::Result::Cancelled) ? 0 : 2;
        if (!Ai.ocrDetail.empty()) {
            add(Kind::Links, L"").buttons.push_back({LOcrDetails, tr(S::TrErrDetails)});
            if (Ai.ocrShowDetail) add(Kind::Code, Ai.ocrDetail);
        }
    }
    if (!Ai.st.device.empty())
        add(Kind::Note, fmt(S::TrLlmDevice, {Ai.st.device == "CPU" ? std::wstring(tr(S::TrLlmDeviceCpu)) : wide(Ai.st.device)}));
    add(Kind::Rule, L"");
    // Download / progress / installed.
    if (busy) {
        const double p = Ai.progress.load();
        add(Kind::Progress, L"").value = static_cast<float>(p);
        const uint64_t tot = Ai.progressTotal.load() ? Ai.progressTotal.load() : Ai.totalBytes;
        const double totalMB = tot / (1024.0 * 1024);
        const double doneMB = Ai.progressTotal.load() ? Ai.doneBytes.load() / (1024.0 * 1024) : totalMB * p;
        wchar_t pct[16], done[24], total[24], speed[32];
        swprintf_s(pct, L"%d", static_cast<int>(p * 100));
        swprintf_s(done, L"%.0f", doneMB);
        swprintf_s(total, L"%.0f", totalMB);
        const double bps = Ai.bytesPerSec.load(), left = Ai.secondsLeft.load();
        if (Ai.cancelling) {
            add(Kind::Status, tr(S::TrDlCancelling));
        } else if (Ai.verifying.load()) {
            add(Kind::Status, tr(S::TrDlVerifying));
        } else if (const std::wstring tail = pendingParts(); p >= 0.9 && !tail.empty()) {
            // The model is in, a small part is still coming: say which, so it does not look stuck.
            add(Kind::Status, fmt(S::TrLlmProgressTail, {pct, tail}));
        } else if (bps > 0 && left >= 0) {
            if (bps >= 1024.0 * 1024) swprintf_s(speed, L"%.1f MB/s", bps / (1024.0 * 1024));
            else swprintf_s(speed, L"%.0f KB/s", bps / 1024.0);
            wchar_t n[16];
            std::wstring eta;
            if (left < 60) {
                swprintf_s(n, L"%d", (std::max)(1, static_cast<int>(left + 0.5)));
                eta = fmt(S::TrDlEtaSec, {n});
            } else {
                swprintf_s(n, L"%d", static_cast<int>(left / 60 + 0.5));
                eta = fmt(S::TrDlEtaMin, {n});
            }
            add(Kind::Status, fmt(S::TrLlmProgressRate, {pct, done, total, speed, eta}));
        } else {
            add(Kind::Status, fmt(S::TrLlmProgress, {pct, done, total}));
        }
        Item& b = add(Kind::Buttons, L"");
        b.buttons.push_back({LCancel, tr(S::Cancel)});
        b.buttons.back().enabled = !Ai.cancelling;
    } else {
        if (Ai.st.installed) {
            const llm::Model* m = nullptr;
            for (const auto& x : Ai.models)
                if (x.id == Ai.st.model) m = &x;
            add(Kind::Status, fmt(S::TrLlmReady, {m ? m->name + L" · " + sizeText(m->fileBytes) : std::wstring()})).tone = 1;
        }
        Item& b = add(Kind::Buttons, L"");
        if (Ai.st.missingBytes > 0)
            b.buttons.push_back({LDownload,
                                 Ai.st.partialBytes > 0 ? fmt(S::TrLlmResume, {sizeText(Ai.st.missingBytes)})
                                                       : fmt(S::TrLlmDownload, {sizeText(Ai.st.missingBytes)}),
                                 true});
        if (Ai.st.installed) b.buttons.push_back({LRemove, tr(S::TrLlmRemove), false, true});
        if (b.buttons.empty()) v.pop_back();
        if (Ai.removeFailed) add(Kind::Status, tr(S::TrDeleteFailed)).tone = 2;
        if (Ai.result > 0) {
            add(Kind::Status, llmResultText(Ai.result)).tone = Ai.result == static_cast<int>(llm::Result::Cancelled) ? 0 : 2;
            if (!Ai.detail.empty()) {
                Item& lk = add(Kind::Links, L"");
                lk.buttons.push_back({LDetails, tr(S::TrErrDetails)});
                if (Ai.showDetail) add(Kind::Code, Ai.detail);
            }
        }
    }
    Item& f = add(Kind::Buttons, L"");
    f.footer = true;
    f.buttons.push_back({LClose, tr(S::AboutClose), true});
    return v;
}

void llmRefresh(bool reread = true) {
    if (!Ai.panel.isOpen()) return;
    if (reread && !Ai.downloading) llmRead();
    Ai.panel.setItems(llmItems());
}

void llmSave() {
    std::wstring err;
    Ai.saveFailed = !llm::saveSettings(Ai.set, &err);
    if (Ai.saveFailed) logLine("warn", "local AI settings not saved: " + utf8(err));
}

// --dev --test-no-network: a download that fetches nothing.
llm::Result fakeDownload(const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    const char* e = std::getenv("PM_TEST_LLM_FAKE");
    const std::string end = e ? e : "network";
    const char* ms = std::getenv("PM_TEST_LLM_FAKE_MS");
    const int total = ms ? (std::max)(200, std::atoi(ms)) : 20000;
    const double stopAt = end == "ok" ? 1.0 : 0.6;
    for (int t = 0; t <= total; t += 50) {
        if (cancel->load()) return llm::Result::Cancelled;
        const double p = static_cast<double>(t) / total;
        if (p >= stopAt) break;
        progress(p);
        Sleep(50);
    }
    if (end == "ok") return progress(1.0), llm::Result::Ok;
    if (end == "verify") return llm::Result::Verify;
    if (end == "disk") return llm::Result::Disk;
    if (end == "unpack") return llm::Result::Unpack;
    return llm::Result::Network;
}

void llmDownloadDone(int r, std::wstring detail) {
    if (Ai.worker.joinable()) Ai.worker.join();
    Ai.downloading = false;
    Ai.cancelling = false;
    Ai.result = r;
    Ai.detail = std::move(detail);
    Ai.showDetail = false;
    logLine(r == 0 ? "info" : "warn", std::string("local AI download: ") + llm::resultId(static_cast<llm::Result>(r)));
    if (r == static_cast<int>(llm::Result::Ok)) {
        Ai.result = -1;
        if (host.toast) host.toast(tr(S::TrLlmReadyToast));
        if (llm::loadSettings().enabled) llm::warmUp();
    }
    llmRefresh();
}

void llmStartDownload() {
    if (Ai.downloading) return;
    if (Ai.worker.joinable()) Ai.worker.join();
    Ai.downloading = true;
    Ai.cancel = false;
    Ai.cancelling = false;
    Ai.progress = 0;
    Ai.doneBytes = 0;
    Ai.progressTotal = 0;
    Ai.bytesPerSec = 0;
    Ai.secondsLeft = -1;
    Ai.verifying = false;
    Ai.result = -1;
    Ai.removeFailed = false;
    Ai.totalBytes = Ai.st.missingBytes + Ai.st.partialBytes;
    const bool fake = host.noNetwork;
    logLine("info", std::string("local AI download started: ") + Ai.st.model + ", " + std::to_string(Ai.st.missingBytes) +
                        " bytes" + (fake ? " (test-no-network: fake, nothing fetched)" : ""));
    Ai.worker = std::thread([fake] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        auto progress = [](double p) {
            Ai.progress = p;
            // The panel is redrawn at most ~8 times a second (every block of a
            // fast download reported here); the done message redraws it at the end.
            static std::atomic<ULONGLONG> lastPost{0};
            const ULONGLONG now = GetTickCount64();
            if (now - lastPost.load() < 120 || Ai.progressPosted.exchange(true)) return;
            lastPost = now;
            postUi([] {
                Ai.progressPosted = false;
                if (Ai.downloading) llmRefresh(false);
            });
        };
        // Bytes, speed and time left for the progress line (all files at once).
        auto rich = [progress](const llm::DownloadProgress& d) {
            Ai.doneBytes = d.doneBytes;
            Ai.progressTotal = d.totalBytes;
            Ai.bytesPerSec = d.bytesPerSec;
            Ai.secondsLeft = d.secondsLeft;
            Ai.verifying = d.verifying;
            {
                std::lock_guard<std::mutex> l(Ai.pendingMu);
                Ai.pending = d.pending;
            }
            progress(d.fraction);
        };
        std::wstring detail;
        const llm::Result r = fake ? fakeDownload(progress, &Ai.cancel)
                                   : llm::download(std::function<void(const llm::DownloadProgress&)>(rich), &Ai.cancel, &detail);
        postUi([r, detail] { llmDownloadDone(static_cast<int>(r), detail); });
    });
    llmRefresh(false);
}

void llmAskDownload() {
    llmRead();
    if (Ai.st.missingBytes == 0) return llmRefresh();
    AskPanel::Info a;
    a.glyph = 0xE896;  // Download
    a.title = tr(S::TrLlmConsentTitle);
    // The localised text, then what describeDownload() says (files, URLs, licences; not translated).
    std::wstring files = llm::describeDownload();
    while (!files.empty() && (files.back() == L'\n' || files.back() == L'\r')) files.pop_back();
    a.body = fmt(S::TrLlmConsentBody, {sizeText(Ai.st.missingBytes)}) + L"\n" + files;
    a.primary = tr(S::TrConsentYes);
    a.secondary = tr(S::TrConsentNo);
    a.done = [](int choice) {
        logLine("info", std::string("local AI download consent: ") + (choice == 1 ? "yes" : "no"));
        if (choice == 1 && Ai.panel.isOpen()) llmStartDownload();
    };
    logLine("info", "local AI download consent asked");
    ask.open(Ai.panel.hwnd(), std::move(a));
}

void llmAskRemove() {
    const llm::Model* m = nullptr;
    for (const auto& x : Ai.models)
        if (x.id == Ai.st.model) m = &x;
    AskPanel::Info a;
    a.glyph = 0xE74D;  // Delete
    a.title = tr(S::TrLlmRemoveTitle);
    a.body = fmt(S::TrLlmRemoveAsk, {m ? m->name + L", " + sizeText(m->fileBytes) : std::wstring()});
    a.primary = tr(S::TrDeleteBtn);
    a.secondary = tr(S::Cancel);
    a.danger = true;
    a.done = [](int choice) {
        if (choice != 1) return;
        const bool ok = llm::remove();
        Ai.removeFailed = !ok;
        logLine(ok ? "info" : "warn", std::string("local AI model removed: ") + (ok ? "ok" : "failed (in use)"));
        if (ok && host.toast) host.toast(tr(S::TrLlmRemoved));
        llmRefresh();
    };
    ask.open(Ai.panel.hwnd(), std::move(a));
}

// The OCR GPU add-on (15.4 MB, Microsoft NuGet byte ranges, SHA-256 pinned).
llm::Result ocrClassify(const std::wstring& e) {
    auto has = [&](const wchar_t* x) { return e.find(x) != std::wstring::npos; };
    if (has(L"cancelled")) return llm::Result::Cancelled;
    if (has(L"SHA-256")) return llm::Result::Verify;
    if (has(L"tar")) return llm::Result::Unpack;
    if (has(L"cannot write") || has(L"cannot move")) return llm::Result::Disk;
    return llm::Result::Network;
}

void ocrDownloadDone(int r, std::wstring detail) {
    if (Ai.ocrWorker.joinable()) Ai.ocrWorker.join();
    Ai.ocrDownloading = false;
    Ai.ocrCancelling = false;
    Ai.ocrResult = r == 0 ? -1 : r;
    Ai.ocrDetail = std::move(detail);
    Ai.ocrShowDetail = false;
    logLine(r == 0 ? "info" : "warn", std::string("OCR GPU add-on download: ") + llm::resultId(static_cast<llm::Result>(r)));
    if (r == 0 && host.toast) host.toast(tr(S::TrOcrGpuReady));
    llmRefresh();
}

void ocrStartDownload() {
    if (Ai.ocrDownloading) return;
    if (Ai.ocrWorker.joinable()) Ai.ocrWorker.join();
    Ai.ocrDownloading = true;
    Ai.ocrCancelling = false;
    Ai.ocrCancel = false;
    Ai.ocrProgress = 0;
    Ai.ocrResult = -1;
    Ai.ocrTotal = Ai.ocrMissing;
    const bool fake = host.noNetwork;
    logLine("info", "OCR GPU add-on download started: " + std::to_string(Ai.ocrMissing) + " bytes" +
                        (fake ? " (test-no-network: fake, nothing fetched)" : ""));
    Ai.ocrWorker = std::thread([fake] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        auto progress = [](double p) {
            Ai.ocrProgress = p;
            static std::atomic<ULONGLONG> lastPost{0};  // redrawn at most ~8 times a second
            const ULONGLONG now = GetTickCount64();
            if (now - lastPost.load() < 120 || Ai.ocrPosted.exchange(true)) return;
            lastPost = now;
            postUi([] {
                Ai.ocrPosted = false;
                if (Ai.ocrDownloading) llmRefresh(false);
            });
        };
        std::wstring detail;
        llm::Result r;
        if (fake) {
            r = fakeDownload(progress, &Ai.ocrCancel);
        } else {
            r = pm::translate::ModelStore::downloadOcrGpu(progress, &Ai.ocrCancel, &detail) ? llm::Result::Ok : ocrClassify(detail);
        }
        postUi([r, detail] { ocrDownloadDone(static_cast<int>(r), detail); });
    });
    llmRefresh(false);
}

void llmAction(int id) {
    switch (id) {
    case LEnable:
        Ai.set.enabled = !Ai.set.enabled;
        llmSave();
        logLine("info", std::string("local AI translation ") + (Ai.set.enabled ? "on" : "off"));
        if (Ai.set.enabled) llm::warmUp();  // cheap: loads it in the background when installed
        else llm::release();
        break;
    case LModelBest:
    case LModelSmall: {
        if (Ai.downloading) return;
        const char* id2 = id == LModelBest ? kModelBest : kModelSmall;
        const llm::Model* m = findModel(id2);
        if (!m || Ai.st.model == m->id) return;
        Ai.set.model = m->recommended ? std::string() : m->id;  // "" follows this PC's default
        llmSave();
        Ai.result = -1;
        logLine("info", std::string("local AI model -> ") + m->id);
        if (Ai.set.enabled) llm::warmUp();
        break;
    }
    case LGpu:
        if (Ai.downloading || !Ai.st.discreteGpu) return;
        Ai.set.gpu = !Ai.set.gpu;
        llmSave();
        logLine("info", std::string("local AI GPU ") + (Ai.set.gpu ? "on" : "off"));
        break;
    case LDownload: llmAskDownload(); return;
    case LCancel:
        if (!Ai.downloading || Ai.cancelling) return;  // repeat clicks
        Ai.cancel = true;
        Ai.cancelling = true;
        logLine("info", "local AI download: cancel asked");
        llmRefresh(false);  // 取消中… at once; the download returns within ~0.2 s
        return;
    case LRemove: llmAskRemove(); return;
    case LDetails: Ai.showDetail = !Ai.showDetail; break;
    case LOcrGpu: ocrStartDownload(); return;
    case LOcrCancel:
        if (!Ai.ocrDownloading || Ai.ocrCancelling) return;  // repeat clicks
        Ai.ocrCancel = true;
        Ai.ocrCancelling = true;
        logLine("info", "OCR GPU add-on download: cancel asked");
        llmRefresh(false);  // 取消中… at once; the download returns within ~0.1 s
        return;
    case LOcrDetails: Ai.ocrShowDetail = !Ai.ocrShowDetail; break;
    case LClose:
        ask.close();  // (owned by the panel)
        Ai.panel.close();
        return;
    default: return;
    }
    llmRefresh();
}

}  // namespace

// ===================================================================== public
bool available() { return true; }

void init(Host h) {
    host = std::move(h);
    SettingsPanel::testOffscreen = host.offscreen;
    if (!msgWnd) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = msgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PhoneMirrorTrSettingsMsg";
        RegisterClassW(&wc);
        msgWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    }
}

// When quitting (UI still up: cancel, close, wait up to 2 s) and, last=true,
// at the very end of wWinMain (every exit path; joins whatever still runs).
// The workers are never detached: one still running while the statics it
// uses are destroyed at exit would crash (and a joinable std::thread left
// to its destructor calls std::terminate).
void shutdown(bool last) {
    ask.close();
    Ai.panel.close();
    // Downloads stop at their next block (the partial file is kept).
    if (Ai.downloading) Ai.cancel = true;
    if (Ai.ocrDownloading) Ai.ocrCancel = true;
    // Their done messages (llmDownloadDone / ocrDownloadDone / the OCR
    // download) join them on this thread.
    for (int i = 0; i < 40 && (Ai.downloading || Ai.ocrDownloading ); ++i) {
        MSG m;
        while (msgWnd && PeekMessageW(&m, msgWnd, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        if (Ai.downloading || Ai.ocrDownloading) Sleep(50);
    }
    if (!last) return;
    if (msgWnd) DestroyWindow(msgWnd), msgWnd = nullptr;  // later results are dropped
    if (Ai.worker.joinable()) Ai.worker.join();
    if (Ai.ocrWorker.joinable()) Ai.ocrWorker.join();
}

void openLocalAi() {
    if (!Ai.downloading) llmRead();
    Ai.saveFailed = Ai.removeFailed = false;
    if (!Ai.downloading) Ai.result = -1;
    SettingsPanel::Callbacks cb;
    cb.onAction = llmAction;
    cb.onClose = [] { ask.close(); };
    logLine("info", "local AI panel opened: model " + Ai.st.model + (Ai.st.installed ? " (installed)" : " (not installed)") +
                        ", missing " + std::to_string(Ai.st.missingBytes) + " bytes, discrete GPU " +
                        (Ai.st.discreteGpu ? "yes" : "no"));
    Ai.panel.open(host.owner, tr(S::TrLlmTitle), kIcoLlm, llmItems(), std::move(cb));
}

void retheme() {
    Ai.panel.retheme();
    ask.retheme();
}

void relabel() {
    if (Ai.panel.isOpen()) {
        Ai.panel.relabel();
        Ai.panel.setTitle(tr(S::TrLlmTitle));
        llmRefresh(false);
    }
}

void warmUpLocalAi() { llm::warmUp(); }

bool localAiOn() { return llm::status().usable(); }

int devShots(const std::wstring& dir, const std::wstring& suffix, bool view) {
    int n = 0;
    const std::wstring v = view ? L"_view" : L"";
    n += Ai.panel.renderPng(dir + L"\\llm" + v + suffix + L".png", view) ? 1 : 0;
    if (view) return n;
    n += ask.renderPng(dir + L"\\trask" + suffix + L".png") ? 1 : 0;
    return n;
}

void devClick(int panel, int id) {
    if (panel == 0 && Ai.panel.isOpen()) llmAction(id);
}

void devAnswer(int choice) {
    if (ask.isOpen()) ask.click(choice == 1 ? 1 : choice == 2 ? 2 : 0);
}

void devScroll(int panel, float dips) {
    if (panel == 0) Ai.panel.devScroll(dips);
}
void devKey(int panel, UINT vk) {
    if (panel == 0) Ai.panel.devKey(vk);
}
void devDpiChanged(int dpi, bool stale) {
    Ai.panel.devDpiChanged(dpi, stale);
}
void devSelfCheck() {
    if (Ai.panel.isOpen()) logLine("info", "self-check local AI panel: " + Ai.panel.selfCheck());
}
void devScreen(int dpi, int workAreaPx) {
    SettingsPanel::testDpi = dpi;
    SettingsPanel::testWorkAreaPx = workAreaPx;
}

}  // namespace pm::ui::trset

#else  // !PM_APP_TR_ENGINES: pm_translate built without its optional engines - no menu rows, nothing to show.

namespace pm::ui::trset {
bool available() { return false; }
void init(Host) {}
void shutdown(bool) {}
void openLocalAi() {}
void retheme() {}
void relabel() {}
void warmUpLocalAi() {}
int devShots(const std::wstring&, const std::wstring&, bool) { return 0; }
void devScroll(int, float) {}
void devKey(int, UINT) {}
void devScreen(int, int) {}
void devDpiChanged(int, bool) {}
void devSelfCheck() {}
bool localAiOn() { return false; }
void devClick(int, int) {}
void devAnswer(int) {}
}  // namespace pm::ui::trset

#endif
