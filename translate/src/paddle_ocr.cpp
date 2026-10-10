// PaddleOCR text detection + recognition through ONNX Runtime (C API,
// onnxruntime.dll loaded at run time from the executable's folder).
//
//   detection  PP-OCRv6_det_tiny (DB): probability map -> connected regions
//              -> minimum-area rectangles (tilted lines), unclipped
//   recognition PP-OCRv6_rec_small (CTC, 18,708 characters: Chinese
//              Simplified / Traditional, Japanese kana + kanji, Latin, …);
//              lines that look Korean (empty / unsure / too few characters
//              for their width) again with PP-OCRv5 korean_rec_mobile
//
// No OpenCV: the few image operations (resize, perspective crop, contour
// rectangle) are done here.  See docs/translate.md *OCR*.
#include <windows.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "onnxruntime_c_api.h"
#include "pm/translate.h"
#include "text_util.h"

namespace pm::translate {

namespace {

constexpr char kDetModel[] = "PP-OCRv6_det_tiny.onnx";
constexpr char kRecModel[] = "PP-OCRv6_rec_small.onnx";
constexpr char kKoModel[] = "korean_PP-OCRv5_rec_mobile.onnx";

// Detection: longest side of the network input (multiple of 32) and the DB
// post-processing thresholds (measured on translate/testdata/make_photos.py;
// 0.7.1: 1280 instead of 960 for labels seen at 1x, CER 47.6 -> 32.9 % on
// the 1x + 2x set for +30 ms).
int kDetLimit = 1280;
float kDetThresh = 0.3f, kBoxThresh = 0.5f, kUnclip = 1.6f;
// Test overrides: PM_OCR_DET="limit thresh box unclip" (read for every
// picture: the tests switch settings between runs, as do the other PM_OCR_*).
void detParams() {
    kDetLimit = 1280, kDetThresh = 0.3f, kBoxThresh = 0.5f, kUnclip = 1.6f;
    if (const char* e = std::getenv("PM_OCR_DET")) sscanf_s(e, "%d %f %f %f", &kDetLimit, &kDetThresh, &kBoxThresh, &kUnclip);
}
int envInt(const char* name, int def) {
    const char* e = std::getenv(name);
    return e && *e ? atoi(e) : def;
}
constexpr int kRecH = 48, kRecMaxW = 3200, kRecBatch = 8;

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Tests (PM_OCR_PROF): where one picture's time goes.
struct Prof {
    double detPre = 0, detRun = 0, detPost = 0, cache = 0, probe = 0, main = 0, ko = 0, more = 0, early = 0;
    int detW = 0, detH = 0, cached = 0, nProbe = 0, nMain = 0, nKo = 0, nMore = 0, batches = 0;
    double realPx = 0, paddedPx = 0;  // recognition input columns: crops' own / with batch padding
};
Prof g_prof;

std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

// ---- ONNX Runtime ----
struct Runtime {
    HMODULE dll = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    std::wstring error;
    bool tried = false;
    bool gpu = false;        // the DirectML runtime (models\ocr-gpu) is loaded
    bool gpuBroken = false;  // it failed once: the CPU runtime for the rest of the session
};

// The discrete GPU for DirectML: NVIDIA / AMD / Intel, >= 3 GB of its own
// video memory, not software (the LLM's Vulkan add-on uses the same rule).
// Its DXGI adapter index (DirectML's device_id), -1 = none.  Once per process.
int discreteAdapter() {
    static const int idx = [] {
        HMODULE dx = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dx) return -1;
        using Create = HRESULT(WINAPI*)(REFIID, void**);
        auto create = reinterpret_cast<Create>(GetProcAddress(dx, "CreateDXGIFactory1"));
        IDXGIFactory1* f = nullptr;
        int found = -1;
        if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) && f) {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d{};
                if (found < 0 && SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    (d.VendorId == 0x10DE || d.VendorId == 0x1002 || d.VendorId == 0x8086) && d.DedicatedVideoMemory >= (3ull << 30))
                    found = static_cast<int>(i);
                a->Release();
            }
            f->Release();
        }
        return found;
    }();
    return idx;
}

std::wstring envW(const wchar_t* name) {
    wchar_t b[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(name, b, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(b, n) : std::wstring();
}
Runtime& rt() {
    static Runtime r;
    return r;
}
std::mutex g_rtM;

bool loadRuntimeOnce(Runtime& r);
bool loadRuntime(std::wstring* err) {
    std::lock_guard lk(g_rtM);
    Runtime& r = rt();
    bool ok = loadRuntimeOnce(r);
    if (!ok && r.gpu && !r.gpuBroken) {  // the GPU add-on does not load: the CPU runtime
        r.gpuBroken = true;
        r.tried = false;
        r.dll = nullptr;
        ok = loadRuntimeOnce(r);
    }
    if (!ok && err) *err = r.error;
    return ok;
}
bool loadRuntimeOnce(Runtime& r) {
    if (!r.tried) {
        r.tried = true;
        wchar_t env[MAX_PATH];
        std::wstring path;
        r.gpu = false;
        if (DWORD n = GetEnvironmentVariableW(L"PM_ONNXRUNTIME_DLL", env, MAX_PATH); n > 0 && n < MAX_PATH) {
            path = env;
            r.gpu = envW(L"PM_OCR_GPU") == L"on";  // tests: the given dll is the DirectML build
        } else if (!r.gpuBroken && PaddleOcr::gpuWanted() && ModelStore::ocrGpuInstalled()) {
            // The GPU add-on: its own onnxruntime (the DirectML build, 1.24) and
            // DirectML.dll from the same folder.
            path = ModelStore::ocrGpuDir() + L"\\onnxruntime.dll";
            r.gpu = true;
        } else {
            path = exeDir() + L"\\onnxruntime.dll";
        }
        // Full path + altered search path: never the (different) System32 copy;
        // its own imports (msvcp140_1 …) come from the same folder.
        r.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!r.dll) {
            r.error = L"onnxruntime.dll not loadable (" + path + L", error " + std::to_wstring(GetLastError()) + L")";
        } else {
            using GetBase = const OrtApiBase*(ORT_API_CALL*)();
            auto base = reinterpret_cast<GetBase>(GetProcAddress(r.dll, "OrtGetApiBase"));
            const bool v124 = base && std::string(base()->GetVersionString()).rfind("1.24", 0) == 0;
            r.api = base && !v124 ? base()->GetApi(ORT_API_VERSION) : nullptr;
            // The DirectML build of onnxruntime stops at 1.24: only functions of
            // API 24 are used (checked: the OCR runs on it, CER unchanged).
            if (!r.api && base && (r.gpu || std::getenv("PM_OCR_DML"))) r.api = base()->GetApi(24);
            if (!r.api) r.error = L"onnxruntime.dll: API version " + std::to_wstring(ORT_API_VERSION) + L" not supported";
            else if (OrtStatus* st = r.api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "pm_translate", &r.env)) {
                r.error = L"onnxruntime: " + fromUtf8(r.api->GetErrorMessage(st));
                r.api->ReleaseStatus(st);
                r.api = nullptr;
            }
        }
    }
    return r.api != nullptr;
}

// Throws on error (caught in recognize()).
struct OrtError {
    std::wstring msg;
};
void check(OrtStatus* st) {
    if (!st) return;
    OrtError e{L"onnxruntime: " + fromUtf8(rt().api->GetErrorMessage(st))};
    rt().api->ReleaseStatus(st);
    throw e;
}

struct Session {
    OrtSession* s = nullptr;
    std::string in, out;
    std::vector<std::wstring> chars;  // recognisers: [0] blank, dictionary, last = space
    std::string name;                 // model file (tests)
    ~Session() {
        if (s) rt().api->ReleaseSession(s);
    }
};

