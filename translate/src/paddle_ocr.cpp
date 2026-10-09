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
#include <chrono>
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
// Test overrides: PM_OCR_DET="limit thresh box unclip".
void detParams() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* e = std::getenv("PM_OCR_DET")) sscanf_s(e, "%d %f %f %f", &kDetLimit, &kDetThresh, &kBoxThresh, &kUnclip);
}
constexpr int kRecH = 48, kRecMaxW = 3200, kRecBatch = 8;

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

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
    a.SetIntraOpNumThreads(so, threads);
    a.SetInterOpNumThreads(so, 1);
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

// ---- Detection ----
std::vector<Quad> detect(Session& det, const uint8_t* bgra, int w, int h) {
    detParams();
    const float r = std::min(1.f, static_cast<float>(kDetLimit) / std::max(w, h));
    const int nw = std::max(32, static_cast<int>(std::lround(w * r / 32.f)) * 32);
    const int nh = std::max(32, static_cast<int>(std::lround(h * r / 32.f)) * 32);
    // Area-averaged resize (box filter over the source pixels of each target
    // pixel), BGR planes normalised like PaddleOCR (mean / std on BGR order).
    std::vector<float> in(static_cast<size_t>(3) * nw * nh);
    const float sx = static_cast<float>(w) / nw, sy = static_cast<float>(h) / nh;
    static const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
    for (int y = 0; y < nh; ++y) {
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
    }
    const int64_t shape[4] = {1, 3, nh, nw};
    std::vector<int64_t> os;
    const std::vector<float> prob = run(det, in, shape, os);
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
    static const int n = [] {
        if (const char* e = std::getenv("PM_OCR_PAR"); e && atoi(e) > 0) return std::min(4, atoi(e));
        return std::thread::hardware_concurrency() >= 8 ? 2 : 1;
    }();
    return n;
}

