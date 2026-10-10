// 自在投影 app: core/base/live_toolbar.cpp — core/base（第 0 層：共用型別、狀態與原語）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// ---- live toolbar (drawn by the video window over the picture) -------------
// Shown on mouse movement while a phone is live; clicks come back as
// WM_PM_TOOL (posted: the window is inside its own click handling). Updated
// after every message that may change it (appProc), only when it changed.
void refreshToolbar() {
    refreshPausedTitle();  // 翻譯 / 凍結 changed the frozen state
    using TI = pm::VideoWindow::ToolbarItem;
    std::vector<TI> v;
    const int src = liveSourceNow();
    if (src != SrcNone) {
        const bool fs = isFullscreen(), rec = recording();
        if (src == SrcAndroid) {
            v.push_back({CmdAndroidBack, kIcoBack, tr(S::TipBack)});
            v.push_back({CmdAndroidHome, kIcoHome, tr(S::TipHome)});
            v.push_back({CmdAndroidRecents, kIcoRecents, tr(S::TipRecents)});
        }
        // 0.7.8: 截圖 錄影 放大 翻譯 carry a word next to the icon (when the window is wide enough).
        TI shot{CmdSnapshot, kIcoCamera, tr(S::TipSnapshot)};
        shot.label = tr(S::ToolLabelShot);
        shot.groupStart = src == SrcAndroid;
        v.push_back(shot);
        TI recItem{CmdRecord, kIcoRecord, tr(rec ? S::TipStopRec : S::TipStartRec)};
        recItem.toggled = rec;
        recItem.recording = true;
        recItem.label = tr(S::ToolLabelRec);
        v.push_back(recItem);
        // optional: left out of a window too narrow for every button (still in 更多).
        TI share{CmdShareLast, kIcoShare, tr(S::TipShare)};
        share.optional = true;
        v.push_back(share);
        TI rot{CmdRotateRight, kIcoRotate, tr(S::TipRotate)};
        rot.optional = true;
        v.push_back(rot);
        TI full{CmdFullscreen, fs ? kIcoBackToWindow : kIcoFullscreen, tr(fs ? S::TipExitFullscreen : S::TipFullscreen)};
        full.optional = true;
        v.push_back(full);
        g_volumeUi.appendToolbar(v, true);  // 音量: speaker (靜音) + slider
        if (viewAvailable()) {  // 放大鏡 (1× → 2× → 4×) · 翻譯 · 凍結
            const pm::VideoWindow::ViewState vs = g.window->viewState();
            TI mag{CmdMagCycle, kIcoZoom, fmt(S::TipMagnifier, {zoomText(vs.zoom)})};
            mag.toggled = vs.zoom > 1.01f;
            mag.groupStart = true;
            mag.label = tr(S::ToolLabelZoom);
            v.push_back(mag);
            const bool tra = translating();
            TI trn{CmdTranslateToggle, kIcoTranslate, tr(tra ? S::TipTranslateClose : S::TipTranslate)};
            trn.toggled = tra;
            trn.label = tr(S::ToolLabelTranslate);
            v.push_back(trn);
            TI frz{CmdFreeze, kIcoFreeze, tr(vs.frozen ? S::TipUnfreeze : S::TipFreeze)};
            frz.toggled = vs.frozen;
            frz.optional = true;
            v.push_back(frz);
        }
        if (!offerVersion().empty()) {  // 本機更新 / 自動更新 on offer (never installs by itself)
            TI up{CmdUpdateDialog, kIcoUpdate, fmt(S::MenuUpdateTo, {toWide(offerVersion())})};
            up.groupStart = true;
            v.push_back(up);
        }
        v.push_back({CmdMoreMenu, kIcoMore, tr(S::TipMore)});
        TI dc{CmdDisconnect, kIcoDisconnect, tr(S::TipDisconnect)};
        dc.danger = true;
        dc.groupStart = true;
        v.push_back(dc);
    }
    std::string key;
    for (const TI& t : v)
        key += std::to_string(t.id) + ":" + std::to_string(t.glyph) + (t.toggled ? "t" : "") + toUtf8(t.tooltip) +
               toUtf8(t.label) +
               (t.slider >= 0 ? std::to_string(t.slider) : "") + ";";
    if (key == g.toolbarKey) return;
    g.toolbarKey = key;
    const HWND h = g.hwnd;
    g.window->setLiveToolbar(std::move(v), [h](int id) { PostMessageW(h, WM_PM_TOOL, static_cast<WPARAM>(id), 0); });
}

}  // namespace pm_app
