// Local LLM translation engine (P2 of launch/_work/tr_arch/ARCHITECTURE.md
// §3.7): llama.cpp (MIT, the official CPU build of a pinned release,
// llama.dll + ggml*.dll) runs a small GGUF model (Apache-2.0) on the CPU and
// translates ja / ko / en / zh -> zh-Hant (and en / ja / ko) directly, without
// the English pivot that loses negations.  Only the segments whose
// Bergamot translation failed a check are sent here (pm/translator.h step 3).
//
// Everything is downloaded on demand into %LOCALAPPDATA%\PhoneMirror\models\llm
// (ModelStore::root(), PM_MODELS_DIR for tests), over HTTPS from pinned
// sources (a GitHub release of ggml-org/llama.cpp, a Hugging Face commit),
// SHA-256 verified like ModelStore.  The installer does not grow.
//
// The model is loaded on first use and released after idleSeconds without
// requests (default 300) - the memory is back while the user is not translating.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "downloader.h"
#include "pm/translator.h"

namespace pm::translate::llm {

// ---- Packages (llm_engine_store.cpp, pinned in llm_engine_models.inc) ----
struct ModelInfo {
    const char* id;        // "qwen3.5-2b-q4km" (ITranslator::id() = "llm-" + id)
    const char* family;    // prompt format: "chatml" (Qwen), "gemma3", "gemma4"
    const char* file;      // file name under models\llm
    uint64_t size;
    const char* sha256;
    const char* host;      // "huggingface.co"
    const char* path;      // "/<repo>/resolve/<commit>/<file>"
    const char* license;   // "Apache-2.0"
    const char* name;      // for the consent dialog: "Qwen3.5-2B (Alibaba Qwen, Q4_K_M)"
    int ramMB;             // working memory while loaded (model + context), measured
};
// The models this build can download (the first one is the default).
const std::vector<ModelInfo>& models();
const ModelInfo* findModel(const std::string& id);
// The model in use: PM_LLM_MODEL (an id), else settings.ini "model" (the
// user's choice), else defaultModel().
const ModelInfo& activeModel();
// By device: Qwen3.5-2B when discreteGpu() (and the GPU is not turned off),
// else Qwen3.5-0.8B.
const ModelInfo& defaultModel();
bool discreteGpu();
// models\llm\settings.ini ([llm] enabled=0|1, model=<id>; pm/llm_translate.h).
std::wstring settingsPath();
bool enabled();
// GPU add-on (ggml-vulkan.dll): settings.ini gpu=0 turns it off; possible =
// a Vulkan loader + an NVIDIA / AMD / Intel adapter; wanted = possible, a
// discreteGpu() and not turned off (only then is it downloaded).
bool gpuEnabled();
bool gpuPossible();
bool gpuInstalled();
bool gpuWanted();
// Bytes an interrupted download left in .part files (continued next time).
uint64_t partialBytes(const ModelInfo& m);
// Frees the process' model now (llm_engine.cpp).
void releaseModel();
std::wstring modelPath(const ModelInfo& m);   // where it is / will be
// The llama.cpp runtime (llama.dll, ggml*.dll, libomp.dll): directory and
// whether it is there.  PM_LLAMA_DIR overrides (a llama.cpp build, for tests).
std::wstring runtimeDir();
bool runtimeInstalled();
// The ggml-cpu-<variant>.dll ggml picks on this CPU ("haswell", "zen4", ...;
// "x64" = the baseline) - the same scoring as ggml's cpu-feats.cpp (llm_engine_cpu.cpp).
// The download fetches that one + ggml-cpu-x64 only.
std::string cpuVariant();
unsigned cpuFeatureBits();                    // this CPU's features (bit n = ggml's weight 1 << n)
std::string cpuVariantFor(unsigned features); // tests: the pick for a given feature set
// The runtime files a PC with this ggml-cpu variant needs: llama.dll, ggml*.dll
// except the other ggml-cpu-*.dll, libomp.dll + its license (tests; the
// download and runtimeInstalled() use it with cpuVariant()).
std::vector<std::string> runtimeFilesFor(const std::string& variant);
// Bytes the ranged runtime download fetches for that variant (the zip records).
uint64_t runtimeRangedBytes(const std::string& variant);
// The runtime alone (no model, no GPU add-on): tests (pm_llm_runtime_test).
bool downloadRuntime(const std::function<void(const dl::Progress&)>& progress, const std::atomic<bool>* cancel,
                     std::wstring* err);
// Test hook: how the last runtime download went.
struct RuntimeFetchInfo {
    bool ranged = false;       // only the needed zip records (HTTP Range) were fetched
    bool fellBack = false;     // ... failed, the whole archive was fetched instead
    uint64_t bytes = 0;        // bytes the chosen way fetched
    std::string variant;       // the ggml-cpu variant fetched
};
RuntimeFetchInfo lastRuntimeFetch();
bool modelInstalled(const ModelInfo& m);
// Runtime + model present (verified once, then by size).
bool installed(const ModelInfo& m);
// Bytes to download for m (runtime archive if missing + the model).
uint64_t missingBytes(const ModelInfo& m);
// Download what is missing: HTTPS (WinHTTP), SHA-256 (BCrypt), then moved into
// place; the runtime archive is unpacked by Windows' own tar.exe (no window).
// progress(0..1) from this thread; blocking; cancel from any thread.
bool download(const ModelInfo& m, const std::function<void(double)>& progress, const std::atomic<bool>* cancel,
              std::wstring* err);
// The same with bytes / speed / time left (downloader.h: every file at once,
// parallel Range connections, cancel within ~100 ms).
bool download(const ModelInfo& m, const std::function<void(const dl::Progress&)>& progress, const std::atomic<bool>* cancel,
              std::wstring* err);
// Deletes the model file (and the runtime when no model is left): 「移除」.
bool remove(const ModelInfo& m);
// For the consent dialog: what is downloaded from where, under which license.
std::wstring describeDownload(const ModelInfo& m);
// Enough free memory to load m now (available physical memory >= ramMB + 1 GB).
bool memoryOk(const ModelInfo& m);

// ---- Prompt (llm_engine_prompt.cpp; no model needed, unit-testable) ----
enum class PromptStyle {
    Plain,     // system rules + the text, the model answers the translation only
    FewShot,   // + two worked examples (negation, placeholders) as earlier turns
    Json,      // the answer forced to {"t":"..."} by a GBNF grammar
};
struct PromptParts {
    std::string prefix;   // fixed per (family, style, src, tgt): cached once (KV state)
    std::string suffix;   // this request: context + text + the start of the answer
    std::string grammar;  // GBNF ("" = none)
};
// Always translates rq.text (the source) directly into rq.tgt.  strict: the
// retry after an answer that was left (partly) untranslated.  pivot: an
// English version given as a hint (experiments only: it made answers worse).
PromptParts buildPrompt(const char* family, PromptStyle style, const TrRequest& rq, bool strict = false,
                        const std::wstring& pivot = {});
// The model's answer -> the translation: <think> / channel tags, labels
// (譯文：), wrapping quotes, trailing notes removed; zh-Hant: Traditional
// characters + full-width punctuation.  "" = unusable.
std::wstring cleanOutput(const std::string& raw, const TrRequest& rq, PromptStyle style);

// ---- Engine (llm_engine.cpp) ----
// Where the model runs.  Auto: the discrete GPU with the most memory when the
// GPU add-on (ggml-vulkan.dll / ggml-cuda.dll) is there and its free memory
// holds the model, else the CPU; a GPU that fails to load falls back to the
// CPU.  PM_LLM_GPU=off|auto|igpu and settings.ini gpu=0 override.
enum class Device { Auto, Cpu, IntegratedGpu };
struct Options {
    std::string modelId;              // "" = activeModel()
    std::wstring modelFile;           // a .gguf path instead (tests); family from its metadata
    int threads = 0;                  // 0 = physical cores, at most 8
    int idleSeconds = 300;            // release the model after this long without requests (0 = never)
    PromptStyle style = PromptStyle::FewShot;
    float temperature = 0;            // 0 = greedy (default: most faithful)
    float topP = 0.9f;
    uint32_t seed = 42;
    int parallel = 16;                // requests decoded together (llama.cpp sequences sharing the prompt prefix)
    int ctxTokens = 0;                // 0 = 1024 + 384 per parallel sequence
    Device device = Device::Auto;
};
struct Stats {
    double loadMs = 0;                // last load
    double ms = 0;                    // last call
    int requests = 0;                 // in the last call
    int promptTokens = 0, genTokens = 0;
    double promptTps = 0, genTps = 0; // tokens per second of the last call
    int retried = 0;                  // requests that needed the retry (copied source)
    int viaEnglish = 0;               // ... and in the end went through English
    int cutByBudget = 0;              // requests left "" because the time budget ran out
    std::string device;               // "CPU" or the GPU's name
};

// One model shared by every LlmEngine of the process (loaded once, kept
// loaded while used, released after idleSeconds without requests).
class LlmEngine {
public:
    explicit LlmEngine(Options o = {});
    ~LlmEngine();
    LlmEngine(const LlmEngine&) = delete;
    LlmEngine& operator=(const LlmEngine&) = delete;
    // llama.dll loadable (does not load the model).
    static bool runtimeAvailable(std::wstring* err = nullptr);
    // Loads the model now (else on first translate()).
    bool load(std::wstring* err = nullptr);
    bool loaded() const;
    void unload();
    // All requests of one picture together: decoded in parallel sequences
    // (Options::parallel at a time) after the shared prompt prefix, which
    // stays in the KV cache between calls.  Every request is translated
    // directly from its source.  Answers left untranslated (the source
    // copied) get a retry asked in Chinese, then the English way (rq.pivot,
    // Bergamot's English, else the model's own) - only while the time budget
    // allows (the smallest TrRequest::budgetMs > 0 of the batch, from the
    // start of the call).  out: one entry per request, "" = none.
    // raws (tests): the model's answers.  Blocking; serialised per process.
    bool translate(const std::vector<TrRequest>& in, std::vector<std::wstring>& out, std::wstring* err = nullptr,
                   std::vector<std::string>* raws = nullptr);
    std::wstring translateOne(const TrRequest& rq, std::string* raw = nullptr, std::wstring* err = nullptr);
    // Raw generation of already built prompts, in parallel (tests).
    // deadline: steady-clock ms (nowMs()) after which generation stops (0 = none);
    // cut[i] set when that one was stopped by it.
    bool generate(const std::vector<PromptParts>& ps, const std::vector<int>& maxTokens, std::vector<std::string>& out,
                  double deadline = 0, std::vector<bool>* cut = nullptr, std::wstring* err = nullptr);
    bool generate(const PromptParts& p, int maxTokens, std::string& out, std::wstring* err = nullptr);
    Stats stats() const;
    const Options& options() const { return opt_; }
    std::string family() const;       // of the loaded model
    std::string device() const;       // "CPU" / the GPU's name ("" = not loaded)
    static double now();              // the clock of deadlines (steady, ms)

private:
    Options opt_;
    Stats stats_;
};

// Loads the model on a worker thread now (the translate feature was turned
// on): the first picture does not wait for it.  No-op when not installed.
void warmUp();

// pm/translator.h engine: id "llm-<model id>".
class LlmTranslator : public ITranslator {
public:
    explicit LlmTranslator(Options o = {});
    const char* id() const override { return id_.c_str(); }
    bool supports(Lang src, Lang tgt, bool* direct) const override;
    bool installed(Lang src, Lang tgt) const override;
    uint64_t missingBytes(Lang src, Lang tgt) const override;
    TrCost cost() const override;
    bool translate(const std::vector<TrRequest>& in, std::vector<TrHypothesis>& out, std::wstring* err) override;
    LlmEngine& engine() { return engine_; }

private:
    LlmEngine engine_;
    std::string id_;
};

}  // namespace pm::translate::llm
