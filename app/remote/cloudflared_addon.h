// 自在投影 app: cloudflared as an on-demand add-on (0.8.4).
// cloudflared (Cloudflare Tunnel client, Apache-2.0, 52 MB) was in the installer although only 臨時網址 (and the
// fixed address's fallback when Funnel cannot be used) runs it. Now it is fetched the first time it is needed, like
// the local-AI runtime: from this project's GitHub Pages mirror (/ZizaiCast/addons/), GitHub's own release second,
// SHA-256 checked, resumable, with progress. Installed to <data dir>\addons\cloudflared\. An unmodified official
// release, so the same bytes (and the same pin) on both hosts.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace pm_app::cfaddon {

inline constexpr char kVersion[] = "2026.7.2";
inline constexpr uint64_t kExeSize = 54159760;
inline constexpr char kExeSha256[] = "cdb5d4432f6ae1595654a692a51308b69d2bf7af961f5578d9391837cf072df9";
inline constexpr uint64_t kLicenseSize = 11357;
inline constexpr char kLicenseSha256[] = "58d1e17ffe5109a7ae296caafcadfdbe6a7d176f0bc4ab01e12a689b0499d8bd";
// names on the mirror (launch/_release/addons/)
inline constexpr char kMirrorExeName[] = "cloudflared-2026.7.2-windows-amd64.exe";
inline constexpr char kMirrorLicenseName[] = "cloudflared-2026.7.2-LICENSE.txt";

struct Progress {
    uint64_t done = 0, total = 0;
    double bytesPerSec = 0;
    double secondsLeft = -1;
    bool verifying = false;
    int percent() const { return total ? static_cast<int>(done * 100 / total) : 0; }
};

// <addonsRoot>\cloudflared
std::wstring dirIn(const std::wstring& addonsRoot);
// The installed cloudflared.exe ("" when absent or not the pinned bytes). SHA-256 is computed once per run per file state.
std::wstring installedExe(const std::wstring& addonsRoot);
// Blocking (call it on a worker thread). progress: ~every 100 ms. err: "cancelled" or a short reason. Also writes
// VERSION.txt next to the exe and LICENSE. Already complete + verified files are not fetched again.
bool download(const std::wstring& addonsRoot, const std::function<void(const Progress&)>& progress,
              const std::atomic<bool>* cancel, std::wstring* err);

}  // namespace pm_app::cfaddon