std::unique_ptr<Session> openSession(const char* file, bool rec) {
    const OrtApi& a = *rt().api;
    const std::wstring path = ModelStore::ocrFile(file);
    if (path.empty()) throw OrtError{L"OCR model missing: " + fromUtf8(file)};
    OrtSessionOptions* so = nullptr;
    check(a.CreateSessionOptions(&so));
    int threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency() / 4, 2u, 4u));  // measured: more is slower
    if (const char* e = std::getenv("PM_OCR_THREADS"); e && atoi(e) > 0) threads = atoi(e);  // tests
    if (!rec && envInt("PM_OCR_DET_THREADS", 0) > 0) threads = envInt("PM_OCR_DET_THREADS", 0);
    a.SetIntraOpNumThreads(so, threads);
    a.SetInterOpNumThreads(so, std::max(1, envInt("PM_OCR_INTER", 1)));  // tests
    if (envInt("PM_OCR_INTER", 1) > 1) a.SetSessionExecutionMode(so, ORT_PARALLEL);
    if (const char* sp = std::getenv("PM_OCR_SPIN"); sp && *sp)
        if (OrtStatus* st = a.AddSessionConfigEntry(so, "session.intra_op.allow_spinning", sp)) a.ReleaseStatus(st);
    a.SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
    // GPU through DirectML: the add-on's runtime (PaddleOcr::gpuWanted), or
    // tests: PM_OCR_DML=1 (device 0) / =2N (device N) with the DirectML build
    // of onnxruntime.dll + DirectML.dll next to the exe.  A failure throws:
    // recognize() then goes back to the CPU runtime.
    const char* d = std::getenv("PM_OCR_DML");
    if (rt().gpu || (d && *d != '0')) {
        a.DisableMemPattern(so);
        a.SetSessionExecutionMode(so, ORT_SEQUENTIAL);
        std::string dev = std::to_string(std::max(0, discreteAdapter()));
        if (d && *d != '0') dev = d[0] == '1' ? "0" : d + 1;
        const char* keys[] = {"device_id"};
        const char* vals[] = {dev.c_str()};
        if (OrtStatus* st = a.SessionOptionsAppendExecutionProvider(so, "DML", keys, vals, 1)) {
            const std::wstring m = L"DirectML: " + fromUtf8(a.GetErrorMessage(st));
            a.ReleaseStatus(st);
            a.ReleaseSessionOptions(so);
            if (rt().gpu) throw OrtError{m};
            std::fprintf(stderr, "[ocr] %ls\n", m.c_str());
        }
    }
    auto sess = std::make_unique<Session>();
    sess->name = file;
    OrtStatus* st = a.CreateSession(rt().env, path.c_str(), so, &sess->s);
    a.ReleaseSessionOptions(so);
    check(st);
    OrtAllocator* al = nullptr;
    check(a.GetAllocatorWithDefaultOptions(&al));
    char* name = nullptr;
    check(a.SessionGetInputName(sess->s, 0, al, &name));
    sess->in = name;
    a.AllocatorFree(al, name);
    check(a.SessionGetOutputName(sess->s, 0, al, &name));
    sess->out = name;
    a.AllocatorFree(al, name);
    if (rec) {
        OrtModelMetadata* md = nullptr;
        check(a.SessionGetModelMetadata(sess->s, &md));
        char* v = nullptr;
        OrtStatus* s2 = a.ModelMetadataLookupCustomMetadataMap(md, al, "character", &v);
        a.ReleaseModelMetadata(md);
        check(s2);
        if (!v) throw OrtError{L"OCR model without a character list: " + fromUtf8(file)};
        const std::wstring dict = fromUtf8(v);
        a.AllocatorFree(al, v);
        sess->chars.push_back(L"");  // CTC blank
        size_t p = 0;
        for (;;) {
            const size_t nl = dict.find(L'\n', p);
            std::wstring c = dict.substr(p, nl == std::wstring::npos ? std::wstring::npos : nl - p);
            if (!c.empty() && c.back() == L'\r') c.pop_back();
            sess->chars.push_back(c.empty() ? L" " : c);  // the dictionary's space entry is stored empty
            if (nl == std::wstring::npos) break;
            p = nl + 1;
        }
        sess->chars.push_back(L" ");  // use_space_char
    }
    return sess;
}

struct Models {
    std::unique_ptr<Session> det, rec, ko;
    std::vector<std::unique_ptr<Session>> recMore, koMore;  // parallel recognisers (recognizeCrops)
};
Models g_models;
std::mutex g_modelsM;  // one recognition at a time

// Runs a float tensor [n, 3, h, w] through s; output shape + data.
std::vector<float> run(Session& s, std::vector<float>& input, const int64_t shape[4], std::vector<int64_t>& outShape) {
    const OrtApi& a = *rt().api;
    OrtMemoryInfo* mi = nullptr;
    check(a.CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi));
    OrtValue* in = nullptr;
    OrtStatus* st = a.CreateTensorWithDataAsOrtValue(mi, input.data(), input.size() * sizeof(float), shape, 4,
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in);
    a.ReleaseMemoryInfo(mi);
    check(st);
    const char* inNames[] = {s.in.c_str()};
    const char* outNames[] = {s.out.c_str()};
    OrtValue* out = nullptr;
    st = a.Run(s.s, nullptr, inNames, &in, 1, outNames, 1, &out);
    a.ReleaseValue(in);
    check(st);
    std::vector<float> data;
    try {
        OrtTensorTypeAndShapeInfo* info = nullptr;
        check(a.GetTensorTypeAndShape(out, &info));
        size_t dims = 0;
        a.GetDimensionsCount(info, &dims);
        outShape.assign(dims, 0);
        a.GetDimensions(info, outShape.data(), dims);
        a.ReleaseTensorTypeAndShapeInfo(info);
        size_t n = 1;
        for (int64_t d : outShape) n *= static_cast<size_t>(std::max<int64_t>(d, 0));
        float* p = nullptr;
        check(a.GetTensorMutableData(out, reinterpret_cast<void**>(&p)));
        data.assign(p, p + n);
    } catch (...) {
        a.ReleaseValue(out);
        throw;
    }
    a.ReleaseValue(out);
    return data;
}

// ---- Geometry ----
struct Pt {
    float x, y;
};
float cross(Pt o, Pt a, Pt b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

std::vector<Pt> convexHull(std::vector<Pt> p) {
    std::sort(p.begin(), p.end(), [](Pt a, Pt b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    p.erase(std::unique(p.begin(), p.end(), [](Pt a, Pt b) { return a.x == b.x && a.y == b.y; }), p.end());
    if (p.size() < 3) return p;
    std::vector<Pt> h(2 * p.size());
    size_t k = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
        h[k++] = p[i];
    }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0) --k;
        h[k++] = p[i - 1];
    }
    h.resize(k - 1);
    return h;
}

// Minimum-area rectangle of a convex polygon (rotating calipers over its edges).
struct RRect {
    Pt c;
    float w, h;  // along u, along v
    Pt u, v;     // unit axes
};
RRect minAreaRect(const std::vector<Pt>& hull) {
    RRect best{{0, 0}, 0, 0, {1, 0}, {0, 1}};
    float bestArea = 1e30f;
    for (size_t i = 0; i < hull.size(); ++i) {
        const Pt a = hull[i], b = hull[(i + 1) % hull.size()];
        float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) continue;
        dx /= len, dy /= len;
        float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
        for (const Pt& p : hull) {
            const float pu = p.x * dx + p.y * dy, pv = -p.x * dy + p.y * dx;
            minU = std::min(minU, pu), maxU = std::max(maxU, pu), minV = std::min(minV, pv), maxV = std::max(maxV, pv);
        }
        const float area = (maxU - minU) * (maxV - minV);
        if (area < bestArea) {
            bestArea = area;
            const float cu = (minU + maxU) / 2, cv = (minV + maxV) / 2;
            best.u = {dx, dy};
            best.v = {-dy, dx};
            best.c = {cu * dx - cv * dy, cu * dy + cv * dx};
            best.w = maxU - minU;
            best.h = maxV - minV;
        }
    }
    // u along the longer side (the reading direction of a horizontal line).
    if (best.h > best.w) {
        std::swap(best.w, best.h);
        best.u = {best.v.x, best.v.y};
        best.v = {-best.u.y, best.u.x};
    }
    if (best.u.x < 0 || (best.u.x == 0 && best.u.y < 0)) best.u = {-best.u.x, -best.u.y}, best.v = {-best.v.x, -best.v.y};
    if (best.v.y < 0) best.v = {-best.v.x, -best.v.y};
    return best;
}

float polyArea(const std::vector<Pt>& p) {
    float a = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const Pt& q = p[i];
        const Pt& r = p[(i + 1) % p.size()];
        a += q.x * r.y - r.x * q.y;
    }
    return std::fabs(a) / 2;
}
float polyPerimeter(const std::vector<Pt>& p) {
    float s = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const Pt& q = p[i];
        const Pt& r = p[(i + 1) % p.size()];
        s += std::hypot(r.x - q.x, r.y - q.y);
    }
    return s;
}

// A detected line: quad TL TR BR BL in picture pixels (TL->TR = reading
// direction of a horizontal line).
struct Quad {
    Pt p[4];
    float w() const { return std::max(std::hypot(p[1].x - p[0].x, p[1].y - p[0].y), std::hypot(p[2].x - p[3].x, p[2].y - p[3].y)); }
    float h() const { return std::max(std::hypot(p[3].x - p[0].x, p[3].y - p[0].y), std::hypot(p[2].x - p[1].x, p[2].y - p[1].y)); }
};

// f(0 .. n-1) on a few threads, grain items at a time (picture work before /
// after the networks: resizing, the line crops).  PM_OCR_CROPT: threads (tests).
template <class F>
void parallelFor(size_t n, size_t grain, F&& f) {
    const int nt = envInt("PM_OCR_CROPT", 0) > 0 ? envInt("PM_OCR_CROPT", 0)
                                                  : static_cast<int>(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u));
    const size_t chunks = (n + grain - 1) / grain;
    const size_t k = std::min<size_t>(static_cast<size_t>(nt), chunks);
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t c; (c = next++) < chunks;)
            for (size_t i = c * grain; i < std::min(n, (c + 1) * grain); ++i) f(i);
    };
    std::vector<std::thread> th;
    for (size_t t = 1; t < k; ++t) th.emplace_back(work);
    work();
    for (auto& t : th) t.join();
}

