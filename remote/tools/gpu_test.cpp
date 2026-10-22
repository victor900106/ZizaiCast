// 0.8.3.7: the stats line's GPU percentages (gpu_util.h). The owner's log (2026-10-22 15:11:29) had
// "GPU 3D 36713195488442.6%": reproduced here from raw values with 0.8.3.6's arithmetic (an unsigned running-time
// difference over 5.02 s), and the 0.8.3.7 computation on the same values (left out = not shown), plus the
// ordinary cases and one live PDH run on this process (no GPU work is made; only that it stays 0..100 / n/a).
//   pm_gpu_test.exe   -> prints each check, exit code 0 when all pass
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <regex>
#include <string>

#include "gpu_util.h"
#include "util.h"

using namespace pm::remote;

static int fails = 0, passes = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  -- ", detail.c_str());
    (ok ? passes : fails)++;
}

int main() {
    const std::string I3d = "pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_3D", Ienc = "pid_1_luid_0x0_0x1_phys_0_eng_5_engtype_VideoEncode";
    // 1. the owner's figure: the 3D engine's running time reads 1 tick (100 ns) lower than 5.0247 s before.
    {
        const long long dT = 50246800;  // 5.02468 s in 100 ns (the log's lines were 5.024 s apart)
        GpuRaw a{"3D", 9000000000LL, 1000000000LL}, b{"3D", 9000000000LL - 1, 1000000000LL + dT};
        const double before = gpuPdhStylePercent(a, b);
        char d[160];
        snprintf(d, sizeof(d), "0.8.3.6 arithmetic gives %.1f%% (log: 36713195488442.6%%)", before);
        check(std::fabs(before - 36713195488442.6) / 36713195488442.6 < 1e-4, "owner's overflow reproduced: unsigned dRun over 5.02 s", d);
        std::map<std::string, GpuRaw> p{{I3d, a}, {Ienc, {"VideoEncode", 100, 1000000000LL}}},
            c{{I3d, b}, {Ienc, {"VideoEncode", 100 + dT * 45 / 100, 1000000000LL + dT}}};
        const GpuSample s = gpuPercent(p, c);
        const std::string txt = gpuText(s);
        check(!s.pct.count("3D") && s.dropped == 1, "0.8.3.7: the 3D engine that went backwards is left out (hidden)", "\"" + txt + "\"");
        check(txt == "VideoEncode 45.0%", "0.8.3.7: the other engine still shows", "\"" + txt + "\"");
    }
    // 2. ordinary: 25 % and 5 % on two 3D engines (two adapters) -> the busiest one; NVENC 38.4 %.
    {
        const long long dT = 50000000;
        std::map<std::string, GpuRaw> p{{I3d, {"3D", 0, 0}}, {"pid_1_luid_0x0_0x2_phys_0_eng_0_engtype_3D", {"3D", 0, 0}}, {Ienc, {"VideoEncode", 0, 0}}},
            c{{I3d, {"3D", dT / 4, dT}}, {"pid_1_luid_0x0_0x2_phys_0_eng_0_engtype_3D", {"3D", dT / 20, dT}}, {Ienc, {"VideoEncode", dT * 384 / 1000, dT}}};
        const std::string txt = gpuText(gpuPercent(p, c));
        check(txt == "3D 25.0% VideoEncode 38.4%", "ordinary sample (busiest engine per type)", "\"" + txt + "\"");
    }
    // 3. a little over 100 % (stamps read at slightly different moments) is clamped
    {
        std::map<std::string, GpuRaw> p{{I3d, {"3D", 0, 0}}}, c{{I3d, {"3D", 1010, 1000}}};
        const auto s = gpuPercent(p, c);
        check(s.pct.at("3D") == 100.0, "101 % clamped to 100 %", gpuText(s));
    }
    // 4. no time between the samples / a new instance / an invalid status: nothing made up
    {
        std::map<std::string, GpuRaw> p{{I3d, {"3D", 0, 500}}}, c{{I3d, {"3D", 10, 500}}, {Ienc, {"VideoEncode", 10, 500}}};
        const auto s = gpuPercent(p, c);
        check(s.pct.empty() && gpuText(s) == "n/a", "zero time stamp difference -> n/a; a new instance waits for the next sample", gpuText(s));
        GpuRaw bad{"3D", 100, 1000};
        bad.valid = false;
        std::map<std::string, GpuRaw> c2{{I3d, bad}};
        check(gpuPercent(p, c2).pct.empty(), "invalid PDH status -> left out");
        std::map<std::string, GpuRaw> idle{{I3d, {"3D", 0, 1000}}};
        check(gpuText(gpuPercent(p, idle)) == "0", "an idle engine says 0");
    }
    // 5. live: this process through PDH (no GPU work): every figure 0..100 or n/a
    {
        GpuMeter g;
        std::string all;
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            Sleep(300);
            const std::string t = g.sample();
            all += (all.empty() ? "" : " | ") + t;
            std::smatch m;
            std::string r = t;
            while (std::regex_search(r, m, std::regex("([0-9.]+)%"))) {
                ok &= std::stod(m[1]) >= 0 && std::stod(m[1]) <= 100;
                r = m.suffix();
            }
        }
        double d3 = 0, enc = 0;
        g.last(d3, enc);
        ok &= (d3 == -1 || (d3 >= 0 && d3 <= 100)) && (enc == -1 || (enc >= 0 && enc <= 100));
        check(ok, "live PDH sample of this process stays 0..100 % (or n/a)", all);
    }
    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
