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
//   pm_translate_test --eval OUT.jsonl PNG [PNG...] [--target T] [--gt]
//       the app's path (OCR or --gt lines -> blocks -> pickBlocks ->
//       translateTexts -> translatedOk), one JSON line per picture: lines,
//       blocks, why a block was left out, its translation and the engine's
//       alone; scored by translate/testdata/eval_metrics.py
//   pm_translate_test --live PNG [--target T]
//       即時翻譯 on a synthetic 30 fps stream over PNG (still / scroll / video
//       corner): CPU, pictures looked at, runs, settle -> overlay times
//   pm_translate_test --selftest [--download]
//       the P0 rules without pictures: table rows (layoutBlocks), checks
//       (negation / numbers / brackets), templates, verified facts, the
//       memory cache; with the ja -> zh-Hant models also translateTextsEx
//       (rows 「標籤　值」, a negation through the pivot, cache hits)
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
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <wrl/client.h>

#include <atomic>
#include <exception>
#include <csignal>
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
    if (!g_windowsOcr && PaddleOcr::ready()) {
        // PM_OCR_EARLY_TEST=1: the app's early top part (read first, then the
        // rest; the lines handed out are dropped) - its effect on the text.
        const char* e = std::getenv("PM_OCR_EARLY_TEST");
        static const std::function<void(std::vector<OcrLine>)> drop = [](std::vector<OcrLine>) {};
        return PaddleOcr::recognize(px.data(), w, h, r, &err, src, e && *e == '1' ? drop : std::function<void(std::vector<OcrLine>)>());
    }
    if (!g_windowsOcr) std::printf("  (PaddleOCR not ready: Windows OCR)\n");
    const bool ok = Ocr::recognize(px.data(), w, h, src, r, &err);
    r.backend = OcrBackend::Windows;
    return ok;
}

bool ensureOcrGpu(bool download) {
    if (g_windowsOcr || !PaddleOcr::gpuWanted() || ModelStore::ocrGpuInstalled()) return true;
    const uint64_t miss = ModelStore::ocrGpuMissingBytes();
    if (!download) {
        std::printf("OCR GPU add-on missing (%.1f MB): run with --download (CPU meanwhile)\n", miss / 1e6);
        return true;
    }
    std::printf("downloading the OCR GPU add-on, %.1f MB, into %ls ...\n", miss / 1e6, ModelStore::ocrGpuDir().c_str());
    std::wstring err;
    const double t0 = nowMs();
    const bool ok = ModelStore::downloadOcrGpu(nullptr, nullptr, &err);
    std::printf("OCR GPU add-on download %s in %.1f s%s%s\n", ok ? "OK (SHA-256 verified)" : "FAILED", (nowMs() - t0) / 1000,
                ok ? "" : ": ", u8(err).c_str());
    return true;  // the CPU runtime otherwise
}

bool ensureOcrModels(bool download) {
    ensureOcrGpu(download);
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
    // "label<TAB>value": a table row (Block::labelLen), as layoutBlocks makes it.
    std::vector<size_t> labels(in.size(), 0);
    for (size_t i = 0; i < in.size(); ++i)
        if (const size_t t = in[i].find(L'\t'); t != std::wstring::npos) in[i][t] = L' ', labels[i] = t;
    Escalator esc(engine);
    std::vector<TextInfo> info;
    if (std::getenv("PM_TEXT_WARM")) {  // tests: once before timing (models and parallel translators loaded)
        engine.translate(src, tgt, in, out, &err);
        t0 = nowMs();
    }
    const bool ok = raw ? engine.translate(src, tgt, in, out, &err)
                        : translateTextsEx(engine, &esc, src, tgt, in, labels, out, &info, &err);
    for (size_t i = 0; i < info.size() && i < out.size(); ++i)
        if (info[i].uncertain && !info[i].verified.empty()) out[i] += L"\n     ⚠ " + info[i].verified;
    std::printf("%zu lines %ls -> %ls in %.0f ms (model load %.0f ms)%s\n", in.size(), langTag(src), langTag(tgt), nowMs() - t0,
                engine.lastLoadMs(), ok ? "" : " FAILED");
    if (!ok) std::printf("  %s\n", u8(err).c_str());
    for (size_t i = 0; i < in.size() && i < out.size(); ++i) std::printf("  %s\n   -> %s\n", u8(in[i]).c_str(), u8(out[i]).c_str());
    return ok ? 0 : 1;
}

