// pm_translate_test: OCR / translation / overlay checks (off-screen, no sound).
//
//   pm_translate_test --ocr-langs
//       installed Windows OCR recognisers
//   pm_translate_test --ocr PNG [PNG...] [--engine paddle|windows] [--lang ja|ko|en|zh-Hans|zh-Hant]
//       recognised lines with boxes, recogniser, time; with NAME.gt.tsv the
//       accuracy (character error rate) against it.  --engine: PaddleOCR
//       (default when onnxruntime.dll + the "ocr" models are there) or
//       Windows OCR
//   pm_translate_test --sentences [--target zh-Hant|en]
//       fixed Japanese / Korean / English UI sentences through the engine
//       (quality table + ms per batch)
//   pm_translate_test --pipeline PNG [PNG...] [--target T] [--lang L]
//       OCR -> blocks -> translation without a window: every block with its
//       translation, OCR / translate / model-load times
//   pm_translate_test --overlay OUTDIR PNG [PNG...] [--target T]
//       off-screen VideoWindow fed with each PNG; ScreenTranslator
//       translateScreen (window shots <name>_tr.png, <name>_original.png),
//       then translateRegion with a posted drag (<name>_region.png); times
//   pm_translate_test --text FILE [--lang ja] [--target T]   (or --raw FILE)
//       each UTF-8 line of FILE translated like the blocks of a picture
//       (glossary, table cells, names kept); --raw: the engine alone
//   --window WxH  size of the --overlay window in DIPs (default 540x960)
//   --layout auto|inplace|list, --dark   翻譯 ▸ 顯示方式 for --overlay
//                 (LAYOUT / CHECKS lines: overlapping pairs, cut characters
//                 and 禁則 must be 0, else the run fails)
//   --gt          use NAME.gt.tsv (render.sh: the rendered text lines and
//                 boxes) instead of OCR for --pipeline / --overlay (e.g. when
//                 the Japanese / Korean recogniser is not installed); with
//                 --ocr it prints the OCR accuracy against it
//   --download    answer "yes" to the model download questions (OCR and
//                 translation models; otherwise "no": tests the declined path).  Models go to PM_MODELS_DIR
//                 if set (the tests use build-translate/models), else
//                 %LOCALAPPDATA%\PhoneMirror\models.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pm/i18n.h"
#include "pm/translate.h"
#include "pm/video_window.h"
#include "text_util.h"

using namespace pm::translate;

namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string u8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string o(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), o.data(), n, nullptr, nullptr);
    return o;
}

Lang parseLang(const wchar_t* s) {
    if (!_wcsicmp(s, L"ja")) return Lang::Ja;
    if (!_wcsicmp(s, L"ko")) return Lang::Ko;
    if (!_wcsicmp(s, L"en")) return Lang::En;
    if (!_wcsicmp(s, L"zh-Hans")) return Lang::ZhHans;
    if (!_wcsicmp(s, L"zh-Hant")) return Lang::ZhHant;
    return Lang::Unknown;
}

bool readPng(const std::wstring& path, std::vector<uint8_t>& bgra, int& w, int& h) {
    using Microsoft::WRL::ComPtr;
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

// ---- Ground truth (translate/testdata/render.sh: NAME.gt.tsv next to NAME.png) ----
bool g_useGt = false;
bool g_windowsOcr = false;  // --engine windows
struct GtQuad {
    float q[8];
};
std::vector<GtQuad> g_gtQuads;  // of the last loadGt (same order as its lines)

std::vector<OcrLine> loadGt(const std::wstring& png) {
    std::vector<OcrLine> out;
    g_gtQuads.clear();
    std::wstring path = png.substr(0, png.find_last_of(L'.')) + L".gt.tsv";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return out;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        OcrLine l{};
        char* tab = strchr(line, '\t');
        GtQuad gq{};
        const int nf = tab ? sscanf_s(line, "%f %f %f %f %f %f %f %f %f %f %f %f", &l.x0, &l.y0, &l.x1, &l.y1, &gq.q[0],
                                      &gq.q[1], &gq.q[2], &gq.q[3], &gq.q[4], &gq.q[5], &gq.q[6], &gq.q[7])
                           : 0;
        if (nf < 4) continue;
        if (nf < 12) {  // box only
            const float b[8] = {l.x0, l.y0, l.x1, l.y0, l.x1, l.y1, l.x0, l.y1};
            std::copy(b, b + 8, gq.q);
        }
        g_gtQuads.push_back(gq);
        std::string t = tab + 1;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        const int n = MultiByteToWideChar(CP_UTF8, 0, t.data(), static_cast<int>(t.size()), nullptr, 0);
        l.text.resize(static_cast<size_t>(n));
        MultiByteToWideChar(CP_UTF8, 0, t.data(), static_cast<int>(t.size()), l.text.data(), n);
        l.script = detectScript(l.text);
        out.push_back(std::move(l));
    }
    fclose(f);
    return out;
}

