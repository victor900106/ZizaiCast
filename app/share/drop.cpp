// 自在投影 app: share/drop.cpp — 拖檔到視窗 (0.8.3), see drop.h.

#include <ole2.h>
#include <shlobj.h>

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/drop.h"
#include "share/share.h"

namespace pm_app {

namespace {

enum DropKind : WPARAM { DropDoneMsg = 0, DropFilesMsg = 1 };
struct DropDone {
    fs::path file;
    bool apk = false, ok = false;
    std::wstring msg;
};

bool isApk(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));
    return e == L".apk";
}

// Regular files only (a folder cannot be sent), in the order given.
std::vector<fs::path> onlyFiles(const std::vector<fs::path>& in) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const fs::path& p : in)
        if (fs::is_regular_file(p, ec)) out.push_back(p);
    return out;
}

std::vector<fs::path> pathsOf(IDataObject* obj) {
    std::vector<fs::path> v;
    if (!obj) return v;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (FAILED(obj->GetData(&fe, &sm))) return v;
    if (HDROP drop = static_cast<HDROP>(GlobalLock(sm.hGlobal))) {
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n && i < 500; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring s(len, L'\0');
            if (len && DragQueryFileW(drop, i, s.data(), len + 1)) v.emplace_back(s);
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    return v;
}

// Android pushes / installs, one at a time (the AndroidSource API may not
// take several at once); the callbacks come back as WM_PM_DROP.
std::vector<fs::path> g_dropQueue;
bool g_dropBusy = false;

void dropNext() {
    if (g_dropBusy || g_dropQueue.empty()) return;
    if (!androidPushPath() || !g.android) {  // the phone went away meanwhile: the rest by QR
        std::vector<fs::path> rest = std::move(g_dropQueue);
        g_dropQueue.clear();
        g.log->write("info", "drop: Android gone, " + std::to_string(rest.size()) + " file(s) by QR instead");
        sendViaQr(rest, true);
        return;
    }
    const fs::path f = g_dropQueue.front();
    g_dropQueue.erase(g_dropQueue.begin());
    const bool apk = isApk(f);
    g_dropBusy = true;
    g.log->write("info", std::string("drop: ") + (apk ? "installApk " : "pushFile ") + toUtf8(f.wstring()) + " (" +
                             std::to_string(g_dropQueue.size()) + " more waiting)");
    g.window->showToast(apk ? fmt(S::DropInstalling, {f.filename().wstring()}) : fmt(S::SharePushing, {f.filename().wstring()}),
                        60000);
    const HWND h = g.hwnd;
    auto done = [h, f, apk](bool ok, std::wstring msg) {  // any thread
        auto* d = new DropDone{f, apk, ok, std::move(msg)};
        if (!PostMessageW(h, WM_PM_DROP, DropDoneMsg, reinterpret_cast<LPARAM>(d))) delete d;
    };
    wchar_t fake[4] = {};
    if (g.testAndroid && GetEnvironmentVariableW(L"PM_DROP_FAKE_OK", fake, 4) > 0 && fake[0] == L'1') {
        // --test-source android screenshots: a made-up success (nothing is sent), from another thread like the real one.
        std::thread([done] {
            Sleep(600);
            done(true, L"test");
        }).detach();
        return;
    }
    if (apk) g.android->installApk(f.wstring(), done);
    else g.android->pushFile(f.wstring(), done);
}

// iPhone / iPad / Miracast / no phone: onto the 傳到手機 page (the running one,
// else a new one) and its QR panel up, so the phone can scan and download.
void dropToShare(std::vector<fs::path> files) {
    const size_t max = static_cast<size_t>(pm::share::Server::kMaxFiles);
    if (g.share.running() && !g.share.expired()) {
        int added = 0;
        for (const fs::path& f : files)
            if (g.share.addFile(f.wstring()) >= 0) ++added;
        g.log->write("info", "drop: " + std::to_string(added) + " of " + std::to_string(files.size()) +
                                 " file(s) added to the running share (" + std::to_string(g.share.fileCount()) + " in all)");
        if (added > 0) {
            openSharePanel();
            g.window->showToast(fmt(S::DropAddedShare, {shareFilesLabel(files)}), 3000);
            if (static_cast<size_t>(added) < files.size())
                g.window->showToast(fmt(S::DropTooMany, {std::to_wstring(pm::share::Server::kMaxLiveFiles)}), 4000);
            return;
        }
        // full: a new page with these
    }
    if (files.size() > max) {
        files.resize(max);
        g.window->showToast(fmt(S::DropTooMany, {std::to_wstring(max)}), 4000);
    }
    g.log->write("info", "drop: " + std::to_string(files.size()) + " file(s) → a new QR page");
    startShareQr(files, g.settings.autoShare);
}

class DropTarget final : public IDropTarget {
public:
    DropTarget() { CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_)); }
    ~DropTarget() {
        if (helper_) helper_->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* obj, DWORD, POINTL pt, DWORD* effect) override {
        const std::vector<fs::path> all = pathsOf(obj);
        files_ = onlyFiles(all);
        effect_ = files_.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
        if (effect) *effect = effect_ & *effect ? effect_ : DROPEFFECT_NONE;
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->DragEnter(g.hwnd, obj, &p, effect ? *effect : effect_);
        }
        dropShowHint(all);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        if (effect) *effect = effect_ & *effect ? effect_ : DROPEFFECT_NONE;
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->DragOver(&p, effect ? *effect : effect_);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (helper_) helper_->DragLeave();
        files_.clear();
        dropShowHint({});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* obj, DWORD, POINTL pt, DWORD* effect) override {
        std::vector<fs::path> all = pathsOf(obj);
        if (effect) *effect = all.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->Drop(obj, &p, effect ? *effect : DROPEFFECT_COPY);
        }
        dropShowHint({});
        files_.clear();
        // Handled after the drag source's DoDragDrop returns (Explorer is not kept waiting).
        auto* v = new std::vector<fs::path>(std::move(all));
        if (!PostMessageW(g.hwnd, WM_PM_DROP, DropFilesMsg, reinterpret_cast<LPARAM>(v))) delete v;
        return S_OK;
    }

