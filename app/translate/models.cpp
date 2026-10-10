// 自在投影 app: translate/models.cpp — translate（放大鏡、翻譯的 UI 接線）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "translate/translate.h"

namespace pm_app {

// ---- 管理翻譯模型 ----
fs::path modelPairDir(int i) {
    if (std::string(kModelPairs[i]) == "ocr") return fs::path(pm::translate::ModelStore::root()) / L"ocr";
    return fs::path(pm::translate::ModelStore::root()) / L"bergamot" / kModelPairs[i];
}

// A menu asks for the same model folders many times (every language row, its
// shared / deletable files): each is walked once and remembered for 3 s.
// fresh: walk now (before / after deleting). UI thread.
unsigned long long dirBytes(const fs::path& d, bool fresh) {
    static std::map<std::wstring, std::pair<unsigned long long, ULONGLONG>> seen;
    const ULONGLONG now = GetTickCount64();
    if (auto it = seen.find(d.wstring()); !fresh && it != seen.end() && now - it->second.second < 3000)
        return it->second.first;
    unsigned long long n = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(d, ec))
        if (e.is_regular_file(ec)) n += e.file_size(ec);
    seen[d.wstring()] = {n, now};
    return n;
}

std::wstring mbText(unsigned long long b) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f MB", b / 1e6);
    return buf;
}

// 「日文 → 英文」 for "ja-en".
std::wstring pairLabel(int i) {
    using L = pm::translate::Lang;
    auto lang = [](const std::string& c) {
        return c == "ja" ? L::Ja : c == "ko" ? L::Ko : c == "en" ? L::En : c == "zhHans" ? L::ZhHans : L::ZhHant;
    };
    const std::string p = kModelPairs[i];
    if (p == "ocr") return tr(S::TrOcrModelLabel);
    const size_t dash = p.find('-');
    return pm::translate::langName(lang(p.substr(0, dash))) + L" → " + pm::translate::langName(lang(p.substr(dash + 1)));
}

void deleteModels(std::vector<int> pairs) {
    if (g.translator) g.translator->close();
    returnToLive("closed (models deleted)");
    pm::translate::PaddleOcr::unload();  // the OCR model files are not kept open, but free them anyway
    unsigned long long freed = 0;
    bool failed = false;
    for (int i : pairs) {
        const fs::path d = modelPairDir(i);
        const unsigned long long before = dirBytes(d, true);
        std::error_code ec;
        fs::remove_all(d, ec);
        if (ec || fs::exists(d)) failed = true;
        freed += before - dirBytes(d, true);
        g.log->write("info", std::string("translation model ") + kModelPairs[i] + (ec ? " delete failed: " + ec.message() : " deleted"));
    }
    g.window->showToast(failed ? std::wstring(tr(S::TrDeleteFailed)) : fmt(S::TrDeleted, {mbText(freed)}), 4000);
}

void askDeleteModels(int pair) {  // -1: all
    std::vector<int> pairs;
    for (int i = 0; i < kModelPairCount; ++i)
        if ((pair < 0 || pair == i) && dirBytes(modelPairDir(i)) > 0) pairs.push_back(i);
    askDeleteModelSet(std::move(pairs), pair < 0 ? std::wstring() : pairLabel(pair), {});
}

int modelPairIndex(const std::string& pair) {
    for (int i = 0; i < kModelPairCount; ++i)
        if (pair == kModelPairs[i]) return i;
    return -1;
}

// kModelPairs indices that source `src` needs for the current target (empty:
// none needed, or not supported: *supported false).
std::vector<int> sourcePairs(pm::translate::Lang src, bool* supported) {
    bool ok = true;
    std::vector<int> v;
    for (const std::string& p : pm::translate::ModelStore::pairsFor(src, translateTarget(), &ok)) {
        const int i = modelPairIndex(p);
        if (i >= 0) v.push_back(i);
        else ok = false;
    }
    if (supported) *supported = ok;
    return v;
}

bool sourceDownloaded(pm::translate::Lang src) {
    bool ok = false;
    const std::vector<int> v = sourcePairs(src, &ok);
    if (!ok || v.empty()) return false;
    for (int i : v)
        if (!pm::translate::ModelStore::installed(kModelPairs[i])) return false;
    return true;
}

// Pairs deleting language `src` frees: its own, minus those another
// downloaded language (same target) still needs (en-zhHant for 韓文 too).
std::vector<int> sourceDeletablePairs(int source) {
    std::vector<int> keep;
    for (int k = 0; k < 5; ++k)
        if (k != source && kModelSources[k] != translateTarget() && sourceDownloaded(kModelSources[k]))
            for (int i : sourcePairs(kModelSources[k])) keep.push_back(i);
    std::vector<int> v;
    for (int i : sourcePairs(kModelSources[source]))
        if (std::find(keep.begin(), keep.end(), i) == keep.end() && dirBytes(modelPairDir(i)) > 0) v.push_back(i);
    return v;
}

std::wstring modelSourceLabel(int source) {
    return pm::translate::langName(kModelSources[source]) + L" → " + pm::translate::langName(translateTarget());
}
SharedModels sourceSharedModels(int source) {
    SharedModels r;
    if (!sourceDeletablePairs(source).empty()) return r;
    for (int i : sourcePairs(kModelSources[source]))
        if (dirBytes(modelPairDir(i)) > 0) r.pairs.push_back(i);
    if (r.pairs.empty()) return r;
    const bool spaced = pm::i18n::en() || pm::i18n::lang() == pm::i18n::Lang::Ko;
    for (int k = 0; k < 5; ++k) {
        if (k == source || kModelSources[k] == translateTarget() || !sourceDownloaded(kModelSources[k])) continue;
        const std::vector<int> theirs = sourcePairs(kModelSources[k]);
        bool uses = false;
        for (int i : r.pairs) uses |= std::find(theirs.begin(), theirs.end(), i) != theirs.end();
        if (!uses) continue;
        if (!r.users.empty()) r.users += spaced ? L", " : L"、";
        r.users += pm::translate::langName(kModelSources[k]);
    }
    if (r.users.empty()) r.pairs.clear();
    return r;
}

void askDeleteModelSet(std::vector<int> pairs, const std::wstring& label, const std::wstring& body) {  // label empty: 刪除全部
    unsigned long long bytes = 0;
    for (int i : pairs) bytes += dirBytes(modelPairDir(i));
    if (pairs.empty()) return;
    const int pair = label.empty() ? -1 : 0;
    if (IsWindowVisible(g.hwnd) == FALSE) bringToFront();
    pm::ui::AskPanel::Info a;
    a.glyph = 0xE74D;  // Delete
    a.title = tr(S::TrDeleteTitle);
    a.body = !body.empty() ? body
             : pair < 0    ? fmt(S::TrDeleteAllAsk, {mbText(bytes)})
                           : fmt(S::TrDeleteAsk, {label, mbText(bytes)});
    a.primary = tr(S::TrDeleteBtn);
    a.secondary = tr(S::Cancel);
    a.danger = true;
    a.done = [pairs](int choice) {
        g.log->write("info", std::string("delete translation models: ") + (choice == 1 ? "confirmed" : "cancelled"));
        if (choice == 1) deleteModels(pairs);
    };
    g.askPanel.open(g.hwnd, std::move(a));
}

void openModelsFolder() {
    const fs::path d = fs::path(pm::translate::ModelStore::root());
    std::error_code ec;
    fs::create_directories(d, ec);
    g.log->write("info", "models folder " + toUtf8(d.wstring()));
    if (!g.testOffscreen) ShellExecuteW(g.hwnd, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace pm_app