// ---- --ocr with PM_OCR_AB="name:K=V;K=V|name:K=V|...": OCR settings compared
// on each picture in turn (one process, the order rotated per round, so the
// load of the PC falls on all alike).  PM_OCR_REPS rounds (default 3).  One
// ABSTAT line per picture and setting: median ms, detection / recognition of
// that run, character errors of the last run.
int runOcrAb(const std::vector<std::wstring>& pngs, Lang src, const std::string& spec) {
    struct Variant {
        std::string name;
        std::vector<std::pair<std::string, std::string>> kv;
    };
    std::vector<Variant> vs;
    std::vector<std::string> keys;
    for (size_t p = 0; p <= spec.size();) {
        size_t e = spec.find('|', p);
        if (e == std::string::npos) e = spec.size();
        std::string part = spec.substr(p, e - p);
        Variant v;
        if (const size_t c = part.find(':'); c != std::string::npos) v.name = part.substr(0, c), part = part.substr(c + 1);
        for (size_t q = 0; q < part.size();) {
            size_t f = part.find(';', q);
            if (f == std::string::npos) f = part.size();
            const std::string kv = part.substr(q, f - q);
            if (const size_t eq = kv.find('='); eq != std::string::npos) {
                v.kv.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
                if (std::find(keys.begin(), keys.end(), kv.substr(0, eq)) == keys.end()) keys.push_back(kv.substr(0, eq));
            }
            q = f + 1;
        }
        if (v.name.empty()) v.name = "v" + std::to_string(vs.size());
        vs.push_back(v);
        p = e + 1;
    }
    const int reps = std::max(1, std::getenv("PM_OCR_REPS") ? atoi(std::getenv("PM_OCR_REPS")) : 3);
    auto sessionKey = [](const std::string& k) { return k == "PM_OCR_THREADS" || k == "PM_OCR_INTER" || k == "PM_OCR_SPIN" || k == "PM_OCR_DET_THREADS"; };
    std::string sessionNow = "?";
    auto apply = [&](const Variant& v) {
        std::string sess;
        for (const auto& k : keys) _putenv_s(k.c_str(), "");
        for (const auto& [k, val] : v.kv) {
            _putenv_s(k.c_str(), val.c_str());
            if (sessionKey(k)) sess += k + "=" + val + ";";
        }
        if (sess != sessionNow) {  // other session options: new sessions, one untimed run
            PaddleOcr::unload();
            sessionNow = sess;
            return true;
        }
        return false;
    };
    int fails = 0;
    for (const auto& png : pngs) {
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!readPng(png, px, w, h)) {
            ++fails;
            continue;
        }
        const auto gt = loadGt(png);
        std::vector<std::vector<OcrResult>> res(vs.size());
        std::wstring err;
        for (int r = 0; r < reps; ++r)
            for (size_t k = 0; k < vs.size(); ++k) {
                const size_t vi = (k + r) % vs.size();
                OcrResult o;
                if (apply(vs[vi]) || (r == 0 && &png == &pngs.front())) doOcr(px, w, h, src, o, err);
                if (doOcr(px, w, h, src, o, err)) res[vi].push_back(std::move(o));
                else ++fails;
            }
        for (size_t vi = 0; vi < vs.size(); ++vi) {
            auto& v = res[vi];
            if (v.empty()) continue;
            const OcrResult last = v.back();
            std::sort(v.begin(), v.end(), [](const OcrResult& a, const OcrResult& b) { return a.ms < b.ms; });
            const OcrResult& m = v[v.size() / 2];
            Accuracy a;
            if (!gt.empty()) a = printAccuracy(gt, last.lines, false);
            std::printf("ABSTAT %ls %s ms %.0f det %.0f rec %.0f lines %zu ko %d gtchars %zu errs %zu exact %zu/%zu min %.0f\n",
                        baseName(png).c_str(), vs[vi].name.c_str(), m.ms, m.detMs, m.recMs, last.lines.size(), last.koLines, a.chars,
                        a.errs, a.exact, a.lines, v.front().ms);
            if (std::getenv("PM_OCR_AB_TEXT"))
                for (const auto& l : last.lines) std::printf("ABLINE %ls %s %s\n", baseName(png).c_str(), vs[vi].name.c_str(), u8(l.text).c_str());
        }
        std::fflush(stdout);
    }
    return fails;
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
        // Warm second run: the steady-state time.  PM_OCR_REPS=N: N more runs
        // (0: the first only), the median of them reported.
        static const int reps = std::getenv("PM_OCR_REPS") ? atoi(std::getenv("PM_OCR_REPS")) : 1;
        if (!g_useGt || !translateToo) {
            std::vector<OcrResult> runs;
            for (int k = 0; k < reps; ++k) {
                OcrResult r2;
                if (doOcr(px, w, h, src, r2, err)) runs.push_back(std::move(r2));
            }
            if (!runs.empty()) {
                std::sort(runs.begin(), runs.end(), [](const OcrResult& a, const OcrResult& b) { return a.ms < b.ms; });
                const OcrResult& m = runs[runs.size() / 2];
                r.ms = m.ms, r.detMs = m.detMs, r.recMs = m.recMs;
            }
        }
        std::printf("== %ls (%dx%d): %ls %ls, %zu lines, OCR %.0f ms (warm; detection %.0f, recognition %.0f, %d Korean)\n",
                    baseName(png).c_str(), w, h, r.backend == OcrBackend::Paddle ? L"PaddleOCR" : L"Windows OCR",
                    langTag(r.engine), r.lines.size(), r.ms, r.detMs, r.recMs, r.koLines);
        {
            std::wstring all;
            for (const auto& l : r.lines) all += l.text;
            std::printf("  looksMisread: %s\n", looksMisread(all) ? "YES (Japanese / Korean recogniser missing?)" : "no");
        }
        if (!translateToo) {  // one machine-readable line per picture (OCR speed work)
            Accuracy a;
            if (!gt.empty()) a = printAccuracy(gt, r.lines);
            std::printf("OCRSTAT %ls ms %.0f det %.0f rec %.0f lines %zu ko %d gtchars %zu errs %zu exact %zu/%zu\n",
                        baseName(png).c_str(), r.ms, r.detMs, r.recMs, r.lines.size(), r.koLines, a.chars, a.errs, a.exact, a.lines);
        }
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
            for (wchar_t c : b.text) kana += (c >= 0x3040 && c <= 0x30FF && c != 0x30FB && c != 0x30FC), hangul += isHangul(c);
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

// ---- --eval OUT.jsonl PNG...: the app's own path, as data for translate/testdata/eval_metrics.py ----
std::string jstr(const std::wstring& s) {
    std::string o = "\"";
    for (char c : u8(s)) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (c == '\n') o += "\\n";
        else if (c == '\r' || c == '\t') o += ' ';
        else o += c;
    }
    return o + "\"";
}
std::string jbox(float x0, float y0, float x1, float y1) {
    char b[96];
    std::snprintf(b, sizeof b, "[%.4f,%.4f,%.4f,%.4f]", x0, y0, x1, y1);
    return b;
}

