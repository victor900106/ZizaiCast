// Bergamot translation engine (bergamot.dll, built by tools/build_bergamot.sh:
// MPL-2.0 bergamot-translator + marian, no BLAS) loaded at run time.
#include <windows.h>

#include <chrono>
#include <map>

#include "pm/translate.h"
#include "text_util.h"

namespace pm::translate {

namespace {

using InitFn = void* (*)(const char** configPaths, int numPaths);
using TranslateMultipleFn = char** (*)(void* translator, const char** texts, size_t count, bool html);
using FreeTranslationsFn = void (*)(char** translations);
using FreeFn = void (*)(void* translator);

struct Dll {
    HMODULE mod = nullptr;
    InitFn init = nullptr;
    TranslateMultipleFn translate = nullptr;
    FreeTranslationsFn freeTranslations = nullptr;
    FreeFn freeTranslator = nullptr;
    std::wstring error;
};

std::wstring dllPath() {
    wchar_t env[MAX_PATH];
    if (DWORD n = GetEnvironmentVariableW(L"PM_BERGAMOT_DLL", env, MAX_PATH); n > 0 && n < MAX_PATH) return env;
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    return dir + L"bergamot.dll";
}

// Loaded once per process, kept (marian keeps global state).
Dll& dll() {
    static Dll d = [] {
        Dll x;
        const std::wstring path = dllPath();
        x.mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!x.mod) {
            x.error = L"cannot load " + path + L" (error " + std::to_wstring(GetLastError()) + L")";
            return x;
        }
        x.init = reinterpret_cast<InitFn>(GetProcAddress(x.mod, "translator_initialize"));
        x.translate = reinterpret_cast<TranslateMultipleFn>(GetProcAddress(x.mod, "translator_translate_multiple"));
        x.freeTranslations = reinterpret_cast<FreeTranslationsFn>(GetProcAddress(x.mod, "translator_free_translations"));
        x.freeTranslator = reinterpret_cast<FreeFn>(GetProcAddress(x.mod, "translator_free"));
        if (!x.init || !x.translate || !x.freeTranslations || !x.freeTranslator) {
            x.error = path + L": not a Bergamot engine";
            FreeLibrary(x.mod);
            x.mod = nullptr;
        }
        return x;
    }();
    return d;
}

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

struct Engine::Impl {
    // Loaded translators by model chain ("ja-en+en-zhHant"), most recent last.
    std::vector<std::pair<std::string, void*>> loaded;
    ~Impl() {
        for (auto& [k, t] : loaded)
            if (t && dll().freeTranslator) dll().freeTranslator(t);
    }
    void* get(const std::vector<std::string>& pairs, double& loadMs, std::wstring* err) {
        std::string key;
        for (const auto& p : pairs) key += (key.empty() ? "" : "+") + p;
        for (size_t i = 0; i < loaded.size(); ++i)
            if (loaded[i].first == key) {
                auto e = loaded[i];
                loaded.erase(loaded.begin() + i);
                loaded.push_back(e);
                return e.second;
            }
        std::vector<std::string> cfg;
        for (const auto& p : pairs) {
            const std::wstring c = ModelStore::configPath(p);
            if (c.empty() || !ModelStore::installed(p)) {
                if (err) *err = L"model " + fromUtf8(p) + L" is not installed";
                return nullptr;
            }
            cfg.push_back(toUtf8(c));
        }
        std::vector<const char*> argv;
        for (const auto& c : cfg) argv.push_back(c.c_str());
        const double t0 = nowMs();
        void* t = dll().init(argv.data(), static_cast<int>(argv.size()));  // 1 model, or 2 = pivot
        loadMs = nowMs() - t0;
        if (!t) {
            if (err) *err = L"cannot load the translation model " + fromUtf8(key);
            return nullptr;
        }
        // At most two chains in memory (~50-100 MB each).
        while (loaded.size() >= 2) {
            dll().freeTranslator(loaded.front().second);
            loaded.erase(loaded.begin());
        }
        loaded.emplace_back(key, t);
        return t;
    }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

bool Engine::available(std::wstring* err) {
    const Dll& d = dll();
    if (!d.mod && err) *err = d.error;
    return d.mod != nullptr;
}

bool Engine::translate(Lang src, Lang tgt, const std::vector<std::wstring>& in, std::vector<std::wstring>& out,
                       std::wstring* err) {
    out.clear();
    loadMs_ = 0;
    if (in.empty()) return true;
    if (src == tgt) {
        out = in;
        return true;
    }
    if (src == Lang::ZhHans && tgt == Lang::ZhHant) {  // characters only
        for (const auto& s : in) out.push_back(toTraditional(s));
        return true;
    }
    bool supported = true;
    const auto pairs = ModelStore::pairsFor(src, tgt, &supported);
    if (!supported || pairs.empty()) {
        if (err) *err = L"unsupported language pair";
        return false;
    }
    if (!available(err)) return false;
    void* t = impl_->get(pairs, loadMs_, err);
    if (!t) return false;
    std::vector<std::string> u8;
    u8.reserve(in.size());
    for (const auto& s : in) u8.push_back(toUtf8(s));
    std::vector<const char*> argv;
    for (const auto& s : u8) argv.push_back(s.c_str());
    char** res = dll().translate(t, argv.data(), argv.size(), false);
    if (!res) {
        if (err) *err = L"translation failed";
        return false;
    }
    for (size_t i = 0; i < in.size(); ++i) {
        std::wstring s = res[i] ? fromUtf8(res[i]) : std::wstring();
        if (tgt == Lang::ZhHant) s = fullWidthPunctuation(toTraditional(s));
        if (tgt == Lang::Ja) {  // 日本語: ？！（）： next to kana / kanji, 、 rather than ，
            s = fullWidthPunctuation(s);
            for (wchar_t& c : s)
                if (c == 0xFF0C) c = 0x3001;
        }
        out.push_back(std::move(s));
    }
    dll().freeTranslations(res);
    return true;
}

}  // namespace pm::translate
