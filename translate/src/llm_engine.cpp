// Local LLM translation engine (llm_engine.h): llama.dll (llama.cpp, MIT)
// loaded at run time like bergamot.dll, one model per process, on the GPU
// when the GPU add-on is installed (else the CPU), kept loaded while used
// and released after a while without requests.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <thread>

#include "llm_api/llama.h"
#include "llm_engine.h"
#include "text_util.h"

namespace pm::translate::llm {

namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[256];
    const DWORD n = GetEnvironmentVariableW(name, buf, 256);
    return n > 0 && n < 256 ? std::wstring(buf, n) : std::wstring();
}

// ---- llama.dll (functions resolved by name; signatures from llm_api/llama.h of the pinned release) ----
#define PM_LLAMA_FUNCS(X)                                                                                              \
    X(llama_backend_init) X(llama_log_set) X(llama_model_default_params) X(llama_context_default_params)               \
    X(llama_sampler_chain_default_params) X(llama_model_load_from_file) X(llama_model_free) X(llama_init_from_model)   \
    X(llama_free) X(llama_model_get_vocab) X(llama_model_meta_val_str) X(llama_tokenize) X(llama_token_to_piece)        \
    X(llama_vocab_is_eog) X(llama_batch_get_one) X(llama_decode) X(llama_get_memory) X(llama_memory_clear)              \
    X(llama_state_seq_get_size) X(llama_state_seq_get_data) X(llama_state_seq_set_data) X(llama_sampler_chain_init)    \
    X(llama_sampler_chain_add) X(llama_sampler_init_greedy) X(llama_sampler_init_dist) X(llama_sampler_init_temp)      \
    X(llama_sampler_init_top_p) X(llama_sampler_init_grammar) X(llama_sampler_sample) X(llama_sampler_free)             \
    X(llama_n_ctx) X(llama_model_size) X(llama_batch_init) X(llama_batch_free) X(llama_memory_seq_cp)                  \
    X(llama_memory_seq_rm)

// ggml's device registry (ggml.dll / ggml-base.dll).
#define PM_GGML_FUNCS(X)                                                                                               \
    X(ggml_backend_load_all_from_path) X(ggml_backend_dev_count) X(ggml_backend_dev_get) X(ggml_backend_dev_type)      \
    X(ggml_backend_dev_description) X(ggml_backend_dev_memory)

struct Api {
#define PM_DECL(f) decltype(&::f) f = nullptr;
    PM_LLAMA_FUNCS(PM_DECL)
    PM_GGML_FUNCS(PM_DECL)
#undef PM_DECL
    HMODULE mod = nullptr;
    std::wstring error;
};

void quietLog(enum ggml_log_level, const char*, void*) {}

// Non-ASCII directories (C:\Users\王小明\...): llama.cpp opens files with
// UTF-8 paths; the 8.3 short name is pure ASCII when it exists.
std::string pathArg(const std::wstring& p) {
    bool ascii = true;
    for (wchar_t c : p) ascii = ascii && c < 0x80;
    if (!ascii) {
        wchar_t sh[MAX_PATH];
        const DWORD n = GetShortPathNameW(p.c_str(), sh, MAX_PATH);
        if (n > 0 && n < MAX_PATH) return toUtf8(std::wstring(sh, n));
    }
    return toUtf8(p);
}