// Ground-truth lines inside region (x0 y0 x1 y1 of the picture), in region coordinates.
OcrResult gtInRegion(const std::vector<OcrLine>& gt, const float r[4]) {
    OcrResult res;
    res.engine = Lang::Unknown;
    const float rw = r[2] - r[0], rh = r[3] - r[1];
    for (const auto& l : gt) {
        const float cx = (l.x0 + l.x1) / 2, cy = (l.y0 + l.y1) / 2;
        if (cx < r[0] || cx > r[2] || cy < r[1] || cy > r[3]) continue;
        OcrLine c = l;
        c.x0 = (l.x0 - r[0]) / rw;
        c.x1 = (l.x1 - r[0]) / rw;
        c.y0 = (l.y0 - r[1]) / rh;
        c.y1 = (l.y1 - r[1]) / rh;
        res.lines.push_back(std::move(c));
    }
    return res;
}

// Characters without spaces (OCR and layout differ in spacing); full-width
// ASCII / punctuation folded to one form.
std::wstring squeeze(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (iswspace(c) || c == 0x3000) continue;
        if (c >= 0xFF01 && c <= 0xFF5E) c = static_cast<wchar_t>(c - 0xFEE0);
        if (c == 0x2019 || c == 0x2018) c = L'\'';
        else if (c == 0x201C || c == 0x201D) c = L'"';
        else if (c == 0xB7 || c == 0x30FB || c == 0xFF65) c = 0x2022;  // middle dots
        else if (c == 0x301C || c == 0xFF5E) c = L'~';
        else if (c == 0xD7) c = L'x';
        o += c;
    }
    return o;
}