// ---- Detection ----
std::vector<Quad> detect(Session& det, const uint8_t* bgra, int w, int h) {
    detParams();
    const double t0 = nowMs();
    const float r = std::min(1.f, static_cast<float>(kDetLimit) / std::max(w, h));
    const int nw = std::max(32, static_cast<int>(std::lround(w * r / 32.f)) * 32);
    const int nh = std::max(32, static_cast<int>(std::lround(h * r / 32.f)) * 32);
    // Area-averaged resize (box filter over the source pixels of each target
    // pixel), BGR planes normalised like PaddleOCR (mean / std on BGR order).
    std::vector<float> in(static_cast<size_t>(3) * nw * nh);
    const float sx = static_cast<float>(w) / nw, sy = static_cast<float>(h) / nh;
    static const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
    // Rows on a few threads (~30 ms for a phone screenshot on one).
    parallelFor(static_cast<size_t>(nh), 16, [&](size_t row) {
        const int y = static_cast<int>(row);
        const int ya = static_cast<int>(y * sy), yb = std::max(ya + 1, std::min(h, static_cast<int>(std::ceil((y + 1) * sy))));
        for (int x = 0; x < nw; ++x) {
            const int xa = static_cast<int>(x * sx), xb = std::max(xa + 1, std::min(w, static_cast<int>(std::ceil((x + 1) * sx))));
            float acc[3] = {0, 0, 0};
            const int stepY = std::max(1, (yb - ya) / 3), stepX = std::max(1, (xb - xa) / 3);
            int n = 0;
            for (int yy = ya; yy < yb; yy += stepY)
                for (int xx = xa; xx < xb; xx += stepX) {
                    const uint8_t* q = bgra + (static_cast<size_t>(yy) * w + xx) * 4;
                    acc[0] += q[0], acc[1] += q[1], acc[2] += q[2];
                    ++n;
                }
            for (int c = 0; c < 3; ++c)
                in[(static_cast<size_t>(c) * nh + y) * nw + x] = (acc[c] / n / 255.f - mean[c]) / stdv[c];
        }
    });
    const int64_t shape[4] = {1, 3, nh, nw};
    std::vector<int64_t> os;
    const double tRun = nowMs();
    g_prof.detPre = tRun - t0, g_prof.detW = nw, g_prof.detH = nh;
    const std::vector<float> prob = run(det, in, shape, os);
    g_prof.detRun = nowMs() - tRun;
    if (os.size() != 4 || os[2] != nh || os[3] != nw) throw OrtError{L"unexpected detection output"};
    // Connected regions of prob > kDetThresh (8-neighbourhood).
    std::vector<int> label(static_cast<size_t>(nw) * nh, 0);
    std::vector<Quad> out;
    std::vector<int> stack;
    int next = 0;
    for (int y0 = 0; y0 < nh; ++y0)
        for (int x0 = 0; x0 < nw; ++x0) {
            const size_t i0 = static_cast<size_t>(y0) * nw + x0;
            if (label[i0] || prob[i0] <= kDetThresh) continue;
            ++next;
            label[i0] = next;
            stack.assign(1, static_cast<int>(i0));
            std::vector<int> rowMin(nh, INT_MAX), rowMax(nh, -1);
            double sum = 0;
            int count = 0, top = y0, bottom = y0;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                const int x = i % nw, y = i / nw;
                sum += prob[i];
                ++count;
                rowMin[y] = std::min(rowMin[y], x), rowMax[y] = std::max(rowMax[y], x);
                top = std::min(top, y), bottom = std::max(bottom, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= nw || yy >= nh) continue;
                        const size_t j = static_cast<size_t>(yy) * nw + xx;
                        if (!label[j] && prob[j] > kDetThresh) {
                            label[j] = next;
                            stack.push_back(static_cast<int>(j));
                        }
                    }
            }
            if (count < 4 || sum / count < kBoxThresh) continue;
            std::vector<Pt> pts;
            for (int y = top; y <= bottom; ++y) {
                if (rowMax[y] < 0) continue;
                // Pixel centres, like a contour through the region's border pixels.
                pts.push_back({static_cast<float>(rowMin[y]), static_cast<float>(y)});
                pts.push_back({static_cast<float>(rowMax[y]), static_cast<float>(y)});
            }
            const std::vector<Pt> hull = convexHull(pts);
            if (hull.size() < 3) continue;
            RRect rr = minAreaRect(hull);
            if (std::min(rr.w, rr.h) < 2) continue;
            // Unclip (DB): grow by area * ratio / perimeter.
            const float d = polyArea(hull) * kUnclip / std::max(1.f, polyPerimeter(hull));
            rr.w += 2 * d, rr.h += 2 * d;
            if (std::min(rr.w, rr.h) < 5) continue;
            Quad q;
            const float hw = rr.w / 2, hh = rr.h / 2;
            const Pt c = rr.c, u = rr.u, v = rr.v;
            q.p[0] = {c.x - u.x * hw - v.x * hh, c.y - u.y * hw - v.y * hh};
            q.p[1] = {c.x + u.x * hw - v.x * hh, c.y + u.y * hw - v.y * hh};
            q.p[2] = {c.x + u.x * hw + v.x * hh, c.y + u.y * hw + v.y * hh};
            q.p[3] = {c.x - u.x * hw + v.x * hh, c.y - u.y * hw + v.y * hh};
            // Back to picture pixels (pixel centre convention).
            for (Pt& p : q.p) {
                p.x = std::clamp((p.x + 0.5f) * w / nw, 0.f, static_cast<float>(w));
                p.y = std::clamp((p.y + 0.5f) * h / nh, 0.f, static_cast<float>(h));
            }
            // Corners in picture order (PaddleOCR get_mini_boxes): the two
            // leftmost are TL / BL by y, the two rightmost TR / BR.  A column
            // then has a tall quad (read as vertical text).
            std::sort(std::begin(q.p), std::end(q.p), [](Pt a, Pt b) { return a.x < b.x; });
            const Pt l0 = q.p[0].y <= q.p[1].y ? q.p[0] : q.p[1], l1 = q.p[0].y <= q.p[1].y ? q.p[1] : q.p[0];
            const Pt r0 = q.p[2].y <= q.p[3].y ? q.p[2] : q.p[3], r1 = q.p[2].y <= q.p[3].y ? q.p[3] : q.p[2];
            q.p[0] = l0, q.p[1] = r0, q.p[2] = r1, q.p[3] = l1;
            if (q.w() < 4 || q.h() < 4) continue;
            out.push_back(q);
        }
    g_prof.detPost = nowMs() - t0 - g_prof.detPre - g_prof.detRun;
    return out;
}

// ---- Recognition ----
// Homography mapping the unit square corners (0,0) (1,0) (1,1) (0,1) to a quad.
struct Homography {
    double m[9];
    Pt map(double u, double v) const {
        const double z = m[6] * u + m[7] * v + m[8];
        return {static_cast<float>((m[0] * u + m[1] * v + m[2]) / z), static_cast<float>((m[3] * u + m[4] * v + m[5]) / z)};
    }
};
Homography squareTo(const Pt q[4]) {
    // Heckbert's closed form.
    const double x0 = q[0].x, y0 = q[0].y, x1 = q[1].x, y1 = q[1].y, x2 = q[2].x, y2 = q[2].y, x3 = q[3].x, y3 = q[3].y;
    const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
    const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    Homography H;
    double g = 0, hh = 0;
    if (std::fabs(sx) > 1e-9 || std::fabs(sy) > 1e-9) {
        const double den = dx1 * dy2 - dx2 * dy1;
        if (std::fabs(den) > 1e-12) {
            g = (sx * dy2 - dx2 * sy) / den;
            hh = (dx1 * sy - sx * dy1) / den;
        }
    }
    H.m[0] = x1 - x0 + g * x1;
    H.m[1] = x3 - x0 + hh * x3;
    H.m[2] = x0;
    H.m[3] = y1 - y0 + g * y1;
    H.m[4] = y3 - y0 + hh * y3;
    H.m[5] = y0;
    H.m[6] = g;
    H.m[7] = hh;
    H.m[8] = 1;
    return H;
}

struct Crop {
    std::vector<float> px;  // 3 x kRecH x w, normalised
    int w = 0;
    float aspect = 0;  // text width / height as read
    bool vertical = false;
    float bg = 0;      // mean brightness 0..1 (mostly background)
};

// The quad straightened to kRecH pixels high (BGR, (x/255 - 0.5) / 0.5);
// columns (height >= 1.5 width) are turned a quarter left, as PaddleOCR does.
Crop makeCrop(const uint8_t* bgra, int w, int h, const Quad& q) {
    Crop c;
    Pt src[4] = {q.p[0], q.p[1], q.p[2], q.p[3]};
    float qw = q.w(), qh = q.h();
    if (qh >= 1.5f * qw) {
        c.vertical = true;
        // rot90: new TL = TR, new TR = BR, new BR = BL, new BL = TL.
        const Pt t[4] = {q.p[1], q.p[2], q.p[3], q.p[0]};
        std::copy(t, t + 4, src);
        std::swap(qw, qh);
    }
    c.aspect = qw / std::max(1.f, qh);
    c.w = std::clamp(static_cast<int>(std::ceil(kRecH * c.aspect)), 8, kRecMaxW);
    c.px.assign(static_cast<size_t>(3) * kRecH * c.w, 0.f);
    const Homography H = squareTo(src);
    // Supersampling when shrinking (big text): k x k samples per pixel.
    const int k = std::clamp(static_cast<int>(std::ceil(qh / kRecH)), 1, 4);
    const size_t plane = static_cast<size_t>(kRecH) * c.w;
    double lum = 0;
    for (int y = 0; y < kRecH; ++y)
        for (int x = 0; x < c.w; ++x) {
            float acc[3] = {0, 0, 0};
            for (int sy = 0; sy < k; ++sy)
                for (int sx = 0; sx < k; ++sx) {
                    const Pt p = H.map((x + (sx + 0.5) / k) / c.w, (y + (sy + 0.5) / k) / kRecH);
                    const float fx = std::clamp(p.x - 0.5f, 0.f, w - 1.f), fy = std::clamp(p.y - 0.5f, 0.f, h - 1.f);
                    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
                    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
                    const float tx = fx - x0, ty = fy - y0;
                    const uint8_t* a = bgra + (static_cast<size_t>(y0) * w + x0) * 4;
                    const uint8_t* b = bgra + (static_cast<size_t>(y0) * w + x1) * 4;
                    const uint8_t* cc = bgra + (static_cast<size_t>(y1) * w + x0) * 4;
                    const uint8_t* d = bgra + (static_cast<size_t>(y1) * w + x1) * 4;
                    for (int ch = 0; ch < 3; ++ch)
                        acc[ch] += (a[ch] * (1 - tx) + b[ch] * tx) * (1 - ty) + (cc[ch] * (1 - tx) + d[ch] * tx) * ty;
                }
            for (int ch = 0; ch < 3; ++ch)
                c.px[ch * plane + static_cast<size_t>(y) * c.w + x] = (acc[ch] / (k * k) / 255.f - 0.5f) / 0.5f;
            lum += (0.114 * acc[0] + 0.587 * acc[1] + 0.299 * acc[2]) / (k * k) / 255.0;
        }
    c.bg = static_cast<float>(lum / (static_cast<double>(kRecH) * c.w));
    return c;
}