// OCR (or --gt) -> groupLines -> pickBlocks -> translateTexts -> translatedOk,
// as ScreenTranslator does, one JSON line per picture: the lines, every block
// with why it was left out or its translation (tx; raw: the engine alone on
// the block, for telling engine errors from the label fixes).
int runEval(const std::wstring& outFile, const std::vector<std::wstring>& pngs, Lang src, Lang tgt, bool download) {
    FILE* out = nullptr;
    if (_wfopen_s(&out, outFile.c_str(), L"wb") || !out) {
        std::printf("cannot write %ls\n", outFile.c_str());
        return 1;
    }
    Engine engine;
    Escalator esc(engine);
    {
        // PM_EVAL_ALT=1: the local LLM on every ja / ko engine piece too
        // (block "alt"), the routing data of translate/testdata/route_eval.py.
        EscalationConfig c = esc.config();
        c.collectAlt = std::getenv("PM_EVAL_ALT") != nullptr;
        c.llmShortItems = std::getenv("PM_EVAL_SHORT") != nullptr;  // route_eval policy U+T+S10
        esc.setConfig(c);
    }
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
        esc.resetBudget();
        const double t0 = nowMs();
        if (g_useGt) {
            r.lines = loadGt(png);
        } else if (!doOcr(px, w, h, src, r, err)) {
            std::printf("%ls: OCR failed: %s\n", baseName(png).c_str(), u8(err).c_str());
            ++fails;
            continue;
        }
        const double ocrMs = nowMs() - t0;
        // The app's own steps (screen_translator.cpp planPicture / translatePlan).
        const bool old = std::getenv("PM_EVAL_072") != nullptr;  // 0.7.2's path (baseline)
        const PicturePlan plan = planPicture(r.lines, src, tgt, static_cast<float>(w) / h, g_useGt, old);
        const auto& blocks = plan.blocks;
        const auto& why = plan.why;
        std::vector<std::wstring> tx(blocks.size()), raw(blocks.size());
        std::vector<int> ok(blocks.size(), 0);
        std::vector<TextInfo> infos(blocks.size());
        const double t1 = nowMs();
        double loadMs = 0, rawMs = 0;
        // As ScreenTranslator: the result shown at once (pass 1), the LLM on
        // the queued pieces within the picture's budget, then the improved
        // result (pass 2, from the cache).
        bool modelsOk = true;
        auto pass = [&](bool first) {
            PlanHooks hooks;
            hooks.legacy = old;
            hooks.beforeLang = [&](Lang l) {
                if (ensureModels(l, tgt, download)) return true;
                modelsOk = false;
                return false;
            };
            hooks.chunkDone = [&](Lang l, const std::vector<size_t>& idx, const std::vector<std::wstring>& in, const std::vector<std::wstring>& o,
                                  const std::vector<TextInfo>& info) {
                std::vector<std::wstring> ro;
                loadMs += engine.lastLoadMs();
                if (first) {  // the engine alone (metrics' "raw"; not part of the times)
                    const double r0 = nowMs();
                    engine.translate(l, tgt, in, ro, &err);
                    rawMs += nowMs() - r0;
                }
                for (size_t k = 0; k < idx.size() && k < o.size(); ++k) {
                    const TextInfo* inf = k < info.size() ? &info[k] : nullptr;
                    tx[idx[k]] = cardText(o[k], inf);
                    if (inf) infos[idx[k]] = *inf;
                    if (first && k < ro.size()) raw[idx[k]] = ro[k];
                    ok[idx[k]] = (old ? translatedOk(blocks[idx[k]].text, o[k], l, tgt) : cardWorthy(blocks[idx[k]], o[k], inf, l, tgt)) ? 1 : 0;
                }
                return true;
            };
            if (!translatePlan(engine, &esc, plan, tgt, kPlanChunk, hooks, &err) && modelsOk) {
                std::printf("%ls: translation failed: %s\n", baseName(png).c_str(), u8(err).c_str());
                ++fails;
            }
        };
        pass(true);
        if (!modelsOk) return fails + 1;
        // GT lines: no OCR ran; PM_EVAL_OCR_MS charges a typical OCR time to the budget.
        const double ocrCharged = g_useGt && std::getenv("PM_EVAL_OCR_MS") ? atof(std::getenv("PM_EVAL_OCR_MS")) : 0;
        const double firstMs = nowMs() - t0 - rawMs - loadMs + ocrCharged;
        std::vector<Escalator::RouteTrace> route;
        const size_t queued = esc.pendingCount();
        if (queued && !esc.config().collectAlt) {
            const double start = t0 + rawMs + loadMs - ocrCharged;  // the picture's clock without the test-only work
            if (esc.runPending(start + esc.config().targetMs, &route) > 0) pass(false);
        }
        const double totalMs = nowMs() - t0 - rawMs - loadMs + ocrCharged;
        esc.warmUp(true);  // as ScreenTranslator after showing the picture (not timed)
        const double trMs = nowMs() - t1 - loadMs - rawMs;
        std::string j = "{\"image\":" + jstr(baseName(png)) + ",\"gt\":" + (g_useGt ? "true" : "false") +
                        ",\"ocr_ms\":" + std::to_string(static_cast<int>(ocrMs)) + ",\"tr_ms\":" + std::to_string(static_cast<int>(trMs)) +
                        ",\"first_ms\":" + std::to_string(static_cast<int>(firstMs)) +
                        ",\"det_ms\":" + std::to_string(static_cast<int>(r.detMs)) + ",\"rec_ms\":" +
                        std::to_string(static_cast<int>(r.recMs)) + ",\"ko_lines\":" + std::to_string(r.koLines) +
                        ",\"pic\":\"" + std::to_string(w) + "x" + std::to_string(h) + "\"" +
                        ",\"total_ms\":" + std::to_string(static_cast<int>(totalMs)) + ",\"route\":[";
        for (size_t i = 0; i < route.size(); ++i)
            j += (i ? "," : "") + std::string("{\"t\":") + jstr(route[i].plain) + ",\"out\":" + jstr(route[i].out) +
                 ",\"prio\":" + std::to_string(route[i].prio) + ",\"ms\":" + std::to_string(static_cast<int>(route[i].ms)) +
                 ",\"pred\":" + std::to_string(static_cast<int>(route[i].predictedMs)) + ",\"res\":" + jstr(fromUtf8(route[i].result)) + "}";
        j += "],\"lines\":[";
        for (size_t i = 0; i < r.lines.size(); ++i) {
            const auto& l = r.lines[i];
            j += (i ? "," : "") + std::string("{\"t\":") + jstr(l.text) + ",\"b\":" + jbox(l.x0, l.y0, l.x1, l.y1) + "}";
        }
        j += "],\"blocks\":[";
        int picked = 0, shown = 0;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const Block& b = blocks[i];
            picked += why[i].empty();
            shown += ok[i];
            j += (i ? "," : "") + std::string("{\"t\":") + jstr(b.text) + ",\"b\":" + jbox(b.x0, b.y0, b.x1, b.y1) +
                 ",\"lang\":" + jstr(langTag(b.lang)) + ",\"skip\":" + jstr(fromUtf8(why[i])) + ",\"tx\":" + jstr(tx[i]) +
                 ",\"raw\":" + jstr(raw[i]) + ",\"ok\":" + (ok[i] ? "true" : "false") + ",\"label\":" +
                 std::to_string(b.labelLen) + ",\"step\":" + std::to_string(infos[i].step) +
                 ",\"uncertain\":" + (infos[i].uncertain ? "true" : "false") + ",\"alt\":" + jstr(infos[i].alt) +
                 ",\"alt_ms\":" + std::to_string(static_cast<int>(infos[i].altMs)) + ",\"eng\":" + jstr(fromUtf8(infos[i].engines)) +
                 ",\"alt_flags\":" + jstr(fromUtf8(infos[i].altFlags)) +
                 ",\"flags\":\"";
            for (const auto& f : infos[i].flags) j += f + " ";
            j += "\"}";
        }
        j += "]}\n";
        fwrite(j.data(), 1, j.size(), out);
        int accepted = 0;
        for (const auto& rt : route) accepted += rt.result == "accepted";
        std::printf("== %ls: %zu lines, %zu blocks, %d picked, %d shown, OCR %.0f ms, translate %.0f ms, first %.0f ms, total %.0f ms, "
                    "LLM %zu queued %d accepted\n",
                    baseName(png).c_str(), r.lines.size(), blocks.size(), picked, shown, ocrMs, trMs, firstMs, totalMs, queued, accepted);
    }
    fclose(out);
    return fails;
}

