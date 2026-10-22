// pm_cf_addon_test (0.8.4): the cloudflared add-on download (app/remote/cloudflared_addon.cpp) against local fake servers.
//   pm_cf_addon_test --root DIR [--cancel-after-ms N]
// Env (test build only): PM_CF_TEST_MIRROR / PM_CF_TEST_ORIGIN = 127.0.0.1:PORT, PM_CF_TEST_PIN = "<size>:<sha256>".
// Prints "PROGRESS n first last" lines' summary and "RESULT ok|err <text> installed=0|1 steps=N maxPct=P". Driven by cf_addon_test.mjs.
#include "remote/cloudflared_addon.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    std::wstring root;
    int cancelAfterMs = -1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--root") && i + 1 < argc) {
            const std::string s = argv[++i];
            root.assign(s.begin(), s.end());
        } else if (!std::strcmp(argv[i], "--cancel-after-ms") && i + 1 < argc) {
            cancelAfterMs = std::atoi(argv[++i]);
        }
    }
    std::atomic<bool> cancel{false};
    std::atomic<int> steps{0}, maxPct{0};
    std::atomic<bool> done{false};
    std::thread killer;
    if (cancelAfterMs >= 0)
        killer = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(cancelAfterMs));
            cancel = true;
        });
    std::wstring err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = pm_app::cfaddon::download(root, [&](const pm_app::cfaddon::Progress& p) {
        ++steps;
        if (p.percent() > maxPct) maxPct = p.percent();
    }, &cancel, &err);
    done = true;
    if (killer.joinable()) killer.join();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const bool installed = !pm_app::cfaddon::installedExe(root).empty();
    std::string e(err.begin(), err.end());
    std::printf("RESULT %s %s installed=%d steps=%d maxPct=%d ms=%lld\n", ok ? "ok" : "err", e.empty() ? "-" : e.c_str(), installed ? 1 : 0, steps.load(),
                maxPct.load(), ms);
    return ok ? 0 : 1;
}
