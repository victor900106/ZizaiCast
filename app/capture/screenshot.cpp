// 自在投影 app: capture/screenshot.cpp — capture（截圖、錄影）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/share.h"
#include "capture/capture.h"

namespace pm_app {

// dir\自在投影_YYYYMMDD_HHMMSS<ext> (ZizaiCast_… in English), with _2, _3 ... if taken.
fs::path stampedFile(const fs::path& dir, const wchar_t* ext) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y%m%d_%H%M%S", &tm);
    const std::wstring prefix = std::wstring(tr(S::FilePrefix)) + L"_";
    fs::path file = dir / (prefix + stamp + ext);
    std::error_code ec;
    for (int i = 2; fs::exists(file, ec); ++i) file = dir / (prefix + stamp + L"_" + std::to_wstring(i) + ext);
    return file;
}

void takeSnapshot() {
    const fs::path file = stampedFile(screenshotDir(), L".png");
    // With the iPhone frame on, the snapshot shows the framed picture too.
    const bool ok = g.settings.deviceFrame ? g.window->saveSnapshotFramed(file.wstring())
                                           : g.window->saveSnapshot(file.wstring());
    if (ok) {
        g.log->write("info", std::string("snapshot ") + (g.settings.deviceFrame ? "(framed) " : "") +
                                 toUtf8(file.wstring()));
        g.window->flash();  // shutter: a short veil over the picture
        g.window->showToast(tr(S::ShotSaved));
        g.lastShot = file;
        offerShare(file);
    } else {
        g.window->showToast(tr(S::ShotNone));
    }
}

}  // namespace pm_app