// CTC greedy decoding of one batch: crops order[b .. b + kRecBatch).
void recognizeBatch(Session& rec, const std::vector<Crop>& crops, const std::vector<size_t>& order, size_t b,
                    std::vector<RecOut>& res) {
    const size_t n = std::min<size_t>(kRecBatch, order.size() - b);
    int bw = 0;
    for (size_t k = 0; k < n; ++k) bw = std::max(bw, crops[order[b + k]].w);
    bw = std::max(bw, 64);
    // Fewer distinct shapes (a GPU compiles one graph per shape): widths in steps of 128.
    static const bool bucket = std::getenv("PM_OCR_DML") || std::getenv("PM_OCR_BUCKET");
    if (bucket) bw = (bw + 127) / 128 * 128;
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

// Crops (batched by similar width) on the sessions in parallel: the widest
// batches first, dealt out in turn, so the threads finish together.
std::vector<RecOut> recognizeCrops(const std::vector<Session*>& sessions, const std::vector<Crop>& crops,
                                   const std::vector<size_t>& which) {
    std::vector<RecOut> res(crops.size());
    std::vector<size_t> order = which;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return crops[a].w < crops[b].w; });
    std::vector<size_t> starts;
    for (size_t b = 0; b < order.size(); b += kRecBatch) starts.push_back(b);
    const size_t nw = std::min(sessions.size(), starts.size());
    std::vector<OrtError> errors(nw);
    std::vector<int> failed(nw, 0);
    auto work = [&](size_t wi) {
        try {
            for (size_t j = wi; j < starts.size(); j += nw)
                recognizeBatch(*sessions[wi], crops, order, starts[starts.size() - 1 - j], res);
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
    while (static_cast<int>(more.size()) + 1 < recParallel()) more.push_back(openSession(file, true));
    for (auto& s : more) v.push_back(s.get());
    return v;
}

bool isKanaChar(wchar_t c) { return (c >= 0x3041 && c <= 0x30FA) || c == 0x30FC || (c >= 0x31F0 && c <= 0x31FF); }
bool isKatakana(wchar_t c) { return (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC; }
bool isHangulChar(wchar_t c) { return c >= 0xAC00 && c <= 0xD7AF; }
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
    if (!rt().api) return;
    g_models = {};
}

bool PaddleOcr::recognize(const uint8_t* bgra, int width, int height, OcrResult& out, std::wstring* err) {
    out = {};
    out.backend = OcrBackend::Paddle;
    const double t0 = nowMs();
    if (!bgra || width <= 0 || height <= 0) {
        if (err) *err = L"empty picture";
        return false;
    }
    if (!loadRuntime(err)) return false;
    std::unique_lock lk(g_modelsM);
    try {
        if (!g_models.det) g_models.det = openSession(kDetModel, false);
        if (!g_models.rec) g_models.rec = openSession(kRecModel, true);
        const double td = nowMs();
        const std::vector<Quad> quads = detect(*g_models.det, bgra, width, height);
        out.detMs = nowMs() - td;
        const double tr = nowMs();
        std::vector<Crop> crops;
        crops.reserve(quads.size());
        for (const Quad& q : quads) crops.push_back(makeCrop(bgra, width, height, q));
        std::vector<size_t> all(crops.size());
        std::iota(all.begin(), all.end(), 0);
        const double tCrops = nowMs();
        // A Korean picture (the main recogniser has no hangul: on 0.7.x every
        // Korean line was read twice, 18 s for a Wikipedia page): one batch of
        // the Korean recogniser on mid-width lines first; mostly hangul ->
        // Korean first, the main recogniser only for the lines it did not read.
        const bool haveKo = !ModelStore::ocrFile(kKoModel).empty();
        std::vector<RecOut> rec(crops.size()), kor;
        std::vector<char> korDone(crops.size(), 0);
        auto hangulOk = [](const RecOut& r) {
            int hangul = 0, letters = 0;
            for (wchar_t c : r.text) hangul += isHangulChar(c), letters += !iswspace(c) && !iswdigit(c) && !iswpunct(c);
            return hangul > 0 && hangul * 10 >= letters * 3;
        };
        bool koreanFirst = false;
        static const bool noProbe = std::getenv("PM_OCR_KO_PROBE") && std::getenv("PM_OCR_KO_PROBE")[0] == '0';  // tests: the 0.7.x order
        if (haveKo && !noProbe && crops.size() >= 6) {
            std::vector<size_t> byW(crops.size());
            std::iota(byW.begin(), byW.end(), 0);
            std::sort(byW.begin(), byW.end(), [&](size_t x, size_t y) { return crops[x].w < crops[y].w; });
            std::vector<size_t> probe;
            for (size_t k = byW.size() / 4; k < byW.size() && probe.size() < 8; ++k)
                if (crops[byW[k]].aspect >= 1.5f && !crops[byW[k]].vertical) probe.push_back(byW[k]);
            kor = recognizeCrops(recSessions(g_models.ko, g_models.koMore, kKoModel), crops, probe);
            int hits = 0;
            for (size_t i : probe) {
                korDone[i] = 1;
                hits += hangulOk(kor[i]) && kor[i].conf >= 0.8f;
            }
            koreanFirst = hits * 2 >= static_cast<int>(probe.size()) && hits >= 3;
        }
        if (kor.empty()) kor.assign(crops.size(), {});
        int koLines = 0;
        double tKo1 = 0;
        size_t nMore = 0;
        std::vector<size_t> ko;
        if (koreanFirst) {
            std::vector<size_t> rest;
            for (size_t i = 0; i < crops.size(); ++i)
                if (!korDone[i]) rest.push_back(i);
            const auto k2 = recognizeCrops(recSessions(g_models.ko, g_models.koMore, kKoModel), crops, rest);
            for (size_t i : rest) kor[i] = k2[i];
            // The main recogniser: lines without (confident) hangul - numbers,
            // Latin, kanji / kana - and the decision of 0.7.x for each.
            std::vector<size_t> mainIdx;
            for (size_t i = 0; i < crops.size(); ++i) {
                if (hangulOk(kor[i]) && kor[i].conf >= 0.6f) {
                    rec[i] = kor[i];
                    ++koLines;
                } else {
                    mainIdx.push_back(i);
                }
            }
            tKo1 = nowMs();
            const auto m = recognizeCrops(recSessions(g_models.rec, g_models.recMore, kRecModel), crops, mainIdx);
            for (size_t i : mainIdx) {
                rec[i] = m[i];
                bool mainHan = false;
                for (wchar_t c : m[i].text) mainHan |= isHanChar(c);
                const RecOut& r = kor[i];
                if (hangulOk(r) && (r.conf >= std::max(0.6f, m[i].conf - 0.05f) || nonSpace(m[i].text) * 2 < nonSpace(r.text) ||
                                    (r.conf >= 0.8f && (nonSpace(r.text) > nonSpace(m[i].text) || mainHan)))) {
                    rec[i] = kor[i];
                    ++koLines;
                }
            }
            nMore = mainIdx.size();
        }
        const double tMain = nowMs();
        if (!koreanFirst) {
        rec = recognizeCrops(recSessions(g_models.rec, g_models.recMore, kRecModel), crops, all);
        // Korean: lines the main recogniser could not read (no hangul in its
        // dictionary: empty, unsure, or far too few characters for the width).
        for (size_t i = 0; i < crops.size(); ++i) {
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
                for (wchar_t c : r.text) hangul += isHangulChar(c), letters += !iswspace(c) && !iswdigit(c) && !iswpunct(c);
                if (hangul == 0 || hangul * 10 < letters * 3) continue;
                bool mainHan = false;
                for (wchar_t c : rec[i].text) mainHan |= isHanChar(c);
                const bool better =
                    digitsOnly ? r.conf >= 0.8f && (nonSpace(r.text) > nonSpace(rec[i].text) || mainHan)
                               : r.conf >= std::max(0.6f, rec[i].conf - 0.05f) || nonSpace(rec[i].text) * 2 < nonSpace(r.text);
                if (better) {
                    rec[i] = r;
                    ++koLines;
                }
            }
        };
        tryKorean(ko, false);
        tKo1 = nowMs();
        if (koLines >= 2) {
            // A Korean picture: prices / numbers lose their 원 / 개 with the main recogniser.
            std::vector<size_t> more;
            for (size_t i = 0; i < crops.size(); ++i) {
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
        }
        }
        out.recMs = nowMs() - tr;
        out.koLines = koLines;
        static const bool prof = std::getenv("PM_OCR_PROF") != nullptr;  // tests: where the time goes
        if (prof) {
            size_t px = 0, pxKo = 0;
            for (const Crop& c : crops) px += c.w;
            for (size_t i : ko) pxKo += crops[i].w;
            if (koreanFirst) std::fprintf(stderr, "[ocr] Korean first: %d lines Korean, %zu read by the main recogniser\n", koLines, nMore);
            std::fprintf(stderr, "[ocr] %dx%d det %.0f ms, %zu crops (%zu px wide in all) %.0f ms, main rec %.0f ms, korean %zu crops (%zu px) %.0f ms, more %zu crops %.0f ms\n",
                         width, height, out.detMs, crops.size(), px, tCrops - tr, tMain - tCrops, ko.size(), pxKo, tKo1 - tMain, nMore, nowMs() - tKo1);
        }
        // Lines in reading order: top to bottom, left to right within a row.
        int kanaAll = 0;
        for (const RecOut& r : rec)
            for (wchar_t c : r.text) kanaAll += isKanaChar(c);
        std::vector<OcrLine> lines;
        for (size_t i = 0; i < quads.size(); ++i) {
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
        return recognize(bgra, width, height, out, err);
    }
    return false;
}

}  // namespace pm::translate
