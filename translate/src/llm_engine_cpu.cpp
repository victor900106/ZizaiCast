// Which ggml-cpu-<variant>.dll this PC's ggml would load (llm_engine.h:
// cpuVariant), so the runtime download can fetch just that one (+ the x64
// fallback) instead of all 14.  Mirrors ggml's own selection in
// ggml/src/ggml-cpu/arch/x86/cpu-feats.cpp (release b11514): every variant has
// a list of required CPU features and a score (1 + a bit per feature); a
// variant is usable when the CPU has all its features, and ggml loads the
// usable one with the highest score.  The feature sets are those of
// ggml/src/CMakeLists.txt (GGML_CPU_ALL_VARIANTS).  Like ggml this reads the
// CPUID bits only (no XCR0 check), so the pick is the one ggml makes.
#include <intrin.h>
#include <windows.h>

#include <cstring>

#include "llm_engine.h"

namespace pm::translate::llm {

namespace {

// Feature bits in the order of ggml's score (weights 1 << n).
enum Feat : unsigned {
    kFma = 1u << 0, kF16c = 1u << 1, kSse42 = 1u << 2, kBmi2 = 1u << 3, kAvx = 1u << 4, kAvx2 = 1u << 5,
    kAvxVnni = 1u << 6, kAvx512 = 1u << 7, kVbmi = 1u << 8, kBf16 = 1u << 9, kVnni = 1u << 10, kAmxInt8 = 1u << 11,
};
constexpr unsigned kHaswell = kSse42 | kAvx | kF16c | kFma | kAvx2 | kBmi2;
constexpr unsigned kSkylake = kHaswell | kAvx512;

struct Variant {
    const char* name;
    unsigned need;
};
// Order does not matter: the highest score wins (score = 1 + need, as every feature weighs its own bit).
constexpr Variant kVariants[] = {
    {"x64", 0},
    {"sse42", kSse42},
    {"sandybridge", kSse42 | kAvx},
    {"ivybridge", kSse42 | kAvx | kF16c},
    {"piledriver", kSse42 | kAvx | kF16c | kFma},
    {"haswell", kHaswell},
    {"alderlake", kHaswell | kAvxVnni},
    {"skylakex", kSkylake},
    {"cannonlake", kSkylake | kVbmi},
    {"cascadelake", kSkylake | kVnni},
    {"icelake", kSkylake | kVbmi | kVnni},
    {"cooperlake", kSkylake | kVnni | kBf16},
    {"zen4", kSkylake | kVbmi | kVnni | kBf16},
    {"sapphirerapids", kSkylake | kVbmi | kVnni | kBf16 | kAmxInt8},
};

unsigned bit(unsigned v, int n) { return (v >> n) & 1u; }

}  // namespace

unsigned cpuFeatureBits() {
    int r[4] = {};
    __cpuid(r, 0);
    const int nIds = r[0];
    if (nIds < 1) return 0;
    __cpuidex(r, 1, 0);
    const unsigned c1 = static_cast<unsigned>(r[2]);
    unsigned f = 0;
    if (bit(c1, 12)) f |= kFma;
    if (bit(c1, 29)) f |= kF16c;
    if (bit(c1, 20)) f |= kSse42;
    if (bit(c1, 28)) f |= kAvx;
    if (nIds < 7) return f;
    __cpuidex(r, 7, 0);
    const unsigned b7 = static_cast<unsigned>(r[1]), c7 = static_cast<unsigned>(r[2]), d7 = static_cast<unsigned>(r[3]);
    __cpuidex(r, 7, 1);
    const unsigned a71 = static_cast<unsigned>(r[0]);
    if (bit(b7, 8)) f |= kBmi2;
    if (bit(b7, 5)) f |= kAvx2;
    if (bit(a71, 4)) f |= kAvxVnni;
    // AVX512 = F + CD + VL + DQ + BW
    if (bit(b7, 16) && bit(b7, 28) && bit(b7, 31) && bit(b7, 17) && bit(b7, 30)) f |= kAvx512;
    if (bit(c7, 1)) f |= kVbmi;
    if (bit(a71, 5)) f |= kBf16;
    if (bit(c7, 11)) f |= kVnni;
    if (bit(d7, 25)) f |= kAmxInt8;
    return f;
}

std::string cpuVariantFor(unsigned features) {
    const char* best = "x64";
    int bestScore = 0;
    for (const auto& v : kVariants) {
        if ((features & v.need) != v.need) continue;  // ggml: score 0, not loadable
        const int score = 1 + static_cast<int>(v.need);
        if (score > bestScore) {
            bestScore = score;
            best = v.name;
        }
    }
    return best;
}

std::string cpuVariant() {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) return "x64";
    return cpuVariantFor(cpuFeatureBits());
}

}  // namespace pm::translate::llm