struct RecOut {
    std::wstring text;
    float conf = 0;
    std::vector<float> pos;  // per character of text: its centre along the crop (0..1), from the CTC time step
};

// Recognition sessions run side by side (each with its own intra-op
// threads): 0.7.1, measured on the owner's label (60 lines, i7-13700K with
// games running): warm OCR 1.31-2.02 s with one session, 1.03-1.58 s with
// two; three were slower again.  PM_OCR_PAR overrides (tests).
int recParallel() {
    const int n = envInt("PM_OCR_PAR", 0);
    if (n > 0) return std::min(8, n);
    return std::thread::hardware_concurrency() >= 8 ? 2 : 1;
}

// The batch's input width for crops of at most maxW columns.  Tests with a
// GPU: in steps of 128 (DirectML builds its graph again for every new shape).
int batchWidth(int maxW) {
    int bw = std::max(maxW, 64);
    static const bool bucket = std::getenv("PM_OCR_DML") || std::getenv("PM_OCR_BUCKET");
    if (bucket) bw = (bw + 127) / 128 * 128;
    return bw;
}

// CTC greedy decoding of one batch: crops order[b .. b + n).
void recognizeBatch(Session& rec, const std::vector<Crop>& crops, const std::vector<size_t>& order, size_t b, size_t n,
                    std::vector<RecOut>& res) {
    int bw = 0;
    for (size_t k = 0; k < n; ++k) bw = std::max(bw, crops[order[b + k]].w);
    bw = batchWidth(bw);
    const size_t plane = static_cast<size_t>(kRecH) * bw;
    std::vector<float> in(n * 3 * plane, 0.f);
    for (size_t k = 0; k < n; ++k) {
        const Crop& c = crops[order[b + k]];
        for (int ch = 0; ch < 3; ++ch)
            for (int y = 0; y < kRecH; ++y)
                std::memcpy(&in[(k * 3 + ch) * plane + static_cast<size_t>(y) * bw],
                            &c.px[(static_cast<size_t>(ch) * kRecH + y) * c.w], sizeof(float) * c.w);
    }
    const int64_t shape[4] = {static_cast<int64_t>(n), 3, kRecH, bw};
    std::vector<int64_t> os;
    const double tb = nowMs();
    {
        static std::mutex pm;
        std::lock_guard pl(pm);
        ++g_prof.batches;
        g_prof.paddedPx += static_cast<double>(n) * bw;
        for (size_t k = 0; k < n; ++k) g_prof.realPx += crops[order[b + k]].w;
    }
    const std::vector<float> p = run(rec, in, shape, os);
    static const bool prof = std::getenv("PM_OCR_PROF") != nullptr && std::getenv("PM_OCR_PROF")[0] == '2';
    if (prof) std::fprintf(stderr, "[rec] %s n %zu w %d: %.0f ms\n", rec.name.c_str(), n, bw, nowMs() - tb);
    if (os.size() != 3) throw OrtError{L"unexpected recognition output"};
    const size_t T = static_cast<size_t>(os[1]), C = static_cast<size_t>(os[2]);
    for (size_t k = 0; k < n; ++k) {
        const Crop& c = crops[order[b + k]];
        // Time steps beyond this crop's own width are padding.
        const size_t steps = std::min(T, static_cast<size_t>(std::ceil(static_cast<double>(T) * c.w / bw)) + 1);
        RecOut& r = res[order[b + k]];
        size_t last = 0;
        double sum = 0;
        int emitted = 0;
        // The crop's own time steps (the rest of the batch width is padding).
        const double own = std::max(1.0, static_cast<double>(T) * c.w / bw);
        std::vector<float> pos;
        for (size_t t = 0; t < steps; ++t) {
            const float* row = &p[(k * T + t) * C];
            const size_t am = static_cast<size_t>(std::max_element(row, row + C) - row);
            if (am != 0 && am != last && am < rec.chars.size()) {
                r.text += rec.chars[am];
                pos.resize(r.text.size(), static_cast<float>(std::min(1.0, (t + 0.5) / own)));
                sum += row[am];
                ++emitted;
            }
            last = am;
        }
        // Spaces: collapse runs, trim.
        std::wstring t;
        std::vector<float> tp;
        for (size_t i = 0; i < r.text.size(); ++i) {
            const wchar_t ch = r.text[i];
            if (ch == L' ' && (t.empty() || t.back() == L' ')) continue;
            t += ch;
            tp.push_back(i < pos.size() ? pos[i] : 1.f);
        }
        while (!t.empty() && t.back() == L' ') t.pop_back(), tp.pop_back();
        r.text = t;
        r.pos = std::move(tp);
        r.conf = emitted ? static_cast<float>(sum / emitted) : 0.f;
    }
}

// Batching (tests: PM_OCR_BATCH = most crops per batch, PM_OCR_PADCAP = the
// most padding a batch may add, as input columns over the crops' own;
// PM_OCR_SCHED=1: these, handed to whichever session is free; default 0:
// 0.7.7's fixed batches of 8 dealt out in turn - measured on eval_web (50
// pictures, E-cores): the free-session order was 3 % slower and changed 3
// pictures' text (other batches, other padding), so it stays a test).
int recBatchMax() { return std::max(1, envInt("PM_OCR_BATCH", kRecBatch)); }
float recPadCap() {
    const char* e = std::getenv("PM_OCR_PADCAP");
    return e && *e ? static_cast<float>(atof(e)) : 0.f;
}
bool recOldSchedule() { return envInt("PM_OCR_SCHED", 0) == 0; }

// Crops (batched by similar width) on the sessions in parallel.
// first (a part of which, e.g. the top of the picture): batched on its own and
// read before the others; onFirst(res) is called (on one of the recognition
// threads, the others going on) as soon as all of it is read.
std::vector<RecOut> recognizeCrops(const std::vector<Session*>& sessions, const std::vector<Crop>& crops,
                                   const std::vector<size_t>& which, const std::vector<size_t>* first = nullptr,
                                   const std::function<void(const std::vector<RecOut>&)>& onFirst = {}) {
    std::vector<RecOut> res(crops.size());
    auto byW = [&](size_t a, size_t b) { return crops[a].w < crops[b].w; };
    std::vector<size_t> order;
    size_t nFirst = 0;  // order[0, nFirst): the first group
    if (first && !first->empty()) {
        std::vector<char> in(crops.size(), 0);
        for (size_t i : *first) in[i] = 1;
        for (size_t i : which)
            if (in[i]) order.push_back(i);
        nFirst = order.size();
        for (size_t i : which)
            if (!in[i]) order.push_back(i);
        std::sort(order.begin(), order.begin() + nFirst, byW);
        std::sort(order.begin() + nFirst, order.end(), byW);
    } else {
        order = which;
        std::sort(order.begin(), order.end(), byW);
    }
    struct Batch {
        size_t b, n;
        double cost;
        int grp = 1;  // 0: the first group
    };
    std::vector<Batch> batches;
    auto costOf = [&](size_t b, size_t n) { return static_cast<double>(n) * batchWidth(crops[order[b + n - 1]].w); };
    if (recOldSchedule()) {
        // 0.7.7: batches of 8, the widest first, dealt out in turn.
        // With a first group: its batches come first (each group on its own).
        for (size_t g0 = 0, g1 = nFirst ? nFirst : order.size(), grp = nFirst ? 0 : 1; g0 < order.size();
             g0 = g1, g1 = order.size(), grp = 1) {
            const size_t at = batches.size();
            for (size_t b = g0; b < g1; b += kRecBatch)
                batches.push_back({b, std::min<size_t>(kRecBatch, g1 - b), 0, static_cast<int>(grp)});
            std::reverse(batches.begin() + at, batches.end());
        }
    } else {
        // Up to recBatchMax crops of similar width; a batch ends where the
        // next crop would make the padding exceed recPadCap.
        const size_t maxN = static_cast<size_t>(recBatchMax());
        const float cap = recPadCap();
        for (size_t b = 0; b < order.size();) {
            const size_t end = b < nFirst ? nFirst : order.size();  // batches never mix the groups
            size_t e = b;
            double own = 0;
            while (e < end && e - b < maxN) {
                const double w = batchWidth(crops[order[e]].w);
                if (e > b && cap > 0 && (e - b + 1) * w > cap * (own + w)) break;
                own += w;
                ++e;
            }
            batches.push_back({b, e - b, 0, b < nFirst ? 0 : 1});
            b = e;
        }
        // Every session gets work: the largest batches split in halves.
        while (batches.size() < sessions.size()) {
            auto big = std::max_element(batches.begin(), batches.end(), [](const Batch& x, const Batch& y) { return x.n < y.n; });
            if (big == batches.end() || big->n < 2) break;
            const Batch h{big->b + big->n / 2, big->n - big->n / 2, 0, big->grp};
            big->n /= 2;
            batches.push_back(h);
        }
        for (Batch& x : batches) x.cost = costOf(x.b, x.n);
        // The first group first; the costliest first, each session taking the
        // next when it is free.
        std::stable_sort(batches.begin(), batches.end(),
                         [](const Batch& x, const Batch& y) { return x.grp != y.grp ? x.grp < y.grp : x.cost > y.cost; });
    }
    const size_t nw = std::min(sessions.size(), batches.size());
    std::vector<OrtError> errors(nw);
    std::vector<int> failed(nw, 0);
    std::atomic<size_t> next{0};
    std::atomic<int> firstLeft{static_cast<int>(std::count_if(batches.begin(), batches.end(), [](const Batch& x) { return x.grp == 0; }))};
    auto work = [&](size_t wi) {
        try {
            if (recOldSchedule()) {
                for (size_t j = wi; j < batches.size(); j += nw) {
                    recognizeBatch(*sessions[wi], crops, order, batches[j].b, batches[j].n, res);
                    if (batches[j].grp == 0 && --firstLeft == 0 && onFirst) onFirst(res);
                }
            } else {
                for (size_t j; (j = next++) < batches.size();) {
                    recognizeBatch(*sessions[wi], crops, order, batches[j].b, batches[j].n, res);
                    if (batches[j].grp == 0 && --firstLeft == 0 && onFirst) onFirst(res);
                }
            }
        } catch (const OrtError& e) {
            errors[wi] = e;
            failed[wi] = 1;
        } catch (...) {
            errors[wi] = OrtError{L"recognition failed"};
            failed[wi] = 1;
        }
    };
    std::vector<std::thread> threads;
    for (size_t wi = 1; wi < nw; ++wi) threads.emplace_back(work, wi);
    if (nw) work(0);
    for (auto& t : threads) t.join();
    for (size_t wi = 0; wi < nw; ++wi)
        if (failed[wi]) throw errors[wi];
    return res;
}

