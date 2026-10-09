// pm_llm_eval: runs the local LLM engine (translate/src/llm_engine.h) on a
// list of segments and measures it - quality is scored by llm_eval.py.
//
//   pm_llm_eval --model ID|FILE.gguf --in SEGMENTS.tsv --out RESULT.jsonl
//               [--tgt zh-Hant|en|ja|ko] [--style plain|fewshot|json] [--temp T]
//               [--threads N] [--cores N] [--ctx] [--limit N] [--pivot fallback|src]
//               [--pictures 0|-1|N] [--parallel N] [--budget MS] [--device auto|cpu|igpu]
//   --pictures: segments translated together like one picture (0: one at a
//   time; -1: by picture, the id before ':'; N: N consecutive ones);
//   latency is reported per call (= per picture).
//   pm_llm_eval --dl-test HOST PATH SIZE DEST [SHA|-] [--dl-conns START MAX] [--dl-cancel-ms T]
//                                    the shared downloader on one file: speed, connections, cancel latency
//   pm_llm_eval --status             the model / GPU add-on / download size for this PC
//   pm_llm_eval --download [ID]      download runtime + model (no dialog; for tests)
//   pm_llm_eval --idle-test FILE.gguf  load, translate, wait for the idle release
//
// SEGMENTS.tsv: id <TAB> src lang (ja/ko/en/zh-Hans) <TAB> text <TAB> context
// <TAB> heading <TAB> English pivot ("\n" written as \n).  --cores N pins the process to N
// physical performance cores (a 4-core laptop: --cores 4 --threads 4).
// The process runs at below-normal priority (the PC stays usable).
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "llm_engine.h"
#include "text_util.h"

using namespace pm::translate;
using namespace pm::translate::llm;

namespace {

Lang langOf(const std::string& s) {
    if (s == "ja") return Lang::Ja;
    if (s == "ko") return Lang::Ko;
    if (s == "en") return Lang::En;
    if (s == "zh-Hans") return Lang::ZhHans;
    if (s == "zh-Hant") return Lang::ZhHant;
    return Lang::Unknown;
}

std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            o += '\n';
            ++i;
        } else o += s[i];
    }
    return o;
}

std::string json(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += static_cast<char>(c);
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else o += static_cast<char>(c);
    }
    return o + "\"";
}

std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == d) {
            v.push_back(cur);
            cur.clear();
        } else if (c != '\r') cur += c;
    }
    v.push_back(cur);
    return v;
}

// The first n performance cores (one logical processor each).
DWORD_PTR coreMask(int n) {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<char> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
        return 0;
    BYTE maxClass = 0;
    for (DWORD off = 0; off < len;) {
        auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
        maxClass = std::max(maxClass, p->Processor.EfficiencyClass);
        off += p->Size;
    }
    DWORD_PTR mask = 0;
    int got = 0;
    for (DWORD off = 0; off < len && got < n;) {
        auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
        if (p->Processor.EfficiencyClass == maxClass && p->Processor.GroupMask[0].Group == 0) {
            const KAFFINITY m = p->Processor.GroupMask[0].Mask;
            mask |= m & (~m + 1);  // lowest logical processor of the core
            ++got;
        }
        off += p->Size;
    }
    return mask;
}