// Loaded once per process and kept (like bergamot.dll).
Api& api() {
    static Api a = [] {
        Api x;
        const std::wstring dir = runtimeDir();
        const std::wstring path = dir + L"\\llama.dll";
        // Its own directory (ggml*.dll, libomp.dll), the application's (the
        // VC++ runtime next to the exe) and System32 - nothing else.
        x.mod = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                   LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (!x.mod) {
            x.error = L"cannot load " + path + L" (error " + std::to_wstring(GetLastError()) + L")";
            return x;
        }
        bool ok = true;
#define PM_RESOLVE(f)                                                                     \
    x.f = reinterpret_cast<decltype(&::f)>(GetProcAddress(x.mod, #f));                    \
    ok = ok && x.f;
        PM_LLAMA_FUNCS(PM_RESOLVE)
#undef PM_RESOLVE
        HMODULE g = GetModuleHandleW((dir + L"\\ggml.dll").c_str());
        HMODULE gb = GetModuleHandleW((dir + L"\\ggml-base.dll").c_str());
#define PM_RESOLVE_G(f)                                                                                      \
    x.f = reinterpret_cast<decltype(&::f)>(g ? GetProcAddress(g, #f) : nullptr);                             \
    if (!x.f) x.f = reinterpret_cast<decltype(&::f)>(gb ? GetProcAddress(gb, #f) : nullptr);                 \
    ok = ok && x.f;
        PM_GGML_FUNCS(PM_RESOLVE_G)
#undef PM_RESOLVE_G
        if (!ok) {
            x.error = path + L": not the expected llama.cpp build";
            FreeLibrary(x.mod);
            x.mod = nullptr;
            return x;
        }
        x.llama_log_set(quietLog, nullptr);
        // The backends in dir: the CPU variant for this CPU (ggml-cpu-alderlake.dll,
        // -haswell, ...) and the GPU add-on when installed (ggml-vulkan.dll /
        // ggml-cuda.dll; skipped by ggml when the driver is missing).
        {
            // ggml loads each one with LoadLibraryW: allow its directory for their imports.
            DLL_DIRECTORY_COOKIE c = AddDllDirectory(dir.c_str());
            x.ggml_backend_load_all_from_path(pathArg(dir).c_str());
            if (c) RemoveDllDirectory(c);
        }
        x.llama_backend_init();
        return x;
    }();
    return a;
}

int defaultThreads() {
    // Physical cores (performance cores first on hybrid CPUs), at most 8:
    // generation is memory-bound, more threads only add contention.
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<char> buf(len);
    int cores = 0, perf = 0;
    BYTE maxClass = 0;
    if (len && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
        for (DWORD off = 0; off < len;) {
            auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
            maxClass = std::max(maxClass, p->Processor.EfficiencyClass);
            off += p->Size;
        }
        for (DWORD off = 0; off < len;) {
            auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
            ++cores;
            if (p->Processor.EfficiencyClass == maxClass) ++perf;
            off += p->Size;
        }
    }
    int n = perf > 0 ? perf : cores;
    if (n <= 0) n = 4;
    return std::clamp(n, 1, 8);
}

std::string metaStr(const llama_model* m, const char* key) {
    char buf[256] = {};
    const int n = api().llama_model_meta_val_str(m, key, buf, sizeof(buf));
    return n > 0 ? std::string(buf, std::min<int>(n, sizeof(buf) - 1)) : std::string();
}

// The device to run on: the discrete GPU with the most memory whose free
// memory holds the model + 768 MB (the video decoder and the desktop use
// the same GPU), else nullptr = CPU.
ggml_backend_dev_t pickGpu(Device want, uint64_t modelBytes, std::string& name) {
    name = "CPU";
    const std::wstring e = envVar(L"PM_LLM_GPU");
    if (e == L"off" || e == L"cpu") return nullptr;
    if (e == L"igpu") want = Device::IntegratedGpu;
    if (want == Device::Cpu || (e.empty() && !gpuEnabled())) return nullptr;
    Api& a = api();
    ggml_backend_dev_t best = nullptr;
    size_t bestTotal = 0;
    for (size_t i = 0; i < a.ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t d = a.ggml_backend_dev_get(i);
        const auto t = a.ggml_backend_dev_type(d);
        if (t != (want == Device::IntegratedGpu ? GGML_BACKEND_DEVICE_TYPE_IGPU : GGML_BACKEND_DEVICE_TYPE_GPU)) continue;
        size_t free = 0, total = 0;
        a.ggml_backend_dev_memory(d, &free, &total);
        if (t == GGML_BACKEND_DEVICE_TYPE_GPU && free < modelBytes + (768ull << 20)) continue;
        if (!best || total > bestTotal) {
            best = d;
            bestTotal = total;
        }
    }
    if (best) name = a.ggml_backend_dev_description(best);
    return best;
}

// The model of the process.
struct Shared {
    std::mutex mu;                  // one generation at a time; guards everything below
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    std::wstring path;
    std::string family, device;
    int threads = 0, ctxTokens = 0, parallel = 0;
    Device want = Device::Auto;
    double loadMs = 0;
    double lastUse = 0;
    int idleSeconds = 300;
    // Sequence 0 holds the prompt prefix `resident` (nResident tokens); the
    // requests run in sequences 1..parallel, copied from it and removed after.
    std::string resident;
    int nResident = 0;
    // Saved prefixes (other language pairs): text -> state of sequence 0.
    struct Prefix {
        std::string text;
        std::vector<uint8_t> state;
        int tokens = 0;
    };
    std::vector<Prefix> prefixes;   // most recent last, at most 4
    // Idle release (a detached thread; the process exit ends it).
    bool watcherStarted = false;
    std::condition_variable cv;
    bool stop = false;

    void freeLocked() {
        prefixes.clear();
        resident.clear();
        nResident = 0;
        if (ctx) api().llama_free(ctx);
        if (model) api().llama_model_free(model);
        ctx = nullptr;
        model = nullptr;
        device.clear();
    }
    void startWatcher() {
        if (watcherStarted) return;
        watcherStarted = true;
        std::thread([this] {
            std::unique_lock<std::mutex> l(mu);
            while (!stop) {
                cv.wait_for(l, std::chrono::seconds(5));
                if (model && idleSeconds > 0 && nowMs() - lastUse > idleSeconds * 1000.0) freeLocked();
            }
        }).detach();
    }
};

// Never destroyed: the model is freed by the idle watcher or unload(), and at
// process exit the OS takes the memory back (no llama.dll calls during exit).
Shared& shared() {
    static Shared* s = new Shared;
    return *s;
}

std::wstring familyFromArch(const std::string& arch) {
    if (arch == "gemma3") return L"gemma3";
    if (arch.rfind("gemma4", 0) == 0) return L"gemma4";
    return L"chatml";
}

int ctxFor(const Options& o) {
    const int par = std::clamp(o.parallel, 1, 32);
    return o.ctxTokens > 0 ? o.ctxTokens : 1024 + 384 * par;
}

// Vulkan builds its compute pipelines on first use of each kernel / size
// class (the driver caches them on disk, but the first picture after an
// install or a driver update would wait seconds): run the batch sizes a
// picture uses once, at load.
std::vector<llama_token> tokenize(const llama_vocab* v, const std::string& text, bool addSpecial);

void warmGpu(Shared& s) {
    Api& a = api();
    llama_memory_t mem = a.llama_get_memory(s.ctx);
    const llama_vocab* vocab = a.llama_model_get_vocab(s.model);
    std::vector<llama_token> t = tokenize(vocab, "warm up warm up warm up", false);
    if (t.empty()) return;
    for (int n : {1, 2, 4, 8, 16, 32, 64, 128, 512}) {
        if (n > s.ctxTokens / 2) break;
        llama_batch b = a.llama_batch_init(n, 0, 1);
        for (int i = 0; i < n; ++i) {
            b.token[i] = t[i % t.size()];
            b.pos[i] = i;
            b.n_seq_id[i] = 1;
            b.seq_id[i][0] = 1;
            b.logits[i] = i == n - 1;
        }
        b.n_tokens = n;
        a.llama_decode(s.ctx, b);
        a.llama_batch_free(b);
        a.llama_memory_seq_rm(mem, 1, -1, -1);
    }
    // Several sequences in one batch (the parallel requests' generation steps).
    const int k = std::min(s.parallel, 16);
    llama_batch b = a.llama_batch_init(k, 0, 1);
    for (int i = 0; i < k; ++i) {
        b.token[i] = t[0];
        b.pos[i] = 0;
        b.n_seq_id[i] = 1;
        b.seq_id[i][0] = i + 1;
        b.logits[i] = 1;
    }
    b.n_tokens = k;
    a.llama_decode(s.ctx, b);
    a.llama_batch_free(b);
    a.llama_memory_clear(mem, true);
}

bool loadLocked(Shared& s, const Options& o, std::wstring* err) {
    Api& a = api();
    if (!a.mod) {
        if (err) *err = a.error;
        return false;
    }
    std::wstring path;
    std::string family;
    if (!o.modelFile.empty()) path = o.modelFile;
    else {
        const ModelInfo* m = o.modelId.empty() ? &activeModel() : findModel(o.modelId);
        if (!m) {
            if (err) *err = L"unknown model " + fromUtf8(o.modelId);
            return false;
        }
        if (!modelInstalled(*m)) {
            if (err) *err = L"model " + fromUtf8(m->id) + L" is not installed";
            return false;
        }
        path = modelPath(*m);
        family = m->family;
    }
    const int threads = o.threads > 0 ? o.threads : defaultThreads();
    const int par = std::clamp(o.parallel, 1, 32), nctx = ctxFor(o);
    s.lastUse = nowMs();
    s.idleSeconds = o.idleSeconds;
    if (s.model && s.path == path && s.threads == threads && s.ctxTokens == nctx && s.parallel == par && s.want == o.device)
        return true;
    s.freeLocked();
    const double t0 = nowMs();
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa);
    const uint64_t bytes = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    std::string devName;
    ggml_backend_dev_t gpu = pickGpu(o.device, bytes, devName);
    for (int attempt = 0; attempt < 2; ++attempt) {
        ggml_backend_dev_t devs[2] = {gpu, nullptr};
        llama_model_params mp = a.llama_model_default_params();
        mp.devices = devs;                 // {nullptr}: no GPU at all
        mp.n_gpu_layers = gpu ? -1 : 0;    // all layers on the GPU, or none
        mp.split_mode = LLAMA_SPLIT_MODE_NONE;
        mp.main_gpu = 0;
        mp.load_mtp = false;
        s.model = a.llama_model_load_from_file(pathArg(path).c_str(), mp);
        if (s.model) {
            llama_context_params cp = a.llama_context_default_params();
            cp.n_ctx = static_cast<uint32_t>(nctx);
            cp.n_batch = 2048;
            cp.n_ubatch = 512;
            cp.n_seq_max = static_cast<uint32_t>(par + 1);
            cp.kv_unified = true;          // the requests share the prefix cells
            cp.n_threads = threads;
            cp.n_threads_batch = threads;
            cp.no_perf = true;
            s.ctx = a.llama_init_from_model(s.model, cp);
            if (s.ctx) break;
            a.llama_model_free(s.model);
            s.model = nullptr;
        }
        if (!gpu) break;
        gpu = nullptr;  // the GPU failed (driver, memory): the CPU
        devName = "CPU";
    }
    if (!s.ctx) {
        if (err) *err = L"cannot load the model " + path;
        return false;
    }
    s.path = path;
    s.threads = threads;
    s.ctxTokens = nctx;
    s.parallel = par;
    s.want = o.device;
    s.device = devName;
    s.family = family.empty() ? toUtf8(familyFromArch(metaStr(s.model, "general.architecture"))) : family;
    if (gpu) warmGpu(s);
    s.loadMs = nowMs() - t0;
    s.lastUse = nowMs();
    s.startWatcher();
    return true;
}

std::vector<llama_token> tokenize(const llama_vocab* v, const std::string& text, bool addSpecial) {
    std::vector<llama_token> t(text.size() + 8);
    int n = api().llama_tokenize(v, text.data(), static_cast<int32_t>(text.size()), t.data(), static_cast<int32_t>(t.size()),
                                 addSpecial, true);
    if (n < 0) {
        t.resize(-n);
        n = api().llama_tokenize(v, text.data(), static_cast<int32_t>(text.size()), t.data(), static_cast<int32_t>(t.size()),
                                 addSpecial, true);
    }
    t.resize(std::max(n, 0));
    return t;
}

bool decodeAll(llama_context* ctx, std::vector<llama_token>& toks) {
    for (size_t i = 0; i < toks.size(); i += 2048) {
        const int n = static_cast<int>(std::min<size_t>(2048, toks.size() - i));
        if (api().llama_decode(ctx, api().llama_batch_get_one(toks.data() + i, n)) != 0) return false;
    }
    return true;
}

// Sequence 0 = `prefix`: kept when it is already there (the usual case: the
// same language pair as the last picture), else restored from a saved state
// or decoded (and saved).  Sequences 1.. are emptied.
bool setPrefix(Shared& s, const std::string& prefix, int& nTokens) {
    Api& a = api();
    llama_memory_t mem = a.llama_get_memory(s.ctx);
    if (!s.resident.empty() && s.resident == prefix) {
        for (int i = 1; i <= s.parallel; ++i) a.llama_memory_seq_rm(mem, i, -1, -1);
        nTokens = s.nResident;
        return true;
    }
    if (!s.resident.empty()) {  // keep the current one for later
        Shared::Prefix p;
        p.text = s.resident;
        p.tokens = s.nResident;
        for (int i = 1; i <= s.parallel; ++i) a.llama_memory_seq_rm(mem, i, -1, -1);
        bool known = false;
        for (const auto& q : s.prefixes) known = known || q.text == p.text;
        if (!known) {
            p.state.resize(a.llama_state_seq_get_size(s.ctx, 0));
            p.state.resize(a.llama_state_seq_get_data(s.ctx, p.state.data(), p.state.size(), 0));
            if (!p.state.empty()) {
                if (s.prefixes.size() >= 4) s.prefixes.erase(s.prefixes.begin());
                s.prefixes.push_back(std::move(p));
            }
        }
    }
    s.resident.clear();
    a.llama_memory_clear(mem, true);
    for (size_t i = 0; i < s.prefixes.size(); ++i)
        if (s.prefixes[i].text == prefix) {
            if (a.llama_state_seq_set_data(s.ctx, s.prefixes[i].state.data(), s.prefixes[i].state.size(), 0) > 0) {
                s.resident = prefix;
                s.nResident = nTokens = s.prefixes[i].tokens;
                return true;
            }
            a.llama_memory_clear(mem, true);
            break;
        }
    auto toks = tokenize(a.llama_model_get_vocab(s.model), prefix, true);
    if (!decodeAll(s.ctx, toks)) {
        a.llama_memory_clear(mem, true);
        return false;
    }
    s.resident = prefix;
    s.nResident = nTokens = static_cast<int>(toks.size());
    return true;
}

// A small model stuck in a loop: the same 16 bytes four times at the end.
bool looping(const std::string& out) {
    if (out.size() <= 96) return false;
    const std::string tail = out.substr(out.size() - 16);
    size_t reps = 0;
    for (size_t pos = out.size() - 16; pos >= 16 && out.compare(pos - 16, 16, tail) == 0; pos -= 16) ++reps;
    return reps >= 3;
}

int maxTokensFor(const TrRequest& rq) { return std::clamp(static_cast<int>(rq.text.size()) * 3 + 32, 48, 768); }

}  // namespace

LlmEngine::LlmEngine(Options o) : opt_(std::move(o)) {}
LlmEngine::~LlmEngine() = default;

double LlmEngine::now() { return nowMs(); }

void releaseModel() {
    Shared& s = shared();
    std::lock_guard<std::mutex> l(s.mu);
    s.freeLocked();
}

void warmUp() {
    // Cheap and idempotent: one loader thread at a time, nothing when the
    // model is already loaded (then it only counts as a use: the idle
    // release starts over).
    static std::atomic<bool> busy{false};
    if (busy.load()) return;
    {
        Shared& s = shared();
        std::unique_lock<std::mutex> l(s.mu, std::try_to_lock);
        if (!l.owns_lock()) return;  // translating right now: loaded anyway
        if (s.model) {
            s.lastUse = nowMs();
            return;
        }
    }
    const ModelInfo& m = activeModel();
    if (!enabled() || !installed(m) || !memoryOk(m)) return;
    if (busy.exchange(true)) return;
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        LlmEngine e;
        e.load();
        busy = false;
    }).detach();
}

bool LlmEngine::runtimeAvailable(std::wstring* err) {
    if (!runtimeInstalled() && GetFileAttributesW((runtimeDir() + L"\\llama.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = L"llama.cpp runtime not installed";
        return false;
    }
    const Api& a = api();
    if (!a.mod && err) *err = a.error;
    return a.mod != nullptr;
}

bool LlmEngine::load(std::wstring* err) {
    Shared& s = shared();
    std::lock_guard<std::mutex> l(s.mu);
    const bool was = s.model != nullptr;
    const bool ok = loadLocked(s, opt_, err);
    if (ok && !was) stats_.loadMs = s.loadMs;
    if (ok) stats_.device = s.device;
    return ok;
}

bool LlmEngine::loaded() const {
    Shared& s = shared();
    std::lock_guard<std::mutex> l(s.mu);
    return s.model != nullptr;
}

void LlmEngine::unload() {
    Shared& s = shared();
    std::lock_guard<std::mutex> l(s.mu);
    s.freeLocked();
}

std::string LlmEngine::family() const {
    Shared& s = shared();
    std::lock_guard<std::mutex> l(s.mu);
    return s.family;
}

std::string LlmEngine::device() const {
    Shared& s = shared();
    std::lock_guard<std::mutex> l(s.mu);
    return s.device;
}

Stats LlmEngine::stats() const { return stats_; }

bool LlmEngine::generate(const PromptParts& p, int maxTokens, std::string& out, std::wstring* err) {
    std::vector<std::string> o;
    const bool ok = generate(std::vector<PromptParts>{p}, std::vector<int>{maxTokens}, o, 0, nullptr, err);
    out = o.empty() ? std::string() : o[0];
    return ok;
}

bool LlmEngine::generate(const std::vector<PromptParts>& ps, const std::vector<int>& maxTokens, std::vector<std::string>& out,
                         double deadline, std::vector<bool>* cut, std::wstring* err) {
    out.assign(ps.size(), std::string());
    if (cut) cut->assign(ps.size(), false);
    Shared& s = shared();
    std::lock_guard<std::mutex> l(s.mu);
    const bool was = s.model != nullptr;
    if (!loadLocked(s, opt_, err)) return false;
    stats_.loadMs = was ? 0 : s.loadMs;
    stats_.device = s.device;
    Api& a = api();
    llama_memory_t mem = a.llama_get_memory(s.ctx);
    const llama_vocab* vocab = a.llama_model_get_vocab(s.model);
    const int nctx = static_cast<int>(a.llama_n_ctx(s.ctx));
    const double t0 = nowMs();
    double tPrompt = 0, tGen = 0;
    int promptTok = 0, genTok = 0;
    bool ok = true;
    static const bool trace = !envVar(L"PM_LLM_TRACE").empty();

    // Groups of requests with the same prefix (one language pair: one group),
    // at most `parallel` at a time.
    std::vector<size_t> order(ps.size());
    for (size_t i = 0; i < ps.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return ps[x].prefix < ps[y].prefix; });
    for (size_t g0 = 0; g0 < order.size();) {
        size_t g1 = g0 + 1;
        while (g1 < order.size() && g1 - g0 < static_cast<size_t>(s.parallel) && ps[order[g1]].prefix == ps[order[g0]].prefix)
            ++g1;
        const std::vector<size_t> grp(order.begin() + g0, order.begin() + g1);
        g0 = g1;
        if (deadline > 0 && nowMs() > deadline) {
            for (size_t k : grp)
                if (cut) (*cut)[k] = true;
            continue;
        }
        const double ta = nowMs();
        int nPrefix = 0;
        if (!setPrefix(s, ps[grp[0]].prefix, nPrefix)) {
            if (err) *err = L"prompt evaluation failed";
            ok = false;
            continue;
        }
        const double tPre = nowMs();
        // The suffixes, sequence j+1 for grp[j]; they must fit the context together.
        const int n = static_cast<int>(grp.size());
        std::vector<std::vector<llama_token>> suf(n);
        std::vector<int> budget(n), pos(n);
        int need = nPrefix;
        for (int j = 0; j < n; ++j) {
            suf[j] = tokenize(vocab, ps[grp[j]].suffix, ps[grp[j]].prefix.empty());
            budget[j] = std::max(1, maxTokens[grp[j]]);
            need += static_cast<int>(suf[j].size()) + budget[j];
        }
        if (need > nctx) {  // shrink the answers' room evenly (long texts)
            const int over = need - nctx;
            for (int j = 0; j < n; ++j) budget[j] = std::max(8, budget[j] - (over + n - 1) / n);
        }
        for (int j = 0; j < n; ++j) a.llama_memory_seq_cp(mem, 0, j + 1, -1, -1);
        int total = 0;
        for (const auto& t : suf) total += static_cast<int>(t.size());
        llama_batch b = a.llama_batch_init(std::max(total, n), 0, 1);
        // All suffix tokens but each one's last (no output), in chunks of n_batch...
        std::vector<int> lastIdx(n, -1);
        auto flush = [&]() -> bool {
            if (b.n_tokens == 0) return true;
            const double td = nowMs();
            const bool r = a.llama_decode(s.ctx, b) == 0;
            if (trace && nowMs() - td > 200) std::fprintf(stderr, "[llm] slow decode: %d tokens %.0f ms\n", b.n_tokens, nowMs() - td);
            b.n_tokens = 0;
            return r;
        };
        auto add = [&](llama_token t, int p, int seq, bool logits) {
            b.token[b.n_tokens] = t;
            b.pos[b.n_tokens] = p;
            b.n_seq_id[b.n_tokens] = 1;
            b.seq_id[b.n_tokens][0] = seq;
            b.logits[b.n_tokens] = logits ? 1 : 0;
            ++b.n_tokens;
        };
        // Small chunks, the deadline checked after each: a prefill never
        // overruns it by more than one chunk (~60 ms GPU, ~0.3 s CPU).
        const int chunk = s.device == "CPU" ? 64 : 256;
        bool good = true, late = false;
        auto lateNow = [&] { return deadline > 0 && nowMs() > deadline; };
        for (int j = 0; j < n && good && !late; ++j) {
            pos[j] = nPrefix;
            for (size_t k = 0; k + 1 < suf[j].size() && good && !late; ++k) {
                add(suf[j][k], pos[j]++, j + 1, false);
                if (b.n_tokens >= chunk) {
                    good = flush();
                    late = lateNow();
                }
            }
        }
        good = good && (late || flush());
        late = late || lateNow();
        if (late) {
            a.llama_batch_free(b);
            for (int j = 0; j < n; ++j) {
                a.llama_memory_seq_rm(mem, j + 1, -1, -1);
                if (cut) (*cut)[grp[j]] = true;
            }
            tPrompt += nowMs() - ta;
            continue;
        }
        // ...then every last token together, with output.
        std::vector<bool> active(n, false);
        for (int j = 0; j < n && good; ++j) {
            if (suf[j].empty()) continue;
            lastIdx[j] = b.n_tokens;
            add(suf[j].back(), pos[j]++, j + 1, true);
            active[j] = true;
        }
        good = good && flush();
        promptTok += total;
        const double tb = nowMs();
        tPrompt += tb - ta;
        if (!good) {
            a.llama_batch_free(b);
            for (int j = 0; j < n; ++j) a.llama_memory_seq_rm(mem, j + 1, -1, -1);
            if (err) *err = L"prompt evaluation failed";
            ok = false;
            continue;
        }
        std::vector<llama_sampler*> chains(n);
        for (int j = 0; j < n; ++j) {
            chains[j] = a.llama_sampler_chain_init(a.llama_sampler_chain_default_params());
            if (!ps[grp[j]].grammar.empty())
                if (llama_sampler* gs = a.llama_sampler_init_grammar(vocab, ps[grp[j]].grammar.c_str(), "root"))
                    a.llama_sampler_chain_add(chains[j], gs);
            if (opt_.temperature > 0) {
                a.llama_sampler_chain_add(chains[j], a.llama_sampler_init_top_p(opt_.topP, 1));
                a.llama_sampler_chain_add(chains[j], a.llama_sampler_init_temp(opt_.temperature));
                a.llama_sampler_chain_add(chains[j], a.llama_sampler_init_dist(opt_.seed));
            } else {
                a.llama_sampler_chain_add(chains[j], a.llama_sampler_init_greedy());
            }
        }
        std::vector<int> gen(n, 0);
        char piece[256];
        int steps = 0;
        for (;; ++steps) {
            // One token for every active sequence, sampled from the last decode.
            std::vector<std::pair<int, llama_token>> next;
            for (int j = 0; j < n; ++j) {
                if (!active[j]) continue;
                const llama_token t = a.llama_sampler_sample(chains[j], s.ctx, lastIdx[j]);
                std::string& o = out[grp[j]];
                if (a.llama_vocab_is_eog(vocab, t)) {
                    active[j] = false;
                    continue;
                }
                const int k = a.llama_token_to_piece(vocab, t, piece, sizeof(piece), 0, false);
                if (k > 0) o.append(piece, k);
                ++gen[j];
                if (gen[j] >= budget[j] || looping(o)) {
                    active[j] = false;
                    continue;
                }
                next.push_back({j, t});
            }
            if (next.empty()) break;
            if (deadline > 0 && nowMs() > deadline) {
                for (auto [j, t] : next) {
                    active[j] = false;
                    if (cut) (*cut)[grp[j]] = true;
                    out[grp[j]].clear();  // half an answer is worse than none
                }
                break;
            }
            for (auto [j, t] : next) {
                lastIdx[j] = b.n_tokens;
                add(t, pos[j]++, j + 1, true);
            }
            if (!flush()) {
                if (err) *err = L"generation failed";
                break;
            }
        }
        for (int j = 0; j < n; ++j) {
            genTok += gen[j];
            a.llama_sampler_free(chains[j]);
            a.llama_memory_seq_rm(mem, j + 1, -1, -1);
        }
        a.llama_batch_free(b);
        tGen += nowMs() - tb;
        if (trace) std::fprintf(stderr, "[llm] group %d: prefix %d tok %.0f ms, suffix %d tok %.0f ms, gen %d steps %.0f ms\n", n, nPrefix, tPre - ta, total, tb - tPre, steps, nowMs() - tb);
    }
    const double t2 = nowMs();
    stats_.ms = t2 - t0;
    stats_.requests = static_cast<int>(ps.size());
    stats_.promptTokens = promptTok;
    stats_.genTokens = genTok;
    stats_.promptTps = tPrompt > 0 ? promptTok * 1000.0 / tPrompt : 0;
    stats_.genTps = tGen > 0 ? genTok * 1000.0 / tGen : 0;
    s.lastUse = nowMs();
    s.idleSeconds = opt_.idleSeconds;
    return ok;
}

bool LlmEngine::translate(const std::vector<TrRequest>& in, std::vector<std::wstring>& out, std::wstring* err,
                          std::vector<std::string>* raws) {
    out.assign(in.size(), std::wstring());
    if (raws) raws->assign(in.size(), std::string());
    const double t0 = nowMs();
    double deadline = 0;
    for (const auto& rq : in)
        if (rq.budgetMs > 0) deadline = deadline > 0 ? std::min(deadline, t0 + rq.budgetMs) : t0 + rq.budgetMs;
    std::string fam;
    {
        Shared& s = shared();
        std::lock_guard<std::mutex> l(s.mu);
        if (!loadLocked(s, opt_, err)) return false;
        fam = s.family;
    }
    Stats sum;
    auto run = [&](const std::vector<size_t>& idx, const std::vector<TrRequest>& rqs, std::vector<std::string>& got,
                   std::vector<bool>& cut, bool strict) {
        std::vector<PromptParts> ps;
        std::vector<int> mt;
        for (const auto& rq : rqs) {
            ps.push_back(buildPrompt(fam.c_str(), opt_.style, rq, strict));
            mt.push_back(maxTokensFor(rq));
        }
        const bool r = generate(ps, mt, got, deadline, &cut, err);
        sum.loadMs += stats_.loadMs;
        sum.promptTokens += stats_.promptTokens;
        sum.genTokens += stats_.genTokens;
        sum.device = stats_.device;
        (void)idx;
        return r;
    };
    // 1) every request, directly from its source.
    std::vector<size_t> todo;
    std::vector<TrRequest> rqs;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i].text.empty()) continue;
        if (in[i].src == in[i].tgt) {
            out[i] = in[i].text;
            continue;
        }
        todo.push_back(i);
        rqs.push_back(in[i]);
    }
    std::vector<std::string> got;
    std::vector<bool> cut;
    bool any = false;
    if (!todo.empty()) run(todo, rqs, got, cut, false);
    std::vector<size_t> again;
    for (size_t k = 0; k < todo.size(); ++k) {
        const size_t i = todo[k];
        if (k < cut.size() && cut[k]) ++sum.cutByBudget;
        out[i] = cleanOutput(got[k], in[i], opt_.style);
        if (raws) (*raws)[i] = got[k];
        if (out[i].empty() && !got[k].empty()) again.push_back(i);
    }
    const double firstMs = nowMs() - t0;
    // A retry round takes about as long as the first one: only when it fits.
    auto fits = [&] { return deadline <= 0 || nowMs() + std::max(300.0, firstMs) <= deadline; };
    // 2) left untranslated (the source copied - Qwen3.5-2B does it on some
    // polite Japanese sentences): once more, asked in Chinese.
    if (!again.empty() && fits()) {
        rqs.clear();
        for (size_t i : again) rqs.push_back(in[i]);
        run(again, rqs, got, cut, true);
        std::vector<size_t> still;
        for (size_t k = 0; k < again.size(); ++k) {
            const size_t i = again[k];
            ++sum.retried;
            out[i] = cleanOutput(got[k], in[i], opt_.style);
            if (raws) (*raws)[i] += "\n[retry]\n" + got[k];
            if (out[i].empty() && in[i].src != Lang::En && in[i].tgt != Lang::En) still.push_back(i);
        }
        again.swap(still);
    } else {
        again.clear();
    }
    // 3) still: the English way - Bergamot's English (rq.pivot) when given,
    // else the model's own (one more round).
    if (!again.empty() && fits()) {
        std::vector<size_t> needEn;
        std::vector<std::wstring> en(in.size());
        for (size_t i : again) {
            if (!in[i].pivot.empty()) en[i] = in[i].pivot;
            else needEn.push_back(i);
        }
        if (!needEn.empty()) {
            rqs.clear();
            for (size_t i : needEn) {
                TrRequest t = in[i];
                t.tgt = Lang::En;
                t.context.clear();
                rqs.push_back(t);
            }
            run(needEn, rqs, got, cut, false);
            for (size_t k = 0; k < needEn.size(); ++k) {
                en[needEn[k]] = cleanOutput(got[k], rqs[k], opt_.style);
                if (raws) (*raws)[needEn[k]] += "\n[en]\n" + got[k];
            }
        }
        std::vector<size_t> viaEn;
        rqs.clear();
        for (size_t i : again)
            if (!en[i].empty()) {
                TrRequest t = in[i];
                t.text = en[i];
                t.src = Lang::En;
                viaEn.push_back(i);
                rqs.push_back(t);
            }
        if (!viaEn.empty() && (needEn.empty() || fits())) {
            run(viaEn, rqs, got, cut, false);
            for (size_t k = 0; k < viaEn.size(); ++k) {
                const size_t i = viaEn[k];
                out[i] = cleanOutput(got[k], rqs[k], opt_.style);
                if (raws) (*raws)[i] += "\n[via en]\n" + got[k];
                if (!out[i].empty()) ++sum.viaEnglish;
            }
        }
    }
    for (const auto& o : out) any = any || !o.empty();
    sum.ms = nowMs() - t0;
    sum.requests = static_cast<int>(in.size());
    sum.promptTps = stats_.promptTps;
    sum.genTps = stats_.genTps;
    stats_ = sum;
    return any || todo.empty();
}

