// Bergamot translation engine (bergamot.dll, built by tools/build_bergamot.sh:
// MPL-2.0 bergamot-translator + marian, no BLAS) loaded at run time.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

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

// Bergamot's translator_initialize / translator_free are not thread-safe:
// marian and its spdlog loggers are process globals ("logger with name
// already exists" thrown when two translators are made at once - a C++
// exception out of the DLL: std::terminate, the app gone without a word;
// found by pm_translate_test --init-race).  Every init / free goes through
// this lock, one at a time; translate() calls on different translators run
// side by side (each has its own model and graph).
std::mutex& initLock() {
    static std::mutex m;
    return m;
}
// The DLL's C functions may still throw (a C++ exception through the C API):
// caught here, the call counts as failed.
void* safeInit(const char** cfg, int n) {
    std::lock_guard lk(initLock());
    try {
        return dll().init(cfg, n);
    } catch (...) {
        return nullptr;
    }
}
void safeFree(void* t) {
    if (!t || !dll().freeTranslator) return;
    std::lock_guard lk(initLock());
    try {
        dll().freeTranslator(t);
    } catch (...) {
    }
}
char** safeTranslate(void* t, const char** texts, size_t n) {
    try {
        return dll().translate(t, texts, n, false);
    } catch (...) {
        return nullptr;
    }
}

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

struct Engine::Impl {
    // Loaded translators by model chain ("ja-en+en-zhHant"), most recent last.
    std::vector<std::pair<std::string, void*>> loaded;
    // More translators of the current chain for dense pictures (Bergamot's
    // BlockingService is one thread per translator): the texts are split
    // between them and translated side by side.
    // Kept for the chains in `loaded` (at most two), freed with them.
    std::map<std::string, std::vector<void*>> helpersOf;
    ~Impl() {
        for (auto& [k, t] : loaded)
            safeFree(t);
        for (auto& [k, v] : helpersOf) dropHelpers(k);
    }
    void dropHelpers(const std::string& key) {
        auto it = helpersOf.find(key);
        if (it == helpersOf.end()) return;
        for (void* t : it->second)
            safeFree(t);
        it->second.clear();
    }
    // n more translators of the chain (loaded once, kept while the chain is).
    std::vector<void*> more(const std::vector<std::string>& pairs, size_t n, double& loadMs) {
        std::string key;
        for (const auto& p : pairs) key += (key.empty() ? "" : "+") + p;
        std::vector<void*>& helpers = helpersOf[key];
        std::vector<std::string> cfg;
        for (const auto& p : pairs) cfg.push_back(toUtf8(ModelStore::configPath(p)));
        std::vector<const char*> argv;
        for (const auto& c : cfg) argv.push_back(c.c_str());
        const double t0 = nowMs();
        if (helpers.size() < n) {  // loaded side by side
            std::vector<void*> got(n - helpers.size(), nullptr);
            // One at a time (see initLock): ~0.3 s each.
            for (size_t k = 0; k < got.size(); ++k) got[k] = safeInit(argv.data(), static_cast<int>(argv.size()));
            for (void* t : got)
                if (t) helpers.push_back(t);
        }
        loadMs += nowMs() - t0;
        return std::vector<void*>(helpers.begin(), helpers.begin() + std::min(n, helpers.size()));
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
        void* t = safeInit(argv.data(), static_cast<int>(argv.size()));  // 1 model, or 2 = pivot
        loadMs = nowMs() - t0;
        if (!t) {
            if (err) *err = L"cannot load the translation model " + fromUtf8(key);
            return nullptr;
        }
        // At most two chains in memory (~50-100 MB each).
        while (loaded.size() >= 2) {
            safeFree(loaded.front().second);
            dropHelpers(loaded.front().first);
            helpersOf.erase(loaded.front().first);
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
    size_t chars = 0;
    for (const auto& s : in) u8.push_back(toUtf8(s)), chars += s.size();
    std::vector<const char*> argv;
    for (const auto& s : u8) argv.push_back(s.c_str());
    // Dense pictures (many texts): split between parallel translators,
    // balanced by length (measured: a Wikipedia page 8.6 s with one).
    // 2026-10: 4 or 6 (OCR on the GPU leaves the cores free) were tried on the
    // 8 dense eval_web pages: no faster (sum of first-pass times 5.8 / 5.8 /
    // 6.2 s for 3 / 4 / 6) and 30 blocks translated differently (another
    // split, another batch for Marian int8) - kept at 3.
    static const size_t kWorkers = [] {
        if (const char* e = std::getenv("PM_TR_WORKERS"); e && atoi(e) > 0) return static_cast<size_t>(std::min(8, atoi(e)));
        return static_cast<size_t>(std::clamp(std::thread::hardware_concurrency() / 6, 1u, 3u));
    }();
    std::vector<const char*> tx(in.size(), nullptr);
    std::vector<char**> results;
    std::vector<void*> ts{t};
    if (kWorkers > 1 && in.size() >= 12 && chars >= 400) {
        const auto extra = impl_->more(pairs, kWorkers - 1, loadMs_);
        ts.insert(ts.end(), extra.begin(), extra.end());
    }
    std::vector<std::vector<size_t>> part(ts.size());
    {
        std::vector<size_t> order(in.size()), load(ts.size(), 0);
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return in[a].size() > in[b].size(); });
        for (size_t i : order) {  // longest first, to the least loaded translator
            const size_t w = static_cast<size_t>(std::min_element(load.begin(), load.end()) - load.begin());
            part[w].push_back(i);
            load[w] += in[i].size() + 8;
        }
    }
    results.assign(ts.size(), nullptr);
    {
        std::vector<std::thread> th;
        auto run = [&](size_t w) {
            std::vector<const char*> a;
            for (size_t i : part[w]) a.push_back(argv[i]);
            results[w] = a.empty() ? nullptr : safeTranslate(ts[w], a.data(), a.size());
        };
        for (size_t w = 1; w < ts.size(); ++w)
            if (!part[w].empty()) th.emplace_back(run, w);
        run(0);
        for (auto& x : th) x.join();
    }
    bool failed = false;
    for (size_t w = 0; w < ts.size(); ++w) {
        if (part[w].empty()) continue;
        if (!results[w]) {
            failed = true;
            continue;
        }
        for (size_t k = 0; k < part[w].size(); ++k) tx[part[w][k]] = results[w][k];
    }
    if (failed) {
        for (char** r : results)
            if (r) dll().freeTranslations(r);
        if (err) *err = L"translation failed";
        return false;
    }
    for (size_t i = 0; i < in.size(); ++i) {
        std::wstring s = tx[i] ? fromUtf8(tx[i]) : std::wstring();
        if (tgt == Lang::ZhHant) s = fullWidthPunctuation(toTraditional(s));
        if (tgt == Lang::Ja) {  // 日本語: ？！（）： next to kana / kanji, 、 rather than ，
            s = fullWidthPunctuation(s);
            for (wchar_t& c : s)
                if (c == 0xFF0C) c = 0x3001;
        }
        out.push_back(std::move(s));
    }
    for (char** r : results)
        if (r) dll().freeTranslations(r);
    return true;
}

}  // namespace pm::translate
