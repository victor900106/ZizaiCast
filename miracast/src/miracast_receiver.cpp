#include "pm/miracast_receiver.h"

#include <windows.h>
#include <combaseapi.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Miracast.h>

#include <cstdarg>
#include <mutex>

#include "frame_pump.h"
#include "pm/video_window.h"
#include "wireless_display.h"

namespace pm {

namespace wmm = winrt::Windows::Media::Miracast;

namespace {

// WinRT calls work from any thread once the process has an MTA (threads
// without COM init then use the implicit MTA); no init_apartment() on the
// caller's thread, which may already be an STA.
void ensureMta() {
    static std::once_flag once;
    std::call_once(once, [] {
        CO_MTA_USAGE_COOKIE cookie{};
        CoIncrementMTAUsage(&cookie);  // kept for the process lifetime
    });
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

const char* listeningName(wmm::MiracastReceiverListeningStatus s) {
    switch (s) {
    case wmm::MiracastReceiverListeningStatus::NotListening: return "NotListening";
    case wmm::MiracastReceiverListeningStatus::Listening: return "Listening";
    case wmm::MiracastReceiverListeningStatus::ConnectionPending: return "ConnectionPending";
    case wmm::MiracastReceiverListeningStatus::Connected: return "Connected";
    case wmm::MiracastReceiverListeningStatus::DisabledByPolicy: return "DisabledByPolicy";
    case wmm::MiracastReceiverListeningStatus::TemporarilyDisabled: return "TemporarilyDisabled";
    }
    return "?";
}

const char* wifiName(wmm::MiracastReceiverWiFiStatus s) {
    switch (s) {
    case wmm::MiracastReceiverWiFiStatus::MiracastSupportUndetermined: return "SupportUndetermined";
    case wmm::MiracastReceiverWiFiStatus::MiracastNotSupported: return "NotSupported";
    case wmm::MiracastReceiverWiFiStatus::MiracastSupportNotOptimized: return "SupportNotOptimized";
    case wmm::MiracastReceiverWiFiStatus::MiracastSupported: return "Supported";
    }
    return "?";
}

const wchar_t* kReasonOldWindows = L"需要 Windows 10 2004（20H1）或更新版本才能接收 Android 投影（Miracast）。";
const wchar_t* kReasonFeature =
    L"請先安裝 Windows 選用功能「無線顯示器」：設定 → 系統 → 選用功能 → 檢視功能 → 搜尋「無線顯示器」→ 安裝（需要網路，約 1 分鐘）。";
const wchar_t* kReasonReboot = L"Windows 選用功能「無線顯示器」已安裝，請重新開機後再使用 Android 投影。";
const wchar_t* kReasonHardware =
    L"這台電腦目前無法接收 Android 投影（Miracast）：請確認 Wi-Fi 已開啟；若沒有 Wi-Fi 網卡、或網卡/顯示卡驅動程式不支援 Wi-Fi Direct，請改用 USB 連線。";
const wchar_t* kReasonPolicy = L"Android 投影（Miracast 接收）已被系統管理原則停用。";
const wchar_t* kReasonBusy =
    L"Miracast 接收暫時無法使用：可能是 Windows「連線」應用程式或其他投影接收程式正在使用，或 Wi-Fi 已關閉。";

}  // namespace

struct MiracastReceiver::Impl : std::enable_shared_from_this<Impl> {
    mutable std::mutex m;
    Status st = Status::Disabled;
    VideoWindow* window = nullptr;
    Events ev;
    std::function<void(const std::string&)> log;
    double volume = 1.0;

    wmm::MiracastReceiver receiver{nullptr};
    wmm::MiracastReceiverSession session{nullptr};
    wmm::MiracastReceiverConnection conn{nullptr};
    std::wstring deviceName;
    bool pinShown = false;
    winrt::event_token tokStatus{}, tokConn{}, tokMedia{}, tokDisc{};
    std::unique_ptr<FramePump> pump;
    std::mutex pumpMutex;  // play/stop of the pump (outside m)

    void logf(const char* fmt, ...) {
        if (!log) return;
        char buf[600];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        log(buf);
    }

    // Updates the status and reports it (outside the lock) if it changed or
    // force is set.
    void setStatus(Status s, const std::wstring& detail, bool force = false) {
        decltype(ev.onStatus) cb;
        {
            std::lock_guard lk(m);
            if (st == s && !force) return;
            st = s;
            cb = ev.onStatus;
        }
        static const char* names[] = {"Unavailable", "Disabled", "Idle", "Connecting", "Connected"};
        logf("miracast: status %s %s", names[static_cast<int>(s)], narrow(detail).c_str());
        if (cb) cb(s, detail);
    }

