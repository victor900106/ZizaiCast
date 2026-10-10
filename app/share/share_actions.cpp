// 自在投影 app: share/share_actions.cpp — share（傳到手機）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/share.h"
#include "share/share_internal.h"

namespace pm_app {

void onShareEvent(WPARAM kind, LPARAM lp) {
    if (kind == SharePushDone) {
        std::unique_ptr<PushDone> d(reinterpret_cast<PushDone*>(lp));
        if (d) onPushDone(*d);
        return;
    }
    if (kind == ShareExpired) {
        if (static_cast<unsigned>(lp) != g_shareGen) return;  // an earlier share's
        if (g.share.running()) g.sharePanel.setExpired();
        return;
    }
    if (kind == ShareAccess) {
        std::unique_ptr<ShareAccessMsg> m(reinterpret_cast<ShareAccessMsg*>(lp));
        if (!m || m->gen != g_shareGen) return;  // an earlier share's (its file index means another list)
        const pm::share::Server::Access* a = &m->a;
        if (!g.sharePanel.isOpen() || a->status >= 400) return;
        if (g.share.live()) g.sharePanel.setLive(true, g.share.fileCount());
        const std::wstring ip = toWide(a->ip);
        if (a->file < 0) {
            g.sharePanel.setStatus(fmt(S::SharePanelOpened, {ip}));
            return;
        }
        const std::vector<std::wstring> files = g.share.files();
        if (a->file >= static_cast<int>(files.size())) return;
        const std::wstring name = fs::path(files[static_cast<size_t>(a->file)]).filename().wstring();
        g.sharePanel.setStatus(fmt(a->download ? S::SharePanelDownloading : S::SharePanelViewing, {name}));
    }
}

// 傳到手機… : any screenshots / recordings (multi-select), starting in 截圖.
std::vector<fs::path> pickShareFiles() {
    std::vector<fs::path> out;
    if (g.testOffscreen) {  // no dialog on the user's screen: the last of each
        g.log->write("info", "share: file picker skipped (test-offscreen): last screenshot + last recording");
        for (const fs::path& f : {lastShotFile(), lastRecFile()})
            if (!f.empty()) out.push_back(f);
        return out;
    }
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(tr(S::SharePickTitle));
        const std::wstring filterName = tr(S::SharePickFilter);
        COMDLG_FILTERSPEC spec[] = {{filterName.c_str(), L"*.png;*.jpg;*.jpeg;*.mp4;*.mov"}};
        dlg->SetFileTypes(1, spec);
        const fs::path shots = screenshotDir(), recs = recordingDir();
        for (const fs::path& d : {recs, shots}) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(d.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                dlg->AddPlace(item, FDAP_TOP);
                if (d == shots) dlg->SetFolder(item);
                item->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(g.hwnd))) {
            IShellItemArray* arr = nullptr;
            if (SUCCEEDED(dlg->GetResults(&arr))) {
                DWORD n = 0;
                arr->GetCount(&n);
                for (DWORD i = 0; i < n && out.size() < static_cast<size_t>(pm::share::Server::kMaxFiles); ++i) {
                    IShellItem* it = nullptr;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(arr->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        out.push_back(path);
                        CoTaskMemFree(path);
                    }
                    if (it) it->Release();
                }
                arr->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return out;
}

// 傳到手機 with several of this run's captures: thumbnails, the unsent ones ticked.
void openSharePicker() {
    std::vector<pm::ui::SharePicker::Item> items;
    std::error_code ec;
    for (auto it = g.captures.rbegin(); it != g.captures.rend() && items.size() < 48; ++it) {  // newest first
        if (!fs::exists(it->file, ec)) continue;
        pm::ui::SharePicker::Item p;
        p.path = it->file.wstring();
        p.video = it->video;
        p.sent = it->sent;
        p.checked = !it->sent;
        std::tm tm{};
        localtime_s(&tm, &it->at);
        wchar_t hhmmss[16];
        wcsftime(hhmmss, 16, L"%H:%M:%S", &tm);
        p.label = hhmmss;
        items.push_back(std::move(p));
    }
    g.shareChip.hide();
    g.log->write("info", "share: picker with " + std::to_string(items.size()) + " capture(s)");
    g.sharePicker.open(g.hwnd, std::move(items), [](std::vector<std::wstring> paths) {
        std::vector<fs::path> files(paths.begin(), paths.end());
        shareFiles(std::move(files));
    });
}

// 傳到手機 (toolbar, chip, menu): the captures not sent yet; one (or none: the
// newest file, as before) goes at once, several open the picker.
void shareSession() {
    std::vector<fs::path> unsent = unsentCaptures();
    if (unsent.size() >= 2) {
        openSharePicker();
        return;
    }
    shareFiles(unsent.empty() ? std::vector<fs::path>{lastShareable()} : unsent);
}

void setAutoShare(bool on) {
    if (on == g.settings.autoShare) return;
    g.settings.autoShare = on;
    saveSettings();
    g.log->write("info", std::string("share: auto-send ") + (on ? "on" : "off"));
    if (on) {
        g.window->showToast(tr(S::ShareAutoOn), 3000);
        // Not an Android phone over adb: the live page now, so the phone can open it before the first capture.
        if (!androidPushPath() && !liveShareOn()) startShareQr({}, true);
        return;
    }
    if (g.share.live()) {
        g.sharePanel.close();
        g.share.stop();
    }
    g.window->showToast(tr(S::ShareAutoOff), 3000);
}

// A new screenshot / recording (自動傳到手機).
void autoSend(const fs::path& f) {
    g.log->write("info", "share: auto-send " + toUtf8(f.filename().wstring()));
    if (androidPushPath()) queuePush({f});
    else sendViaQr({f}, false);
}

void shareCommand(UINT cmd) {
    switch (cmd) {
    case CmdShareLast: shareSession(); break;
    case CmdShareAuto: setAutoShare(!g.settings.autoShare); break;
    case CmdShareLastShot: shareFiles({lastShotFile()}); break;
    case CmdShareLastRec: shareFiles({lastRecFile()}); break;
    case CmdSharePick: {
        std::vector<fs::path> files = pickShareFiles();
        if (!files.empty()) shareFiles(std::move(files));
        break;
    }
    case CmdShareStop:
        g.sharePanel.close();
        if (g.share.running()) {
            g.share.stop();
            g.window->showToast(tr(S::ShareStopped));
        }
        break;
    }
}

// After a screenshot / recording: remembered for 傳到手機, then sent at once
// (自動傳到手機) or offered. 0.7.8: the chip above the 「截圖已儲存」 /
// 「錄影已儲存」 toast says where it went: 開啟資料夾 · 傳到手機 (only
// 開啟資料夾 when it is sent automatically).
void offerShare(const fs::path& file) {
    if (g.quitting) return;
    const bool video = isShareVideo(file);
    g.captures.push_back(App::Capture{file, video, false, std::time(nullptr)});
    std::vector<pm::ui::ShareChip::Action> acts;
    acts.push_back({video ? kIcoVideoFolder : kIcoFolder, tr(S::ChipOpenFolder), [video]() {
                        g.log->write("info", std::string("capture chip: open the ") + (video ? "recordings" : "screenshots") + " folder");
                        if (video) openRecordingDir();
                        else openScreenshotDir();
                    }});
    if (g.settings.autoShare) {
        autoSend(file);
    } else {
        acts.push_back({kIcoShare, tr(S::ShareChip), []() { shareSession(); }});
    }
    g.applyChip.hide();  // same place (p9); it stays in 設定 ▸
    g.shareChip.show(g.hwnd, std::move(acts));
}

void stopSharing() {
    g.shareChip.hide();
    g.sharePicker.close();
    g.sharePanel.close();
    g.share.stop();
    g.testPush.reset();
}

}  // namespace pm_app