// ---- --selftest, QE garbage / negation (qe.cpp, translator.cpp checkTranslation) ----
int qeGarbageSelfTest(int& n) {
    int fails = 0;
    auto expect = [&](bool ok, const char* what, const std::wstring& got = L"") {
        ++n;
        if (!ok) ++fails;
        std::printf("  %s %s%s%s\n", ok ? "ok  " : "FAIL", what, got.empty() ? "" : "  -> ", u8(got).c_str());
    };
    auto has = [](const std::vector<std::string>& v, const char* f) { return std::find(v.begin(), v.end(), f) != v.end(); };
    auto chk = [](const wchar_t* s, const wchar_t* t, Lang l = Lang::Ja) { return checkTranslation(s, t, l, Lang::ZhHant); };
    std::printf("checks (qe garbage / negation):\n");
    // Kana spelled out in sound characters (owner 0.7.7 screenshots).
    for (const auto& [s, t] : {std::pair{L"エッセイ", L"艾莎"}, {L"カンジ", L"康司"}, {L"かこさい", L"加古塞伊"}, {L"・あさり", L"・亞薩里"},
                               {L"ラーメン", L"拉曼"}, {L"メンマタンタンメン 800", L"曼瑪坦·塔曼 800"}, {L"・ほたて", L"・霍特"}})
        expect(has(chk(s, t), "translit"), "kana -> sound characters fails", t);
    for (const auto& [s, t] : {std::pair{L"ラーメン", L"拉麵"}, {L"サラダ", L"沙拉"}, {L"モヒート ゼロ ¥480", L"莫吉托零度 ¥480"},
                               {L"・いくら", L"・多少"}, {L"キャンセル", L"取消"}, {L"ストロベリー", L"草莓"}, {L"漢字", L"漢字"}})
        expect(!has(chk(s, t), "translit"), "a real translation / loanword passes", t);
    expect(!has(chk(L"マリア様", L"瑪麗亞大人"), "translit"), "kanji in the source: not kana-only");
    // Stray Latin / currency (「使用 sy 的 s€」 for ふりがな).
    expect(has(chk(L"ふりがなを使う", L"使用 sy 的 s€"), "stray-latin"), "stray Latin and € fail");
    expect(!has(chk(L"価格 380円", L"價格 ¥380"), "stray-latin"), "¥ for 円 passes");
    // ASCII words changed on the way.
    expect(has(chk(L"PITCH ACCENT", L"PATCH ACCENT", Lang::En), "latin-changed"), "PITCH -> PATCH fails");
    expect(has(chk(L"プレゼン.odp - LibreOffice Impress", L"簡報.odp - LiberOffice 印象"), "latin-changed"), "LibreOffice -> LiberOffice fails");
    expect(!has(chk(L"PITCH ACCENT", L"音高重音", Lang::En), "latin-changed"), "a translated word passes");
    expect(!has(chk(L"Open the app", L"打開 apps", Lang::En), "latin-changed"), "a plural is not a change");
    expect(!has(chk(L"JLPT N5", L"JLPT N5 級", Lang::En), "latin-changed"), "kept as written passes");
    expect(!has(chk(L"Tamaño de Porción", L"Tamaño de Porcion", Lang::En), "latin-changed") &&
               !has(chk(L"Epson Perfection™ V800", L"Epson PerfectionTM V800", Lang::En), "latin-changed"),
           "an accent dropped / ™ spelled out is not a change");
    // Loops.
    expect(has(chk(L"タンタンメン", L"玉米密塔坦萬特萬特坦萬特萬特坦萬特萬特坦"), "repeat"), "a five-character loop fails");
    expect(!has(chk(L"はい、はい", L"是的，是的"), "repeat"), "a phrase said twice is not a loop");
    // Negation: prohibitions and the instruction of a negated sentence.
    for (const wchar_t* s : {L"無断転載禁止", L"無断使用を禁じます", L"許可なく使用できません", L"無断複製を禁じます"}) {
        expect(has(chk(s, L"所有教學材料均可未經許可使用。"), "neg-missing"), "無断 / 許可なく: 「可未經許可使用」 fails", s);
        expect(chk(s, L"未經許可不得使用。").empty() || chk(s, L"禁止未經許可轉載。").empty() || chk(s, L"禁止擅自複製。").empty(),
               "無断 / 許可なく: 不得 / 禁止 passes", s);
    }
    expect(has(chk(L"値段表示のない露店で買わない", L"在沒有標價的攤位購買"), "neg-missing"), "のない…買わない: the instruction lost fails");
    expect(chk(L"値段表示のない露店で買わない", L"不要在沒有標價的攤販購買").empty(), "のない…買わない: 不要 + 沒有 passes");
    expect(has(chk(L"会員カードをお持ちでない方", L"持有會員卡的人"), "neg-missing"), "〜ない方 lost fails");
    expect(chk(L"会員カードをお持ちでない方", L"沒有會員卡的人").empty(), "〜ない方: 沒有 passes");
    expect(has(chk(L"写真撮影はご遠慮ください", L"請拍照"), "neg-missing"), "ご遠慮 lost fails");
    expect(chk(L"写真撮影はご遠慮ください", L"請勿拍照").empty(), "ご遠慮: 請勿 passes");
    expect(has(chk(L"返品不可", L"可退貨"), "neg-missing") && chk(L"返品不可", L"不可退貨").empty(), "不可 kept / lost");
    expect(has(chk(L"割引対象外", L"折扣對象"), "neg-missing") && chk(L"割引対象外", L"不適用折扣").empty(), "対象外 kept / lost");
    expect(has(chk(L"保存料を使用しない", L"使用防腐劑"), "neg-missing") && chk(L"保存料を使用しない", L"不使用防腐劑").empty(), "〜しない kept / lost");
    expect(chk(L"必ず確認しなければなりません", L"必須確認").empty(), "〜なければならない (must) needs no negation word");
    return fails;
}