bool inQuad(const float q[8], float x, float y) {
    bool in = false;
    for (int i = 0, j = 3; i < 4; j = i++) {
        const float xi = q[i * 2], yi = q[i * 2 + 1], xj = q[j * 2], yj = q[j * 2 + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

size_t editDistance(const std::wstring& a, const std::wstring& b) {
    std::vector<size_t> d(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) d[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t prev = d[0];
        d[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t cur = d[j];
            d[j] = std::min({d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] == b[j - 1] ? 0 : 1)});
            prev = cur;
        }
    }
    return d[b.size()];
}

// OCR accuracy against the ground truth: every OCR line goes to the GT line
// (quad) that contains most of 5 points along its middle; each GT line is
// compared with its OCR lines joined left to right (CER = edit distance /
// GT characters, spaces ignored).  Totals for the summary.
struct Accuracy {
    size_t chars = 0, errs = 0, exact = 0, lines = 0;
};
Accuracy g_total;
Accuracy printAccuracy(const std::vector<OcrLine>& gt, const std::vector<OcrLine>& ocr, bool verbose = true) {
    std::vector<std::vector<const OcrLine*>> got(gt.size());
    for (const auto& o : ocr) {
        std::vector<int> votes(gt.size(), 0);
        const float cy = (o.y0 + o.y1) / 2;
        for (float f : {0.1f, 0.3f, 0.5f, 0.7f, 0.9f}) {
            // Along the middle of the (tilted) line; columns top to bottom.
            float x = o.x0 + (o.x1 - o.x0) * f, y = cy;
            if (o.vertical) x = (o.x0 + o.x1) / 2, y = o.y0 + (o.y1 - o.y0) * f;
            else if (o.lineH > 0) y = o.angle > 0 ? o.y0 + o.lineH / 2 + (o.y1 - o.y0 - o.lineH) * f
                                                  : o.y1 - o.lineH / 2 - (o.y1 - o.y0 - o.lineH) * f;
            for (size_t i = 0; i < gt.size() && i < g_gtQuads.size(); ++i)
                if (inQuad(g_gtQuads[i].q, x, y)) ++votes[i];
        }
        const auto best = std::max_element(votes.begin(), votes.end());
        if (best != votes.end() && *best > 0) got[best - votes.begin()].push_back(&o);
    }
    Accuracy a;
    for (size_t i = 0; i < gt.size(); ++i) {
        auto& v = got[i];
        std::sort(v.begin(), v.end(), [](const OcrLine* x, const OcrLine* y) { return x->x0 + x->y0 < y->x0 + y->y0; });
        std::wstring text;
        for (const OcrLine* o : v) text += o->text;
        const std::wstring g = squeeze(gt[i].text), b = squeeze(text);
        a.chars += g.size();
        const size_t e = std::min(editDistance(g, b), g.size());
        a.errs += e;
        a.exact += e == 0;
        if (e && verbose) std::printf("    OCR diff: \"%s\" read as \"%s\"\n", u8(gt[i].text).c_str(), u8(text).c_str());
    }
    a.lines = gt.size();
    std::printf("  OCR accuracy vs ground truth: %zu/%zu lines exact, character error rate %.1f %% (%zu/%zu)\n", a.exact,
                a.lines, a.chars ? 100.0 * a.errs / a.chars : 0.0, a.errs, a.chars);
    g_total.chars += a.chars, g_total.errs += a.errs, g_total.exact += a.exact, g_total.lines += a.lines;
    return a;
}

// OCR with the chosen engine (PaddleOCR unless --engine windows / not ready).
bool doOcr(const std::vector<uint8_t>& px, int w, int h, Lang src, OcrResult& r, std::wstring& err) {
    if (!g_windowsOcr && PaddleOcr::ready()) return PaddleOcr::recognize(px.data(), w, h, r, &err);
    if (!g_windowsOcr) std::printf("  (PaddleOCR not ready: Windows OCR)\n");
    const bool ok = Ocr::recognize(px.data(), w, h, src, r, &err);
    r.backend = OcrBackend::Windows;
    return ok;
}

bool ensureOcrModels(bool download) {
    if (g_windowsOcr || PaddleOcr::modelsInstalled() || !PaddleOcr::runtimeAvailable()) return true;
    const uint64_t miss = ModelStore::missingBytes({"ocr"});
    if (!download) {
        std::printf("OCR models missing (%.1f MB): run with --download\n", miss / 1e6);
        return false;
    }
    std::printf("downloading %.1f MB of OCR models into %ls ...\n", miss / 1e6, ModelStore::root().c_str());
    std::wstring err;
    const double t0 = nowMs();
    const bool ok = ModelStore::download({"ocr"}, nullptr, nullptr, &err);
    std::printf("OCR model download %s in %.1f s%s%s\n", ok ? "OK (SHA-256 verified)" : "FAILED", (nowMs() - t0) / 1000,
                ok ? "" : ": ", u8(err).c_str());
    return ok;
}

bool ensureModels(Lang src, Lang tgt, bool download) {
    const auto pairs = ModelStore::pairsFor(src, tgt);
    const uint64_t miss = ModelStore::missingBytes(pairs);
    if (!miss) return true;
    if (!download) {
        std::printf("models for %ls -> %ls missing (%.1f MB): run with --download\n", langTag(src), langTag(tgt), miss / 1e6);
        return false;
    }
    std::printf("downloading %.1f MB of models for %ls -> %ls into %ls ...\n", miss / 1e6, langTag(src), langTag(tgt),
                ModelStore::root().c_str());
    std::fflush(stdout);
    const double t0 = nowMs();
    std::wstring err;
    int last = -1;
    const bool ok = ModelStore::download(pairs, [&](double f) {
        const int p = static_cast<int>(f * 10);
        if (p != last) {
            last = p;
            std::printf("  %d%%\n", p * 10);
            std::fflush(stdout);
        }
    }, nullptr, &err);
    std::printf("download %s in %.1f s%s%s\n", ok ? "OK (SHA-256 verified)" : "FAILED", (nowMs() - t0) / 1000,
                ok ? "" : ": ", u8(err).c_str());
    return ok;
}

// ---- --sentences ----
int runSentences(Lang tgt, bool download) {
    const std::vector<std::wstring> ja = {L"設定", L"機内モード", L"通知をオンにすると、新しいメッセージを受け取ったときにお知らせします。",
                                          L"バッテリー残量が少なくなっています", L"カートに追加", L"送料無料（3,980円以上のご注文）",
                                          L"この写真を削除してもよろしいですか？", L"パスワードをお忘れですか？", L"キャンセル"};
    const std::vector<std::wstring> ko = {L"설정", L"비행기 모드", L"배터리 잔량이 부족합니다", L"장바구니에 담기",
                                          L"이 사진을 삭제하시겠습니까?", L"비밀번호를 잊으셨나요?",
                                          L"알림을 켜면 새 메시지를 받을 때 알려드립니다.", L"저장 공간이 거의 가득 찼습니다"};
    const std::vector<std::wstring> en = {L"Settings", L"Add to Cart", L"Free shipping on orders over $35",
                                          L"Are you sure you want to delete this photo?", L"Forgot your password?",
                                          L"Your package will arrive tomorrow by 8 PM.", L"Allow “Maps” to use your location?"};
    Engine engine;
    int fails = 0;
    for (auto [src, list] : {std::pair{Lang::Ja, &ja}, std::pair{Lang::Ko, &ko}, std::pair{Lang::En, &en}}) {
        if (src == tgt) continue;
        if (!ensureModels(src, tgt, download)) {
            ++fails;
            continue;
        }
        std::vector<std::wstring> out;
        std::wstring err;
        double t0 = nowMs();
        bool ok = engine.translate(src, tgt, *list, out, &err);  // first call loads the models
        const double first = nowMs() - t0, load = engine.lastLoadMs();
        t0 = nowMs();
        ok = ok && engine.translate(src, tgt, *list, out, &err);
        const double warm = nowMs() - t0;
        std::printf("== %ls -> %ls: model load %.0f ms, first batch %.0f ms (incl. load), warm batch %.0f ms for %zu lines\n",
                    langTag(src), langTag(tgt), load, first, warm, list->size());
        if (!ok) {
            std::printf("  FAILED: %s\n", u8(err).c_str());
            ++fails;
            continue;
        }
        for (size_t i = 0; i < list->size(); ++i) std::printf("  %s  ->  %s\n", u8((*list)[i]).c_str(), u8(out[i]).c_str());
    }
    return fails;
}

// ---- --text FILE: each UTF-8 line translated (--raw: the engine alone, no label fixes) ----
int runText(const std::wstring& file, Lang src, Lang tgt, bool raw) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") || !f) {
        std::printf("cannot read %ls\n", file.c_str());
        return 1;
    }
    std::string all;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) all.append(buf, n);
    fclose(f);
    std::vector<std::wstring> in;
    for (size_t p = 0; p < all.size();) {
        size_t e = all.find('\n', p);
        if (e == std::string::npos) e = all.size();
        std::string l = all.substr(p, e - p);
        if (!l.empty() && l.back() == '\n') l.pop_back();
        if (l.size() >= 3 && (unsigned char)l[0] == 0xEF) l = l.substr(3);
        if (!l.empty() && l[0] != '#') in.push_back(pm::translate::fromUtf8(l));
        p = e + 1;
    }
    Engine engine;
    std::vector<std::wstring> out;
    std::wstring err;
    double t0 = nowMs();
    const bool ok = raw ? engine.translate(src, tgt, in, out, &err) : translateTexts(engine, src, tgt, in, out, &err);
    std::printf("%zu lines %ls -> %ls in %.0f ms (model load %.0f ms)%s\n", in.size(), langTag(src), langTag(tgt), nowMs() - t0,
                engine.lastLoadMs(), ok ? "" : " FAILED");
    if (!ok) std::printf("  %s\n", u8(err).c_str());
    for (size_t i = 0; i < in.size() && i < out.size(); ++i) std::printf("  %s\n   -> %s\n", u8(in[i]).c_str(), u8(out[i]).c_str());
    return ok ? 0 : 1;
}