void memory(double& peakWsMB, double& peakPrivMB, double& wsMB) {
    PROCESS_MEMORY_COUNTERS_EX pm{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm));
    peakWsMB = pm.PeakWorkingSetSize / 1048576.0;
    peakPrivMB = pm.PeakPagefileUsage / 1048576.0;
    wsMB = pm.WorkingSetSize / 1048576.0;
}

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    Options o;
    std::wstring in, out, idleFile;
    std::string tgtS = "zh-Hant";
    bool useCtx = false, doDownload = false, doStatus = false;
    std::wstring dlHost, dlPath, dlDest, dlSha, msTest;
    uint64_t dlSize = 0;
    int dlStart = 2, dlMax = 8;
    double dlCancelMs = 0;
    std::string downloadId;
    int limit = 1 << 30, cores = 0, pivotMode = 0, pictures = 0;
    double budgetMs = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--model") {
            const std::wstring m = next();
            if (m.size() > 5 && m.substr(m.size() - 5) == L".gguf") o.modelFile = m;
            else o.modelId = toUtf8(m);
        } else if (a == L"--in") in = next();
        else if (a == L"--out") out = next();
        else if (a == L"--tgt") tgtS = toUtf8(next());
        else if (a == L"--style") {
            const std::wstring s = next();
            o.style = s == L"plain" ? PromptStyle::Plain : s == L"json" ? PromptStyle::Json : PromptStyle::FewShot;
        } else if (a == L"--temp") o.temperature = std::stof(next());
        else if (a == L"--threads") o.threads = std::stoi(next());
        else if (a == L"--cores") cores = std::stoi(next());
        else if (a == L"--ctx") useCtx = true;
        else if (a == L"--pivot") {
            const std::wstring m = next();
            pivotMode = m == L"fallback" ? 1 : m == L"src" ? 2 : 0;
        }
        else if (a == L"--limit") limit = std::stoi(next());
        else if (a == L"--pictures") pictures = std::stoi(next());
        else if (a == L"--budget") budgetMs = std::stod(next());
        else if (a == L"--parallel") o.parallel = std::stoi(next());
        else if (a == L"--device") {
            const std::wstring d = next();
            o.device = d == L"cpu" ? Device::Cpu : d == L"igpu" ? Device::IntegratedGpu : Device::Auto;
        }
        else if (a == L"--download") {
            doDownload = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') downloadId = toUtf8(next());
        } else if (a == L"--idle-test") idleFile = next();
        else if (a == L"--status") doStatus = true;
        else if (a == L"--dl-test") {  // HOST PATH SIZE DEST [SHA|-]
            dlHost = next();
            dlPath = next();
            dlSize = std::stoull(next());
            dlDest = next();
            if (i + 1 < argc && argv[i + 1][0] != L'-') dlSha = next();
            if (dlSha == L"-") dlSha.clear();
        } else if (a == L"--ms-test") msTest = next();
        else if (a == L"--dl-conns") {
            dlStart = std::stoi(next());
            dlMax = std::stoi(next());
        } else if (a == L"--dl-cancel-ms") dlCancelMs = std::stod(next());
    }
    if (cores > 0)
        if (DWORD_PTR m = coreMask(cores)) SetProcessAffinityMask(GetCurrentProcess(), m);

    if (!msTest.empty()) {  // --ms-test ja-en|ko-en|…|ocr|ocrgpu: ModelStore downloads through the shared downloader
        std::atomic<bool> cancel{false};
        std::thread canceller;
        double cancelAt = 0;
        if (dlCancelMs > 0) canceller = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(dlCancelMs)));
            cancelAt = nowMs();
            cancel = true;
        });
        const std::string what = toUtf8(msTest);
        std::cout << "missing " << (what == "ocrgpu" ? ModelStore::ocrGpuMissingBytes() : ModelStore::missingBytes({what})) / 1048576.0
                  << " MB\n";
        std::wstring e;
        int lastPct = -1;
        auto prog = [&](double f) {
            const int pct = static_cast<int>(f * 100);
            if (pct / 20 != lastPct / 20) std::cout << pct << "% " << std::flush;
            lastPct = pct;
        };
        const double t0 = nowMs();
        const bool ok = what == "ocrgpu" ? ModelStore::downloadOcrGpu(prog, &cancel, &e) : ModelStore::download({what}, prog, &cancel, &e);
        const double t1 = nowMs();
        if (canceller.joinable()) canceller.join();
        std::cout << "\n" << (ok ? "ok" : "failed: " + toUtf8(e)) << ", " << (t1 - t0) / 1000 << " s"
                  << (cancelAt > 0 && !ok ? ", cancel latency " + std::to_string(t1 - cancelAt) + " ms" : "") << ", installed "
                  << (what == "ocrgpu" ? ModelStore::ocrGpuInstalled() : ModelStore::installed(what)) << "\n";
        return ok ? 0 : 1;
    }
    if (!dlHost.empty()) {  // --dl-test: the shared downloader on one file (speed, connections, cancel latency)
        dl::Item it{toUtf8(dlHost), toUtf8(dlPath), dlDest, dlSize, toUtf8(dlSha)};
        dl::Options op;
        op.startConnections = dlStart;
        op.maxConnections = dlMax;
        std::atomic<bool> cancel{false};
        std::thread canceller;
        if (dlCancelMs > 0) canceller = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(dlCancelMs)));
            cancel = true;
        });
        const double t0 = nowMs();
        double lastShown = 0;
        auto r = dl::fetchAll({it}, [&](const dl::Progress& p) {
            if (nowMs() - lastShown < 2000) return;
            lastShown = nowMs();
            std::cout << "  " << p.done / 1048576.0 << " MB, " << p.bytesPerSec / 1048576.0 << " MB/s, " << p.connections
                      << " connections, left " << p.secondsLeft << " s\n";
        }, &cancel, op);
        if (canceller.joinable()) canceller.join();
        const double s = (nowMs() - t0) / 1000;
        const dl::Stats st = dl::lastStats();
        std::cout << "result " << static_cast<int>(r[0].status) << " " << toUtf8(r[0].detail) << ", " << s << " s, "
                  << (st.cancelMs < 0 ? dlSize / 1048576.0 / s : 0) << " MB/s, max connections " << st.maxConnections
                  << ", ranged " << (st.ranged.empty() ? 0 : static_cast<int>(st.ranged[0])) << ", cancel latency "
                  << st.cancelMs << " ms\n";
        return r[0].status == dl::Status::Ok || r[0].status == dl::Status::Cancelled ? 0 : 1;
    }
    if (doStatus) {  // what this PC would use / download (no network)
        const ModelInfo& m = activeModel();
        std::cout << "model " << m.id << " (default " << defaultModel().id << "), discrete GPU " << discreteGpu()
                  << ", GPU add-on wanted " << gpuWanted() << " installed " << gpuInstalled() << ", to download "
                  << missingBytes(m) / 1048576.0 << " MB\n";
        std::wcout << describeDownload(m);
        return 0;
    }
    if (doDownload) {
        const ModelInfo* m = downloadId.empty() ? &activeModel() : findModel(downloadId);
        if (!m) {
            std::cerr << "unknown model\n";
            return 2;
        }
        std::wcout << L"to download: " << missingBytes(*m) / 1048576.0 << L" MB\n" << describeDownload(*m);
        std::wstring err;
        int lastPct = -1;
        const double t0 = nowMs();
        const bool ok = download(*m, [&](double f) {
            const int pct = static_cast<int>(f * 100);
            if (pct / 10 != lastPct / 10) std::wcout << pct << L"% " << std::flush;
            lastPct = pct;
        }, nullptr, &err);
        std::wcout << L"\n" << (ok ? L"ok" : L"FAILED: " + err) << L" (" << (nowMs() - t0) / 1000 << L" s), installed="
                   << installed(*m) << L"\n";
        return ok ? 0 : 1;
    }

    if (!idleFile.empty()) {
        o.modelFile = idleFile;
        o.idleSeconds = 6;
        LlmEngine e(o);
        std::vector<TrRequest> rq(1);
        rq[0].text = L"直射日光を避けてください。";
        rq[0].src = Lang::Ja;
        rq[0].tgt = Lang::ZhHant;
        std::vector<std::wstring> tx;
        std::wstring err;
        const bool ok = e.translate(rq, tx, &err);
        double pw, pp, ws;
        memory(pw, pp, ws);
        std::cout << "translated=" << ok << " " << toUtf8(tx.empty() ? L"" : tx[0]) << " loaded=" << e.loaded()
                  << " ws=" << ws << "MB\n";
        for (int k = 0; k < 15 && e.loaded(); ++k) std::this_thread::sleep_for(std::chrono::seconds(1));
        PROCESS_MEMORY_COUNTERS_EX pm{};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm));
        std::cout << "after idle: loaded=" << e.loaded() << " private=" << pm.PrivateUsage / 1048576.0 << "MB ws="
                  << pm.WorkingSetSize / 1048576.0 << "MB\n";
        const double t0 = nowMs();
        const bool ok2 = e.translate(rq, tx, &err);
        std::cout << "reload+translate=" << ok2 << " " << (nowMs() - t0) << " ms: " << toUtf8(tx.empty() ? L"" : tx[0]) << "\n";
        return ok && ok2 ? 0 : 1;
    }

    if (in.empty() || out.empty()) {
        std::cerr << "usage: pm_llm_eval --model ID|FILE.gguf --in SEGMENTS.tsv --out RESULT.jsonl [options]\n";
        return 2;
    }
    std::wstring err;
    if (!LlmEngine::runtimeAvailable(&err)) {
        std::cerr << "runtime: " << toUtf8(err) << "\n";
        return 1;
    }
    LlmEngine eng(o);
    const double t0 = nowMs();
    if (!eng.load(&err)) {
        std::cerr << "load: " << toUtf8(err) << "\n";
        return 1;
    }
    const double loadMs = nowMs() - t0;
    double pw, pp, ws;
    memory(pw, pp, ws);
    const double wsAfterLoad = ws;

    // All segments, then the "pictures": --pictures 0 = one call per segment,
    // -1 = by the id's picture (the part before ':'), N = N consecutive ones.
    struct Seg {
        std::vector<std::string> c;
        TrRequest rq;
    };
    std::vector<Seg> segs;
    {
        std::ifstream f(in, std::ios::binary);
        std::string line;
        const Lang tgt = langOf(tgtS);
        while (std::getline(f, line) && static_cast<int>(segs.size()) < limit) {
            if (line.empty() || line[0] == '#') continue;
            Seg g;
            g.c = split(line, '\t');
            if (g.c.size() < 3) continue;
            TrRequest& rq = g.rq;
            rq.src = langOf(g.c[1]);
            rq.tgt = tgt;
            rq.text = fromUtf8(unescape(g.c[2]));
            if (useCtx && g.c.size() > 3) rq.context = fromUtf8(unescape(g.c[3]));
            if (useCtx && g.c.size() > 4) rq.heading = fromUtf8(unescape(g.c[4]));
            if (rq.src == rq.tgt) continue;
            // Column 6: the English of the Bergamot pivot.  --pivot fallback:
            // used when the direct answer stays untranslated; --pivot src: the
            // LLM translates the English instead of the source (the pivot path).
            const std::wstring pivotText = g.c.size() > 5 ? fromUtf8(unescape(g.c[5])) : L"";
            if (pivotMode == 2 && !pivotText.empty()) {
                rq.text = pivotText;
                rq.src = Lang::En;
            } else if (pivotMode == 1) {
                rq.pivot = pivotText;
            }
            rq.budgetMs = budgetMs;
            segs.push_back(std::move(g));
        }
    }
    std::vector<std::pair<size_t, size_t>> pics;
    for (size_t i = 0; i < segs.size();) {
        size_t j = i + 1;
        if (pictures < 0) {
            const std::string pic = segs[i].c[0].substr(0, segs[i].c[0].find(':'));
            while (j < segs.size() && segs[j].c[0].substr(0, segs[j].c[0].find(':')) == pic) ++j;
        } else if (pictures > 0) {
            j = std::min(segs.size(), i + static_cast<size_t>(pictures));
        }
        pics.push_back({i, j});
        i = j;
    }
    std::ofstream res(out, std::ios::binary);
    int n = 0, retried = 0, viaEn = 0, cutN = 0;
    double sumMs = 0;
    std::vector<double> lat;
    for (auto [i0, i1] : pics) {
        std::vector<TrRequest> rqs;
        for (size_t i = i0; i < i1; ++i) rqs.push_back(segs[i].rq);
        std::vector<std::wstring> tx;
        std::vector<std::string> raws;
        const double s0 = nowMs();
        eng.translate(rqs, tx, &err, &raws);
        const double ms = nowMs() - s0;
        const Stats st = eng.stats();
        sumMs += ms;
        lat.push_back(ms);
        retried += st.retried;
        viaEn += st.viaEnglish;
        cutN += st.cutByBudget;
        for (size_t i = i0; i < i1; ++i) {
            const auto& c = segs[i].c;
            res << "{\"id\":" << json(c[0]) << ",\"src\":" << json(c[2]) << ",\"out\":" << json(toUtf8(tx[i - i0]))
                << ",\"raw\":" << json(raws[i - i0]) << ",\"ms\":" << ms / (i1 - i0) << ",\"pic_ms\":" << ms
                << ",\"pic_n\":" << (i1 - i0) << "}\n";
            ++n;
        }
        std::cout << (i1 - i0) << " segments " << static_cast<int>(ms) << " ms (" << st.genTokens << " tokens, "
                  << static_cast<int>(st.genTps) << " tok/s)  " << toUtf8(tx.empty() ? L"" : tx[0]) << "\n";
    }
    memory(pw, pp, ws);
    std::sort(lat.begin(), lat.end());
    const double p50 = lat.empty() ? 0 : lat[lat.size() / 2], p95 = lat.empty() ? 0 : lat[std::min(lat.size() - 1, lat.size() * 95 / 100)];
    const double maxMs = lat.empty() ? 0 : lat.back();
    const size_t nb = lat.size();
    res << "{\"summary\":1,\"family\":" << json(eng.family()) << ",\"device\":" << json(eng.device()) << ",\"threads\":"
        << o.threads << ",\"cores\":" << cores << ",\"parallel\":" << o.parallel << ",\"pictures\":" << pictures
        << ",\"budget_ms\":" << budgetMs << ",\"load_ms\":" << loadMs << ",\"n\":" << n << ",\"calls\":" << nb
        << ",\"mean_ms\":" << (nb ? sumMs / nb : 0) << ",\"p50_ms\":" << p50 << ",\"p95_ms\":" << p95 << ",\"max_ms\":" << maxMs
        << ",\"retried\":" << retried << ",\"via_en\":" << viaEn << ",\"cut\":" << cutN << ",\"ws_after_load_mb\":" << wsAfterLoad
        << ",\"peak_ws_mb\":" << pw << ",\"peak_private_mb\":" << pp << "}\n";
    std::cout << "device " << eng.device() << ", load " << loadMs << " ms, " << n << " segments in " << nb << " calls, mean "
              << (nb ? sumMs / nb : 0) << " ms, p50 " << p50 << " p95 " << p95 << " max " << maxMs << ", retried " << retried
              << ", via en " << viaEn << ", cut " << cutN << ", peak working set " << pw << " MB\n";
    return 0;
}
