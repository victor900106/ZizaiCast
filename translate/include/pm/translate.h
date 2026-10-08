// pm_translate: on-screen translation of the mirrored phone picture.
//
//   picture (VideoWindow::grabPicture) -> OCR (PaddleOCR PP-OCRv6 via ONNX
//   Runtime, any language without Windows language packs; Windows.Media.Ocr
//   as the fallback) -> lines grouped into blocks -> offline machine translation
//   (Mozilla Firefox Translations / Bergamot models, bergamot.dll) ->
//   VideoWindow::setTextOverlay (translated cards over the original text).
//
// Everything runs on this PC: the OCR and translation models are downloaded
// once (with the user's consent, SHA-256 verified) into
// %LOCALAPPDATA%\PhoneMirror\models and never send text anywhere.
//
// Most apps only need ScreenTranslator (the whole 「翻譯畫面」 flow for one
// VideoWindow).  See docs/translate.md.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pm {
class VideoWindow;
}

namespace pm::translate {

enum class Lang { Unknown, Ja, Ko, En, ZhHans, ZhHant };
const wchar_t* langTag(Lang l);  // "ja", "ko", "en", "zh-Hans", "zh-Hant" ("" for Unknown)
std::wstring langName(Lang l);   // in the UI language: 日文 / Japanese …
// The translation target that matches the UI language (繁體中文: ZhHant, English: En, 日本語: Ja, 한국어: Ko).
Lang defaultTarget();

// ---- OCR (PaddleOCR via ONNX Runtime; Windows.Media.Ocr fallback) ----
struct OcrLine {
    std::wstring text;            // words joined (no spaces between CJK characters)
    float x0, y0, x1, y1;         // 0..1 of the picture (box around the possibly tilted line)
    Lang script = Lang::Unknown;  // by the characters (kana: Ja, hangul: Ko, Han: Zh*, Latin: En)
    float conf = 1;               // recogniser confidence 0..1 (Windows OCR: 1)
    float angle = 0;              // baseline angle in picture pixels (radians, + = descending to the right)
    float lineH = 0;              // glyph height across the line, 0..1 of the picture height (0: y1 - y0)
    bool vertical = false;        // a top-to-bottom column (Japanese vertical writing)
    float bg = -1;                // mean brightness 0..1 around the text (-1: unknown); light text on
                                  // a dark box (a label's black banner) is never merged with dark text
};
enum class OcrBackend { None, Paddle, Windows };
struct OcrResult {
    std::vector<OcrLine> lines;
    Lang engine = Lang::Unknown;  // Windows OCR: the recogniser that produced them
    OcrBackend backend = OcrBackend::None;
    double ms = 0;                // recognition time (all recognisers tried)
    double detMs = 0, recMs = 0;  // PaddleOCR: detection / recognition (incl. Korean) parts
    int koLines = 0;              // PaddleOCR: lines read by the Korean recogniser
};

// PaddleOCR (PP-OCRv6 detection + recognition for Chinese / Japanese /
// English / 50 languages, PP-OCRv5 Korean recognition), run by onnxruntime.dll
// next to the executable.  Needs the "ocr" models (ModelStore, ~37 MB).
class PaddleOcr {
public:
    // onnxruntime.dll loadable (PM_ONNXRUNTIME_DLL overrides its path, for tests).
    static bool runtimeAvailable(std::wstring* err = nullptr);
    static bool modelsInstalled();
    // runtime + models.
    static bool ready() { return runtimeAvailable() && modelsInstalled(); }
    // Lines of bgra (stride width*4), reading order.  Blocking, any thread
    // (one recognition at a time).  Loads the models on first use.
    static bool recognize(const uint8_t* bgra, int width, int height, OcrResult& out, std::wstring* err = nullptr);
    // Frees the models (e.g. after they were deleted).
    static void unload();
};

class Ocr {
public:
    // Recognisers installed on this PC among Ja / Ko / En / ZhHans / ZhHant
    // (Windows "Optical character recognition" language features).
    static std::vector<Lang> installed();
    static bool available(Lang l);
    // Recognises bgra (stride width*4).  lang Unknown: every installed
    // recogniser is tried and the one whose own script dominates its result
    // wins (e.g. kana for Japanese).  Large pictures are scaled to the
    // engine's maximum, small ones (< 1000 px) up x2.  Call from a
    // multi-threaded-apartment thread (not a UI thread).  False + err on failure.
    static bool recognize(const uint8_t* bgra, int width, int height, Lang lang, OcrResult& out,
                          std::wstring* err = nullptr);
};

// Script of a text (Unknown: digits / punctuation only).
Lang detectScript(const std::wstring& text);
// Simplified <-> Traditional Chinese characters (Windows LCMapStringEx).
std::wstring toTraditional(const std::wstring& s);

// ---- Lines -> blocks (paragraphs / UI labels) ----
struct Block {
    std::wstring text;           // lines joined (CJK without spaces)
    float x0, y0, x1, y1;        // union of the lines (0..1)
    float cy0 = 0, cy1 = 0;      // the glyph band (y) for the card: tilted lines' boxes are taller
    Lang lang = Lang::Unknown;   // detectScript of the whole block
    int lines = 0;
    float conf = 1;              // lowest recogniser confidence of its lines
    float lineH = 0;             // largest glyph height of its lines (0..1 of the picture height)
};
// Consecutive lines of one paragraph (similar height, small gap, overlapping
// horizontally, previous line not ending a sentence) become one block.
// aspect = picture width / height.
std::vector<Block> groupLines(const std::vector<OcrLine>& lines, float aspect);

// ---- Models (Firefox Translations, MPL-2.0) ----
class ModelStore {
public:
    // %LOCALAPPDATA%\PhoneMirror\models (PM_MODELS_DIR overrides, for tests).
    static std::wstring root();
    // Model pairs ("ja-en", "en-zhHant", …) needed for src -> tgt (pivot via
    // English); empty when no model is needed (same language, zh-Hans ->
    // zh-Hant) or the pair is not supported (*supported = false).
    static std::vector<std::string> pairsFor(Lang src, Lang tgt, bool* supported = nullptr);
    static bool installed(const std::string& pair);
    // Bytes still to download for these pairs.
    static uint64_t missingBytes(const std::vector<std::string>& pairs);
    // Downloads the missing files over HTTPS (Mozilla's CDN), verifies each
    // SHA-256, then moves them into place.  progress(0..1) from this thread.
    // Blocking; cancel may be set from any thread.
    static bool download(const std::vector<std::string>& pairs, const std::function<void(double)>& progress,
                         const std::atomic<bool>* cancel, std::wstring* err);
    // Bergamot configuration file of an installed pair (written on demand).
    static std::wstring configPath(const std::string& pair);
    // OCR models: pair "ocr" (PaddleOCR, models\ocr\).  Path of an installed
    // (verified) file of it, "" if missing.
    static std::wstring ocrFile(const char* name);
};

// ---- Translation engine (Bergamot, bergamot.dll next to the executable) ----
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    // bergamot.dll loadable (PM_BERGAMOT_DLL overrides its path, for tests).
    static bool available(std::wstring* err = nullptr);
    // Translates texts src -> tgt; the models (ModelStore::pairsFor) must be
    // installed.  Loads (and keeps, at most two) translators on first use.
    // Not thread-safe (one caller thread).  out has one entry per input.
    bool translate(Lang src, Lang tgt, const std::vector<std::wstring>& in, std::vector<std::wstring>& out,
                   std::wstring* err = nullptr);
    double lastLoadMs() const { return loadMs_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    double loadMs_ = 0;
};

// ---- The 「翻譯畫面」 flow on a VideoWindow ----
// translateScreen(): freezes the picture, OCRs it, translates every block
// into target() and shows the cards (setTextOverlay) with a 「正在辨識文字…」
// / 「正在翻譯…」 busy card meanwhile.  translateRegion(): the same for a
// rectangle the user drags (beginRegionSelect).  First use of a language pair
// asks to download its model (callbacks.askDownload); a missing OCR language
// shows how to add it (callbacks.notify).  All public methods: UI thread.
class ScreenTranslator {
public:
    struct Timing {
        double grabMs = 0, ocrMs = 0, translateMs = 0, totalMs = 0;
        int lines = 0, blocks = 0, translated = 0;
        Lang source = Lang::Unknown;  // dominant language of the blocks
    };
    struct Callbacks {
        // Ask whether to download `megabytes` of models for src -> tgt; call
        // answer(yes) once (any thread).  Default: a Yes/No message box
        // owned by the video window (TrModelAsk).
        std::function<void(Lang src, Lang tgt, double megabytes, std::function<void(bool)> answer)> askDownload;
        // Messages: important = needs reading (a message box by default:
        // missing OCR language, engine missing), else a toast on the picture.
        std::function<void(const std::wstring& title, const std::wstring& text, bool important)> notify;
        // State changed (active / original / live / busy): refresh menus.
        std::function<void()> changed;
        // A run finished (tests, logs); UI thread.
        std::function<void(bool ok, const Timing& t)> finished;
        // Test hook: replaces Ocr::recognize (the picture or the crop of
        // region x0 y0 x1 y1 of it; lines in crop coordinates) - e.g.
        // ground-truth boxes when a recogniser is not installed.  Worker thread.
        std::function<bool(const uint8_t* bgra, int w, int h, const float region[4], OcrResult& out)> ocrOverride;
    };
    ScreenTranslator(VideoWindow& win, Callbacks cb = {});
    ~ScreenTranslator();
    ScreenTranslator(const ScreenTranslator&) = delete;
    ScreenTranslator& operator=(const ScreenTranslator&) = delete;

    void setTarget(Lang tgt);  // ZhHant, En, Ja or Ko (default: the UI language, defaultTarget())
    Lang target() const;
    void setSource(Lang src);  // Unknown (default): automatic
    Lang source() const;
    void translateScreen();
    void translateRegion();
    // Show the picture's own text (translations hidden, their areas outlined).
    void setShowOriginal(bool on);
    bool showOriginal() const;
    // Live mode: re-translate the live (unfrozen) picture every `seconds`
    // (2..60) until close(); off by default.
    void setLive(bool on, int seconds = 5);
    bool live() const;
    // Overlay on screen (or a run in progress).
    bool active() const;
    bool busy() const;
    // Removes the overlay, ends live mode and unfreezes (if we froze).
    void close();
    Timing lastTiming() const;
    // The blocks of the last run (original + translation, content coords).
    struct Item {
        std::wstring original, translated;
        Lang lang;
        float x0, y0, x1, y1;
    };
    std::vector<Item> lastItems() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace pm::translate