// The main / Korean recogniser and its parallel copies.
std::vector<Session*> recSessions(std::unique_ptr<Session>& first, std::vector<std::unique_ptr<Session>>& more,
                                  const char* file) {
    if (!first) first = openSession(file, true);
    std::vector<Session*> v{first.get()};
    const size_t n = static_cast<size_t>(recParallel());
    while (more.size() + 1 < n) more.push_back(openSession(file, true));
    for (size_t k = 0; k + 1 < n; ++k) v.push_back(more[k].get());
    return v;
}

bool isKanaChar(wchar_t c) { return (c >= 0x3041 && c <= 0x30FA) || c == 0x30FC || (c >= 0x31F0 && c <= 0x31FF); }
bool isKatakana(wchar_t c) { return (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC; }
bool isHanChar(wchar_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF); }
int nonSpace(const std::wstring& s) {
    return static_cast<int>(std::count_if(s.begin(), s.end(), [](wchar_t c) { return !iswspace(c); }));
}

// Japanese pictures: the recogniser's look-alike slips (Han / Latin for
// katakana, Simplified for Japanese forms, 〒).  japanese = the picture has kana.
bool isLookalike(wchar_t c) { return wcschr(L"力口工二夕卜八一", c) != nullptr; }
std::wstring fixJapanese(std::wstring s, bool japanese) {
    if (!japanese) return s;
    static const struct {
        wchar_t from, to;
    } kKata[] = {{L'力', L'カ'}, {L'口', L'ロ'}, {L'工', L'エ'}, {L'二', L'ニ'},
                 {L'夕', L'タ'}, {L'卜', L'ト'}, {L'八', L'ハ'}, {L'一', L'ー'}};
    // A run of look-alikes next to katakana (力力オマス -> カカオマス, ダ一 -> ダー);
    // not before a small ッ (八ッ橋 is a word).
    for (size_t i = 0; i < s.size();) {
        if (!isLookalike(s[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < s.size() && isLookalike(s[j])) ++j;
        const wchar_t prev = i ? s[i - 1] : 0, next = j < s.size() ? s[j] : 0;
        const bool kataNext = isKatakana(next) && next != L'ッ';
        if ((isKatakana(prev) && !isHanChar(next)) || (kataNext && !isHanChar(prev)) || (isKatakana(prev) && kataNext))
            for (size_t k = i; k < j; ++k)
                for (const auto& m : kKata)
                    if (s[k] == m.from) s[k] = m.to;
        i = j;
    }
    // Simplified-only forms -> the Japanese ones (labels: 膨張剂 -> 膨張剤).
    static const wchar_t* const kSimp = L"剂凉时间门东车长图书对开关买卖无产业电话语说请认识进过运连选题页项饮馆鸡鱼鸟龙齐动这岛县";
    static const wchar_t* const kJa = L"剤涼時間門東車長図書対開関買売無産業電話語説請認識進過運連選題頁項飲館鶏魚鳥竜斉動這島県";
    for (wchar_t& c : s)
        if (const wchar_t* p = wcschr(kSimp, c)) c = kJa[p - kSimp];
    // 〒 read as 平 / T before a postal code (平601-8446).
    for (size_t i = 0; i + 4 < s.size(); ++i)
        if ((s[i] == L'平' || s[i] == L'T' || s[i] == L'干') && iswdigit(s[i + 1]) && iswdigit(s[i + 2]) && iswdigit(s[i + 3]) &&
            s[i + 4] == L'-')
            s[i] = L'〒';
    return s;
}

// ---- Line cache ----
// The last picture's pixels and its lines as read.  A detected line whose
// pixels (its box and a margin) are the same as those of a line of the
// last picture at the same size, anywhere (scrolled, or the magnified part
// read before the whole picture), takes that reading instead of being read
// again.  即時翻譯 runs after a scroll / a small change, and the whole picture
// after its magnified part, so read only what changed.  PM_OCR_CACHE=0: off.
struct CacheLine {
    Quad q;
    RecOut r;
    Crop meta;  // without its pixels
    bool ko = false;
};
struct LineCache {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    std::vector<CacheLine> lines;
    bool koreanFirst = false;
    // The whole result: a picture with the very same pixels (翻譯整個畫面 again
    // on an unchanged screen: 250-400 ms of detection with every line cached)
    // returns it without running anything.
    std::vector<OcrLine> result;
    int koLines = 0;
    bool haveResult = false;
    Lang hint = Lang::Unknown;  // recognize()'s, for the result
};
LineCache g_cache;

bool cacheOn() {
    const char* e = std::getenv("PM_OCR_CACHE");  // read each time: the tests switch it
    return !(e && e[0] == '0');
}

struct IBox {
    int x0, y0, x1, y1;
};
IBox boxOf(const Quad& q, float dx, float dy, int m) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const Pt& p : q.p) x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
    return {static_cast<int>(std::floor(x0 + dx)) - m, static_cast<int>(std::floor(y0 + dy)) - m,
            static_cast<int>(std::ceil(x1 + dx)) + m, static_cast<int>(std::ceil(y1 + dy)) + m};
}

// The new picture's box b and the cached picture's at b - (dx, dy) hold the
// same pixels (a little video noise allowed, no stroke).
bool samePixels(const uint8_t* a, int aw, int ah, IBox b, const LineCache& c, int dx, int dy) {
    b.x0 = std::max(b.x0, 0), b.y0 = std::max(b.y0, 0), b.x1 = std::min(b.x1, aw), b.y1 = std::min(b.y1, ah);
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return false;
    if (b.x0 - dx < 0 || b.y0 - dy < 0 || b.x1 - dx > c.w || b.y1 - dy > c.h) return false;
    constexpr int kMax = 40;       // one channel, one pixel
    constexpr double kMean = 2.0;  // per channel over the box
    uint64_t sum = 0;
    const int n = (b.x1 - b.x0) * 4;
    for (int y = b.y0; y < b.y1; ++y) {
        const uint8_t* p = a + (static_cast<size_t>(y) * aw + b.x0) * 4;
        const uint8_t* q = c.px.data() + (static_cast<size_t>(y - dy) * c.w + (b.x0 - dx)) * 4;
        if (std::memcmp(p, q, n) == 0) continue;
        for (int k = 0; k < n; ++k) {
            if ((k & 3) == 3) continue;  // alpha
            const int d = std::abs(p[k] - q[k]);
            if (d > kMax) return false;
            sum += d;
        }
    }
    return sum <= kMean * 3.0 * (b.x1 - b.x0) * (b.y1 - b.y0);
}