private:
    LONG ref_ = 1;
    IDropTargetHelper* helper_ = nullptr;
    std::vector<fs::path> files_;
    DWORD effect_ = DROPEFFECT_NONE;
};

DropTarget* g_dropTarget = nullptr;
bool g_oleInit = false;

}  // namespace

void dropInit() {
    const HRESULT ole = OleInitialize(nullptr);  // RegisterDragDrop needs OLE on this (STA) thread
    g_oleInit = SUCCEEDED(ole);
    g_dropTarget = new DropTarget();
    const HRESULT hr = RegisterDragDrop(g.hwnd, g_dropTarget);
    if (FAILED(hr)) {
        char b[96];
        std::snprintf(b, sizeof b, "drop: RegisterDragDrop failed (0x%08lx, OleInitialize 0x%08lx)",
                      static_cast<unsigned long>(hr), static_cast<unsigned long>(ole));
        g.log->write("warn", b);
        g_dropTarget->Release();
        g_dropTarget = nullptr;
    }
}

void dropShutdown() {
    if (g_dropTarget) {
        RevokeDragDrop(g.hwnd);
        g_dropTarget->Release();
        g_dropTarget = nullptr;
    }
    if (g_oleInit) OleUninitialize();
    g_oleInit = false;
}

void dropShowHint(const std::vector<fs::path>& all) {
    if (all.empty()) {
        g.window->setDropHint({});
        return;
    }
    const std::vector<fs::path> files = onlyFiles(all);
    if (files.empty()) {
        g.window->setDropHint(tr(S::DropNoFiles));
        return;
    }
    if (androidPushPath()) {
        const size_t apks = static_cast<size_t>(std::count_if(files.begin(), files.end(), isApk));
        const S title = apks == files.size() ? S::DropHintInstall : apks ? S::DropHintBoth : S::DropHintShare;
        g.window->setDropHint(tr(title), fmt(S::DropSubAndroid, {liveName(true)}));
        return;
    }
    g.window->setDropHint(tr(S::DropHintShare), tr(S::DropSubQr));
}

void dropFiles(std::vector<fs::path> all) {
    std::vector<fs::path> files = onlyFiles(all);
    g.log->write("info", "drop: " + std::to_string(all.size()) + " item(s), " + std::to_string(files.size()) + " file(s)" +
                             (androidPushPath() ? " → Android (adb)" : " → QR page"));
    if (files.empty()) {
        g.window->showToast(tr(S::DropNoFiles), 3000);
        return;
    }
    if (IsWindowVisible(g.hwnd) && !g.testOffscreen) SetForegroundWindow(g.hwnd);  // the answer is in this window
    if (androidPushPath()) {
        for (const fs::path& f : files)
            if (std::find(g_dropQueue.begin(), g_dropQueue.end(), f) == g_dropQueue.end()) g_dropQueue.push_back(f);
        if (g_dropBusy) g.window->showToast(fmt(S::ShareQueued, {std::to_wstring(g_dropQueue.size())}), 2500);
        dropNext();
        return;
    }
    dropToShare(std::move(files));
}

void onDropDone(WPARAM wp, LPARAM lp) {
    if (wp == DropFilesMsg) {  // posted by DropTarget::Drop
        std::unique_ptr<std::vector<fs::path>> v(reinterpret_cast<std::vector<fs::path>*>(lp));
        if (v) dropFiles(std::move(*v));
        return;
    }
    std::unique_ptr<DropDone> d(reinterpret_cast<DropDone*>(lp));
    g_dropBusy = false;
    if (d) {
        const std::wstring name = d->file.filename().wstring();
        const std::wstring msg = d->msg.empty() ? std::wstring(L"-") : d->msg;
        g.log->write(d->ok ? "info" : "warn", std::string("drop: ") + (d->apk ? "install " : "push ") + toUtf8(name) +
                                                  (d->ok ? " ok" : " failed") + " (" + toUtf8(msg) + ")");
        if (d->ok) g.window->showToast(fmt(d->apk ? S::DropInstalled : S::DropPushed, {name}), 3500);
        else g.window->showToast(fmt(d->apk ? S::DropInstallFail : S::DropPushFail, {name, moduleText(msg)}), 5000);  // msg: AndroidSource's zh-TW sentence
    }
    dropNext();
}

}  // namespace pm_app