// ---- --selftest: the rules of P0 (layout rows, checks, templates, escalation, cache) ----
int runSelfTest(bool download) {
    int fails = 0, n = 0;
    auto expect = [&](bool ok, const char* what, const std::wstring& got = L"") {
        ++n;
        if (!ok) ++fails;
        std::printf("  %s %s%s%s\n", ok ? "ok  " : "FAIL", what, got.empty() ? "" : "  -> ", u8(got).c_str());
    };
    auto has = [](const std::vector<std::string>& v, const char* f) { return std::find(v.begin(), v.end(), f) != v.end(); };
    fails += qeGarbageSelfTest(n);
    std::printf("checks (qe):\n");
    {
        const auto f = checkTranslation(L"直射日光、高温多湿を避けて常温で保存してください", L"存放於遠離陽光直射、高溫且潮濕的環境中", Lang::Ja, Lang::ZhHant);
        expect(has(f, "neg-scope"), "list negation scope lost (避けて -> 存放於…環境)");
        expect(checkTranslation(L"直射日光、高温多湿を避けて常温で保存してください", L"請避免陽光直射、高溫潮濕，於常溫保存。", Lang::Ja, Lang::ZhHant).empty(),
               "template translation passes");
        expect(has(checkTranslation(L"찌개류는 1인분 주문 불가", L"將魷魚分別訂購一份", Lang::Ko, Lang::ZhHant), "neg-missing"),
               "분별 / 分別 is not a negation (불가 lost)");
        expect(checkTranslation(L"찌개류는 1인분 주문 불가", L"鍋類不接受1人份點餐", Lang::Ko, Lang::ZhHant).empty(), "불가 kept");
        expect(has(checkTranslation(L"찌개류는 1인분 주문 불가", L"可單獨訂購一份無空氣的米飯", Lang::Ko, Lang::ZhHant), "neg-missing"),
               "a prohibition is not kept by 無 (absence)");
        expect(has(checkTranslation(L"脂質 1.5g", L"脂肪 1.58", Lang::Ja, Lang::ZhHant), "num-missing"), "number changed (1.5 -> 1.58)");
        expect(checkTranslation(L"2枚入り", L"兩片裝", Lang::Ja, Lang::ZhHant).empty(), "2 written as 兩");
        expect(has(checkTranslation(L"チョコレートコーチング（タイ製造）", L"泰國巧克力塗層", Lang::Ja, Lang::ZhHant), "paren-lost"),
               "bracketed note lost (タイ製造)");
        expect(has(checkTranslation(L"Do not microwave.", L"Microwave it.", Lang::En, Lang::ZhHant), "neg-missing") ||
                   has(checkTranslation(L"Do not microwave.", L"微波加熱。", Lang::En, Lang::ZhHant), "neg-missing"),
               "en -> zh: not lost");
        // 無断 / 許可なく / 禁じます: the prohibition must stay (owner 0.7.7: a site's
        // terms read 「…均可未經許可使用」).
        for (const wchar_t* s : {L"無断転載禁止", L"無断使用禁止", L"無断複製を禁じます", L"許可なく使用できません", L"無断で使用することはできません",
                                 L"無断使用を禁じます"}) {
            expect(has(checkTranslation(s, L"所有教學材料均可未經許可使用。", Lang::Ja, Lang::ZhHant), "neg-missing"),
                   "無断 + 禁: 「可未經許可使用」 is a flipped prohibition", s);
            expect(checkTranslation(s, L"未經許可不得使用。", Lang::Ja, Lang::ZhHant).empty() ||
                       checkTranslation(s, L"禁止未經許可轉載。", Lang::Ja, Lang::ZhHant).empty(),
                   "無断 + 禁: 不得 / 禁止 kept", s);
        }
        const std::wstring v = verifiedFacts(L"찌개류는 1인분 주문 불가", Lang::Ko, Lang::ZhHant);
        expect(v.find(L"不可") != std::wstring::npos && v.find(L"1人份") != std::wstring::npos, "verified facts: 불가 = 不可, 1인분 = 1人份", v);
    }
    std::printf("rules (templates, kanji, quantities):\n");
    {
        std::wstring t;
        expect(applyTemplate(L"直射日光、高温多湿を避けて常温で保存してください", Lang::Ja, Lang::ZhHant, t) && t.find(L"避免") != std::wstring::npos &&
                   t.find(L"常溫") != std::wstring::npos,
               "保存方法 template", t);
        expect(!applyTemplate(L"直射日光、謎の物質を避けて常温で保存してください", Lang::Ja, Lang::ZhHant, t), "unknown slot item: no template");
        expect(convertJapaneseKanji(L"焼菓子") == L"烘焙點心" && convertJapaneseKanji(L"脂質") == L"脂肪", "kanji words",
               convertJapaneseKanji(L"焼菓子"));
        expect(quantityPhrase(L"1袋（2枚）あたり", Lang::Ja, Lang::ZhHant, t) && t == L"每1袋（2片）", "per serving header", t);
        expect(looksLikeAddress(L"〒601-8446 京都市南区西九条") && !looksLikeAddress(L"1-800-555-0142"), "addresses");
    }
    std::printf("layout (rows):\n");
    {
        auto line = [](const wchar_t* t, float x0, float x1, float y0) {
            OcrLine l;
            l.text = t;
            l.x0 = x0, l.x1 = x1, l.y0 = y0, l.y1 = y0 + 0.03f;
            l.lineH = 0.025f;
            l.conf = 0.95f;
            l.script = detectScript(l.text);
            return l;
        };
        // 「名 称　焼菓子」 / 「内容量　8袋（16枚）」 / 賞味期限 over 26.12.09
        std::vector<OcrLine> ls{line(L"名", 0.05f, 0.08f, 0.10f), line(L"称", 0.13f, 0.16f, 0.10f), line(L"焼菓子", 0.30f, 0.42f, 0.10f),
                                line(L"内容量", 0.05f, 0.16f, 0.16f), line(L"8袋（16枚）", 0.30f, 0.50f, 0.16f)};
        const auto bs = layoutBlocks(ls, 1.f);
        std::wstring all;
        bool name = false, qty = false;
        for (const auto& b : bs) {
            all += b.text + L" | ";
            name |= b.text.substr(0, b.labelLen) == L"名称" && b.text.find(L"焼菓子") != std::wstring::npos;
            qty |= b.text.substr(0, b.labelLen) == L"内容量" && b.kind == 2;
        }
        expect(name, "spaced-out label 名 称 + value", all);
        expect(qty, "label + quantity row", all);
    }
    std::printf("cache (memory only):\n");
    {
        Engine e;
        Escalator esc(e);
        TrHypothesis h;
        h.text = L"脂肪";
        esc.remember(Lang::Ja, Lang::ZhHant, L"脂質", h);
        TrHypothesis u = h;
        u.uncertain = true;
        esc.remember(Lang::Ja, Lang::ZhHant, L"不明", u);
        TrHypothesis g;
        expect(esc.cached(Lang::Ja, Lang::ZhHant, L"脂質", g) && g.text == L"脂肪", "checked result kept");
        expect(!esc.cached(Lang::Ja, Lang::En, L"脂質", g), "keyed by target");
        expect(!esc.cached(Lang::Ja, Lang::ZhHant, L"不明", g), "doubtful result not kept");
        EscalationConfig c = esc.config();
        c.online = !c.online;
        esc.setConfig(c);
        expect(esc.cacheSize() == 0, "cleared when the engines allowed change");
    }
    // With the models: rows, a negation through the pivot, the cache.
    if (Engine::available() && ensureModels(Lang::Ja, Lang::ZhHant, download)) {
        std::printf("engine (ja -> zh-Hant):\n");
        Engine e;
        Escalator esc(e);
        const std::vector<std::wstring> in{L"内容量 8袋（16枚）", L"脂質 1.5g", L"直射日光、高温多湿な場所を避けて常温で保存してください。",
                                           L"フタを開けてお湯を注ぎ、3分待ってください。"};
        const std::vector<size_t> labels{3, 2, 0, 0};
        std::vector<std::wstring> out, out2;
        std::vector<TextInfo> info;
        std::wstring err;
        const double t0 = nowMs();
        const bool ok = translateTextsEx(e, &esc, Lang::Ja, Lang::ZhHant, in, labels, out, &info, &err);
        const double t1 = nowMs();
        expect(ok && out.size() == 4, "translated", err);
        if (ok && out.size() == 4) {
            expect(out[0] == L"內容量\x3000" L"8袋（16枚）", "row 「標籤　值」", out[0]);
            expect(out[1].find(L"脂肪") == 0 && out[1].find(L"1.5g") != std::wstring::npos, "nutrition row", out[1]);
            expect(checkTranslation(in[2], out[2], Lang::Ja, Lang::ZhHant).empty() || (info[2].uncertain && !info[2].verified.empty()),
                   "negation kept, or marked with the checked facts", out[2]);
            const size_t cached = esc.cacheSize();
            translateTextsEx(e, &esc, Lang::Ja, Lang::ZhHant, in, labels, out2, nullptr, &err);
            expect(cached > 0 && out2 == out, "second run from the cache, same text", out[3]);
            std::printf("     first %.0f ms, cached %.0f ms\n", t1 - t0, nowMs() - t1);
        }
    } else {
        std::printf("engine: skipped (bergamot.dll / ja -> zh-Hant models missing)\n");
    }
    std::printf("SELFTEST %d/%d passed\n", n - fails, n);
    return fails;
}