// Vertical shifts of the picture since the cached one (a scroll; 0 for fixed
// bars): rows of the same pixels vote, the most voted first.
std::vector<int> scrollShifts(const uint8_t* bgra, int w, int h) {
    std::vector<int> out;
    if (g_cache.w != w || g_cache.px.empty()) return out;
    auto rowHash = [w](const uint8_t* row, bool* flat) {
        uint64_t x = 1469598103934665603ull;
        const uint32_t* p = reinterpret_cast<const uint32_t*>(row);
        *flat = true;
        for (int i = 0; i < w; ++i) {
            x = (x ^ (p[i] & 0xFFFFFF)) * 1099511628211ull;
            *flat = *flat && (p[i] & 0xFFFFFF) == (p[0] & 0xFFFFFF);
        }
        return x;
    };
    std::vector<std::pair<uint64_t, int>> old;
    old.reserve(g_cache.h);
    for (int y = 0; y < g_cache.h; ++y) {
        bool flat;
        const uint64_t hh = rowHash(g_cache.px.data() + static_cast<size_t>(y) * w * 4, &flat);
        if (!flat) old.emplace_back(hh, y);
    }
    std::sort(old.begin(), old.end());
    std::vector<std::pair<int, int>> votes;  // shift, count
    for (int y = 0; y < h; ++y) {
        bool flat;
        const uint64_t hh = rowHash(bgra + static_cast<size_t>(y) * w * 4, &flat);
        if (flat) continue;
        auto it = std::lower_bound(old.begin(), old.end(), std::make_pair(hh, INT_MIN));
        for (int k = 0; it != old.end() && it->first == hh && k < 4; ++it, ++k) {  // a repeated row: a few candidates
            const int d = y - it->second;
            auto v = std::find_if(votes.begin(), votes.end(), [d](const auto& p) { return p.first == d; });
            if (v == votes.end()) votes.emplace_back(d, 1);
            else ++v->second;
        }
    }
    std::sort(votes.begin(), votes.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [d, n] : votes)
        if (n >= 8 && out.size() < 3) out.push_back(d);
    return out;
}

// The cached line showing the same text as quad q of the new picture, -1 if none.
int findCached(const uint8_t* bgra, int w, int h, const Quad& q, bool vertical, const std::vector<int>& shifts) {
    const float qw = q.w(), qh = q.h();
    const float cx = (q.p[0].x + q.p[1].x + q.p[2].x + q.p[3].x) / 4, cy = (q.p[0].y + q.p[1].y + q.p[2].y + q.p[3].y) / 4;
    for (size_t j = 0; j < g_cache.lines.size(); ++j) {
        const CacheLine& c = g_cache.lines[j];
        if (c.meta.vertical != vertical) continue;
        // Detection boxes of the same line move by a few pixels with its position.
        if (std::fabs(c.q.w() - qw) > 6 + 0.01f * qw || std::fabs(c.q.h() - qh) > 6) continue;
        const float ox = (c.q.p[0].x + c.q.p[1].x + c.q.p[2].x + c.q.p[3].x) / 4, oy = (c.q.p[0].y + c.q.p[1].y + c.q.p[2].y + c.q.p[3].y) / 4;
        const int dx0 = static_cast<int>(std::lround(cx - ox)), dy0 = static_cast<int>(std::lround(cy - oy));
        // The picture's shifts (exact) when they fit this line, then offsets around its own.
        std::vector<std::pair<int, int>> offs;
        for (int s : shifts)
            if (std::abs(s - dy0) <= 6 && std::abs(dx0) <= 6) offs.emplace_back(0, s);
        static const int kOff[][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1},
                                      {2, 0}, {-2, 0}, {0, 2}, {0, -2}};
        for (const auto& o : kOff) offs.emplace_back(dx0 + o[0], dy0 + o[1]);
        for (const auto& [dx, dy] : offs) {
            // Both boxes (the new line's and the cached one moved) with a margin.
            const IBox a = boxOf(q, 0, 0, 2), b = boxOf(c.q, static_cast<float>(dx), static_cast<float>(dy), 2);
            const IBox u{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
            if (samePixels(bgra, w, h, u, g_cache, dx, dy)) return static_cast<int>(j);
        }
    }
    return -1;
}

}  // namespace

bool PaddleOcr::runtimeAvailable(std::wstring* err) { return loadRuntime(err); }

bool PaddleOcr::gpuWanted() {
    const std::wstring e = envW(L"PM_OCR_GPU");
    if (e == L"off") return false;
    if (discreteAdapter() < 0) return false;
    if (e == L"on") return true;
    // 「使用顯示卡加速」: the local LLM's setting (one switch for both).
    wchar_t v[16] = {};
    GetPrivateProfileStringW(L"llm", L"gpu", L"", v, 16, (ModelStore::root() + L"\\llm\\settings.ini").c_str());
    return std::wstring(v) != L"0";
}

bool PaddleOcr::gpuActive() {
    std::lock_guard lk(g_rtM);
    return rt().api && rt().gpu;
}

bool PaddleOcr::modelsInstalled() { return ModelStore::installed("ocr"); }

void PaddleOcr::unload() {
    std::lock_guard lk(g_modelsM);
    g_cache = {};
    if (!rt().api) return;
    g_models = {};
}

bool PaddleOcr::warmUp(std::wstring* err) {
    if (!loadRuntime(err) || !modelsInstalled()) return false;
    std::lock_guard lk(g_modelsM);
    static bool warm = false;  // (unload() keeps it: sessions opened again lazily, a cheap part)
    if (warm && g_models.det && g_models.rec) return true;
    try {
        if (!g_models.det) g_models.det = openSession(kDetModel, false);
        // Detection on a small blank picture, recognition of one blank crop
        // per session (the first Run allocates; DirectML builds its graph).
        std::vector<uint8_t> px(static_cast<size_t>(64) * 64 * 4, 255);
        detect(*g_models.det, px.data(), 64, 64);
        Crop c;
        c.w = 320;
        c.aspect = static_cast<float>(c.w) / kRecH;
        c.px.assign(static_cast<size_t>(3) * kRecH * c.w, 1.f);
        const std::vector<Crop> crops{c};
        const std::vector<size_t> order{0};
        std::vector<RecOut> res(1);
        for (Session* s : recSessions(g_models.rec, g_models.recMore, kRecModel)) recognizeBatch(*s, crops, order, 0, 1, res);
        if (!ModelStore::ocrFile(kKoModel).empty())
            for (Session* s : recSessions(g_models.ko, g_models.koMore, kKoModel)) recognizeBatch(*s, crops, order, 0, 1, res);
        warm = true;
        return true;
    } catch (const OrtError& e) {
        if (err) *err = e.msg;
    } catch (const std::exception& e) {
        if (err) *err = L"OCR error: " + fromUtf8(e.what());
    }
    return false;  // recognize() opens them again (and falls back to the CPU)
}

