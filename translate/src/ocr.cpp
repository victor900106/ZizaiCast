// Windows.Media.Ocr wrapper: installed recognisers, recognition with
// automatic recogniser choice, words joined into lines with boxes.
#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "pm/translate.h"
#include "text_util.h"

namespace pm::translate {

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;

namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// BCP-47 tag of an installed recogniser for l ("" if none).
std::wstring recognizerTag(Lang l) {
    try {
        for (const auto& lang : OcrEngine::AvailableRecognizerLanguages()) {
            const std::wstring t{lang.LanguageTag()};
            auto starts = [&](const wchar_t* p) { return _wcsnicmp(t.c_str(), p, wcslen(p)) == 0; };
            switch (l) {
            case Lang::Ja: if (starts(L"ja")) return t; break;
            case Lang::Ko: if (starts(L"ko")) return t; break;
            case Lang::En: if (starts(L"en")) return t; break;
            case Lang::ZhHans: if (starts(L"zh-Hans") || starts(L"zh-CN") || starts(L"zh-SG")) return t; break;
            case Lang::ZhHant: if (starts(L"zh-Hant") || starts(L"zh-TW") || starts(L"zh-HK") || starts(L"zh-MO")) return t; break;
            default: break;
            }
        }
    } catch (...) {
    }
    return {};
}

// Bilinear resample of BGRA (stride w*4).
std::vector<uint8_t> resample(const uint8_t* src, int w, int h, int nw, int nh) {
    std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * 4);
    const float sx = static_cast<float>(w) / nw, sy = static_cast<float>(h) / nh;
    for (int y = 0; y < nh; ++y) {
        const float fy = std::clamp((y + 0.5f) * sy - 0.5f, 0.f, h - 1.f);
        const int y0 = static_cast<int>(fy), y1 = std::min(y0 + 1, h - 1);
        const float ty = fy - y0;
        for (int x = 0; x < nw; ++x) {
            const float fx = std::clamp((x + 0.5f) * sx - 0.5f, 0.f, w - 1.f);
            const int x0 = static_cast<int>(fx), x1 = std::min(x0 + 1, w - 1);
            const float tx = fx - x0;
            const uint8_t* a = src + (static_cast<size_t>(y0) * w + x0) * 4;
            const uint8_t* b = src + (static_cast<size_t>(y0) * w + x1) * 4;
            const uint8_t* c = src + (static_cast<size_t>(y1) * w + x0) * 4;
            const uint8_t* d = src + (static_cast<size_t>(y1) * w + x1) * 4;
            uint8_t* o = out.data() + (static_cast<size_t>(y) * nw + x) * 4;
            for (int k = 0; k < 4; ++k)
                o[k] = static_cast<uint8_t>(std::lround((a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty));
        }
    }
    return out;
}

// Score of a result for recogniser l: characters of its own script.
double scoreFor(Lang l, const std::vector<OcrLine>& lines) {
    ScriptCount total;
    for (const auto& ln : lines) total += countScripts(ln.text);
    switch (l) {
    case Lang::Ja: {
        // Kanji alone are as much Chinese: kana make it Japanese.
        const bool kana = total.kana >= 3 && total.kana * 20 >= total.kana + total.han;
        return kana ? total.kana * 1.5 + total.han : total.han * 0.5;
    }
    case Lang::Ko: return total.hangul * 1.5 + total.han * 0.2;
    case Lang::ZhHans:
    case Lang::ZhHant: return total.han * 1.0 - total.kana - total.hangul;
    case Lang::En: return total.latin * 0.5;
    default: return 0;
    }
}

bool runEngine(const std::wstring& tag, const SoftwareBitmap& bmp, float scaleX, float scaleY, int w, int h,
               std::vector<OcrLine>& lines) {
    OcrEngine engine = OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(tag));
    if (!engine) return false;
    auto result = engine.RecognizeAsync(bmp).get();
    lines.clear();
    for (const auto& line : result.Lines()) {
        OcrLine ol{};
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        std::wstring text;
        wchar_t last = 0;
        for (const auto& word : line.Words()) {
            const std::wstring wt{word.Text()};
            if (wt.empty()) continue;
            const auto r = word.BoundingRect();
            x0 = std::min(x0, r.X);
            y0 = std::min(y0, r.Y);
            x1 = std::max(x1, r.X + r.Width);
            y1 = std::max(y1, r.Y + r.Height);
            // Spaces only between two non-CJK characters (Latin words, digits).
            if (!text.empty() && !isCjk(last) && !isCjk(wt.front())) text += L' ';
            text += wt;
            last = wt.back();
        }
        if (text.empty() || x1 <= x0 || y1 <= y0) continue;
        ol.text = text;
        ol.x0 = std::clamp(x0 / scaleX / w, 0.f, 1.f);
        ol.y0 = std::clamp(y0 / scaleY / h, 0.f, 1.f);
        ol.x1 = std::clamp(x1 / scaleX / w, 0.f, 1.f);
        ol.y1 = std::clamp(y1 / scaleY / h, 0.f, 1.f);
        ol.script = detectScript(text);
        lines.push_back(std::move(ol));
    }
    return true;
}

}  // namespace

