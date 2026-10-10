// pm_llm_runtime_test: the per-CPU llama.cpp runtime download (llm_engine.h:
// cpuVariant / runtimeFilesFor / downloadRuntime).
//
//   pm_llm_runtime_test              offline: ggml's variant pick for known CPUs,
//                                    the files each pick needs, this PC's pick
//   pm_llm_runtime_test --fetch DIR  the runtime alone into DIR\llm (PM_MODELS_DIR=DIR;
//                                    never the user's %LOCALAPPDATA%): ranged or whole,
//                                    bytes, installed.  Built with PM_DL_TEST_HTTP:
//                                    PM_LLM_TEST_MIRROR / PM_LLM_TEST_ORIGIN = 127.0.0.1:PORT
//                                    point the mirror / GitHub at a local server,
//                                    PM_LLM_TEST_VARIANT forces the variant.
//   pm_llm_runtime_test --fetch DIR --expect ranged|whole|none|fail
//                                    + checks how it was fetched (llm_runtime_test.py
//                                    runs the local-server cases: no Range, a mirror
//                                    serving broken bytes, a missing mirror, ...)
// Exit 0 = every check passed.  The process runs at below-normal priority.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "llm_engine.h"

using namespace pm::translate::llm;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// ggml's feature weights (llm_engine_cpu.cpp).
enum : unsigned {
    FMA = 1u << 0, F16C = 1u << 1, SSE42 = 1u << 2, BMI2 = 1u << 3, AVX = 1u << 4, AVX2 = 1u << 5,
    AVXVNNI = 1u << 6, AVX512 = 1u << 7, VBMI = 1u << 8, BF16 = 1u << 9, VNNI = 1u << 10, AMX = 1u << 11,
};
constexpr unsigned HSW = SSE42 | AVX | F16C | FMA | AVX2 | BMI2;
constexpr unsigned SKX = HSW | AVX512;

const char* const kAll[] = {"x64", "sse42", "sandybridge", "ivybridge", "piledriver", "haswell", "alderlake",
                            "skylakex", "cannonlake", "cascadelake", "icelake", "cooperlake", "zen4", "sapphirerapids"};