    void showPin(const std::wstring& pin) {
        decltype(ev.onPin) cb;
        {
            std::lock_guard lk(m);
            if (pin.empty() && !pinShown) return;
            pinShown = !pin.empty();
            cb = ev.onPin;
        }
        if (cb) cb(pin);
    }

    // Receiver ListeningStatus -> our status (while no cast is active).
    void refreshFromReceiver() {
        wmm::MiracastReceiver r{nullptr};
        {
            std::lock_guard lk(m);
            r = receiver;
            if (!r || conn) return;  // a cast in progress owns the status
        }
        try {
            auto s = r.GetStatus();
            const auto ls = s.ListeningStatus();
            logf("miracast: receiver listening=%s wifi=%s", listeningName(ls), wifiName(s.WiFiStatus()));
            switch (ls) {
            case wmm::MiracastReceiverListeningStatus::Listening:
                setStatus(Status::Idle, L"");
                break;
            case wmm::MiracastReceiverListeningStatus::ConnectionPending:
                setStatus(Status::Connecting, L"");
                break;
            case wmm::MiracastReceiverListeningStatus::Connected:
                break;  // our ConnectionCreated / MediaSourceCreated report it
            case wmm::MiracastReceiverListeningStatus::DisabledByPolicy:
                setStatus(Status::Disabled, kReasonPolicy);
                break;
            case wmm::MiracastReceiverListeningStatus::TemporarilyDisabled:
                setStatus(Status::Disabled, kReasonBusy);
                break;
            case wmm::MiracastReceiverListeningStatus::NotListening:
                setStatus(Status::Disabled, L"Miracast 接收未啟動。");
                break;
            }
        } catch (const winrt::hresult_error& e) {
            logf("miracast: GetStatus failed 0x%08lx", static_cast<long>(e.code()));
        }
    }

    void onConnectionCreated(const wmm::MiracastReceiverConnectionCreatedEventArgs& a) {
        auto c = a.Connection();
        std::wstring name;
        try {
            name = std::wstring(c.Transmitter().Name());
        } catch (...) {
        }
        if (name.empty()) name = L"Android 手機";
        const std::wstring pin(a.Pin());
        logf("miracast: connection from \"%s\" (pin %s)", narrow(name).c_str(), pin.empty() ? "none" : "requested");
        {
            std::lock_guard lk(m);
            conn = c;
            deviceName = name;
        }
        if (!pin.empty()) showPin(pin);
        setStatus(Status::Connecting, name, true);
    }

    void onMediaSourceCreated(const wmm::MiracastReceiverMediaSourceCreatedEventArgs& a) {
        try {
            a.CursorImageChannelSettings().IsEnabled(false);  // cursor drawn into the video
        } catch (...) {
        }
        auto source = a.MediaSource();
        std::wstring name;
        {
            std::lock_guard lk(m);
            if (!conn) conn = a.Connection();
            name = deviceName;
        }
        showPin(L"");
        bool ok;
        {
            std::lock_guard pl(pumpMutex);
            ok = pump && pump->play(source, /*realTime=*/true);
        }
        if (!ok) {
            logf("miracast: cannot play the cast's media source");
            try {
                a.Connection().Disconnect(wmm::MiracastReceiverDisconnectReason::AppSpecificError);
            } catch (...) {
            }
            return;
        }
        setStatus(Status::Connected, name, true);
        decltype(ev.onConnected) cb;
        {
            std::lock_guard lk(m);
            cb = ev.onConnected;
        }
        if (cb) cb(name);
    }

    // The session's Disconnected event.  With takeover, phone B's
    // ConnectionCreated can come before phone A's Disconnected: an event for
    // a connection that is no longer the current one must not end B's cast.
    void onDisconnected(const wmm::MiracastReceiverDisconnectedEventArgs& a) {
        wmm::MiracastReceiverConnection gone{nullptr};
        try {
            gone = a.Connection();
        } catch (...) {
        }
        std::wstring stays;
        {
            std::lock_guard lk(m);
            if (gone && conn && !(gone == conn)) stays = deviceName.empty() ? L"?" : deviceName;
        }
        if (!stays.empty()) {
            logf("miracast: late disconnect of an earlier phone ignored (\"%s\" stays)", narrow(stays).c_str());
            return;
        }
        endCast("phone disconnected", true);
    }