// ---- --ocr / --pipeline ----
int runPipeline(const std::vector<std::wstring>& pngs, Lang src, Lang tgt, bool translateToo, bool download) {
    Engine engine;
    int fails = 0;
    for (const auto& png : pngs) {
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!readPng(png, px, w, h)) {
            std::printf("cannot read %ls\n", png.c_str());
            ++fails;
            continue;
        }
        OcrResult r;
        std::wstring err;
        const auto gt = loadGt(png);
        if (g_useGt && translateToo) {
            if (gt.empty()) {
                std::printf("%ls: no ground truth (.gt.tsv)\n", baseName(png).c_str());
                ++fails;
                continue;
            }
            r.lines = gt;
            std::printf("  (ground-truth lines instead of OCR)\n");
        } else if (!doOcr(px, w, h, src, r, err)) {
            std::printf("%ls: OCR failed: %s\n", baseName(png).c_str(), u8(err).c_str());
            ++fails;
            continue;
        }
        if (!g_useGt || !translateToo) {  // warm second run: the steady-state time
            OcrResult r2;
            if (doOcr(px, w, h, src, r2, err)) r.ms = r2.ms;
        }
        std::printf("== %ls (%dx%d): %ls %ls, %zu lines, OCR %.0f ms (warm; detection %.0f, recognition %.0f, %d Korean)\n",
                    baseName(png).c_str(), w, h, r.backend == OcrBackend::Paddle ? L"PaddleOCR" : L"Windows OCR",
                    langTag(r.engine), r.lines.size(), r.ms, r.detMs, r.recMs, r.koLines);
        {
            std::wstring all;
            for (const auto& l : r.lines) all += l.text;
            std::printf("  looksMisread: %s\n", looksMisread(all) ? "YES (Japanese / Korean recogniser missing?)" : "no");
        }
        if (!translateToo && !gt.empty()) printAccuracy(gt, r.lines);
        if (!translateToo) {
            for (const auto& l : r.lines)
                std::printf("  [%.3f %.3f %.3f %.3f] h %.4f a %+.3f %.2f %-7ls %s%s\n", l.x0, l.y0, l.x1, l.y1, l.lineH,
                            l.angle, l.conf, langTag(l.script),
                            u8(l.text).c_str(), l.vertical ? "  (vertical)" : "");
            continue;
        }
        const auto blocks = groupLines(r.lines, static_cast<float>(w) / h);
        // Same per-block language rule as ScreenTranslator (kanji-only labels on a kana screen = ja).
        int kana = 0, hangul = 0;
        for (const auto& b : blocks)
            for (wchar_t c : b.text) kana += (c >= 0x3040 && c <= 0x30FF && c != 0x30FB && c != 0x30FC), hangul += (c >= 0xAC00 && c <= 0xD7AF);
        const Lang screen = kana >= 2 ? Lang::Ja : hangul >= 2 ? Lang::Ko : Lang::Unknown;
        double trMs = 0, loadMs = 0;
        int n = 0;
        for (const auto& b : blocks) {
            Lang l = b.lang;
            if (l == Lang::ZhHant || l == Lang::ZhHans) {
                if (screen != Lang::Unknown) l = screen;
            }
            std::wstring out = L"(kept)";
            if (l != Lang::Unknown && l != tgt) {
                if (!ensureModels(l, tgt, download)) return fails + 1;
                std::vector<std::wstring> o;
                const double t0 = nowMs();
                if (translateTexts(engine, l, tgt, {b.text}, o, &err) && !o.empty()) out = o[0];
                else out = L"FAILED " + err;
                trMs += nowMs() - t0 - engine.lastLoadMs();
                loadMs += engine.lastLoadMs();
                ++n;
            }
            std::printf("  %-7ls %s  ->  %s\n", langTag(l), u8(b.text).c_str(), u8(out).c_str());
        }
        std::printf("  %zu blocks, %d translated one by one in %.0f ms (+ model load %.0f ms)\n", blocks.size(), n, trMs, loadMs);
    }
    return fails;
}