void offline() {
    struct Case {
        const char* cpu;
        unsigned f;
        const char* want;
    };
    const Case cases[] = {
        {"no features", 0, "x64"},
        {"Core 2 / Nehalem (SSE4.2)", SSE42, "sse42"},
        {"AVX without SSE4.2 (impossible, ggml: x64)", AVX, "x64"},
        {"Sandy Bridge", SSE42 | AVX, "sandybridge"},
        {"Ivy Bridge", SSE42 | AVX | F16C, "ivybridge"},
        {"Piledriver / Steamroller", SSE42 | AVX | F16C | FMA, "piledriver"},
        {"Haswell .. Comet Lake, Zen 1-3", HSW, "haswell"},
        {"Haswell without FMA (ggml: ivybridge)", HSW & ~FMA, "ivybridge"},
        {"Alder / Raptor / Meteor Lake", HSW | AVXVNNI, "alderlake"},
        {"Skylake-X", SKX, "skylakex"},
        {"Cannon Lake", SKX | VBMI, "cannonlake"},
        {"Cascade Lake", SKX | VNNI, "cascadelake"},
        {"Ice / Tiger / Rocket Lake", SKX | VBMI | VNNI, "icelake"},
        {"Cooper Lake", SKX | VNNI | BF16, "cooperlake"},
        {"Zen 4 / Zen 5", SKX | VBMI | VNNI | BF16, "zen4"},
        {"Zen 4 + AVX-VNNI (zen4 outscores alderlake)", SKX | VBMI | VNNI | BF16 | AVXVNNI, "zen4"},
        {"Sapphire / Emerald / Granite Rapids", SKX | VBMI | VNNI | BF16 | AMX | AVXVNNI, "sapphirerapids"},
        {"AVX-512 F..BW without AVX2 (ggml: sandybridge)", SSE42 | AVX | AVX512, "sandybridge"},
    };
    for (const auto& c : cases) {
        const std::string got = cpuVariantFor(c.f);
        check(got == c.want, std::string(c.cpu) + " -> " + got + " (want " + c.want + ")");
    }
    // Every variant: the pinned files are there, its DLL + x64 + the 5 shared files.
    for (const char* v : kAll) {
        const auto files = runtimeFilesFor(v);
        const std::string dll = std::string("ggml-cpu-") + v + ".dll";
        const bool has = std::find(files.begin(), files.end(), dll) != files.end();
        const size_t cpuDlls = std::count_if(files.begin(), files.end(), [](const std::string& s) { return s.rfind("ggml-cpu-", 0) == 0; });
        const size_t want = std::string(v) == "x64" ? 6 : 7;
        check(has && files.size() == want && cpuDlls == (want == 6 ? 1u : 2u) && runtimeRangedBytes(v) > 0,
              std::string(v) + ": " + std::to_string(files.size()) + " files, " + std::to_string(runtimeRangedBytes(v)) + " bytes ranged");
    }
    for (const char* f : {"llama.dll", "ggml.dll", "ggml-base.dll", "libomp.dll", "LICENSE-LLVM-OpenMP"}) {
        const auto files = runtimeFilesFor("haswell");
        check(std::find(files.begin(), files.end(), f) != files.end(), std::string("haswell needs ") + f);
    }
    const unsigned bits = cpuFeatureBits();
    std::printf("this PC: features 0x%03x -> ggml-cpu-%s.dll, ranged runtime %.2f MB\n", bits, cpuVariant().c_str(),
                runtimeRangedBytes(cpuVariant()) / 1048576.0);
    check(cpuVariant() == cpuVariantFor(bits), "cpuVariant() == cpuVariantFor(cpuFeatureBits())");
}

int fetch(const std::wstring& dir, const std::string& expect) {
    SetEnvironmentVariableW(L"PM_MODELS_DIR", dir.c_str());
    SetEnvironmentVariableW(L"PM_LLAMA_DIR", nullptr);
    std::printf("runtime dir %ls, installed before %d\n", runtimeDir().c_str(), runtimeInstalled());
    const DWORD t0 = GetTickCount();
    std::wstring err;
    const bool ok = downloadRuntime(nullptr, nullptr, &err);
    const RuntimeFetchInfo i = lastRuntimeFetch();
    std::printf("download %s%ls, %.1f s: variant %s, %s, %.2f MB fetched\n", ok ? "ok" : "FAILED: ", err.c_str(),
                (GetTickCount() - t0) / 1000.0, i.variant.c_str(),
                i.ranged ? "ranged" : i.fellBack ? "whole archive (fallback)" : "nothing fetched", i.bytes / 1048576.0);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((runtimeDir() + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) std::printf("  %ls %lu\n", fd.cFileName, fd.nFileSizeLow);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (expect == "fail") {  // every source broken: an error, nothing installed
        check(!ok && err == L"SHA-256" && !runtimeInstalled(), "failed with SHA-256, nothing installed");
        return 0;
    }
    check(ok && runtimeInstalled(), "runtime installed");
    if (!expect.empty()) {
        const std::string how = i.ranged ? "ranged" : i.fellBack ? "whole" : "none";
        check(how == expect, "fetched " + how + " (want " + expect + ")");
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    std::string expect;
    for (int k = 1; k + 1 < argc; ++k)
        if (std::wstring(argv[k]) == L"--expect") {
            const std::wstring e = argv[k + 1];
            for (const wchar_t c : e) expect += static_cast<char>(c);
        }
    if (argc >= 3 && std::wstring(argv[1]) == L"--fetch") fetch(argv[2], expect);
    else offline();
    std::printf("%s (%d failed)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