    // Cast ended (Disconnected event, disconnect(), or stop()).
    void endCast(const char* why, bool report) {
        bool had;
        {
            std::lock_guard lk(m);
            had = conn != nullptr;
            conn = nullptr;
            deviceName.clear();
        }
        {
            std::lock_guard pl(pumpMutex);
            if (pump) pump->stop();  // -> window->onReset() if a picture was shown
        }
        showPin(L"");
        if (!had) return;
        logf("miracast: cast ended (%s)", why);
        if (!report) return;
        decltype(ev.onDisconnected) cb;
        {
            std::lock_guard lk(m);
            cb = ev.onDisconnected;
        }
        if (cb) cb();
        setStatus(Status::Idle, L"", true);
        refreshFromReceiver();
    }
};

MiracastReceiver::MiracastReceiver() : impl_(std::make_shared<Impl>()) {}

MiracastReceiver::~MiracastReceiver() { stop(); }

std::wstring MiracastReceiver::unsupportedReason() {
    ensureMta();
    try {
        if (!winrt::Windows::Foundation::Metadata::ApiInformation::IsTypePresent(
                L"Windows.Media.Miracast.MiracastReceiver"))
            return kReasonOldWindows;
        wmm::MiracastReceiver r;
        const auto wifi = r.GetStatus().WiFiStatus();
        if (wifi == wmm::MiracastReceiverWiFiStatus::MiracastSupported ||
            wifi == wmm::MiracastReceiverWiFiStatus::MiracastSupportNotOptimized)
            return {};
        switch (miracast::wirelessDisplayFeatureState()) {
        case miracast::FeatureState::NotInstalled: return kReasonFeature;
        case miracast::FeatureState::PendingReboot: return kReasonReboot;
        default: break;
        }
        if (wifi == wmm::MiracastReceiverWiFiStatus::MiracastNotSupported) return kReasonHardware;
        return {};  // undetermined (e.g. Wi-Fi off): try; start() reports the details
    } catch (const winrt::hresult_error& e) {
        wchar_t buf[160];
        swprintf_s(buf, L"無法查詢 Miracast 狀態（0x%08lX）。", static_cast<unsigned long>(e.code()));
        return buf;
    }
}

bool MiracastReceiver::start(const std::wstring& friendlyName, VideoWindow* window, Events events) {
    stop();
    ensureMta();
    auto& I = *impl_;
    {
        std::lock_guard lk(I.m);
        I.window = window;
        I.ev = std::move(events);
        I.log = log;
    }
    const std::wstring reason = unsupportedReason();
    if (!reason.empty()) {
        I.setStatus(Status::Unavailable, reason, true);
        return false;
    }
    if (!window) {
        I.setStatus(Status::Unavailable, L"no video window", true);
        return false;
    }
    {
        std::lock_guard pl(I.pumpMutex);
        I.pump = std::make_unique<FramePump>(window, I.log);
        if (!I.pump->init()) {
            I.pump.reset();
            I.setStatus(Status::Unavailable, L"無法初始化 Direct3D 11。", true);
            return false;
        }
        I.pump->setVolume(I.volume);
    }

    try {
        wmm::MiracastReceiver r;
        auto st = r.GetStatus();
        I.logf("miracast: receiver status before start: listening=%s wifi=%s takeover=%d maxConnections=%d",
               listeningName(st.ListeningStatus()), wifiName(st.WiFiStatus()),
               static_cast<int>(st.IsConnectionTakeoverSupported()), st.MaxSimultaneousConnections());

        auto settings = r.GetDefaultSettings();
        settings.FriendlyName(friendlyName);
        settings.ModelName(L"自在投影");
        settings.ModelNumber(L"PhoneMirror");
        // No prompt; a PIN only if the phone insists (shown via onPin).
        settings.AuthorizationMethod(wmm::MiracastReceiverAuthorizationMethod::PinDisplayIfRequested);
        settings.RequireAuthorizationFromKnownTransmitters(false);
        auto applied = r.DisconnectAllAndApplySettings(settings);
        if (applied.Status() != wmm::MiracastReceiverApplySettingsStatus::Success) {
            I.logf("miracast: ApplySettings status %d ext 0x%08lx", static_cast<int>(applied.Status()),
                   static_cast<long>(applied.ExtendedError()));
            std::wstring why;
            switch (applied.Status()) {
            case wmm::MiracastReceiverApplySettingsStatus::MiracastNotSupported: why = kReasonHardware; break;
            case wmm::MiracastReceiverApplySettingsStatus::AccessDenied: why = kReasonPolicy; break;
            case wmm::MiracastReceiverApplySettingsStatus::FriendlyNameTooLong: why = L"投影名稱太長。"; break;
            default: why = L"無法套用 Miracast 接收設定。"; break;
            }
            I.setStatus(Status::Unavailable, why, true);
            stop();
            return false;
        }

        // Desktop (unpackaged) app: there is no CoreApplicationView; a null
        // view is accepted (verified on Windows 11 26300).
        auto s = r.CreateSession(nullptr);
        try {
            if (st.IsConnectionTakeoverSupported()) s.AllowConnectionTakeover(true);
            s.MaxSimultaneousConnections(1);
        } catch (const winrt::hresult_error& e) {
            I.logf("miracast: session options 0x%08lx", static_cast<long>(e.code()));
        }
        std::weak_ptr<Impl> weak = impl_;
        auto tokConn = s.ConnectionCreated([weak](const auto&, const wmm::MiracastReceiverConnectionCreatedEventArgs& a) {
            if (auto i = weak.lock()) try {
                    i->onConnectionCreated(a);
                } catch (const winrt::hresult_error& e) {
                    i->logf("miracast: ConnectionCreated error 0x%08lx", static_cast<long>(e.code()));
                }
        });
        auto tokMedia =
            s.MediaSourceCreated([weak](const auto&, const wmm::MiracastReceiverMediaSourceCreatedEventArgs& a) {
                if (auto i = weak.lock()) try {
                        i->onMediaSourceCreated(a);
                    } catch (const winrt::hresult_error& e) {
                        i->logf("miracast: MediaSourceCreated error 0x%08lx", static_cast<long>(e.code()));
                    }
            });
        auto tokDisc = s.Disconnected([weak](const auto&, const wmm::MiracastReceiverDisconnectedEventArgs& a) {
            if (auto i = weak.lock()) i->onDisconnected(a);
        });
        auto tokStatus = r.StatusChanged([weak](const auto&, const auto&) {
            if (auto i = weak.lock()) i->refreshFromReceiver();
        });
        {
            std::lock_guard lk(I.m);
            I.receiver = r;
            I.session = s;
            I.tokConn = tokConn;
            I.tokMedia = tokMedia;
            I.tokDisc = tokDisc;
            I.tokStatus = tokStatus;
        }

        auto started = s.Start();
        if (started.Status() != wmm::MiracastReceiverSessionStartStatus::Success) {
            I.logf("miracast: session start status %d ext 0x%08lx", static_cast<int>(started.Status()),
                   static_cast<long>(started.ExtendedError()));
            std::wstring why = started.Status() == wmm::MiracastReceiverSessionStartStatus::MiracastNotSupported
                                   ? kReasonHardware
                               : started.Status() == wmm::MiracastReceiverSessionStartStatus::AccessDenied
                                   ? kReasonPolicy
                                   : kReasonBusy;
            stop();
            I.setStatus(Status::Disabled, why, true);
            return false;
        }
        I.logf("miracast: listening as \"%s\"", narrow(friendlyName).c_str());
    } catch (const winrt::hresult_error& e) {
        I.logf("miracast: start failed 0x%08lx %s", static_cast<long>(e.code()), narrow(std::wstring(e.message())).c_str());
        stop();
        I.setStatus(Status::Unavailable, L"Miracast 接收啟動失敗。", true);
        return false;
    }
    I.setStatus(Status::Idle, L"", true);
    I.refreshFromReceiver();
    return true;
}

void MiracastReceiver::stop() {
    auto& I = *impl_;
    wmm::MiracastReceiver r{nullptr};
    wmm::MiracastReceiverSession s{nullptr};
    wmm::MiracastReceiverConnection c{nullptr};
    {
        std::lock_guard lk(I.m);
        r = I.receiver;
        s = I.session;
        c = I.conn;
        I.receiver = nullptr;
        I.session = nullptr;
    }
    if (!r && !s) {
        std::lock_guard pl(I.pumpMutex);
        I.pump.reset();
        return;
    }
    try {
        if (s) {
            s.ConnectionCreated(I.tokConn);
            s.MediaSourceCreated(I.tokMedia);
            s.Disconnected(I.tokDisc);
        }
        if (r) r.StatusChanged(I.tokStatus);
        if (c) c.Disconnect(wmm::MiracastReceiverDisconnectReason::Finished);
    } catch (const winrt::hresult_error& e) {
        I.logf("miracast: stop 0x%08lx", static_cast<long>(e.code()));
    }
    I.endCast("receiver stopped", /*report=*/true);
    try {
        if (s) s.Close();  // stops listening
    } catch (...) {
    }
    {
        std::lock_guard pl(I.pumpMutex);
        I.pump.reset();
    }
    I.setStatus(Status::Disabled, L"已停止", true);
}

void MiracastReceiver::disconnect() {
    auto& I = *impl_;
    wmm::MiracastReceiverConnection c{nullptr};
    {
        std::lock_guard lk(I.m);
        c = I.conn;
    }
    if (!c) return;
    try {
        c.Disconnect(wmm::MiracastReceiverDisconnectReason::DisconnectedByUser);
    } catch (const winrt::hresult_error& e) {
        I.logf("miracast: Disconnect 0x%08lx", static_cast<long>(e.code()));
    }
    I.endCast("disconnected by user", true);  // no-op if the event already ran
}

MiracastReceiver::Status MiracastReceiver::status() const {
    std::lock_guard lk(impl_->m);
    return impl_->st;
}

void MiracastReceiver::setVolume(double volume01) {
    auto& I = *impl_;
    std::lock_guard pl(I.pumpMutex);
    I.volume = volume01;
    if (I.pump) I.pump->setVolume(volume01);
}

}  // namespace pm
