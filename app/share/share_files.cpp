// 自在投影 app: share/share_files.cpp — share（傳到手機）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/share.h"
#include "share/share_internal.h"

namespace pm_app {

// ---- 傳到手機: which files ----
bool isShareImage(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = towlower(c);
    return e == L".png" || e == L".jpg" || e == L".jpeg";
}
bool isShareVideo(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = towlower(c);
    return e == L".mp4" || e == L".mov";
}
// Newest picture / video in `dir` (not the recording in progress).
fs::path newestIn(const fs::path& dir, bool video) {
    // Every menu / toolbar refresh asks: a big folder is listed at most every
    // 10 s (a capture of this run is g.lastShot / g.lastRec, found at once).
    struct Memo {
        std::wstring dir, rec;
        fs::path best;
        ULONGLONG at = 0;
    };
    static Memo memo[2];
    Memo& m = memo[video ? 1 : 0];
    const ULONGLONG now = GetTickCount64();
    std::error_code ec;
    if (m.at && now - m.at < 10000 && m.dir == dir.wstring() && m.rec == g.recordingFile.wstring() &&
        (m.best.empty() || fs::exists(m.best, ec)))
        return m.best;
    fs::path best;
    fs::file_time_type bestT{};
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || (video ? !isShareVideo(e.path()) : !isShareImage(e.path()))) continue;
        if (!g.recordingFile.empty() && e.path() == g.recordingFile) continue;
        const auto t = e.last_write_time(ec);
        if (best.empty() || t > bestT) best = e.path(), bestT = t;
    }
    m = {dir.wstring(), g.recordingFile.wstring(), best, now};
    return best;
}
fs::path lastShotFile() {
    std::error_code ec;
    if (!g.lastShot.empty() && fs::exists(g.lastShot, ec)) return g.lastShot;
    return newestIn(screenshotDir(), false);
}
fs::path lastRecFile() {
    std::error_code ec;
    if (!g.lastRec.empty() && fs::exists(g.lastRec, ec)) return g.lastRec;
    return newestIn(recordingDir(), true);
}
// Toolbar 傳到手機: the newer of the two.
fs::path lastShareable() {
    const fs::path a = lastShotFile(), b = lastRecFile();
    if (a.empty() || b.empty()) return a.empty() ? b : a;
    std::error_code e1, e2;
    return fs::last_write_time(b, e2) > fs::last_write_time(a, e1) ? b : a;
}

}  // namespace pm_app
