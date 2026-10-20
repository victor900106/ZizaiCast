// 自在投影 app: share/share_clip.cpp — 電腦剪貼簿 on the 傳到手機 page (0.8.3).
// While a share the owner opened himself runs, the window listens for
// clipboard changes (AddClipboardFormatListener: no polling) and keeps a copy
// of the copied text, which the share server hands to the page (/clip).  The
// share stopping stops the listener and forgets the text.  Text marked as
// private by password managers (ExcludeClipboardContentFromMonitorProcessing /
// CanIncludeInClipboardHistory = 0) is never offered.  --test-offscreen never
// reads the user's clipboard: the text comes from PM_CLIP_TEST.

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/share.h"

namespace pm_app {

namespace {
std::mutex g_clipM;
std::wstring g_clipText;  // guarded by g_clipM (the server reads it on its threads)
bool g_clipOn = false;    // UI thread
constexpr size_t kClipMaxChars = 100000;

bool privateClip() {
    static const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (exclude && IsClipboardFormatAvailable(exclude)) return true;
    if (history && IsClipboardFormatAvailable(history)) {
        bool no = false;
        if (HANDLE h = GetClipboardData(history))
            if (const DWORD* v = static_cast<const DWORD*>(GlobalLock(h))) {
                no = *v == 0;
                GlobalUnlock(h);
            }
        return no;
    }
    return false;
}

void readNow() {
    std::wstring t;
    if (g.testOffscreen) {  // never the user's clipboard in a test
        wchar_t buf[2048] = {};
        const DWORD n = GetEnvironmentVariableW(L"PM_CLIP_TEST", buf, 2048);
        if (n && n < 2048) t.assign(buf, n);
    } else {
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(g.hwnd)) {
            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) return;  // busy: keep the last text, the next update reads it
        } else {
            if (!privateClip())
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
                    if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                        const size_t n = wcsnlen(p, kClipMaxChars);
                        t.assign(p, n);
                        GlobalUnlock(h);
                    }
            CloseClipboard();
        }
    }
    std::lock_guard lk(g_clipM);
    g_clipText = std::move(t);
}
}  // namespace

void shareClipOn() {
    if (!g_clipOn) {
        g_clipOn = AddClipboardFormatListener(g.hwnd) != FALSE || g.testOffscreen;
        g.log->write("info", std::string("share: PC clipboard offered on the page") + (g_clipOn ? "" : " (listener failed)"));
    }
    readNow();
}

void shareClipOff() {
    if (g_clipOn) {
        RemoveClipboardFormatListener(g.hwnd);
        g_clipOn = false;
        g.log->write("info", "share: PC clipboard no longer offered");
    }
    std::lock_guard lk(g_clipM);
    g_clipText.clear();
}

bool shareClipActive() { return g_clipOn; }

std::wstring shareClipText() {
    std::lock_guard lk(g_clipM);
    return g_clipText;
}

void onClipboardUpdate() {
    if (!g_clipOn) return;
    if (!g.share.running() || g.share.expired()) {  // the share ended some other way
        shareClipOff();
        return;
    }
    readNow();
}

}  // namespace pm_app