std::wstring LlmEngine::translateOne(const TrRequest& rq, std::string* raw, std::wstring* err) {
    std::vector<std::wstring> out;
    std::vector<std::string> raws;
    translate({rq}, out, err, &raws);
    if (raw && !raws.empty()) *raw = raws[0];
    return out.empty() ? std::wstring() : out[0];
}

// ---- ITranslator ----

LlmTranslator::LlmTranslator(Options o) : engine_(o) {
    if (!o.modelFile.empty()) id_ = "llm-file";
    else {
        const ModelInfo* m = o.modelId.empty() ? &activeModel() : findModel(o.modelId);
        id_ = std::string("llm-") + (m ? m->id : "unknown");
    }
}

bool LlmTranslator::supports(Lang src, Lang tgt, bool* direct) const {
    if (direct) *direct = true;
    const bool srcOk = src == Lang::Ja || src == Lang::Ko || src == Lang::En || src == Lang::ZhHans || src == Lang::ZhHant;
    const bool tgtOk = tgt == Lang::ZhHant || tgt == Lang::En || tgt == Lang::Ja || tgt == Lang::Ko;
    return srcOk && tgtOk && src != tgt;
}

bool LlmTranslator::installed(Lang src, Lang tgt) const {
    if (!supports(src, tgt, nullptr)) return false;
    const Options& o = engine_.options();
    if (!o.modelFile.empty()) return GetFileAttributesW(o.modelFile.c_str()) != INVALID_FILE_ATTRIBUTES;
    const ModelInfo* m = o.modelId.empty() ? &activeModel() : findModel(o.modelId);
    if (!m || !llm::enabled() || !llm::installed(*m)) return false;
    // Already in memory, or room to load it (not when the PC is short of memory).
    return engine_.loaded() || memoryOk(*m);
}