// ---- --overlay ----
int g_winW = 540, g_winH = 960;  // --window WxH (DIPs)
int g_layoutMode = 0;            // --layout auto|inplace|list
bool g_dark = false;             // --dark: 深色方框

int runOverlay(const std::wstring& outDir, const std::vector<std::wstring>& pngs, Lang tgt, bool download) {
    SetEnvironmentVariableW(L"PM_VIDEO_OFFSCREEN", L"1");
    CreateDirectoryW(outDir.c_str(), nullptr);
    pm::VideoWindow win;
    if (!win.create(L"pm_translate_test", g_winW, g_winH)) return 1;
    win.setTheme(pm::VideoWindow::Theme::Sakura);
    win.setTextOverlayStyle(g_layoutMode, g_dark);
    int fails = 0;
    std::thread t([&]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::atomic<int> done{0};
        std::atomic<bool> lastOk{false};
        ScreenTranslator::Timing last;
        std::mutex lm;
        ScreenTranslator::Callbacks cb;
        cb.askDownload = [download](Lang s, Lang g, double mb, std::function<void(bool)> answer) {
            if (s == Lang::Unknown)
                std::printf("  [consent] download %.0f MB of OCR models? %s\n", mb, download ? "yes" : "no");
            else
                std::printf("  [consent] download %.0f MB for %ls -> %ls? %s\n", mb, langTag(s), langTag(g), download ? "yes" : "no");
            answer(download);
        };
        cb.notify = [](const std::wstring& title, const std::wstring& text, bool important) {
            std::printf("  [notify%s] %s: %s\n", important ? " IMPORTANT" : "", u8(title).c_str(), u8(text).c_str());
        };
        std::vector<OcrLine> gtLines;  // of the picture being translated (--gt)
        if (g_useGt)
            cb.ocrOverride = [&](const uint8_t*, int, int, const float region[4], OcrResult& out) {
                out = gtInRegion(gtLines, region);
                out.ms = 0;
                return !out.lines.empty();
            };
        cb.finished = [&](bool ok, const ScreenTranslator::Timing& tm) {
            std::lock_guard lk(lm);
            last = tm;
            lastOk = ok;
            done++;
        };
        ScreenTranslator tr(win, cb);
        tr.setTarget(tgt);
        auto waitDone = [&](int n, int ms) {
            const double t0 = nowMs();
            while (done < n && nowMs() - t0 < ms) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return done >= n;
        };
        auto shot = [&](const std::wstring& name) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            bool ok = false;
            HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            win.post([&] {
                ok = win.saveWindowShot(outDir + L"\\" + name);
                SetEvent(ev);
            });
            WaitForSingleObject(ev, 5000);
            CloseHandle(ev);
            std::printf("  shot %ls%s\n", name.c_str(), ok ? "" : " FAILED");
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
        int runs = 0;
        for (const auto& png : pngs) {
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            if (!readPng(png, px, w, h)) {
                std::printf("cannot read %ls\n", png.c_str());
                ++fails;
                continue;
            }
            const std::wstring n = baseName(png);
            std::printf("== %ls%s\n", n.c_str(), g_useGt ? " (ground-truth text boxes instead of OCR)" : "");
            gtLines = loadGt(png);
            for (int i = 0; i < 8; ++i) {  // a few frames: the picture is up
                win.submitBgraFrame(px.data(), w, h, w * 4, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            onUi([&] { tr.translateScreen(); });
            if (!waitDone(++runs, 120000)) {
                std::printf("  FAIL: no result within 120 s\n");
                ++fails;
                break;
            }
            {
                std::lock_guard lk(lm);
                std::printf("  %s: grab %.0f ms, OCR %.0f ms, translate %.0f ms (incl. model load), total %.0f ms; %d lines, "
                            "%d blocks, %d translated, source %ls\n",
                            lastOk ? "OK" : "FAILED", last.grabMs, last.ocrMs, last.translateMs, last.totalMs, last.lines,
                            last.blocks, last.translated, langTag(last.source));
                if (!lastOk) ++fails;
            }
            for (const auto& it : tr.lastItems())
                std::printf("    %-7ls %s  ->  %s\n", langTag(it.lang), u8(it.original).c_str(), u8(it.translated).c_str());
            {
                std::lock_guard lk(lm);
                std::printf("  SUMMARY %ls: %d lines, %d blocks, %d translated, OCR %.0f ms, translate %.0f ms, total %.0f ms\n",
                            n.c_str(), last.lines, last.blocks, last.translated, last.ocrMs, last.translateMs, last.totalMs);
            }
            shot(n + L"_tr.png");
            {
                // 0.7.1 layout: in place / listed; the list's hover, a marker
                // click, 放大這一塊 and the high-contrast style.
                static const UINT testMsg = RegisterWindowMessageW(L"PhoneMirror.Video.Test");
                HWND hw = win.hwnd();
                auto info = win.textOverlayInfo();
                std::printf("  LAYOUT %ls: %d in place, %d listed (%d did not fit%s)%s\n", n.c_str(), info.inPlace, info.listed,
                            info.notFitting, info.listAll ? ", all listed" : "", info.zoomButton ? ", zoom button" : "");
                std::printf("  CHECKS %ls: overlaps %d, too close %d, cut characters %d, kinsoku %d, short last lines %d, "
                            "marker clashes %d, %d text sizes, smallest %.1f px\n",
                            n.c_str(), info.overlaps, info.tooClose, info.truncated, info.kinsoku, info.shortLast, info.markerClashes,
                            info.fontSizes, info.minFontPx);
                if (info.overlaps || info.truncated || info.kinsoku) ++fails;
                if (std::getenv("PM_OVERLAY_070"))
                    std::printf("  CHECKS-0.7.0 %ls: %d cards, overlaps %d, cut characters %d, short last lines %d\n", n.c_str(),
                                info.old070Cards, info.old070Overlaps, info.old070Cut, info.old070ShortLast);
                SendMessageW(hw, testMsg, 12, 1);  // posted mouse messages only
                if (info.listed > 0) {
                    const LRESULT c = SendMessageW(hw, testMsg, 14, 0);
                    if (c != -1) {
                        PostMessageW(hw, WM_MOUSEMOVE, 0, c);
                        shot(n + L"_hover.png");
                    }
                    const LRESULT mk = SendMessageW(hw, testMsg, 15, info.listed - 1);
                    if (mk != -1) {
                        PostMessageW(hw, WM_MOUSEMOVE, 0, mk);
                        PostMessageW(hw, WM_LBUTTONDOWN, MK_LBUTTON, mk);
                        PostMessageW(hw, WM_LBUTTONUP, 0, mk);
                        shot(n + L"_marker.png");
                    }
                    PostMessageW(hw, WM_MOUSEMOVE, 0, MAKELPARAM(1, 1));
                }
                if (info.zoomButton) {
                    const LRESULT z = SendMessageW(hw, testMsg, 16, 0);
                    if (z != -1) {
                        PostMessageW(hw, WM_MOUSEMOVE, 0, z);
                        PostMessageW(hw, WM_LBUTTONDOWN, MK_LBUTTON, z);
                        PostMessageW(hw, WM_LBUTTONUP, 0, z);
                        PostMessageW(hw, WM_MOUSEMOVE, 0, MAKELPARAM(1, 1));
                        shot(n + L"_zoomed.png");
                        info = win.textOverlayInfo();
                        std::printf("  LAYOUT %ls zoomed %.1fx: %d in place, %d listed\n", n.c_str(), win.viewState().zoom,
                                    info.inPlace, info.listed);
                        onUi([&] { win.resetMagnifier(); });
                    }
                }
                if (&png == &pngs.front()) {
                    onUi([&] { win.setFilter(pm::VideoWindow::Filter::YellowOnBlack); });
                    shot(n + L"_yellow.png");
                    onUi([&] { win.setFilter(pm::VideoWindow::Filter::None); });
                }
                SendMessageW(hw, testMsg, 12, 0);
            }
            onUi([&] { tr.setShowOriginal(true); });
            shot(n + L"_original.png");
            // Again, now warm (models loaded): the steady-state time.
            onUi([&] { tr.close(); });
            onUi([&] { tr.translateScreen(); });
            if (waitDone(++runs, 60000)) {
                std::lock_guard lk(lm);
                std::printf("  warm run: OCR %.0f ms, translate %.0f ms, total %.0f ms\n", last.ocrMs, last.translateMs,
                            last.totalMs);
            }
            // Region: drag over the middle third of the picture.
            onUi([&] { tr.close(); });
            onUi([&] { tr.translateRegion(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            HWND hw = win.hwnd();
            RECT cr{};
            GetClientRect(hw, &cr);
            const float sc = std::min(cr.right / static_cast<float>(w), cr.bottom / static_cast<float>(h));
            const float vw = std::round(w * sc), vh = std::round(h * sc);
            const float left = std::floor((cr.right - vw) / 2), top = std::floor((cr.bottom - vh) / 2);
            auto pt = [&](float fx, float fy) {
                return MAKELPARAM(static_cast<int>(left + vw * fx), static_cast<int>(top + vh * fy));
            };
            PostMessageW(hw, WM_LBUTTONDOWN, MK_LBUTTON, pt(0.02f, 0.36f));
            PostMessageW(hw, WM_MOUSEMOVE, MK_LBUTTON, pt(0.98f, 0.66f));
            PostMessageW(hw, WM_LBUTTONUP, 0, pt(0.98f, 0.66f));
            if (waitDone(++runs, 60000)) {
                std::lock_guard lk(lm);
                std::printf("  region run: %s, %d lines, %d translated, OCR %.0f ms, translate %.0f ms\n",
                            lastOk ? "OK" : "FAILED", last.lines, last.translated, last.ocrMs, last.translateMs);
            } else {
                std::printf("  FAIL: region run did not finish\n");
                ++fails;
            }
            shot(n + L"_region.png");
            onUi([&] { tr.close(); });
            if (&png == &pngs.front()) {
                // Live mode: no freeze, re-translated every 2 s.
                onUi([&] {
                    tr.setLive(true, 2);
                    tr.translateScreen();
                });
                const int before = runs;
                const double t0 = nowMs();
                runs += 2;
                const bool two = waitDone(runs, 15000);
                const bool frozen = win.viewState().frozen;
                std::printf("  live mode: %s (%d runs in %.1f s, picture %s)\n", two && !frozen ? "OK" : "FAIL",
                            done.load() - before, (nowMs() - t0) / 1000, frozen ? "FROZEN" : "not frozen");
                if (!two || frozen) ++fails;
                onUi([&] { tr.close(); });
                runs = done.load();
            }
            win.onReset();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
        onUi([&] { tr.close(); });
        CoUninitialize();
        win.close();
    });
    win.runMessageLoop();
    t.join();
    std::printf("%d failure(s)\n", fails);
    return fails;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Lang tgt = defaultTarget(), src = Lang::Unknown;
    bool download = false;
    std::wstring mode, outDir;
    std::vector<std::wstring> files;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool v = i + 1 < argc;
        if (a == L"--target" && v) tgt = parseLang(argv[++i]);
        else if (a == L"--lang" && v) src = parseLang(argv[++i]);
        else if (a == L"--download") download = true;
        else if (a == L"--gt") g_useGt = true;
        else if (a == L"--engine" && v) g_windowsOcr = !_wcsicmp(argv[++i], L"windows");
        else if (a == L"--en") pm::i18n::setLang(pm::i18n::Lang::En);
        else if (a == L"--overlay" && v) mode = a, outDir = argv[++i];
        else if (a == L"--window" && v) swscanf_s(argv[++i], L"%dx%d", &g_winW, &g_winH);
        else if (a == L"--layout" && v) {
            const std::wstring m = argv[++i];
            g_layoutMode = m == L"inplace" ? 1 : m == L"list" ? 2 : 0;
        } else if (a == L"--dark") g_dark = true;
        else if (a.rfind(L"--", 0) == 0) mode = a;
        else files.push_back(a);
    }
    if (mode == L"--ocr-langs") {
        for (Lang l : {Lang::Ja, Lang::Ko, Lang::En, Lang::ZhHans, Lang::ZhHant})
            std::printf("%-8ls %s\n", langTag(l), Ocr::available(l) ? "installed" : "missing");
        std::wstring err;
        std::printf("bergamot.dll: %s %s\n", Engine::available(&err) ? "OK" : "MISSING", u8(err).c_str());
        err.clear();
        std::printf("onnxruntime.dll: %s %s\n", PaddleOcr::runtimeAvailable(&err) ? "OK" : "MISSING", u8(err).c_str());
        std::printf("PaddleOCR models: %s\n", PaddleOcr::modelsInstalled() ? "installed" : "missing");
        std::printf("models dir: %ls\n", ModelStore::root().c_str());
        return 0;
    }
    if (mode == L"--sentences") return runSentences(tgt, download);
    if (mode == L"--text" || mode == L"--raw") {
        if (files.empty()) return 2;
        if (src == Lang::Unknown) src = Lang::Ja;
        if (!ensureModels(src, tgt, download)) return 1;
        return runText(files[0], src, tgt, mode == L"--raw");
    }
    if (mode == L"--ocr" || mode == L"--pipeline") {
        // OCR needs an MTA thread.
        int rc = 0;
        std::thread([&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (!ensureOcrModels(download)) {
                rc = 1;
                CoUninitialize();
                return;
            }
            rc = runPipeline(files, src, tgt, mode == L"--pipeline", download);
            if (g_total.lines)
                std::printf("TOTAL OCR: %zu/%zu lines exact, character error rate %.2f %% (%zu/%zu)\n", g_total.exact,
                            g_total.lines, g_total.chars ? 100.0 * g_total.errs / g_total.chars : 0.0, g_total.errs, g_total.chars);
            CoUninitialize();
        }).join();
        return rc;
    }
    if (mode == L"--overlay") return runOverlay(outDir, files, tgt, download);
    std::fprintf(stderr, "usage: see the header of translate/tools/pm_translate_test.cpp\n");
    return 2;
}