// ---- --overlay ----
int g_winW = 540, g_winH = 960;  // --window WxH (DIPs)
int g_layoutMode = 0;            // --layout auto|inplace|list
float g_zoom = 1;                // --zoom Z: magnified before 翻譯畫面
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
            if (g_zoom > 1) onUi([&] { win.setZoom(g_zoom); });  // --zoom: the magnified part is translated first
            onUi([&] { tr.translateScreen(); });
            if (std::getenv("PM_OVERLAY_FIRST_SHOT")) {  // the first cards on screen (dense pictures: before the result)
                const double s0 = nowMs();
                while (done < runs + 1 && nowMs() - s0 < 120000) {
                    const auto fi = win.textOverlayInfo();
                    if (fi.inPlace + fi.listed > 0) {
                        std::printf("  FIRST %ls at %.0f ms: %d in place, %d listed; overlaps %d, cut characters %d, kinsoku %d\n", n.c_str(),
                                    nowMs() - s0, fi.inPlace, fi.listed, fi.overlaps, fi.truncated, fi.kinsoku);
                        shot(n + L"_first.png");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            if (!waitDone(++runs, 120000)) {
                std::printf("  FAIL: no result within 120 s\n");
                ++fails;
                break;
            }
            {
                std::lock_guard lk(lm);
                std::printf("  %s: grab %.0f ms, OCR %.0f ms, translate %.0f ms (incl. model load), first shown %.0f ms, total %.0f ms; "
                            "%d lines, %d blocks, %d translated, source %ls\n",
                            lastOk ? "OK" : "FAILED", last.grabMs, last.ocrMs, last.translateMs,
                            last.firstMs > 0 ? last.firstMs : last.totalMs, last.totalMs, last.lines, last.blocks, last.translated,
                            langTag(last.source));
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
                // 即時翻譯 is change-driven: one run, then nothing while the
                // picture does not change (--live measures the rest).
                const int before = runs;
                const double t0 = nowMs();
                const bool one = waitDone(runs + 1, 15000);
                std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                const bool idle = done.load() - before == 1 && tr.liveStats().runs == 0;
                const bool frozen = win.viewState().frozen;
                std::printf("  live mode: %s (%d runs in %.1f s on a still picture, picture %s)\n", one && idle && !frozen ? "OK" : "FAIL",
                            done.load() - before, (nowMs() - t0) / 1000, frozen ? "FROZEN" : "not frozen");
                if (!one || !idle || frozen) ++fails;
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

// ---- --live PNG: 即時翻譯 on a synthetic stream (a viewport over a tall
// picture fed at 30 fps like a phone that sends frames all the time):
// still / scroll / still ... / a playing-video corner.  Prints per phase the
// CPU used (cores), pictures looked at, runs, and settle -> overlay times.
// cold (--live-cold PNG): 即時翻譯 turned on as the first translation action of a
// fresh ScreenTranslator, no 翻譯整個畫面 before (0.7.6 did nothing then): the
// first run must happen, or the download consent be asked when models are
// missing (answered no: nothing is downloaded; PM_MODELS_DIR = an empty folder).
int runLive(const std::wstring& png, Lang tgt, bool cold) {
    SetEnvironmentVariableW(L"PM_VIDEO_OFFSCREEN", L"1");
    std::vector<uint8_t> full;
    int fw = 0, fh = 0;
    if (!readPng(png, full, fw, fh)) {
        std::printf("cannot read %ls\n", png.c_str());
        return 1;
    }
    const int vh = std::min(fh, fw * 16 / 10);  // the viewport (a phone screen)
    pm::VideoWindow win;
    if (!win.create(L"pm_translate_test", g_winW, g_winH)) return 1;
    int fails = 0;
    std::thread t([&]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::atomic<int> done{0};
        std::mutex lm;
        std::vector<double> doneAt;
        ScreenTranslator::Callbacks cb;
        std::atomic<int> asked{0};
        cb.askDownload = [&](Lang, Lang, double mb, std::function<void(bool)> answer) {
            std::printf("  [consent] download %.0f MB? no\n", mb);
            ++asked;
            answer(false);
        };
        cb.notify = [](const std::wstring&, const std::wstring& text, bool) { std::printf("  [notify] %s\n", u8(text).c_str()); };
        cb.finished = [&](bool, const ScreenTranslator::Timing&) {
            std::lock_guard lk(lm);
            doneAt.push_back(nowMs());
            done++;
        };
        ScreenTranslator tr(win, cb);
        tr.setTarget(tgt);
        std::atomic<int> offset{0};
        std::atomic<bool> noise{false}, feeding{true};
        std::thread feeder([&] {
            std::vector<uint8_t> frame(static_cast<size_t>(fw) * vh * 4);
            uint32_t seed = 1;
            while (feeding) {
                const int off = std::clamp(offset.load(), 0, fh - vh);
                std::memcpy(frame.data(), full.data() + static_cast<size_t>(off) * fw * 4, frame.size());
                if (noise)  // a playing video in the top right corner
                    for (int y = 0; y < vh / 4; ++y)
                        for (int x = fw / 2; x < fw; ++x) {
                            seed = seed * 1664525u + 1013904223u;
                            uint8_t* p = &frame[(static_cast<size_t>(y) * fw + x) * 4];
                            p[0] = p[1] = p[2] = static_cast<uint8_t>(seed >> 24);
                        }
                win.submitBgraFrame(frame.data(), fw, vh, fw * 4, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
        });
        auto cpu = [] {
            FILETIME c, e, k, u;
            GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
            auto v = [](FILETIME f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 1e4; };
            return v(k) + v(u);
        };
        auto phase = [&](const char* name, double ms, std::function<void(double)> step) {
            const auto s0 = tr.liveStats();
            const int d0 = done;
            const double c0 = cpu(), t0 = nowMs();
            while (nowMs() - t0 < ms) {
                if (step) step(nowMs() - t0);
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
            const auto s1 = tr.liveStats();
            const double wall = nowMs() - t0;
            std::printf("  %-28s %5.0f ms: CPU %.2f cores (feeder + window included), looked %lld, hidden %lld, runs started %lld, finished %d\n",
                        name, wall, (cpu() - c0) / wall, s1.grabs - s0.grabs, s1.hidden - s0.hidden, s1.runs - s0.runs, done - d0);
        };
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (cold) {
            win.post([&] { tr.setLive(true); });  // the only action
            const double c0 = nowMs();
            while (done < 1 && asked < 1 && nowMs() - c0 < 60000) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const bool ok = done >= 1 || asked >= 1;
            std::printf("LIVE-COLD %s: %d run(s), consent asked %d time(s), live %s, after %.0f ms\n", ok ? "OK" : "FAIL", done.load(),
                        asked.load(), tr.live() ? "on" : "off", nowMs() - c0);
            if (!ok) ++fails;
            feeding = false;
            feeder.join();
            win.post([&] { tr.close(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CoUninitialize();
            win.post([&] { win.close(); });
            return;
        }
        phase("feeder only (live off)", 3000, nullptr);
        win.post([&] {
            tr.setLive(true);
            tr.translateScreen();
        });
        const double tStart = nowMs();
        while (done < 1 && nowMs() - tStart < 60000) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::printf("  first translation: %.0f ms\n", nowMs() - tStart);
        phase("still (baseline)", 3000, nullptr);
        std::vector<double> settle;
        for (int k = 0; k < 3; ++k) {
            const int from = offset, dir = k % 2 ? -1 : 1, dist = std::min(vh / 4, (fh - vh) / 2);
            phase("scroll 1 s", 1000, [&](double el) { offset = from + dir * static_cast<int>(el / 1000 * dist); });
            const auto st = tr.liveStats();
            const int d0 = done;
            phase("still after the scroll", 4000, nullptr);
            const auto st2 = tr.liveStats();
            std::lock_guard lk(lm);
            if (done > d0 && st2.runs > st.runs) settle.push_back(doneAt.back() - (st2.lastSettleMs + 300));
            else ++fails;
        }
        noise = true;
        phase("video playing (corner)", 6000, nullptr);
        noise = false;
        phase("video stopped", 4000, nullptr);
        std::sort(settle.begin(), settle.end());
        std::printf("  settle (300 ms still) -> overlay complete: ");
        for (double s : settle) std::printf("%.0f ms ", s);
        std::printf("\n");
        feeding = false;
        feeder.join();
        win.post([&] { tr.close(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CoUninitialize();
        win.post([&] { win.close(); });
    });
    win.runMessageLoop();
    t.join();
    std::printf("%d failure(s)\n", fails);
    return fails;
}

// ---- --stress N PNG...: the app's patterns, back to back (crash hunt) ----
// N rounds on an off-screen window: a random picture, a random target
// language (the language pairs and the parallel translators switch), then
// translateScreen and, at random, close() at once / after 0-1.5 s (cancel
// mid-run), a region run, or 即時翻譯 on while frames keep coming.
int runStress(int rounds, const std::vector<std::wstring>& pngs) {
    SetEnvironmentVariableW(L"PM_VIDEO_OFFSCREEN", L"1");
    std::vector<std::vector<uint8_t>> pics(pngs.size());
    std::vector<int> pw(pngs.size()), ph(pngs.size());
    for (size_t i = 0; i < pngs.size(); ++i)
        if (!readPng(pngs[i], pics[i], pw[i], ph[i])) {
            std::printf("cannot read %ls\n", pngs[i].c_str());
            return 1;
        }
    pm::VideoWindow win;
    if (!win.create(L"pm_translate_test", g_winW, g_winH)) return 1;
    int finished = 0, closedEarly = 0, regions = 0, lives = 0;
    std::thread t([&]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::atomic<int> done{0};
        ScreenTranslator::Callbacks cb;
        cb.askDownload = [](Lang, Lang, double, std::function<void(bool)> answer) { answer(false); };
        cb.notify = [](const std::wstring&, const std::wstring&, bool) {};
        cb.finished = [&](bool, const ScreenTranslator::Timing&) { done++; };
        auto tr = std::make_unique<ScreenTranslator>(win, cb);
        auto onUi = [&](std::function<void()> fn) {
            HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            win.post([&] {
                fn();
                SetEvent(ev);
            });
            WaitForSingleObject(ev, 10000);
            CloseHandle(ev);
        };
        uint32_t seed = 12345;
        auto rnd = [&](uint32_t n) {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) % n;
        };
        const Lang targets[] = {Lang::ZhHant, Lang::ZhHant, Lang::ZhHant, Lang::En};
        const double t0 = nowMs();
        for (int r = 0; r < rounds; ++r) {
            const size_t k = rnd(static_cast<uint32_t>(pics.size()));
            for (int f = 0; f < 3; ++f) {
                win.submitBgraFrame(pics[k].data(), pw[k], ph[k], pw[k] * 4, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            const Lang tgt = targets[rnd(4)];
            const int action = static_cast<int>(rnd(10));
            const int before = done;
            onUi([&] {
                tr->setTarget(tgt);
                tr->translateScreen();
            });
            if (action < 3) {  // closed mid-run
                std::this_thread::sleep_for(std::chrono::milliseconds(rnd(1500)));
                onUi([&] { tr->close(); });
                ++closedEarly;
            } else if (action < 5) {  // region run right after
                std::this_thread::sleep_for(std::chrono::milliseconds(rnd(800)));
                onUi([&] { tr->close(); });
                HWND hw = win.hwnd();
                onUi([&] { tr->translateRegion(); });
                RECT cr;
                GetClientRect(hw, &cr);
                PostMessageW(hw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cr.right / 10, cr.bottom / 4));
                PostMessageW(hw, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(cr.right * 9 / 10, cr.bottom * 3 / 4));
                PostMessageW(hw, WM_LBUTTONUP, 0, MAKELPARAM(cr.right * 9 / 10, cr.bottom * 3 / 4));
                std::this_thread::sleep_for(std::chrono::milliseconds(rnd(2000)));
                onUi([&] { tr->close(); });
                ++regions;
            } else if (action < 7) {  // 即時翻譯 with changing frames, then off
                onUi([&] { tr->setLive(true); });
                for (int f = 0; f < 30; ++f) {
                    const size_t j = f < 15 ? k : rnd(static_cast<uint32_t>(pics.size()));
                    win.submitBgraFrame(pics[j].data(), pw[j], ph[j], pw[j] * 4, 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                onUi([&] { tr->close(); });
                ++lives;
            } else {  // waits for the result
                const double w0 = nowMs();
                while (done == before && nowMs() - w0 < 60000) std::this_thread::sleep_for(std::chrono::milliseconds(20));
                onUi([&] { tr->close(); });
                ++finished;
            }
            if (rnd(25) == 0) {  // the translator itself destroyed and made again (app restart of the feature)
                onUi([&] { tr.reset(); });
                tr = std::make_unique<ScreenTranslator>(win, cb);
            }
            if ((r + 1) % 10 == 0)
                std::printf("  round %d: %.0f s, %d finished, %d closed early, %d region, %d live\n", r + 1, (nowMs() - t0) / 1000,
                            finished, closedEarly, regions, lives);
            std::fflush(stdout);
        }
        onUi([&] { tr.reset(); });
        CoUninitialize();
        win.post([&] { win.close(); });
    });
    win.runMessageLoop();
    t.join();
    std::printf("STRESS %d rounds done: %d finished, %d closed early, %d region, %d live\n", rounds, finished, closedEarly, regions, lives);
    return 0;
}

// ---- Crash reports (tests): a minidump in %TEMP%\pm_dumps and the stack on stderr ----
// Unhandled SEH exceptions, std::terminate, abort, pure virtual calls and CRT
// invalid parameters all end here (a silent exit was seen once in an eval run).
LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"pm_dumps";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[MAX_PATH];
    swprintf_s(name, L"%s\\pm_translate_test_%04d%02d%02d_%02d%02d%02d_%lu.dmp", dir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond, GetCurrentProcessId());
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    const void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    std::fprintf(stderr, "\n*** CRASH: exception 0x%08lX at %p, thread %lu; dump %ls\n", code, addr, GetCurrentThreadId(), name);
    HANDLE f = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithThreadInfo | MiniDumpWithHandleData),
                          ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(f);
    }
    // The faulting thread's stack, symbolised (the PDBs are next to the exe).
    if (ep && ep->ContextRecord) {
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, nullptr, TRUE);
        CONTEXT ctx = *ep->ContextRecord;
        STACKFRAME64 sf{};
        sf.AddrPC.Offset = ctx.Rip, sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp, sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp, sf.AddrStack.Mode = AddrModeFlat;
        for (int k = 0; k < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr,
                                              SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
             ++k) {
            char buf[sizeof(SYMBOL_INFO) + 512] = {};
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 500;
            DWORD64 disp = 0;
            IMAGEHLP_LINE64 line{sizeof(IMAGEHLP_LINE64)};
            DWORD ld = 0;
            char mod[MAX_PATH] = "?";
            if (DWORD64 base = SymGetModuleBase64(proc, sf.AddrPC.Offset))
                GetModuleFileNameA(reinterpret_cast<HMODULE>(base), mod, MAX_PATH);
            const bool hasSym = SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym);
            const bool hasLine = SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &ld, &line);
            std::fprintf(stderr, "  #%02d %s!%s +0x%llx %s:%lu\n", k, std::strrchr(mod, '\\') ? std::strrchr(mod, '\\') + 1 : mod,
                         hasSym ? sym->Name : "?", static_cast<unsigned long long>(disp), hasLine ? line.FileName : "",
                         hasLine ? line.LineNumber : 0);
        }
    }
    std::fflush(stderr);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0xC0DE0000 | (code & 0xFFFF));
    return EXCEPTION_EXECUTE_HANDLER;
}

void installCrashReports() {
    SetUnhandledExceptionFilter(crashFilter);
    auto raise = [] { RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr); };
    static void (*raiseFn)() = raise;
    std::set_terminate([] {
        std::fprintf(stderr, "\n*** std::terminate\n");
        raiseFn();
    });
    _set_purecall_handler([] {
        std::fprintf(stderr, "\n*** pure virtual call\n");
        raiseFn();
    });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        std::fprintf(stderr, "\n*** CRT invalid parameter\n");
        raiseFn();
    });
    signal(SIGABRT, [](int) {
        std::fprintf(stderr, "\n*** abort()\n");
        raiseFn();
    });
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    installCrashReports();
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
        else if ((a == L"--overlay" || a == L"--eval") && v) mode = a, outDir = argv[++i];
        else if (a == L"--window" && v) swscanf_s(argv[++i], L"%dx%d", &g_winW, &g_winH);
        else if (a == L"--layout" && v) {
            const std::wstring m = argv[++i];
            g_layoutMode = m == L"inplace" ? 1 : m == L"list" ? 2 : 0;
        } else if (a == L"--dark") g_dark = true;
        else if (a == L"--zoom" && v) g_zoom = static_cast<float>(_wtof(argv[++i]));
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
        std::printf("OCR GPU (DirectML): wanted %s, add-on %s (%.1f MB missing), running on the GPU: %s\n",
                    PaddleOcr::gpuWanted() ? "yes" : "no", ModelStore::ocrGpuInstalled() ? "installed" : "missing",
                    ModelStore::ocrGpuMissingBytes() / 1e6, PaddleOcr::gpuActive() ? "yes" : "no");
        std::printf("models dir: %ls\n", ModelStore::root().c_str());
        return 0;
    }
    if (mode == L"--sentences") return runSentences(tgt, download);
    if (mode == L"--selftest") return runSelfTest(download);
    if (mode == L"--init-race") {  // engines made side by side (each loads its translators): Bergamot / marian init races
        std::vector<std::wstring> in;
        for (int i = 0; i < 30; ++i) in.push_back(L"直射日光、高温多湿を避けて常温で保存してください。開封後はお早めにお召し上がりください。" + std::to_wstring(i));
        for (int round = 0; round < 5; ++round) {
            std::vector<std::thread> th;
            for (int k = 0; k < 4; ++k)
                th.emplace_back([&] {
                    Engine e;
                    std::vector<std::wstring> out;
                    std::wstring err;
                    e.translate(Lang::Ja, Lang::ZhHant, in, out, &err);
                });
            for (auto& x : th) x.join();
            std::printf("init race round %d ok\n", round + 1);
            std::fflush(stdout);
        }
        return 0;
    }
    if (mode == L"--stress") return files.size() < 2 ? 2 : runStress(_wtoi(files[0].c_str()), std::vector<std::wstring>(files.begin() + 1, files.end()));
    if (mode == L"--crash-test") {  // checks the crash report (a null write on a worker thread)
        std::thread([] { *static_cast<volatile int*>(nullptr) = 1; }).join();
        return 1;
    }
    if (mode == L"--live") return files.empty() ? 2 : runLive(files[0], tgt, false);
    if (mode == L"--live-cold") return files.empty() ? 2 : runLive(files[0], tgt, true);
    if (mode == L"--text" || mode == L"--raw") {
        if (files.empty()) return 2;
        if (src == Lang::Unknown) src = Lang::Ja;
        if (!ensureModels(src, tgt, download)) return 1;
        return runText(files[0], src, tgt, mode == L"--raw");
    }
    if (mode == L"--ocr" || mode == L"--pipeline" || mode == L"--eval") {
        // Each picture read on its own (PaddleOCR's line cache would take
        // lines of the picture before; PM_OCR_CACHE=1 keeps it).
        if (!std::getenv("PM_OCR_CACHE")) _putenv_s("PM_OCR_CACHE", "0");
        // OCR needs an MTA thread.
        int rc = 0;
        std::thread([&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (!ensureOcrModels(download)) {
                rc = 1;
                CoUninitialize();
                return;
            }
            if (std::getenv("PM_OCR_WARM") && !g_windowsOcr) {  // as ScreenTranslator::prewarm before the first picture
                const double w0 = nowMs();
                std::wstring e;
                const bool ok = PaddleOcr::warmUp(&e);
                std::printf("OCR warm-up %s in %.0f ms%s%s\n", ok ? "OK" : "FAILED", nowMs() - w0, ok ? "" : ": ", u8(e).c_str());
            }
            const char* ab = std::getenv("PM_OCR_AB");
            rc = mode == L"--eval"                ? runEval(outDir, files, src, tgt, download)
                 : mode == L"--ocr" && ab && *ab ? runOcrAb(files, src, ab)
                                                  : runPipeline(files, src, tgt, mode == L"--pipeline", download);
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