uint64_t LlmTranslator::missingBytes(Lang src, Lang tgt) const {
    if (!supports(src, tgt, nullptr)) return 0;
    const Options& o = engine_.options();
    const ModelInfo* m = o.modelId.empty() ? &activeModel() : findModel(o.modelId);
    return m ? llm::missingBytes(*m) : 0;
}

TrCost LlmTranslator::cost() const {
    // Measured with pm_llm_eval --pictures 5 / 10 / 20 on the 203 eval
    // segments (build-llm/runs/mx-*; the requests of one call decoded in
    // parallel), least squares per call:
    //   Qwen3.5-2B, RTX 3060 Ti (Vulkan):   ms ~ 459 + 62 x requests  (+0.4 / char)
    //   Qwen3.5-2B, CPU (4 P-cores, busy PC): ms ~ 826 x requests
    //   Qwen3.5-0.8B, Vulkan:  ms ~ 338 + 4 / char;  CPU: ms ~ 183 + 176 x requests
    // TrCost is per request: the GPU's fixed part of a call is spread as
    // 110 ms a request (a call of 1-3 requests is under-estimated; the
    // engine's own deadline, TrRequest::budgetMs, still holds).
    const Options& o = engine_.options();
    const ModelInfo* m = o.modelId.empty() ? &activeModel() : findModel(o.modelId);
    const bool small = m && std::string(m->id).find("0.8b") != std::string::npos;
    std::string dev = engine_.device();
    const bool gpu = dev.empty() ? (o.device == Device::Auto && gpuEnabled() && gpuInstalled() && gpuPossible()) : dev != "CPU";
    TrCost c;
    if (gpu) {
        c.msPerRequest = small ? 40 : 110;
        c.msPerChar = small ? 4 : 1;
        c.loadMs = 1700;  // incl. the pipeline warm-up (first time after an install: up to ~16 s)
    } else {
        c.msPerRequest = small ? 190 : 800;
        c.msPerChar = small ? 1.3 : 5;
        c.loadMs = 1000;
    }
    c.memoryMB = m ? m->ramMB : 1700;
    return c;
}

bool LlmTranslator::translate(const std::vector<TrRequest>& in, std::vector<TrHypothesis>& out, std::wstring* err) {
    std::vector<std::wstring> tx;
    const double t0 = nowMs();
    if (engine_.options().modelId.empty() && engine_.options().modelFile.empty())
        id_ = std::string("llm-") + activeModel().id;  // the settings may have switched it
    const bool ok = engine_.translate(in, tx, err);
    const double each = in.empty() ? 0 : (nowMs() - t0) / in.size();
    out.assign(in.size(), TrHypothesis{});
    for (size_t i = 0; i < in.size() && i < tx.size(); ++i) {
        out[i].text = tx[i];
        out[i].engine = id_;
        out[i].ms = each;
    }
    return ok;
}

}  // namespace pm::translate::llm

// The factory pm/translator.h looks for (translator.cpp has a default returning nullptr).
extern "C" pm::translate::ITranslator* pm_create_local_llm_translator() {
    // Always an object: installed() tells the escalator whether it can run now
    // (the user may download the model while the app is open).
    return new pm::translate::llm::LlmTranslator();
}