bool PaddleOcr::recognize(const uint8_t* bgra, int width, int height, OcrResult& out, std::wstring* err, Lang hint,
                          const std::function<void(std::vector<OcrLine>)>& early) {
    out = {};
    out.backend = OcrBackend::Paddle;
    const double t0 = nowMs();
    if (!bgra || width <= 0 || height <= 0) {
        if (err) *err = L"empty picture";
        return false;
    }
    if (!loadRuntime(err)) return false;
    std::unique_lock lk(g_modelsM);
    if (cacheOn() && g_cache.haveResult && g_cache.hint == hint && g_cache.w == width && g_cache.h == height &&
        g_cache.px.size() == static_cast<size_t>(width) * height * 4 && !std::memcmp(g_cache.px.data(), bgra, g_cache.px.size())) {
        out.lines = g_cache.result;
        out.koLines = g_cache.koLines;
        out.ms = nowMs() - t0;
        return true;
    }
    try {
        if (!g_models.det) g_models.det = openSession(kDetModel, false);
        if (!g_models.rec) g_models.rec = openSession(kRecModel, true);
        g_prof = {};
        const double td = nowMs();
        const std::vector<Quad> quads = detect(*g_models.det, bgra, width, height);
        out.detMs = nowMs() - td;
        const double tr = nowMs();
        // Lines of the last picture with the same pixels: their reading again.
        std::vector<int> hit(quads.size(), -1);
        std::vector<size_t> todo;
        const bool useCache = cacheOn();
        int koCached = 0;
        const std::vector<int> shifts = useCache && !g_cache.lines.empty() ? scrollShifts(bgra, width, height) : std::vector<int>();
        for (size_t i = 0; i < quads.size(); ++i) {
            if (useCache && !g_cache.lines.empty())
                hit[i] = findCached(bgra, width, height, quads[i], quads[i].h() >= 1.5f * quads[i].w(), shifts);
            if (hit[i] < 0) todo.push_back(i);
            else koCached += g_cache.lines[hit[i]].ko;
        }
        g_prof.cached = static_cast<int>(quads.size() - todo.size());
        g_prof.cache = nowMs() - tr;
        // The crops on a few threads (independent; ~100 ms for a dense page on one).
        std::vector<Crop> crops(quads.size());
        for (size_t i = 0; i < quads.size(); ++i)
            if (hit[i] >= 0) crops[i] = g_cache.lines[hit[i]].meta;
        parallelFor(todo.size(), 2, [&](size_t k) { crops[todo[k]] = makeCrop(bgra, width, height, quads[todo[k]]); });
        const std::vector<size_t>& all = todo;
        const double tCrops = nowMs();
        // A Korean picture (the main recogniser has no hangul: on 0.7.x every
        // Korean line was read twice, 18 s for a Wikipedia page): one batch of
        // the Korean recogniser on mid-width lines first; mostly hangul ->
        // Korean first, the main recogniser only for the lines it did not read.
        const bool haveKo = !ModelStore::ocrFile(kKoModel).empty();
        std::vector<RecOut> rec(crops.size()), kor;
        std::vector<char> korDone(crops.size(), 0), fromKo(crops.size(), 0);
        for (size_t i = 0; i < crops.size(); ++i)
            if (hit[i] >= 0) rec[i] = g_cache.lines[hit[i]].r, fromKo[i] = g_cache.lines[hit[i]].ko;
        // Lines in reading order: top to bottom, left to right within a row
        // (which: these quads only - the early part - else all).
        auto makeLines = [&](const std::vector<size_t>* which) {
        int kanaAll = 0;
        for (size_t k = 0; k < (which ? which->size() : rec.size()); ++k)
            for (wchar_t c : rec[which ? (*which)[k] : k].text) kanaAll += isKanaChar(c);
        std::vector<OcrLine> lines;
        for (size_t k = 0; k < (which ? which->size() : quads.size()); ++k) {
            const size_t i = which ? (*which)[k] : k;
            RecOut& r = rec[i];
            const int n = nonSpace(r.text);
            // Garbage: unsure lines, single unsure characters.
            if (n == 0 || r.conf < 0.5f || (n == 1 && r.conf < 0.85f) || (n <= 4 && r.conf < 0.75f)) continue;
            const Quad& q = quads[i];
            OcrLine l{};
            l.text = fixJapanese(r.text, kanaAll >= 4);
            l.conf = r.conf;
            l.vertical = crops[i].vertical;
            l.bg = crops[i].bg;
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (const Pt& p : q.p) x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
            l.x0 = x0 / width, l.y0 = y0 / height, l.x1 = x1 / width, l.y1 = y1 / height;
            if (l.vertical) {
                l.angle = 0;
                l.lineH = (y1 - y0) / height;
            } else {
                l.angle = std::atan2(q.p[1].y - q.p[0].y, q.p[1].x - q.p[0].x);
                // Glyph height: the box side across the line without the unclip
                // margin (measured: boxes are ~1.4x the glyphs).
                l.lineH = q.h() * 0.72f / height;
            }
            l.script = detectScript(l.text);
            // Character positions (cells inside one detected line), when the
            // text was not changed by fixJapanese.
            if (!l.vertical && l.text == r.text && r.pos.size() == l.text.size())
                for (float f : r.pos)
                    l.charX.push_back((q.p[0].x + f * (q.p[1].x - q.p[0].x) + q.p[3].x + f * (q.p[2].x - q.p[3].x)) / 2 / width);
            lines.push_back(std::move(l));
        }
        std::sort(lines.begin(), lines.end(), [](const OcrLine& a, const OcrLine& b) { return a.y0 + a.y1 < b.y0 + b.y1; });
        for (size_t i = 0; i < lines.size();) {
            const float cy = (lines[i].y0 + lines[i].y1) / 2;
            const float hi = lines[i].lineH > 0 ? lines[i].lineH : lines[i].y1 - lines[i].y0;
            size_t j = i + 1;
            while (j < lines.size()) {
                const float hj = lines[j].lineH > 0 ? lines[j].lineH : lines[j].y1 - lines[j].y0;
                if ((lines[j].y0 + lines[j].y1) / 2 - cy >= 0.5f * std::min(hi, hj)) break;
                ++j;
            }
            std::sort(lines.begin() + i, lines.begin() + j, [](const OcrLine& a, const OcrLine& b) { return a.x0 < b.x0; });
            i = j;
        }
        return lines;
        };
        auto hangulOk = [](const RecOut& r) {
            int hangul = 0, letters = 0;
            for (wchar_t c : r.text) hangul += isHangul(c), letters += !iswspace(c) && !iswdigit(c) && !iswpunct(c);
            return hangul > 0 && hangul * 10 >= letters * 3;
        };
        // The user chose a source language other than Korean: no probe (the
        // lines it cannot read still go to the Korean recogniser below - a
        // bilingual sign).  Korean chosen: the probe as for "automatic"
        // (measured: without it, Korean first everywhere, CER a little worse).
        bool koreanFirst = false;
        const bool noKoProbe = hint != Lang::Unknown && hint != Lang::Ko;
        const bool noProbe = envInt("PM_OCR_KO_PROBE", 1) == 0 || noKoProbe;  // tests: the 0.7.x order
        if (haveKo && !noProbe && g_prof.cached > 0 && static_cast<size_t>(g_prof.cached) >= all.size()) {
            koreanFirst = g_cache.koreanFirst;  // mostly the last picture: its decision
        } else if (haveKo && !noProbe && all.size() >= 6) {
            std::vector<size_t> byW = all;
            std::sort(byW.begin(), byW.end(), [&](size_t x, size_t y) { return crops[x].w < crops[y].w; });
            std::vector<size_t> probe;
            for (size_t k = byW.size() / 4; k < byW.size() && probe.size() < 8; ++k)
                if (crops[byW[k]].aspect >= 1.5f && !crops[byW[k]].vertical) probe.push_back(byW[k]);
            const double tp = nowMs();
            const auto koSessions = recSessions(g_models.ko, g_models.koMore, kKoModel);
            // Fewer than 3 lines can never decide "Korean": no probe.  Wide
            // lines (a sparse page: the probe was its widest lines, up to 1.7 s
            // on the E-cores): first their starts only, with a looser test; the
            // full probe below (the same batches as before, the same decision)
            // only when that may be Korean.  PM_OCR_PROBE_W=0: always the full probe.
            bool full = probe.size() >= 3;
            const int cutW = envInt("PM_OCR_PROBE_W", 0);
            bool wide = false;
            for (size_t i : probe) wide |= cutW > 0 && crops[i].w > cutW;
            if (full && wide) {
                std::vector<Crop> cut(probe.size());
                std::vector<size_t> idx(probe.size());
                for (size_t k = 0; k < probe.size(); ++k) {
                    const Crop& c = crops[probe[k]];
                    Crop& d = cut[k];
                    idx[k] = k;
                    d.w = std::min(c.w, cutW);
                    d.aspect = static_cast<float>(d.w) / kRecH;
                    d.px.resize(static_cast<size_t>(3) * kRecH * d.w);
                    for (int ch = 0; ch < 3; ++ch)
                        for (int y = 0; y < kRecH; ++y)
                            std::memcpy(&d.px[(static_cast<size_t>(ch) * kRecH + y) * d.w], &c.px[(static_cast<size_t>(ch) * kRecH + y) * c.w],
                                        sizeof(float) * d.w);
                }
                const auto kc = recognizeCrops(koSessions, cut, idx);
                int maybe = 0;
                for (const RecOut& r : kc) maybe += hangulOk(r) && r.conf >= 0.5f;
                full = maybe >= 2;
                g_prof.nProbe = static_cast<int>(probe.size()) * 100;  // tests: the cut probe
                if (!full && envInt("PM_OCR_PROBE_CHECK", 0)) {  // tests: would the full probe have said Korean?
                    const auto kf = recognizeCrops(koSessions, crops, probe);
                    int hits = 0;
                    for (size_t i : probe) hits += hangulOk(kf[i]) && kf[i].conf >= 0.8f;
                    std::fprintf(stderr, "[probecheck] %dx%d cut maybe %d, full hits %d/%zu%s\n", width, height, maybe, hits,
                                 probe.size(), hits * 2 >= static_cast<int>(probe.size()) && hits >= 3 ? " MISMATCH" : "");
                }
            }
            if (full) {
                kor = recognizeCrops(koSessions, crops, probe);
                g_prof.nProbe += static_cast<int>(probe.size());
                int hits = 0;
                for (size_t i : probe) {
                    korDone[i] = 1;
                    hits += hangulOk(kor[i]) && kor[i].conf >= 0.8f;
                }
                koreanFirst = hits * 2 >= static_cast<int>(probe.size()) && hits >= 3;
            }
            g_prof.probe = nowMs() - tp;
        }
        if (kor.empty()) kor.assign(crops.size(), {});
        int koLines = koCached;
        double tKo1 = 0;
        size_t nMore = 0;
        std::vector<size_t> ko;
        // A dense picture read for the first time (early: 翻譯整個畫面): the
        // top part first, handed out (its translation shown while the rest
        // is read), then the rest.  The split is at the widest gap between
        // lines within the top quarter to half of them, so the paragraphs
        // above it are complete.  Both orders (Korean first or not).
        std::vector<size_t> earlyTopAll, earlyTop, earlyRest;  // the top part's quads (cached ones too) / to read, the rest to read
        if (early && all.size() >= static_cast<size_t>(envInt("PM_OCR_EARLY_MIN", 40))) {
            std::vector<size_t> byY(quads.size());
            for (size_t i = 0; i < byY.size(); ++i) byY[i] = i;
            auto cy = [&](size_t i) {
                float a = 1e9f, b = -1e9f;
                for (const Pt& p : quads[i].p) a = std::min(a, p.y), b = std::max(b, p.y);
                return (a + b) / 2;
            };
            std::sort(byY.begin(), byY.end(), [&](size_t a, size_t b) { return cy(a) < cy(b); });
            std::vector<float> bottomAbove(byY.size()), topBelow(byY.size() + 1, 1e9f);
            float m = -1e9f;
            for (size_t k = 0; k < byY.size(); ++k) {
                for (const Pt& p : quads[byY[k]].p) m = std::max(m, p.y);
                bottomAbove[k] = m;
            }
            for (size_t k = byY.size(); k-- > 0;) {
                float t = 1e9f;
                for (const Pt& p : quads[byY[k]].p) t = std::min(t, p.y);
                topBelow[k] = std::min(topBelow[k + 1], t);
            }
            size_t best = 0;
            float bestGap = 0;
            for (size_t k = byY.size() / 4; k <= byY.size() / 2; ++k) {
                const float gap = topBelow[k] - bottomAbove[k - 1];
                if (gap > bestGap) bestGap = gap, best = k;
            }
            if (best > 0) {
                std::vector<char> isTop(quads.size(), 0);
                for (size_t k = 0; k < best; ++k) isTop[byY[k]] = 1, earlyTopAll.push_back(byY[k]);
                for (size_t i : all) (isTop[i] ? earlyTop : earlyRest).push_back(i);
            }
        }
        if (koreanFirst) {
            // The Korean recogniser on the lines (of a part), then the main one
            // on those without (confident) hangul.
            auto koFirst = [&](const std::vector<size_t>& part) {
                std::vector<size_t> rest;
                for (size_t i : part)
                    if (!korDone[i]) rest.push_back(i);
                const double tk = nowMs();
                const auto k2 = recognizeCrops(recSessions(g_models.ko, g_models.koMore, kKoModel), crops, rest);
                g_prof.ko += nowMs() - tk, g_prof.nKo += static_cast<int>(rest.size());
                for (size_t i : rest) kor[i] = k2[i];
                // The main recogniser: lines without (confident) hangul - numbers,
                // Latin, kanji / kana - and the decision of 0.7.x for each.
                std::vector<size_t> mainIdx;
                for (size_t i : part) {
                    if (hangulOk(kor[i]) && kor[i].conf >= 0.6f) {
                        rec[i] = kor[i];
                        fromKo[i] = 1;
                        ++koLines;
                    } else {
                        mainIdx.push_back(i);
                    }
                }
                tKo1 = nowMs();
                const auto m = recognizeCrops(recSessions(g_models.rec, g_models.recMore, kRecModel), crops, mainIdx);
                g_prof.main += nowMs() - tKo1, g_prof.nMain += static_cast<int>(mainIdx.size());
                for (size_t i : mainIdx) {
                    rec[i] = m[i];
                    bool mainHan = false;
                    for (wchar_t c : m[i].text) mainHan |= isHanChar(c);
                    const RecOut& r = kor[i];
                    if (hangulOk(r) && (r.conf >= std::max(0.6f, m[i].conf - 0.05f) || nonSpace(m[i].text) * 2 < nonSpace(r.text) ||
                                        (r.conf >= 0.8f && (nonSpace(r.text) > nonSpace(m[i].text) || mainHan)))) {
                        rec[i] = kor[i];
                        fromKo[i] = 1;
                        ++koLines;
                    }
                }
                nMore += mainIdx.size();
            };
            if (!earlyTopAll.empty()) {
                koFirst(earlyTop);
                g_prof.early = nowMs() - tr;
                early(makeLines(&earlyTopAll));
                koFirst(earlyRest);
            } else {
                koFirst(all);
            }
        }
        const double tMain = nowMs();
        if (!koreanFirst) {
        {
            const auto sessions = recSessions(g_models.rec, g_models.recMore, kRecModel);
            if (!earlyTopAll.empty()) {
                // The top part's batches first; handed out as soon as they are
                // read (the other sessions keep reading the rest).
                if (earlyTop.empty()) early(makeLines(&earlyTopAll));  // all of it cached
                const auto m = recognizeCrops(sessions, crops, all, &earlyTop, [&](const std::vector<RecOut>& r) {
                    for (size_t i : earlyTop) rec[i] = r[i];
                    g_prof.early = nowMs() - tr;
                    early(makeLines(&earlyTopAll));
                });
                for (size_t i : all) rec[i] = m[i];
            } else {
                const auto m = recognizeCrops(sessions, crops, all);
                for (size_t i : all) rec[i] = m[i];
            }
        }
        g_prof.main = nowMs() - tMain, g_prof.nMain = static_cast<int>(all.size());
        // Korean: lines the main recogniser could not read (no hangul in its
        // dictionary: empty, unsure, or far too few characters for the width).
        for (size_t i : all) {
            const RecOut& r = rec[i];
            int kana = 0;
            for (wchar_t c : r.text) kana += isKanaChar(c);
            const int n = nonSpace(r.text);
            if (kana >= 2 && r.conf >= 0.8f) continue;
            if (r.conf < 0.8f || n == 0 || (crops[i].aspect > 1.6f && n < 0.5f * crops[i].aspect)) ko.push_back(i);
        }
        auto tryKorean = [&](const std::vector<size_t>& idx, bool digitsOnly /* second pass: short lines of a Korean picture */) {
            if (idx.empty() || !ModelStore::ocrFile(kKoModel).size()) return;
            const std::vector<RecOut> k = recognizeCrops(recSessions(g_models.ko, g_models.koMore, kKoModel), crops, idx);
            for (size_t i : idx) {
                const RecOut& r = k[i];
                int hangul = 0, letters = 0;
                for (wchar_t c : r.text) hangul += isHangul(c), letters += !iswspace(c) && !iswdigit(c) && !iswpunct(c);
                if (hangul == 0 || hangul * 10 < letters * 3) continue;
                bool mainHan = false;
                for (wchar_t c : rec[i].text) mainHan |= isHanChar(c);
                const bool better =
                    digitsOnly ? r.conf >= 0.8f && (nonSpace(r.text) > nonSpace(rec[i].text) || mainHan)
                               : r.conf >= std::max(0.6f, rec[i].conf - 0.05f) || nonSpace(rec[i].text) * 2 < nonSpace(r.text);
                if (better) {
                    rec[i] = r;
                    fromKo[i] = 1;
                    ++koLines;
                }
            }
        };
        const double tk = nowMs();
        tryKorean(ko, false);
        tKo1 = nowMs();
        g_prof.ko = tKo1 - tk, g_prof.nKo = static_cast<int>(ko.size());
        if (koLines >= 2) {
            // A Korean picture: prices / numbers lose their 원 / 개 with the main recogniser.
            std::vector<size_t> more;
            for (size_t i : all) {
                if (std::find(ko.begin(), ko.end(), i) != ko.end()) continue;
                bool kana = false;
                int han = 0;
                for (wchar_t c : rec[i].text) han += isHanChar(c), kana |= isKanaChar(c);
                // Short lines only (a price, 켬 read as 君): wide Latin / number
                // lines took 1.6 s again for nothing (eval_web ko_sign_04).
                bool digit = false;
                for (wchar_t c : rec[i].text) digit |= iswdigit(c) != 0;
                if (!kana && han <= 2 && (digit || han > 0 || nonSpace(rec[i].text) <= 3) && nonSpace(rec[i].text) <= 12 && crops[i].aspect <= 10)
                    more.push_back(i);
            }
            tKo1 = nowMs(), nMore = more.size();
            tryKorean(more, true);
            g_prof.more = nowMs() - tKo1, g_prof.nMore = static_cast<int>(more.size());
        }
        }
        out.recMs = nowMs() - tr;
        out.koLines = koLines;
        if (useCache) {
            g_cache.px.assign(bgra, bgra + static_cast<size_t>(width) * height * 4);
            g_cache.w = width, g_cache.h = height;
            g_cache.koreanFirst = koreanFirst;
            g_cache.haveResult = false;  // set below with these pixels' lines
            g_cache.lines.clear();
            for (size_t i = 0; i < quads.size(); ++i) {
                CacheLine c;
                c.q = quads[i];
                c.r = rec[i];
                c.meta = crops[i];
                c.meta.px.clear();
                c.meta.px.shrink_to_fit();
                c.ko = fromKo[i] != 0;
                g_cache.lines.push_back(std::move(c));
            }
        } else {
            g_cache = {};
        }
        static const bool prof = std::getenv("PM_OCR_PROF") != nullptr;  // tests: where the time goes
        if (prof) {
            const Prof& p = g_prof;
            std::fprintf(stderr,
                         "[ocrprof] %dx%d det %.0f (pre %.0f run %.0f post %.0f @%dx%d) cached %d %.0f crops %zu %.0f probe %d %.0f main %d %.0f "
                         "ko %d %.0f more %d %.0f koFirst %d batches %d pad %.2f\n",
                         width, height, out.detMs, p.detPre, p.detRun, p.detPost, p.detW, p.detH, p.cached, p.cache, crops.size(),
                         tCrops - tr - p.cache, p.nProbe, p.probe, p.nMain, p.main, p.nKo, p.ko, p.nMore, p.more, koreanFirst ? 1 : 0,
                         p.batches, p.realPx > 0 ? p.paddedPx / p.realPx : 0.0);
        }
        std::vector<OcrLine> lines = makeLines(nullptr);
        if (useCache) {
            g_cache.result = lines;
            g_cache.koLines = koLines;
            g_cache.haveResult = true;
            g_cache.hint = hint;
        }
        out.lines = std::move(lines);
        out.ms = nowMs() - t0;
        return true;
    } catch (const OrtError& e) {
        if (err) *err = e.msg;
    } catch (const std::exception& e) {
        if (err) *err = L"OCR error: " + fromUtf8(e.what());
    }
    // The GPU failed (driver, device lost, out of video memory): the CPU
    // runtime from now on, and this picture again.
    bool retry = false;
    {
        std::lock_guard rl(g_rtM);
        Runtime& r = rt();
        if (r.gpu && !r.gpuBroken) {
            std::fprintf(stderr, "[ocr] GPU failed (%ls): CPU from now on\n", err ? err->c_str() : L"");
            g_models = {};
            if (r.env) r.api->ReleaseEnv(r.env);
            r = Runtime{};
            r.gpuBroken = true;
            retry = true;
        }
    }
    if (retry) {
        lk.unlock();
        return recognize(bgra, width, height, out, err, hint, early);
    }
    return false;
}

}  // namespace pm::translate
