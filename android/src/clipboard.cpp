#include "clipboard.h"

#include <windows.h>

#include <condition_variable>
#include <thread>

namespace pm::clip {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w, size_t len) {
    if (!len) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, int(len), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, int(len), s.data(), n, nullptr, nullptr);
    return s;
}

// The clipboard is a shared lock: another app may hold it for a moment.
bool openClipboard(HWND owner) {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(10);
    }
    return false;
}

class WindowsBackend : public Backend {
public:
    WindowsBackend() {
        std::unique_lock<std::mutex> lk(m_);
        thread_ = std::thread([this] { run(); });
        cv_.wait(lk, [this] { return started_; });
    }
    ~WindowsBackend() override {
        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
    }
    uint32_t sequence() override { return GetClipboardSequenceNumber(); }
    bool getText(std::string& utf8) override {
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !openClipboard(nullptr)) return false;
        bool ok = false;
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                const size_t max = GlobalSize(h) / sizeof(wchar_t);
                size_t n = 0;
                while (n < max && p[n]) ++n;
                utf8 = narrow(p, n);
                ok = true;
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return ok;
    }
    uint32_t setText(const std::string& utf8) override {
        if (!hwnd_) return 0;
        const std::wstring w = widen(utf8);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
        if (!mem) return 0;
        if (auto* p = static_cast<wchar_t*>(GlobalLock(mem))) {
            memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
        }
        // EmptyClipboard needs a real owner window (with NULL, SetClipboardData fails).
        if (!openClipboard(hwnd_)) {
            GlobalFree(mem);
            return 0;
        }
        EmptyClipboard();
        const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) GlobalFree(mem);
        CloseClipboard();
        return ok ? GetClipboardSequenceNumber() : 0;
    }

private:
    void run() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PmAndroidClipboardOwner";
        RegisterClassW(&wc);  // fails harmlessly when already registered
        HWND h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        {
            std::lock_guard<std::mutex> lk(m_);
            hwnd_ = h;
            started_ = true;
        }
        cv_.notify_all();
        if (!h) return;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_CLOSE && msg.hwnd == h) {
                DestroyWindow(h);
                break;
            }
            DispatchMessageW(&msg);
        }
    }

    std::mutex m_;
    std::condition_variable cv_;
    bool started_ = false;
    HWND hwnd_ = nullptr;
    std::thread thread_;
};

}  // namespace

std::unique_ptr<Backend> makeWindowsBackend() { return std::make_unique<WindowsBackend>(); }

}  // namespace pm::clip
