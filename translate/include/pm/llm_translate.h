// Local LLM translation (translate/src/llm_engine*.cpp) - what the settings
// page needs: the model list, download / remove, on / off.  The engine itself
// is the ITranslator returned by pm_create_local_llm_translator()
// (pm/translator.h, escalation step 3); it reads these settings on every call.
//
// Nothing is bundled with the installer.  download() fetches, over HTTPS from
// pinned sources, the official llama.cpp CPU runtime (MIT, ~19 MB zip,
// github.com/ggml-org/llama.cpp release) and one GGUF model (Apache-2.0,
// huggingface.co at a fixed commit) into %LOCALAPPDATA%\PhoneMirror\models\llm,
// SHA-256 checked; an interrupted download continues where it stopped.
// On a PC with an NVIDIA / AMD / Intel GPU the Vulkan backend (~33 MB zip,
// same release) is downloaded too and the model runs on the discrete GPU
// (about 2.2 GB of video memory for the 2B model with 16 parallel requests);
// otherwise, or when that
// fails, on the CPU (any x64 CPU; AVX2 / AVX-512 used when present).  The
// segments of one picture are translated together.  The model stays loaded
// while translating and is unloaded after 5 minutes without requests.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pm::translate::local_llm {

struct Model {
    std::string id;           // "qwen3.5-2b-q4km"
    std::wstring name;        // "Qwen3.5-2B (Qwen, Q4_K_M)" - a product name, not translated
    std::wstring license;     // "Apache-2.0"
    uint64_t fileBytes = 0;   // the model file on disk
    int ramMB = 0;            // memory while loaded
    bool recommended = false; // the default for this PC (2B with a discrete GPU, else 0.8B)
    bool installed = false;   // runtime + this model downloaded and verified
};
// The models this build can download.
std::vector<Model> models();

// models\llm\settings.ini.  Defaults: enabled, the recommended model.
struct Settings {
    bool enabled = true;      // off: kept on disk but never used
    std::string model;        // "" = the recommended one for this PC (the user's choice otherwise)
    bool gpu = true;          // use the GPU when there is one (off: CPU only, no GPU download)
};
Settings loadSettings();
bool saveSettings(const Settings& s, std::wstring* err = nullptr);

struct Status {
    std::string model;        // the model in use (settings / PM_LLM_MODEL)
    bool installed = false;   // runtime + model present
    bool enabled = false;     // settings
    bool memoryOk = true;     // enough free memory to load it now (else the step is skipped)
    uint64_t missingBytes = 0;  // still to download for this PC: runtime + GPU backend (when wanted) +
                                // the model in use, minus a partial download
    uint64_t partialBytes = 0;  // already downloaded by an interrupted download
    bool gpuPossible = false;   // this PC has a GPU the Vulkan backend can use
    bool gpuInstalled = false;  // ... and the backend is downloaded
    bool discreteGpu = false;   // a discrete GPU with >= 3 GB of video memory (the 2B model is the default)
    bool gpuWillDownload = false;  // download() also fetches the GPU backend (~33 MB; in missingBytes)
    std::string device;         // where the loaded model runs: "CPU" / the GPU's name ("" = not loaded)
    bool usable() const { return installed && enabled && memoryOk; }
};
Status status();

// Consent dialog body: what is downloaded, from where, under which license
// (one line per file, URLs included; not translated).
std::wstring describeDownload();

enum class Result {
    Ok,
    Cancelled,     // cancel was set (the partial file is kept: download() continues it)
    Network,       // no connection / HTTP error / interrupted (partial kept)
    Verify,        // size / SHA-256 mismatch (file deleted)
    Disk,          // cannot write (disk full, permissions)
    Unpack,        // tar.exe missing / failed (Windows 10 1803 or later needed)
    UnknownModel,
};
const char* resultId(Result r);  // "ok", "cancelled", "network", "verify", "disk", "unpack", "unknown-model"

// Downloads what the model in use (or `model`) is missing.  Blocking (minutes):
// call from a worker thread.  progress(0..1) on that thread.  detail: a
// technical message for 「詳細資料」.
Result download(const std::function<void(double)>& progress, const std::atomic<bool>* cancel,
                std::wstring* detail = nullptr, const std::string& model = "");
// The same with what the progress line shows: 「下載中 12%（150/1272 MB）・
// 8.5 MB/s・約 2 分鐘」.  All files are fetched at once over parallel
// connections; cancel is noticed within ~20 ms and the call returns within
// ~0.2 s (show 「取消中…」 meanwhile).  progress: at most every 100 ms.
struct DownloadProgress {
    double fraction = 0;          // 0..1 over every file
    uint64_t doneBytes = 0, totalBytes = 0;
    double bytesPerSec = 0;       // last ~3 s (0: not known yet)
    double secondsLeft = -1;      // -1: not known yet
    int connections = 0;
    bool verifying = false;       // the final SHA-256 check (show 「驗證中…」)
};
Result download(const std::function<void(const DownloadProgress&)>& progress, const std::atomic<bool>* cancel,
                std::wstring* detail = nullptr, const std::string& model = "");
// Deletes the model (and the runtime when no model is left); unloads it first.
bool remove(const std::string& model = "");
// Frees the model's memory now (it is loaded again on the next request).
void release();
// Loads the model in the background now - call when the user turns screen
// translation on, so the first picture does not wait (~1 s).  It is kept
// loaded while pictures are translated and released 5 minutes after the last.
void warmUp();

}  // namespace pm::translate::local_llm
