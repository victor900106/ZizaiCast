// 自在投影 app: core/base/state_query.cpp — core/base（第 0 層：共用型別、狀態與原語）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// ---- AirPlay server lifetime ---------------------------------------------
// Without an HEVC decoder only 標準 (H.264) works; the stored choice is kept
// so it applies once the decoder is installed.
int effectiveQuality() { return g.hevc ? g.settings.quality : 0; }

// ---- 錄影 ---------------------------------------------------------------------
bool recording() { return !g.recordingFile.empty(); }

bool pictureShowing() { return g.videoState == StateMirroring || g.videoState == StatePaused; }

// A phone picture is on screen (AirPlay / Android through the status sink, or
// a Miracast cast, which draws into the window itself).
bool viewAvailable() {
    if (g.videoState == StateMirroring || g.videoState == StatePaused) return g_active.load() != SrcNone;
    return g_active.load() == SrcMiracast && g.liveSource == SrcMiracast;
}

pm::translate::Lang translateTarget() {
    using L = pm::translate::Lang;
    switch (g.settings.translateTo) {
    case 1: return L::ZhHant;
    case 2: return L::En;
    case 3: return L::Ja;
    case 4: return L::Ko;
    default: return pm::translate::defaultTarget();
    }
}

bool translating() { return g.translator && g.translator->active(); }

std::wstring zoomText(float z) {
    wchar_t buf[16];
    swprintf_s(buf, std::fabs(z - std::round(z)) < 0.05f ? L"%.0f×" : L"%.1f×", z);
    return buf;
}

// The update on offer: the newer of an installer in <install>\安裝檔 (本機更新)
// and the manifest's (自動更新); local wins a tie (nothing to download).
bool offerLocal() {
    return !g.localVersion.empty() &&
           (g.update.version.empty() || pm::update::compareVersions(g.localVersion, g.update.version) >= 0);
}
std::string offerVersion() { return offerLocal() ? g.localVersion : g.update.version; }

// The source on screen (or an iPhone connecting / asking for its PIN), else
// SrcNone. Used for 中斷連線 (menus, toolbar, Ctrl+D).
int liveSourceNow() {
    const int a = g_active.load();
    if (a != SrcNone) return a;
    if (g.sessionActive) return SrcAirPlay;
    return SrcNone;
}

// 「Galaxy S24（Android）」 (short: 「Galaxy S24」) for menus and toasts; the
// source's label if no name is known.
std::wstring liveName(bool shortName) {
    if (!g.peerName.empty() && g.liveSource == liveSourceNow()) return shortName ? g.sourceName : g.peerName;
    const int s = liveSourceNow();
    return s == SrcAirPlay ? std::wstring(L"iPhone") : std::wstring(sourceLabel(s));
}

// The short form of a label for a submenu row's value column: 「高 · 建議」 ->
// 「高」, 「自動（跟隨 Windows）」 -> 「自動」.
std::wstring shortLabel(const wchar_t* s) {
    std::wstring t = s;
    for (const wchar_t* cut : {L" · ", L"（", L" (", L"("}) {
        const size_t at = t.find(cut);
        if (at != std::wstring::npos && at > 0) t.resize(at);
    }
    return t;
}

bool androidLive() { return g_active.load() == SrcAndroid; }

}  // namespace pm_app