std::vector<Lang> Ocr::installed() {
    std::vector<Lang> out;
    for (Lang l : {Lang::Ja, Lang::Ko, Lang::ZhHant, Lang::ZhHans, Lang::En})
        if (!recognizerTag(l).empty()) out.push_back(l);
    return out;
}

bool Ocr::available(Lang l) { return !recognizerTag(l).empty(); }

bool Ocr::recognize(const uint8_t* bgra, int width, int height, Lang lang, OcrResult& out, std::wstring* err) {
    out = {};
    const double t0 = nowMs();
    if (!bgra || width <= 0 || height <= 0) {
        if (err) *err = L"empty picture";
        return false;
    }
    try {
        std::vector<Lang> engines;
        if (lang != Lang::Unknown) {
            if (!available(lang)) {
                if (err) *err = L"no OCR recogniser for " + std::wstring(langTag(lang));
                return false;
            }
            engines.push_back(lang);
        } else {
            engines = installed();
            if (engines.empty()) {
                if (err) *err = L"no OCR recogniser installed";
                return false;
            }
        }
        // Size for the recogniser: at most MaxImageDimension, small crops x2.
        const int maxDim = static_cast<int>(OcrEngine::MaxImageDimension());
        float scale = 1;
        if (std::max(width, height) > maxDim) scale = static_cast<float>(maxDim) / std::max(width, height);
        else if (std::max(width, height) < 1000) scale = std::min(2.f, static_cast<float>(maxDim) / std::max(width, height));
        const int nw = std::max(1, static_cast<int>(std::lround(width * scale)));
        const int nh = std::max(1, static_cast<int>(std::lround(height * scale)));
        std::vector<uint8_t> scaled;
        const uint8_t* px = bgra;
        if (nw != width || nh != height) {
            scaled = resample(bgra, width, height, nw, nh);
            px = scaled.data();
        }
        winrt::Windows::Storage::Streams::Buffer buf(static_cast<uint32_t>(static_cast<size_t>(nw) * nh * 4));
        std::memcpy(buf.data(), px, static_cast<size_t>(nw) * nh * 4);
        buf.Length(buf.Capacity());
        SoftwareBitmap bmp = SoftwareBitmap::CreateCopyFromBuffer(buf, BitmapPixelFormat::Bgra8, nw, nh,
                                                                  BitmapAlphaMode::Premultiplied);
        const float sx = static_cast<float>(nw) / width, sy = static_cast<float>(nh) / height;
        double best = -1e18;
        for (Lang l : engines) {
            std::vector<OcrLine> lines;
            if (!runEngine(recognizerTag(l), bmp, sx, sy, width, height, lines)) continue;
            const double sc = engines.size() == 1 ? 0 : scoreFor(l, lines);
            if (sc > best) {
                best = sc;
                out.lines = std::move(lines);
                out.engine = l;
            }
        }
        out.ms = nowMs() - t0;
        if (out.engine == Lang::Unknown) {
            if (err) *err = L"OCR failed";
            return false;
        }
        return true;
    } catch (const winrt::hresult_error& e) {
        if (err) *err = L"OCR error: " + std::wstring(e.message());
    } catch (...) {
        if (err) *err = L"OCR error";
    }
    return false;
}

}  // namespace pm::translate
