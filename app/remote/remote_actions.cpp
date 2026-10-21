// 自在投影 app: remote/remote_actions.cpp — 遠端操控 (0.8.0, docs/remote.md).
// Drives pm::remote::Host from the UI thread: on / off, the QR panel, the
// 允許 / 拒絕 dialog (記住這支手機), the topmost 「正在被遠端操控」 bar with
// 中斷 and 僅觀看, Ctrl+D (a global hotkey while a phone is connected), the
// menu, settings.ini and the remembered phones (remote_devices.txt).
#include "remote/remote.h"

#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <memory>
#include <set>
#include <thread>

#include "connect/connect.h"

#include "help/help.h"
#include "view/view.h"
#include "menus/menus.h"
#include "share/share.h"

#ifdef PM_APP_REMOTE
#include "pm/remote.h"
#endif

namespace pm_app {

namespace {

constexpr int kRemoteHotkeyId = 0x5244;  // 'RD'
constexpr wchar_t kIcoRemote = 0xE8AF;   // Remote

struct RemoteMsg {
    unsigned gen = 0;
    std::string session, device, ip, label, why;
    bool tailscale = false;
    bool tunnel = false;  // 免安裝外網 (0.8.1)
    bool fixed = false;   // 常駐外網 (0.8.2): came in over the fixed link with the password
    int fails = 0;        // RemFixedLogin: wrong passwords in a row
    bool locked = false;  // ... and now locked for 15 minutes
    std::string guest, guestId;  // 0.8.3.2: came in with this 臨時帳號
};
constexpr int kFixedPort = 8443;  // Tailscale Funnel port of the fixed link (443 is never touched)

struct RemoteState {
#ifdef PM_APP_REMOTE
    pm::remote::Host host;
#endif
    pm::ui::RemotePanel panel;
    pm::ui::RemoteBanner banner;
    pm::ui::AskPanel ask;
    bool on = false;
    unsigned gen = 0;
    std::string askSession, askLabel;  // the session the 允許 / 拒絕 dialog is about, its phone
    bool active = false;        // a phone is connected (banner shown)
    std::string activeSession, activeLabel, activeIp;
    bool hotkey = false;
    std::mutex dmu;               // remembered phones (read on the server's threads)
    std::set<std::string> devices;
#ifdef PM_APP_REMOTE
    std::vector<pm::remote::AudioDevice> audioDevs;  // 聲音來源 ▸ rows, as last shown
    pm::remote::QuickTunnel tunnel;                  // 免安裝外網 (cloudflared, 0.8.1)
#endif
    int tunState = 0;        // 0 off, 1 getting the address, 2 ready, 3 failed
    std::string tunUrl;      // https://<random>.trycloudflare.com
    std::wstring tunErr;
    // 常駐外網 (0.8.2)
    pm::ui::RemotePasswordPanel pw;
    pm::ui::RemoteGuestPanel guestPanel;  // 0.8.3.2 新增臨時帳號 / 重設密碼
    std::string activeGuestId;            // 0.8.3.2: the connected phone came in with this 臨時帳號
    pm::ui::AskPanel news;          // 新功能 (once after the upgrade)
    int fixState = 0;               // 0 off, 1 starting, 2 ready, 3 failed
    std::string fixUrl;             // https://<pc>.<tailnet>.ts.net:8443/ (or a cloudflared fallback)
    std::wstring fixErr, fixInfo;   // failed: why; info: why the cloudflared fallback is used
    std::atomic<unsigned> fixGen{0};  // a stale Funnel answer is ignored (0.8.3.1: also read by the waiting thread)
    std::thread fixThread;          // tailscale CLI calls (a few hundred ms) off the UI thread
    std::atomic<bool> fixFunnel{false};  // port 8443 is ours (turned off at stop / quit)
    // 0.8.3.2: the tailscale CLI only ever runs on background threads (one at a time: cliMu), never
    // waited for on the UI thread; fixMu makes "is it still wanted? then it is ours" one step.
    std::mutex cliMu, fixMu;
    std::atomic<int> cliRuns{0};
    std::string fixSetupUrl;        // 0.8.3.2: the page that turns Funnel / HTTPS on for this tailnet (開啟設定頁)
    // 0.8.3.3: the address is known but not usable yet (the HTTPS certificate is still being issued):
    // fixState stays 1 until a TLS check from this PC gets an answer; the page says 正在準備網址.
    bool fixFake = false;            // --dev PM_REMOTE_FUNNELFAKE (no check unless PM_REMOTE_FUNNELPROBE)
    bool fixPreparing = false;       // the check failed at least once: 正在準備網址，約 1–2 分鐘
    std::shared_ptr<std::atomic<bool>> probeStop;  // ends the checking thread (it never touches RemoteState)
    int pageQr = 0;                 // 遠端控制 page: the QR shown in 1 在家 / 2 Tailscale (0 none)
#ifdef PM_APP_REMOTE
    pm::remote::QuickTunnel fixTunnel;   // fallback when Funnel cannot be used
#endif
};
RemoteState& R() {
    static RemoteState r;
    return r;
}

fs::path devicesFile() { return g.settingsFile.parent_path() / L"remote_devices.txt"; }

// remote_devices.txt: one remembered phone per line, "<device id>\t<label>\t<date>".
void loadDevices() {
    std::lock_guard<std::mutex> lk(R().dmu);
    R().devices.clear();
    std::ifstream in(devicesFile());
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string id = line.substr(0, line.find('\t'));
        if (id.size() >= 8 && id.size() <= 64) R().devices.insert(id);
    }
}

void rememberDevice(const std::string& id, const std::string& label) {
    {
        std::lock_guard<std::mutex> lk(R().dmu);
        if (!R().devices.insert(id).second) return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char date[32];
    snprintf(date, sizeof(date), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    std::ofstream out(devicesFile(), std::ios::app);
    out << id << '\t' << label << '\t' << date << '\n';
    g.log->write("info", "remote: remembered " + label + " (" + id.substr(0, 8) + "...)");
}

struct Remembered {
    std::string id, label, date;
};
std::vector<Remembered> rememberedList() {
    std::vector<Remembered> v;
    std::ifstream in(devicesFile());
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Remembered r;
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        r.id = line.substr(0, a);
        if (a != std::string::npos) r.label = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        if (b != std::string::npos) r.date = line.substr(b + 1);
        if (r.id.size() >= 8 && r.id.size() <= 64) v.push_back(std::move(r));
    }
    return v;
}

void forgetDevice(size_t i) {
    std::vector<Remembered> v = rememberedList();
    if (i >= v.size()) return;
    const Remembered gone = v[i];
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
    {
        std::ofstream out(devicesFile(), std::ios::trunc);
        for (const auto& r : v) out << r.id << '\t' << r.label << '\t' << r.date << '\n';
    }
    {
        std::lock_guard<std::mutex> lk(R().dmu);
        R().devices.erase(gone.id);
    }
    g.log->write("info", "remote: forgot " + gone.label + " (" + gone.id.substr(0, 8) + "...)");
    g.window->showToast(pm::i18n::fmt(S::RemoteForgotOne, {toWide(gone.label)}), 3000);
}

size_t rememberedCount() {
    std::lock_guard<std::mutex> lk(R().dmu);
    return R().devices.size();
}

std::string randomToken() {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::string s;
    while (s.size() < 32) {
        uint8_t b[64];
        BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        for (uint8_t x : b)
            if (x < 248 && s.size() < 32) s.push_back(kAlphabet[x % 62]);  // 248 = 4 * 62: no bias
    }
    return s;
}

std::wstring w(const std::string& s) { return toWide(s); }

// ---- 0.8.3 連線紀錄: %LOCALAPPDATA%\PhoneMirror\remote_connections.log, one line per event
// "<local time>\t<source>\t<masked address>\t<device>\t<result>"; never a password or a token.
// Rotated at 64 KB (remote_connections.old.log keeps the previous one).
fs::path connLogFile() { return g.settingsFile.parent_path() / L"remote_connections.log"; }
fs::path connLogOld() { return g.settingsFile.parent_path() / L"remote_connections.old.log"; }
// 192.168.1.37 -> 192.168.*.*, 2001:db8:1:2::5 -> 2001:db8:*; "tunnel" / "funnel" as they are.
std::string maskIp(const std::string& ip) {
    if (ip.find(':') != std::string::npos) {
        const size_t a = ip.find(':'), b = a == std::string::npos ? a : ip.find(':', a + 1);
        return b == std::string::npos ? ip.substr(0, a) + ":*" : ip.substr(0, b) + ":*";
    }
    const size_t a = ip.find('.'), b = a == std::string::npos ? a : ip.find('.', a + 1);
    if (b == std::string::npos) return ip.substr(0, 16);
    return ip.substr(0, b) + ".*.*";
}
// Where an address alone came from (onBlocked has no route): LAN / Tailscale / outside.
const char* srcOfIp(const std::string& ip) {
    unsigned a = 0, b = 0;
    if (sscanf_s(ip.c_str(), "%u.%u.", &a, &b) == 2) {
        if (a == 100 && b >= 64 && b < 128) return "ts";
        if (a == 10 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 169 && b == 254)) return "lan";
    }
    return "out";
}
void connLog(const char* src, const std::string& ip, const std::string& label, const char* result) {
    std::error_code ec;
    if (fs::file_size(connLogFile(), ec) > 64 * 1024 && !ec) {
        fs::remove(connLogOld(), ec);
        fs::rename(connLogFile(), connLogOld(), ec);
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char when[32];
    snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string l = label.substr(0, 60);
    for (char& c : l)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    std::ofstream out(connLogFile(), std::ios::app | std::ios::binary);
    out << when << '\t' << src << '\t' << maskIp(ip) << '\t' << l << '\t' << result << '\n';
}
struct ConnRow {
    std::string when, src, ip, label, result;
};
std::vector<ConnRow> connLogRows(size_t max) {
    std::vector<ConnRow> v;
    for (const fs::path& p : {connLogOld(), connLogFile()}) {
        std::ifstream in(p, std::ios::binary);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            ConnRow r;
            std::string* f[5] = {&r.when, &r.src, &r.ip, &r.label, &r.result};
            size_t a = 0;
            for (int i = 0; i < 5; ++i) {
                const size_t b = line.find('\t', a);
                *f[i] = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
                if (b == std::string::npos) break;
                a = b + 1;
            }
            if (!r.result.empty()) v.push_back(std::move(r));
        }
    }
    if (v.size() > max) v.erase(v.begin(), v.end() - static_cast<std::ptrdiff_t>(max));
    std::reverse(v.begin(), v.end());  // newest first
    return v;
}
std::wstring connLogText() {
    const std::vector<ConnRow> rows = connLogRows(50);
    if (rows.empty()) return tr(S::RemoteLogEmpty);
    auto src = [](const std::string& s) {
        return tr(s == "lan" ? S::RemoteLogSrcLan : s == "ts" ? S::RemoteLogSrcTs : s == "fixed" ? S::RemoteLogSrcFixed : s == "tunnel" ? S::RemoteLogSrcTunnel : S::RemoteLogSrcOutside);
    };
    auto res = [](const std::string& s) {
        return tr(s == "ok" ? S::RemoteLogOk : s == "wrong-password" ? S::RemoteLogWrongPw : s == "blocked" ? S::RemoteLogBlocked : S::RemoteLogDenied);
    };
    std::wstring out;
    for (const ConnRow& r : rows)
        out += toWide(r.when) + L"  " + src(r.src) + L"  " + toWide(r.ip) + L"  " + (r.label.empty() ? std::wstring(L"-") : toWide(r.label)) + L"  " +
               res(r.result) + L"\n";
    return out;
}

// ---- 0.8.3 遠端斷線後自動鎖定電腦 ----
// Only when the phone's side ended it (it disconnected, went idle, lost the connection),
// not when the person at the PC did (中斷 / 關閉 / turned a link off). --dev instances
// never lock (a test must not lock the owner's PC): they log what they would do.
void lockAfterSession(const RemoteMsg& m) {
    const bool outside = m.fixed || m.tunnel;
    if (!(outside ? g.settings.remoteLockOutside : g.settings.remoteLockLan)) return;
    const std::string& why = m.why;
    for (const char* pcSide : {"kicked", "host stopping", "remote control turned off", "replaced", "常駐外網 turned off", "外網連結 turned off", "denied"})
        if (why.rfind(pcSide, 0) == 0) return;
    if (g.dev) {
        g.log->write("info", std::string("remote: TEST STUB: would lock the PC now (") + (outside ? "from outside" : "LAN / Tailscale") +
                                 " session ended: " + why + "); --dev never calls LockWorkStation");
        return;
    }
    g.log->write("info", "remote: the session from " + std::string(outside ? "outside" : "the LAN / Tailscale") + " ended (" + why + ") -> locking the PC");
    LockWorkStation();
}

#ifdef PM_APP_REMOTE
static_assert(pm::ui::kRemoteInputTag == pm::remote::kInjectedInputTag, "one tag for the phone's input");
#endif

// The phone tried a PC-only switch (關閉遠端操控 / 中斷 / 僅觀看): ignored, said on both sides.
void refuseRemote(const char* what) {
    g.log->write("info", std::string("remote: the phone clicked ") + what + " -> ignored (PC only)");
    if (!g.quitting) g.window->showToast(tr(S::RemotePcOnlyToast), 3500);
#ifdef PM_APP_REMOTE
    R().host.notifyPhone("pc-only");
#endif
}

// A menu command that the phone's click could have reached: the click came
// with the phone's tag, or the phone clicked a moment ago.
bool fromPhone() {
    if (pm::ui::fromRemoteInput()) return true;
#ifdef PM_APP_REMOTE
    if (!R().active) return false;
    const int64_t ms = R().host.msSinceRemoteInput();
    return ms >= 0 && ms < 800;
#else
    return false;
#endif
}

void unregisterHotkey() {
    if (R().hotkey) UnregisterHotKey(g.hwnd, kRemoteHotkeyId);
    R().hotkey = false;
}

}  // namespace
void remoteChanged();  // (below: the title + the page after every change)
void openRemotePanel(int tab);  // (below; the --dev fake temporary address opens it too)
namespace {
void startTunnel();  // (below)

// ---- 0.8.3.2 臨時帳號: %LOCALAPPDATA%\PhoneMirror\remote_guests.txt, one account per line,
// "<id>\t<name>\t<pbkdf2 hash>\t<created>\t<expires>\t<once>\t<view only>\t<files>\t<last used>"
// (epoch seconds). Only the hash is kept: the password is shown once when it is made or reset
// (the panel forgets it on close) and is never written to a file or the log. At most 10; an
// expired one goes (not while its phone is connected: judged when the session ends); a 用一次
// one goes when its session ends. The host gets the list (hashes) at start and after every change.
struct GuestRow {
    std::string id, name, pwHash;
    long long created = 0, expires = 0, lastUsed = 0;
    bool once = false, viewOnly = false, files = false;
};
constexpr size_t kMaxGuests = 10;
constexpr UINT_PTR kGuestTimerId = 0x5247;  // 'RG': every 30 s while remote control is on (remaining time, expiry)
fs::path guestsFile() { return g.settingsFile.parent_path() / L"remote_guests.txt"; }
long long nowEpoch() { return static_cast<long long>(time(nullptr)); }

std::vector<GuestRow>& guestRows() {
    static std::vector<GuestRow> v;
    static bool loaded = false;
    if (loaded) return v;
    loaded = true;
    std::ifstream in(guestsFile(), std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        for (size_t a = 0;;) {
            const size_t b = line.find('\t', a);
            f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (f.size() < 9 || f[0].empty() || f[1].empty() || f[2].rfind("pbkdf2-sha256$", 0) != 0 || v.size() >= kMaxGuests) continue;
        GuestRow r;
        r.id = f[0], r.name = f[1].substr(0, 24), r.pwHash = f[2];
        r.created = atoll(f[3].c_str()), r.expires = atoll(f[4].c_str());
        r.once = f[5] == "1", r.viewOnly = f[6] == "1", r.files = f[7] == "1", r.lastUsed = atoll(f[8].c_str());
        v.push_back(std::move(r));
    }
    return v;
}

void saveGuests() {
    const fs::path p = guestsFile(), tmp = p.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << "# 自在投影 臨時帳號 (0.8.3.2): id, name, password hash (never the password), created, expires, once, view only, files, last used\n";
        for (const GuestRow& r : guestRows())
            out << r.id << '\t' << r.name << '\t' << r.pwHash << '\t' << r.created << '\t' << r.expires << '\t' << (r.once ? 1 : 0) << '\t'
                << (r.viewOnly ? 1 : 0) << '\t' << (r.files ? 1 : 0) << '\t' << r.lastUsed << '\n';
    }
    std::error_code ec;
    if (guestRows().empty()) fs::remove(tmp, ec), fs::remove(p, ec);
    else fs::rename(tmp, p, ec);
}

#ifdef PM_APP_REMOTE
std::vector<pm::remote::GuestAccount> hostGuests() {
    std::vector<pm::remote::GuestAccount> v;
    for (const GuestRow& r : guestRows()) v.push_back({r.id, r.name, r.pwHash, r.expires, r.once, r.viewOnly, r.files});
    return v;
}
#endif
void syncGuests() {
#ifdef PM_APP_REMOTE
    if (R().on) R().host.setGuests(hostGuests());
#endif
}

// Expired accounts go (one whose phone is connected stays until that session ends).
bool pruneGuests() {
    std::vector<GuestRow>& v = guestRows();
    const long long now = nowEpoch();
    const size_t n = v.size();
    for (auto it = v.begin(); it != v.end();) {
        if (it->expires <= now && it->id != R().activeGuestId) {
            g.log->write("info", "remote: 臨時帳號 " + it->name + " expired: deleted");
            it = v.erase(it);
        } else {
            ++it;
        }
    }
    if (v.size() == n) return false;
    saveGuests();
    return true;
}

std::wstring guestSummary(const GuestRow& r) {
    const long long life = r.expires - r.created;
    const S l = r.once ? S::RemoteGuestLifeOnce : life <= 3600 + 60 ? S::RemoteGuestLife1h : life <= 86400 + 60 ? S::RemoteGuestLife24h : S::RemoteGuestLife7d;
    return std::wstring(tr(l)) + L" · " + tr(r.viewOnly ? S::RemoteGuestView : S::RemoteGuestControl) + L" · " +
           tr(r.files ? S::RemoteGuestWithFiles : S::RemoteGuestNoFiles);
}

// 「guest-4821　剩 23 小時　可操作　最後使用 10:32」 (用一次: 「用一次」 instead of the time left). A
// space inside a field does not break ("24 小時"); fields are separated by an ideographic space.
// (Word joiners between every character showed as gaps in the Japanese / Korean fonts.)
std::wstring glued(std::wstring s) {
    for (wchar_t& c : s)
        if (c == L' ') c = L'\x00A0';
    return s;
}
std::wstring guestRowText(const GuestRow& r) {
    const long long left = std::max(0LL, r.expires - nowEpoch());
    std::wstring t = glued(toWide(r.name)) + L"　";
    if (r.once) t += glued(tr(S::RemoteGuestLifeOnce));
    else if (left < 3600) t += glued(pm::i18n::fmt(S::RemoteGuestLeftM, {std::to_wstring(std::max(1LL, (left + 59) / 60))}));
    else if (left < 2 * 86400) t += glued(pm::i18n::fmt(S::RemoteGuestLeftH, {std::to_wstring((left + 1799) / 3600)}));
    else t += glued(pm::i18n::fmt(S::RemoteGuestLeftD, {std::to_wstring((left + 43199) / 86400)}));
    t += L"　" + glued(tr(r.viewOnly ? S::RemoteGuestView : S::RemoteGuestControl));
    if (r.files) t += L"　" + glued(tr(S::RemoteGuestWithFiles));
    t += L"　";
    if (r.id == R().activeGuestId && R().active) t += glued(tr(S::RemoteGuestInUse));
    else if (!r.lastUsed) t += glued(tr(S::RemoteGuestNever));
    else {
        const time_t tt = static_cast<time_t>(r.lastUsed);
        tm lt{};
        localtime_s(&lt, &tt);
        wchar_t b[32];
        const time_t nowT = time(nullptr);
        tm nt{};
        localtime_s(&nt, &nowT);
        if (lt.tm_yday == nt.tm_yday && lt.tm_year == nt.tm_year) swprintf_s(b, L"%02d:%02d", lt.tm_hour, lt.tm_min);
        else swprintf_s(b, L"%d/%d %02d:%02d", lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
        t += glued(pm::i18n::fmt(S::RemoteGuestLastUsed, {b}));
    }
    return t;
}

// 連線紀錄 / log: "[guest-4821] iPhone · Safari" (no password, no token)
std::string guestLabel(const RemoteMsg& m) { return m.guest.empty() ? m.label : "[" + m.guest + "] " + m.label; }

bool guestNameOk(const std::wstring& n) {
    if (n.empty() || n.size() > 24) return false;
    for (wchar_t c : n)
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.')) return false;
    return true;
}
bool guestNameUsed(const std::string& n, const std::string& exceptId = "") {
    auto low = [](std::string s) {
        for (char& c : s) c = (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
        return s;
    };
    for (const GuestRow& r : guestRows())
        if (r.id != exceptId && low(r.name) == low(n)) return true;
    return false;
}
std::string suggestGuestName() {
    for (int tries = 0; tries < 50; ++tries) {
        unsigned v = 0;
        BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&v), sizeof(v), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        const std::string n = "guest-" + std::to_string(1000 + v % 9000);
        if (!guestNameUsed(n)) return n;
    }
    return "guest-" + std::to_string(nowEpoch() % 100000);
}

// 0.8.3.3: the address handed to a 臨時帳號 carries its name after "#" ("...:8443/#u=guest-4821"): the page
// opens in 臨時帳號 mode with the account filled in, the friend only types the password. A fragment is
// never sent to the server (nor to Tailscale's relay, nor in a Referer), so it is in no request log.
std::string guestUrl(const std::string& base, const std::string& name) { return base + "#u=" + name; }

// 複製全部 (--dev with PM_TEST_GUEST_OUT: into that test file instead of the real clipboard).
void guestCopy(const std::wstring& all) {
    wchar_t outPath[MAX_PATH] = {};
    if (g.dev && GetEnvironmentVariableW(L"PM_TEST_GUEST_OUT", outPath, MAX_PATH) > 0) {
        std::ofstream o(outPath, std::ios::binary | std::ios::trunc);
        o << toUtf8(all);
        g.log->write("info", "remote: dev: 複製全部 -> the test's file (" + std::to_string(all.size()) + " characters)");
        return;
    }
    copyToClipboard(all);
}

pm::ui::RemoteGuestPanel::Callbacks guestCallbacks() {
    pm::ui::RemoteGuestPanel::Callbacks cb;
    cb.onCreate = [](const pm::ui::RemoteGuestPanel::Request& q, pm::ui::RemoteGuestPanel::Result& res, std::wstring& err) {
#ifdef PM_APP_REMOTE
        RemoteState& r = R();
        pruneGuests();
        if (r.fixState != 2) return err = tr(S::RemoteGuestNoAddr), false;
        if (guestRows().size() >= kMaxGuests) return err = tr(S::RemoteGuestFull), false;
        if (!guestNameOk(q.name)) return err = tr(S::RemoteGuestNameBad), false;
        if (guestNameUsed(toUtf8(q.name))) return err = tr(S::RemoteGuestNameTaken), false;
        GuestRow row;
        row.id = pm::remote::randomHex(8);
        row.name = toUtf8(q.name);
        row.created = nowEpoch();
        row.once = q.life == 0;
        row.expires = row.created + (q.life == 1 ? 3600LL : q.life == 3 ? 7 * 86400LL : 86400LL);  // 用一次: within 24 hours
        row.viewOnly = q.viewOnly, row.files = q.files;
        std::string pw = pm::remote::makeGuestPassword();
        row.pwHash = pm::remote::hashPassword(pm::remote::guestPasswordKey(pw));
        guestRows().push_back(row);
        saveGuests();
        syncGuests();
        g.log->write("info", "remote: 臨時帳號 " + row.name + " created (" + toUtf8(guestSummary(row)) + "; only its hash is kept)");
        res.url = toWide(guestUrl(r.fixUrl, row.name)), res.name = q.name, res.password = toWide(pw), res.summary = guestSummary(row), res.reset = false;
        std::fill(pw.begin(), pw.end(), '\0');
        remoteChanged();
        return true;
#else
        (void)q, (void)res, (void)err;
        return false;
#endif
    };
    cb.onCopy = guestCopy;
    cb.onClosed = [] { remoteChanged(); };
    return cb;
}

void openGuestNew() {
    RemoteState& r = R();
    if (r.fixState != 2) {
        g.window->showToast(tr(S::RemoteGuestNoAddr), 4000);
        return;
    }
    pruneGuests();
    if (guestRows().size() >= kMaxGuests) {
        g.window->showToast(tr(S::RemoteGuestFull), 4000);
        return;
    }
    r.guestPanel.open(g.hwnd, toWide(suggestGuestName()), guestCallbacks());
}

// 刪除: gone at once; a phone connected with it (or waiting for 允許) is ended by the host.
void deleteGuest(size_t i) {
    std::vector<GuestRow>& v = guestRows();
    if (i >= v.size()) return;
    const std::string name = v[i].name;
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
    saveGuests();
    syncGuests();
    g.log->write("info", "remote: 臨時帳號 " + name + " deleted on the PC");
    g.window->showToast(pm::i18n::fmt(S::RemoteGuestDeletedToast, {toWide(name)}), 2500);
}

// 重設密碼: a new one, shown once (a connected phone stays; the old password no longer logs in).
void resetGuest(size_t i) {
#ifdef PM_APP_REMOTE
    std::vector<GuestRow>& v = guestRows();
    if (i >= v.size() || R().fixState != 2) return;
    std::string pw = pm::remote::makeGuestPassword();
    v[i].pwHash = pm::remote::hashPassword(pm::remote::guestPasswordKey(pw));
    saveGuests();
    syncGuests();
    g.log->write("info", "remote: 臨時帳號 " + v[i].name + ": a new password (only its hash is kept)");
    pm::ui::RemoteGuestPanel::Result res{toWide(guestUrl(R().fixUrl, v[i].name)), toWide(v[i].name), toWide(pw), guestSummary(v[i]), true};
    std::fill(pw.begin(), pw.end(), '\0');
    R().guestPanel.showResult(g.hwnd, res, guestCallbacks());
    std::fill(res.password.begin(), res.password.end(), L'\0');
#else
    (void)i;
#endif
}

// A guest's session ended: 用一次 = gone now; past its end = gone now.
void guestSessionEnded(const std::string& id) {
    std::vector<GuestRow>& v = guestRows();
    for (auto it = v.begin(); it != v.end(); ++it)
        if (it->id == id) {
            if (it->once || it->expires <= nowEpoch()) {
                g.log->write("info", "remote: 臨時帳號 " + it->name + (it->once ? " was used once: deleted" : " expired during its session: deleted"));
                v.erase(it);
                saveGuests();
                syncGuests();
            }
            break;
        }
    if (!g.quitting) remoteChanged();
}

void CALLBACK guestTimerProc(HWND, UINT, UINT_PTR, DWORD) {
    if (!R().on || g.quitting) return;
    if (pruneGuests()) syncGuests();
    if (!guestRows().empty()) remoteChanged();  // 剩餘時間
}

void setPanelStatus() {
    RemoteState& r = R();
    if (!r.panel.isOpen()) return;
    if (r.active)
        r.panel.setStatus(pm::i18n::fmt(g.settings.remoteViewOnly ? S::RemoteViewingStatus : S::RemoteActiveStatus, {w(r.activeLabel)}), true);
    else if (r.ask.isOpen())
        r.panel.setStatus(pm::i18n::fmt(S::RemoteAsking, {w(r.askLabel)}), false);
    else
        r.panel.setStatus(tr(S::RemoteWaiting), false);
}

#ifdef PM_APP_REMOTE
bool startHost() {
    RemoteState& r = R();
    pm::remote::Options o;
    if (g.settings.remoteToken.size() != 32) {
        g.settings.remoteToken = randomToken();
        saveSettings();
    }
    o.token = g.settings.remoteToken;
    o.viewOnly = g.settings.remoteViewOnly;
    o.audio = g.settings.remoteAudio;
    o.mutePc = g.settings.remoteMutePc;
    o.audioSource = g.settings.remoteAudioSource;
    o.idleMinutes = g.settings.remoteIdleMinutes;
    o.inject = true;
    if (g.dev) {  // --dev test runs: PM_REMOTE_DRYRUN=1 = input only logged (a test never moves the real mouse)
        wchar_t v[8] = {};
        if (GetEnvironmentVariableW(L"PM_REMOTE_DRYRUN", v, 8) > 0 && v[0] == L'1') o.inject = false;
        if (GetEnvironmentVariableW(L"PM_REMOTE_TESTCLIP", v, 8) > 0 && v[0] == L'1') o.testClipboard = true;
        if (GetEnvironmentVariableW(L"PM_REMOTE_NOTS", v, 8) > 0 && v[0] == L'1') o.tailscale = false;  // as if no Tailscale
        // 0.8.3.4: PM_REMOTE_SYNTHAUDIO=<music|quiet|...> = a silent synthetic sound source: no playback device
        // is captured, and 手機播聲音時電腦靜音 never mutes a real speaker
        wchar_t sa[16] = {};
        if (const DWORD n = GetEnvironmentVariableW(L"PM_REMOTE_SYNTHAUDIO", sa, 16); n > 0 && n < 16) o.audioSynthetic = toUtf8(sa);
        // 0.8.3.1: PM_REMOTE_LOOPBACK=1 = the LAN side on 127.0.0.3 only (a test build path gets no firewall prompt)
        if (GetEnvironmentVariableW(L"PM_REMOTE_LOOPBACK", v, 8) > 0 && v[0] == L'1') o.loopbackTest = true, o.tailscale = false;
        // 0.8.3 files: PM_REMOTE_TESTFILES="a;b" = the "copied files" (with PM_REMOTE_TESTCLIP); uploads go
        // to the instance's own data folder (never the owner's Downloads) or PM_REMOTE_UPLOADDIR.
        wchar_t tf[2048] = {};
        if (const DWORD n = GetEnvironmentVariableW(L"PM_REMOTE_TESTFILES", tf, 2048); n > 0 && n < 2048) {
            std::wstring list = tf;
            for (size_t a = 0; a <= list.size();) {
                const size_t b = list.find(L';', a);
                const std::wstring p = list.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
                if (!p.empty()) o.testClipboardFiles.push_back(p);
                if (b == std::wstring::npos) break;
                a = b + 1;
            }
        }
    }
    {
        // 0.8.3: the phone's files land in Downloads\自在投影遠端 (--dev: <data folder>\uploads).
        wchar_t ud[MAX_PATH] = {};
        const DWORD n = g.dev ? GetEnvironmentVariableW(L"PM_REMOTE_UPLOADDIR", ud, MAX_PATH) : 0;
        if (n > 0 && n < MAX_PATH) o.uploadDir = ud;
        else if (g.dev) o.uploadDir = (g.settingsFile.parent_path() / L"uploads").wstring();
        else if (const fs::path dl = knownFolder(FOLDERID_Downloads); !dl.empty()) o.uploadDir = (dl / L"自在投影遠端").wstring();
    }
    o.requireApproval = true;
    o.lang = static_cast<int>(pm::i18n::lang());
    o.appName = toUtf8(tr(S::AppName));
    const pm::ui::Colors c = pm::ui::currentColors();
    o.pageBg = pm::VideoWindow::themeSwatch(kThemes[g.settings.theme & 3])[0];
    o.pageAccent = c.accent;
    o.pageFg = c.fg;
    o.helpFile = tutorialFile().wstring();
    o.fixedPasswordHash = g.settings.remotePwHash;
    o.fixedSecret = g.settings.remoteFixedSecret;
    o.fixedNeedApproval = g.settings.remoteFixedNeedApproval;
    pruneGuests();
    o.guests = hostGuests();  // 0.8.3.2 臨時帳號 (hashes only)
    for (size_t a = 0; a < g.settings.remoteRevoked.size();) {  // 0.8.3: logins ended by 中斷 stay ended
        const size_t b = g.settings.remoteRevoked.find(',', a);
        const std::string e = g.settings.remoteRevoked.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (!e.empty()) o.revokedLogins.push_back(e);
        if (b == std::string::npos) break;
        a = b + 1;
    }
    const HWND h = g.hwnd;
    Log* log = g.log;
    const unsigned gen = ++r.gen;
    auto post = [h, gen](RemoteEvent ev, const pm::remote::Peer& p, const std::string& why) {
        auto* m = new RemoteMsg{gen, p.session, p.device, p.ip, p.label, why, p.tailscale, p.tunnel, p.fixed};
        m->guest = p.guest, m->guestId = p.guestId;
        if (!PostMessageW(h, WM_PM_REMOTE, ev, reinterpret_cast<LPARAM>(m))) delete m;
    };
    o.log = [log](const std::string& line) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        log->write("remote", l);
    };
    o.remembered = [](const std::string& device) {
        std::lock_guard<std::mutex> lk(R().dmu);
        return R().devices.count(device) > 0;
    };
    o.onRequest = [post](const pm::remote::Peer& p) { post(RemRequest, p, ""); };
    o.onActive = [post](const pm::remote::Peer& p) { post(RemActive, p, ""); };
    o.onEnded = [post](const pm::remote::Peer& p, const std::string& why) { post(RemEnded, p, why); };
    o.onBlocked = [post](const std::string& ip, int) { post(RemBlocked, pm::remote::Peer{"", "", ip, "", false}, ""); };
    o.onTrouble = [post](const std::string& ip, const std::string& why) { post(RemTrouble, pm::remote::Peer{"", "", ip, "", false}, why); };
    // 免安裝外網: the one-time link was used / expired: the panel shows the new one.
    o.onTunnelLink = [post](const std::string& path) { post(RemTunnelLink, pm::remote::Peer{}, path); };
    // 常駐外網: each password try (the log has the address; 5 wrong = a tray note).
    o.onLogin = [h](const std::string& ip, bool ok, int fails, bool locked) {
        auto* m = new RemoteMsg{0, "", "", ip, "", ok ? "ok" : "wrong", false, false, true, fails, locked};
        if (!PostMessageW(h, WM_PM_REMOTE, RemFixedLogin, reinterpret_cast<LPARAM>(m))) delete m;
    };
    // 0.8.3: 中斷 ended a fixed-link login: kept in settings.ini (token id + expiry only).
    o.onLoginRevoked = [post](const std::string& id, long long exp) { post(RemLoginRevoked, pm::remote::Peer{}, id + ":" + std::to_string(exp)); };
    // 0.8.3.2: each 臨時帳號 login try (the name as typed; never the password): 連線紀錄
    o.onGuestLogin = [h](const std::string& ip, const std::string& user, bool ok, int fails, bool locked) {
        auto* m = new RemoteMsg{0, "", "", ip, user, ok ? "ok" : "wrong", false, false, true, fails, locked};
        if (!PostMessageW(h, WM_PM_REMOTE, RemGuestLogin, reinterpret_cast<LPARAM>(m))) delete m;
    };
    o.onUnlocked = [post](const pm::remote::Peer& p) { post(RemUnlocked, p, ""); };
    // 0.8.3.4: the phone's 電腦喇叭靜音 switch (the host already refused 僅觀看 and every 臨時帳號)
    o.onMutePcRequest = [post](bool on, const std::string& label) {
        pm::remote::Peer p;
        p.label = label;
        post(RemMutePc, p, on ? "1" : "0");
    };
    o.onUploaded = [post](const pm::remote::Peer& p, const std::string& name, uint64_t) { post(RemUploaded, p, name); };
    // The kept port (the phone's Home Screen icon has it); the next free one if it is taken.
    for (int i = 0; i < 10; ++i) {
        o.port = static_cast<uint16_t>(g.settings.remotePort + i);
        if (r.host.start(o)) {
            r.host.setLockAfterSession(g.settings.remoteLockOutside, g.settings.remoteLockLan);
            SetTimer(g.hwnd, kGuestTimerId, 30000, guestTimerProc);  // 0.8.3.2 臨時帳號: time left, expiry
            if (i) {
                g.settings.remotePort = o.port;
                saveSettings();
            }
            g.log->write("info", "remote: on at " + r.host.url() +
                                     (r.host.tailscaleUrl().empty() ? std::string(" (no Tailscale)") : " and " + r.host.tailscaleUrl()));
            if (r.tunState == 1 || r.tunState == 2) r.host.setTunnel(true);  // restarted with the link running: new 通行碼 + link
            return true;
        }
    }
    return false;
}
#endif

// ---- 常駐外網 (fixed link + password, 0.8.2) ----

void updateFixedPanel() {
    RemoteState& r = R();
    if (!r.panel.isOpen()) return;
    std::vector<uint8_t> qr;
    int n = 0;
    if (r.fixState == 2) pm::share::renderQr(r.fixUrl, 1, 4, qr, n);
    r.panel.setFixed(r.fixState, r.fixUrl, qr, n, r.fixInfo, r.fixErr, !g.settings.remotePwHash.empty());
}

// 0.8.3.2: never waits (0.8.3.1 joined here on the UI thread: a CLI waiting for the person to enable
// Funnel on a web page froze the app for 30-60 s). A thread still running sees fixGen change and
// cleans up after itself; at quit stopRemote() waits a few seconds at most, then ends the CLI.
void joinFixThread() {
    if (R().fixThread.joinable()) R().fixThread.detach();
}

// tailscale funnel --https=8443 off, on a background thread.
void funnelOffAsync(const std::string& why) {
#ifdef PM_APP_REMOTE
    RemoteState& r = R();
    ++r.cliRuns;
    std::thread([why] {
        RemoteState& rs = R();
        std::string err;
        bool off = false;
        {
            std::lock_guard<std::mutex> lk(rs.cliMu);
            off = pm::remote::TailscaleFunnel::stop(kFixedPort, err);
        }
        g.log->write(off ? "info" : "warn", "remote: 常駐外網: Funnel port 8443 " + std::string(off ? "removed" : "NOT removed: " + err) + " (" + why + ")");
        if (!off) rs.fixFunnel = true;  // still ours: tried again at the next stop / quit
        --rs.cliRuns;
    }).detach();
#else
    (void)why;
#endif
}

// The tailscale CLI's answer in plain words.
std::wstring funnelWhy(const std::string& err) {
    std::string e = err;
    for (char& c : e) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (e == "no-tailscale") return tr(S::RemoteFixedWhyNoTs);
    // 0.8.3.2: what the person has to do (each with its next step)
    if (e.rfind("needs-enable: https", 0) == 0) return tr(S::RemoteFixedWhyEnableHttps);
    if (e.rfind("needs-enable", 0) == 0) return tr(S::RemoteFixedWhyEnableFunnel);
    if (e.rfind("no-funnel-attr", 0) == 0) return tr(S::RemoteFixedWhyNoAttr);
    if (e.rfind("port-taken: ", 0) == 0) return pm::i18n::fmt(S::RemoteFixedWhyPortTaken, {toWide(err.substr(12, 80))});
    if (e == "cli-too-old") return tr(S::RemoteFixedWhyOldCli);
    if (e == "needs-login" || e == "not-running: needslogin") return tr(S::RemoteFixedWhyNeedsLogin);
    if (e.rfind("timeout", 0) == 0) return tr(S::RemoteFixedWhyTimeout);
    if (e.rfind("still-starting", 0) == 0) return tr(S::RemoteFixedWhyTsSlow);  // 0.8.3.1: waited 5 minutes
    if (e.rfind("not-running", 0) == 0 || e.find("logged out") != std::string::npos || e.find("needslogin") != std::string::npos)
        return tr(S::RemoteFixedWhyNotRunning);
    if (e.find("funnel") != std::string::npos &&
        (e.find("not enabled") != std::string::npos || e.find("not available") != std::string::npos ||
         e.find("permission") != std::string::npos || e.find("access") != std::string::npos || e.find("attribute") != std::string::npos))
        return tr(S::RemoteFixedWhyNoFunnel);
    return pm::i18n::fmt(S::RemoteFixedWhyOther, {toWide(err.substr(0, 80))});
}

void stopFixed(const char* why) {
    RemoteState& r = R();
    bool ours = false;
    {
        std::lock_guard<std::mutex> lk(r.fixMu);
        ++r.fixGen;
        ours = r.fixFunnel.exchange(false);
    }
    joinFixThread();
    if (r.probeStop) *r.probeStop = true, r.probeStop.reset();  // 0.8.3.3: the TLS check ends too
    r.fixPreparing = false;
    const bool was = r.fixState != 0 || ours;
#ifdef PM_APP_REMOTE
    if (ours && !g.dev) funnelOffAsync(why);  // 0.8.3.2: in the background (a --dev instance never set it up: see startFixed)
    r.fixTunnel.stop();
    if (r.on) r.host.setFixed(false);
#endif
    r.fixState = 0, r.fixUrl.clear(), r.fixErr.clear(), r.fixInfo.clear(), r.fixSetupUrl.clear();
    if (was) g.log->write("info", std::string("remote: 常駐外網 stopped (") + why + ")");
    updateFixedPanel();
    if (!g.quitting) remoteChanged();
}

void startFixed() {
#ifdef PM_APP_REMOTE
    RemoteState& r = R();
    if (!r.on || g.settings.remotePwHash.empty() || g.settings.remoteFixedSecret.empty()) return;
    if (r.fixState == 1 || r.fixState == 2) return;
    joinFixThread();
    r.host.setFixedOptions(g.settings.remotePwHash, g.settings.remoteFixedSecret, g.settings.remoteFixedNeedApproval);
    r.host.setFixedViaCloudflared(false);  // 0.8.3: Tailscale Funnel: only its X-Forwarded-For entry is believed
    r.host.setFixed(true);
    r.fixState = 1, r.fixErr.clear(), r.fixInfo.clear(), r.fixUrl.clear();
    const unsigned gen = ++r.fixGen;
    const std::string origin = r.host.fixedOrigin();
    const HWND h = g.hwnd;
    g.log->write("info", "remote: 常駐外網 starting: Tailscale Funnel :8443 -> " + origin);
    // --dev tests: PM_REMOTE_FUNNELFAIL=1 = as if this tailnet had no Funnel (nothing is run).
    wchar_t ff[4] = {};
    const bool fakeFail = g.dev && GetEnvironmentVariableW(L"PM_REMOTE_FUNNELFAIL", ff, 4) > 0 && ff[0] == L'1';
    // --dev screenshots: PM_REMOTE_FUNNELFAKE=<url> = as if Funnel were up at that (made-up)
    // address. Nothing is run and port 8443 is never touched (fixFunnel stays false).
    wchar_t fk[256] = {};
    const DWORD fkn = g.dev ? GetEnvironmentVariableW(L"PM_REMOTE_FUNNELFAKE", fk, 256) : 0;
    const std::string fakeUrl = fkn > 0 && fkn < 256 ? toUtf8(fk) : std::string();
    // 0.8.2.1: --dev instances never run the tailscale CLI (a test could change the
    // owner's real Funnel): without PM_REMOTE_FUNNELFAKE they fail like PM_REMOTE_FUNNELFAIL.
    // 0.8.3.1 --dev shots: PM_REMOTE_FUNNELSTARTING=<n> = a stubbed tailscale CLI (no process): BackendState
    // NoState for n checks, then Running with 8443 kept by tailscaled (a made-up name), to show
    // 「等待 Tailscale 啟動」. The stub stays for the whole instance: nothing real can run after it.
    wchar_t fst[4] = {};
    const int stubStarting = g.dev && GetEnvironmentVariableW(L"PM_REMOTE_FUNNELSTARTING", fst, 4) > 0 ? std::clamp(_wtoi(fst), 0, 9) : 0;
    if (stubStarting > 0) {
        auto calls = std::make_shared<int>(0);
        pm::remote::TailscaleFunnel::setRunnerForTest([calls, stubStarting, origin](const std::wstring& cmd, std::string& out, unsigned long& code) {
            code = 0;
            const std::string name = "example-pc.example.ts.net";  // a made-up name
            if (cmd.find(L" funnel status --json") != std::wstring::npos)
                out = "{\"TCP\":{\"8443\":{\"HTTPS\":true}},\"Web\":{\"" + name + ":8443\":{\"Handlers\":{\"/\":{\"Proxy\":\"" + origin +
                      "\"}}}},\"AllowFunnel\":{\"" + name + ":8443\":true}}";
            else if (cmd.find(L" status --json") != std::wstring::npos)
                out = std::string("{\"BackendState\":\"") + ((*calls)++ < stubStarting ? "NoState" : "Running") + "\",\"Self\":{\"DNSName\":\"" + name + ".\"}}";
            else out = "refused (dev stub)", code = 1;
            return true;
        });
        g.log->write("info", "remote: 常駐外網 (PM_REMOTE_FUNNELSTARTING): stubbed tailscale CLI, NoState x" + std::to_string(stubStarting));
    }
    // 0.8.3.2 --dev tests: PM_REMOTE_FAKECLI=<path> = a FAKE tailscale CLI program (pm_fake_tailscale.exe,
    // its scenario in PM_FAKE_TS_MODE): real processes, never tailscale.
    wchar_t fcli[MAX_PATH] = {};
    const DWORD fcn = g.dev ? GetEnvironmentVariableW(L"PM_REMOTE_FAKECLI", fcli, MAX_PATH) : 0;
    const bool fakeCli = fcn > 0 && fcn < MAX_PATH && fs::path(fcli).filename() == L"pm_fake_tailscale.exe";
    if (fakeCli) {
        pm::remote::TailscaleFunnel::setCliForTest(fcli);
        g.log->write("info", "remote: 常駐外網 (PM_REMOTE_FAKECLI): a fake tailscale CLI, not tailscale");
    }
    const bool devNoCli = g.dev && fakeUrl.empty() && stubStarting == 0 && !fakeCli;
    r.fixSetupUrl.clear();
    r.fixFake = !fakeFail && !fakeUrl.empty();
    r.fixPreparing = false;
    ++r.cliRuns;
    r.fixThread = std::thread([h, gen, origin, fakeFail, fakeUrl, devNoCli] {
        struct Done {
            ~Done() { --R().cliRuns; }
        } done;
        std::string url, err = devNoCli ? std::string("--dev: the tailscale CLI is not run (PM_REMOTE_FUNNELFAKE=<url> for a made-up address)")
                                        : std::string("Funnel not available; \"funnel\" node attribute not set (test)");
        const bool fake = !fakeFail && !fakeUrl.empty();
        if (fake) url = fakeUrl;
        // 0.8.3.1: right after a reboot tailscaled is up before it is Running (a log after a reboot:
        // "not-running: NoState", 8443 fine a minute later): wait and try again (2, 4, 8 ... 60 s, about
        // 5 minutes), the page says 「等待 Tailscale 啟動」; stopping 常駐外網 / quitting ends the wait.
        auto wait = [h, gen](int attempt, int secs) {
            auto* wm = new RemoteMsg{gen, "", "", "", "", std::to_string(secs)};
            wm->fails = attempt;
            if (!PostMessageW(h, WM_PM_REMOTE, RemFixedWait, reinterpret_cast<LPARAM>(wm))) delete wm;
            for (int i = 0; i < secs * 4; ++i) {
                if (R().fixGen != gen) return false;  // stopFixed (also at quit) bumps it, then joins
                Sleep(250);
            }
            return R().fixGen == gen;
        };
        bool ok = fake;
        if (!fake && !fakeFail && !devNoCli) {
            std::lock_guard<std::mutex> lk(R().cliMu);  // one CLI job at a time (an off from a moment ago runs first)
            ok = R().fixGen == gen && pm::remote::TailscaleFunnel::startPatiently(kFixedPort, origin, url, err, wait);
            if (!ok && R().fixGen != gen) err = "cancelled";
        }
        if (!ok && err == "cancelled") return;  // stopped meanwhile: nothing to say
        if (ok && !fake) {
            std::unique_lock<std::mutex> fl(R().fixMu);
            if (R().fixGen != gen) {  // 0.8.3.2: turned off while it was starting: undone here (stopFixed did not wait)
                fl.unlock();
                std::lock_guard<std::mutex> lk(R().cliMu);
                std::string e2;
                const bool off = pm::remote::TailscaleFunnel::stop(kFixedPort, e2);
                g.log->write("info", std::string("remote: 常駐外網: started after it was turned off: port 8443 ") + (off ? "removed again" : "NOT removed: " + e2));
                if (!off) R().fixFunnel = true;
                return;
            }
            R().fixFunnel = true;  // ours to remove, whatever happens to the message
        }
        auto* m = new RemoteMsg{gen, "", "", "", "", ok ? url : err};
        if (!PostMessageW(h, WM_PM_REMOTE, ok ? RemFixedUp : RemFixedFail, reinterpret_cast<LPARAM>(m))) delete m;
    });
    updateFixedPanel();
    remoteChanged();
#endif
}

// cloudflared to 127.0.0.2 when Funnel cannot be used (the address is not fixed: said so).
void startFixedFallback(const std::wstring& why) {
#ifdef PM_APP_REMOTE
    RemoteState& r = R();
    wchar_t me[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, me, MAX_PATH);
    const std::wstring exe = pm::remote::QuickTunnel::findExe(fs::path(me).parent_path().wstring());
    if (exe.empty()) {
        r.fixState = 3, r.fixErr = why;
        return;
    }
    const HWND h = g.hwnd;
    const unsigned gen = r.fixGen;
    auto post = [h, gen](RemoteEvent ev, const std::string& s) {
        auto* m = new RemoteMsg{gen, "", "", "", "", s};
        if (!PostMessageW(h, WM_PM_REMOTE, ev, reinterpret_cast<LPARAM>(m))) delete m;
    };
    r.fixInfo = pm::i18n::fmt(S::RemoteFixedViaCf, {why});
    r.host.setFixedViaCloudflared(true);  // 0.8.3: cloudflared: only its CF-Connecting-IP is believed
    if (!r.fixTunnel.start(exe, r.host.port(), [post](const std::string& u) { post(RemFixedCfUrl, u); },
                           [post](const std::string& w) { post(RemFixedCfEnd, w); }, "127.0.0.2")) {
        r.fixState = 3, r.fixErr = why, r.fixInfo.clear();
        return;
    }
    g.log->write("info", "remote: 常駐外網: cloudflared fallback -> 127.0.0.2:" + std::to_string(r.host.port()));
#else
    (void)why;
#endif
}

// 0.8.3.3: the fixed address answers (or a --dev made-up one needs no check): 已開啟 only now.
void fixedReady(const std::string& how) {
    RemoteState& r = R();
    r.fixState = 2, r.fixInfo.clear(), r.fixPreparing = false;
    if (r.probeStop) r.probeStop.reset();
    g.log->write("info", "remote: 常駐外網 ready at " + r.fixUrl + " (Tailscale Funnel; port 443 untouched)" + how);
    g.window->showToast(pm::i18n::fmt(S::RemoteFixedOnToast, {w(r.fixUrl)}), 4000);
}

// 0.8.3.3 (audit D2): the first time a tailnet uses Funnel, Tailscale issues the HTTPS certificate in
// the background and for a minute or two every connection to the address is closed, while the CLI
// already said "available". So: one HTTPS request from this PC (TailscaleFunnel::probeTls) at once,
// then every 3 s for up to 5 minutes; until one gets an answer fixState stays 1 and the page says
// 正在準備網址，約 1–2 分鐘 (after the first failure). --dev instances check only
// PM_REMOTE_FUNNELPROBE=https://127.0.0.1:<port>/ (a fake local server in the tests); without it a
// made-up address counts as ready at once. The thread never touches RemoteState (only its flag).
void startFixedProbe() {
#ifdef PM_APP_REMOTE
    RemoteState& r = R();
    std::string url = r.fixUrl;
    bool insecure = false;
    if (g.dev) {
        wchar_t pv[256] = {};
        const DWORD n = GetEnvironmentVariableW(L"PM_REMOTE_FUNNELPROBE", pv, 256);
        url = n > 0 && n < 256 ? toUtf8(pv) : std::string();
        if (url.rfind("https://127.0.0.1:", 0) != 0) url.clear();  // never anything but the test's own server
        insecure = true;
    }
    if (url.empty()) {
        fixedReady(g.dev ? std::string(" (--dev: not checked)") : std::string());
        return;
    }
    ULONGLONG maxMs = 300000;  // 5 minutes (tests: PM_REMOTE_FUNNELPROBEMAX=<seconds>)
    if (g.dev) {
        wchar_t mx[8] = {};
        if (GetEnvironmentVariableW(L"PM_REMOTE_FUNNELPROBEMAX", mx, 8) > 0) maxMs = static_cast<ULONGLONG>(std::clamp(_wtoi(mx), 3, 600)) * 1000;
    }
    if (r.probeStop) *r.probeStop = true;
    auto stop = std::make_shared<std::atomic<bool>>(false);
    r.probeStop = stop;
    const HWND h = g.hwnd;
    const unsigned gen = r.fixGen;
    g.log->write("info", "remote: 常駐外網 address " + r.fixUrl + ": checking that it answers (HTTPS from this PC" +
                             (g.dev ? ", --dev: the test's server " + url : std::string()) + ")");
    std::thread([h, gen, url, insecure, stop, maxMs] {
        const ULONGLONG t0 = GetTickCount64();
        auto post = [&](int what, const std::string& d) {
            auto* m = new RemoteMsg{gen, "", "", "", "", std::to_string(what) + ":" + std::to_string(GetTickCount64() - t0) + ":" + d};
            if (!PostMessageW(h, WM_PM_REMOTE, RemFixedProbe, reinterpret_cast<LPARAM>(m))) delete m;
        };
        for (int i = 0; !*stop; ++i) {
            std::string d;
            const bool ok = pm::remote::TailscaleFunnel::probeTls(url, d, insecure);
            if (*stop) return;
            if (ok) return post(1, d);
            if (GetTickCount64() - t0 > maxMs) return post(2, d);
            if (i == 0 || i % 10 == 0) post(0, d);  // the first failure: 正在準備網址; then a log line every 30 s
            for (int k = 0; k < 30 && !*stop; ++k) Sleep(100);
        }
    }).detach();
#endif
}

void ensureAutostart() {
    if (g.autostart || g.dev) return;  // (--dev test instances never touch the Run key)
    if (setAutostart(true)) {
        g.autostart = true;
        g.log->write("info", "remote: 常駐外網 turned 開機時自動啟動 on");
    }
}

void setPersistent(bool on);

void openPasswordPanel(bool enableAfter) {
    RemoteState& r = R();
    pm::ui::RemotePasswordPanel::Callbacks cb;
    cb.onSave = [enableAfter](const std::wstring& password) {
        std::string pw = toUtf8(password);
#ifdef PM_APP_REMOTE
        const ULONGLONG t0 = GetTickCount64();
        g.settings.remotePwHash = pm::remote::hashPassword(pw);
        g.settings.remoteFixedSecret = pm::remote::randomHex(32);  // a new password ends every login
        g.log->write("info", "remote: 常駐外網 password set (PBKDF2-SHA256, " + std::to_string(GetTickCount64() - t0) + " ms; only the hash is kept)");
        if (R().on) R().host.setFixedOptions(g.settings.remotePwHash, g.settings.remoteFixedSecret, g.settings.remoteFixedNeedApproval);
#endif
        std::fill(pw.begin(), pw.end(), '\0');
        saveSettings();
        g.window->showToast(tr(S::RemotePwSaved), 2500);
        if (enableAfter) setPersistent(true);
        else updateFixedPanel();
        remoteChanged();
    };
    cb.onCancel = [] {};
    r.pw.open(g.hwnd, std::move(cb));
}

void setPersistent(bool on) {
    RemoteState& r = R();
    if (on) {
        if (g.settings.remotePwHash.empty() || g.settings.remoteFixedSecret.empty()) {
            openPasswordPanel(true);
            return;
        }
        g.settings.remotePersistent = true;
        saveSettings();
        ensureAutostart();
        if (!r.on) setRemote(true);
        startFixed();
        if (r.panel.isOpen()) r.panel.showTab(3);
        updateFixedPanel();
    } else {
        g.settings.remotePersistent = false;
        saveSettings();
        stopFixed("常駐外網 turned off");
        g.window->showToast(tr(S::RemoteFixedOffToast), 3000);
    }
}

// ---- 免安裝外網 (cloudflared quick tunnel, 0.8.1) ----

void updateTunnelPanel() {
    RemoteState& r = R();
    if (!g.quitting && g.hwnd) remoteChanged();  // 0.8.3.2: 允許遠端控制 shows the temporary address too
    if (!r.panel.isOpen()) return;
    std::string link, code;
    std::vector<uint8_t> qr;
    int n = 0;
#ifdef PM_APP_REMOTE
    if (r.tunState == 2) {
        link = r.tunUrl + r.host.tunnelLinkPath();
        code = r.host.tunnelPasscode();
        pm::share::renderQr(link, 1, 4, qr, n);
    }
#endif
    r.panel.setTunnel(r.tunState, link, qr, n, code, r.tunErr);
}

void startTunnel() {
#ifdef PM_APP_REMOTE
    RemoteState& r = R();
    if (!r.on || r.tunState == 1 || r.tunState == 2) return;
    if (g.dev) {  // --dev screenshots: PM_REMOTE_TUNNELFAKE=<url> = as if the temporary address were up at that
                  // (made-up) address; cloudflared is never run
        wchar_t fk[256] = {};
        const DWORD fkn = GetEnvironmentVariableW(L"PM_REMOTE_TUNNELFAKE", fk, 256);
        if (fkn > 0 && fkn < 256) {
            r.host.setTunnel(true);
            r.tunState = 2, r.tunErr.clear(), r.tunUrl = toUtf8(fk);
            g.log->write("info", "remote: 免安裝外網 (PM_REMOTE_TUNNELFAKE) shown at " + r.tunUrl + ": cloudflared not run");
            wchar_t tp[4] = {};  // 0.8.3.3 tests: the one-time link and 通行碼, to come in as cloudflared would (127.0.0.1)
            if (GetEnvironmentVariableW(L"PM_REMOTE_TESTPASS", tp, 4) > 0 && tp[0] == L'1')
                g.log->write("info", "remote: test tunnel link " + r.tunUrl + r.host.tunnelLinkPath() + " passcode " + r.host.tunnelPasscode() +
                                         " (local port " + std::to_string(r.host.port()) + ")");
            if (!r.panel.isOpen()) openRemotePanel(2);
            if (r.panel.isOpen()) r.panel.showTab(2);
            updateTunnelPanel();
            return;
        }
    }
    wchar_t me[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, me, MAX_PATH);
    const std::wstring exe = pm::remote::QuickTunnel::findExe(fs::path(me).parent_path().wstring());
    if (exe.empty()) {
        r.tunState = 3, r.tunErr = tr(S::RemoteTunnelMissing);
        g.log->write("warn", "remote: 免安裝外網: cloudflared not found");
        updateTunnelPanel();
        return;
    }
    r.host.setTunnel(true);
    const HWND h = g.hwnd;
    auto post = [h](RemoteEvent ev, const std::string& s) {
        auto* m = new RemoteMsg{0, "", "", "", "", s};
        if (!PostMessageW(h, WM_PM_REMOTE, ev, reinterpret_cast<LPARAM>(m))) delete m;
    };
    r.tunnel.onCheck = [post](bool ok, int ms) { post(RemTunnelCheck, (ok ? "1:" : "0:") + std::to_string(ms)); };
    if (!r.tunnel.start(exe, r.host.port(), [post](const std::string& u) { post(RemTunnelUrl, u); },
                        [post](const std::string& why) { post(RemTunnelEnd, why); })) {
        r.host.setTunnel(false);
        r.tunState = 3, r.tunErr = L"cloudflared";
        updateTunnelPanel();
        return;
    }
    r.tunState = 1, r.tunErr.clear(), r.tunUrl.clear();
    g.log->write("info", "remote: 免安裝外網 starting (" + toUtf8(exe) + ")");
    updateTunnelPanel();
#endif
}

void stopTunnel(const char* why) {
    RemoteState& r = R();
    if (r.tunState == 0) return;
#ifdef PM_APP_REMOTE
    r.tunnel.stop();
    r.host.setTunnel(false);
#endif
    r.tunState = 0, r.tunUrl.clear(), r.tunErr.clear();
    g.log->write("info", std::string("remote: 免安裝外網 stopped (") + why + ")");
    updateTunnelPanel();
}

void endActive(const std::string& why) {
    RemoteState& r = R();
    if (!r.active) return;
    r.active = false;
    r.banner.hide();
    unregisterHotkey();
    g.log->write("info", "remote: session of " + r.activeLabel + " (" + r.activeIp + ") ended: " + why);
    if (!g.quitting) g.window->showToast(why.rfind("idle", 0) == 0 ? tr(S::RemoteIdleToast) : tr(S::RemoteEndedToast), 3000);
    r.activeSession.clear();
    r.activeGuestId.clear();
    setPanelStatus();
    if (!g.quitting) remoteChanged();
    if (!g.quitting) g.window->setIdleQuiet(false);  // 投投 floats again
    // 0.8.2: no host restart after the session any more (options apply at once).
    // 0.8.1 restarted it here; the port was still held (TIME_WAIT) and the host
    // moved to the next one: the phone's saved link and the tunnel broke (owner's log 23:11:34).
}

void kick() {
#ifdef PM_APP_REMOTE
    if (R().active || R().ask.isOpen()) R().host.kick("ended on the PC (中斷 / Ctrl+D)");
#endif
}

void setViewOnly(bool on) {
    g.settings.remoteViewOnly = on;
    saveSettings();
#ifdef PM_APP_REMOTE
    R().host.setViewOnly(on);
#endif
    if (R().banner.visible()) R().banner.setViewOnly(on);
    setPanelStatus();
    g.window->showToast(tr(on ? S::RemoteViewOnlyOn : S::RemoteViewOnlyOff), 2500);
}

}  // namespace

bool remoteAvailable() {
#ifdef PM_APP_REMOTE
    return true;
#else
    return false;
#endif
}

bool remoteOn() { return R().on; }

void setRemoteImpl(bool on, bool quiet);
void setRemote(bool on) { setRemoteImpl(on, false); }

void setRemoteImpl(bool on, bool quiet) {
    RemoteState& r = R();
    if (on == r.on || !remoteAvailable()) return;
    if (on) {
        loadDevices();
#ifdef PM_APP_REMOTE
        if (!startHost()) {
            g.log->write("warn", "remote: could not start (no LAN interface / ports 8090+ taken)");
            g.window->showToast(tr(S::RemoteStartFailed), 4000);
            return;
        }
#endif
        r.on = true;
        remoteChanged();
        if (quiet) return;  // 常駐外網 at start: no panel, no toast
        g.window->showToast(tr(S::RemoteOnToast), 3000);
        showRemotePanel();
    } else {
        stopTunnel("remote control turned off");
        stopFixed("remote control turned off");
        r.on = false;
        r.ask.close(0);
        endActive("remote control turned off");
#ifdef PM_APP_REMOTE
        r.host.stop();
#endif
        r.panel.close();
        r.banner.hide();
        unregisterHotkey();
        g.log->write("info", "remote: off");
        if (!g.quitting) g.window->showToast(tr(S::RemoteOffToast), 2500);
        if (!g.quitting) remoteChanged();
    }
}

void openRemotePanel(int tab);

void showRemotePanel() {
    RemoteState& r = R();
    if (!r.on) {
        setRemote(true);  // opens the page
        return;
    }
    // 0.8.2: the main window's 遠端控制 page is where the QR codes are; the small
    // panel only when a phone's picture fills the window (the page is not shown then).
    if (liveSourceNow() != SrcNone) {
        openRemotePanel(-1);
        return;
    }
    {
        if (g.settings.idleTab != 1) {
            g.settings.idleTab = 1;
            saveSettings();
        }
        refreshIdleHints();
        if (!IsWindowVisible(g.hwnd) || IsIconic(g.hwnd)) PostMessageW(g.hwnd, WM_PM_TOOL, CmdShow, 0);
    }
}

// The small panel (its tabs: Wi-Fi / Tailscale / 免安裝外網 / 固定網址): over a phone's
// picture, and for the 免安裝外網 link + 通行碼 (tab 2). tab -1: as it was.
void openRemotePanel(int tab) {
    RemoteState& r = R();
    if (!r.on) return;
#ifdef PM_APP_REMOTE
    std::vector<uint8_t> lanQr, tsQr;
    int lanN = 0, tsN = 0;
    const std::string lan = r.host.url(), ts = r.host.tailscaleUrl();
    pm::share::renderQr(lan, 1, 4, lanQr, lanN);
    if (!ts.empty()) pm::share::renderQr(ts, 1, 4, tsQr, tsN);
    pm::ui::RemotePanel::Callbacks cb;
    cb.onStop = [] { setRemote(false); };
    cb.onHide = [] {};
    cb.onCopy = [](const std::string& url) {
        copyToClipboard(toWide(url));
        g.window->showToast(tr(S::RemoteCopied), 2000);
    };
    cb.onTailscaleHelp = [] { openTutorial(L"remote-tailscale"); };
    cb.onRemoteRefused = [] { refuseRemote("關閉遠端操控 / 停止外網連結 (QR panel)"); };
    cb.onTunnelStart = [] { startTunnel(); };
    cb.onTunnelStop = [] {
        stopTunnel("停止外網連結 on the panel");
        g.window->showToast(tr(S::RemoteTunnelOffToast), 2500);
    };
    cb.onFixedStart = [] { setPersistent(true); };
    cb.onFixedStop = [] { setPersistent(false); };
    cb.onFixedPassword = [] { openPasswordPanel(false); };
    r.panel.open(g.hwnd, std::move(cb));
    r.panel.setUrls(lan, lanQr, lanN, ts, tsQr, tsN);
    updateTunnelPanel();
    updateFixedPanel();
    setPanelStatus();
    if (tab >= 0) r.panel.showTab(tab);
#else
    (void)tab;
#endif
}

void setMutePcFrom(bool on, const std::string& who);  // (below) 0.8.3.4

void onRemoteEvent(WPARAM kind, LPARAM lp) {
    std::unique_ptr<RemoteMsg> m(reinterpret_cast<RemoteMsg*>(lp));
    RemoteState& r = R();
    if (m && kind == RemLoginRevoked) {  // 0.8.3: kept, expired ones dropped, at most 100
        std::string keep;
        int n = 0;
        const long long nowS = static_cast<long long>(time(nullptr));
        for (size_t a = 0; a < g.settings.remoteRevoked.size() && n < 99;) {
            const size_t b = g.settings.remoteRevoked.find(',', a);
            const std::string e = g.settings.remoteRevoked.substr(a, b == std::string::npos ? std::string::npos : b - a);
            const size_t c = e.find(':');
            if (c != std::string::npos && atoll(e.c_str() + c + 1) > nowS) keep += (keep.empty() ? "" : ",") + e, ++n;
            if (b == std::string::npos) break;
            a = b + 1;
        }
        g.settings.remoteRevoked = keep.empty() ? m->why : keep + "," + m->why;
        saveSettings();
        g.log->write("info", "remote: 常駐外網: a login ended by 中斷 is kept as revoked (" + std::to_string(n + 1) + " kept)");
        return;
    }
    if (m && kind == RemFixedLogin) {
        if (m->why != "ok") connLog("fixed", m->ip, "", m->locked ? "blocked" : "wrong-password");
        if (m->why != "ok" && m->fails == 3 && !m->locked) {  // 0.8.3: 3 wrong in a row from outside: a tray note
            g.log->write("warn", "remote: 常駐外網: 3 wrong passwords in a row from " + m->ip);
            trayBalloon(tr(S::RemotePanelTitle), pm::i18n::fmt(S::RemoteWrongPwBalloon, {L"3", w(maskIp(m->ip))}));
        }
        if (m->locked) {
            g.log->write("warn", "remote: 常駐外網: 5 wrong passwords from " + m->ip + " -> locked for 15 minutes");
            trayBalloon(tr(S::RemotePanelTitle), pm::i18n::fmt(S::RemoteFixedLockedBalloon, {w(m->ip)}));
        } else {
            g.log->write("info", "remote: 常駐外網 login from " + m->ip + (m->why == "ok" ? ": right password" : ": wrong password"));
        }
        return;
    }
    if (m && kind == RemFixedStale) {
        g.log->write("info", "remote: " + m->why);
        return;
    }
    if (m && kind == RemFixedWait) {  // 0.8.3.1: Tailscale is still starting: 「等待 Tailscale 啟動」, not 改用臨時網址
        if (m->gen != r.fixGen || !r.on || r.fixState != 1) return;
        r.fixInfo = pm::i18n::fmt(S::RemoteFixedWaitTs, {w(m->why)});
        g.log->write("info", "remote: 常駐外網: waiting for Tailscale to start (try " + std::to_string(m->fails + 1) + " in " + m->why + " s)");
        updateFixedPanel();
        remoteChanged();
        return;
    }
    if (m && kind == RemFixedProbe) {  // 0.8.3.3: the fixed address's TLS check
        if (m->gen != r.fixGen || !r.on || r.fixState != 1 || r.fixUrl.empty()) return;
        const size_t c2 = m->why.find(':', 2);
        const std::string ms = m->why.size() > 2 ? m->why.substr(2, c2 == std::string::npos ? std::string::npos : c2 - 2) : std::string();
        const std::string detail = c2 == std::string::npos ? std::string() : m->why.substr(c2 + 1);
        if (m->why.rfind("1:", 0) == 0) {
            fixedReady("; 固定網址 reachable after " + ms + " ms (" + detail + ")");
        } else if (m->why.rfind("2:", 0) == 0) {
            r.fixState = 3, r.fixErr = tr(S::RemoteFixedWhyCertSlow), r.fixInfo.clear(), r.fixPreparing = false, r.fixSetupUrl.clear();
            r.probeStop.reset();
            g.log->write("warn", "remote: 常駐外網: " + r.fixUrl + " still not reachable after " + ms + " ms (" + detail + "): gave up (Try again / temporary address)");
        } else {
            g.log->write("info", "remote: 常駐外網: " + r.fixUrl + " not reachable yet after " + ms + " ms (" + detail +
                                     (r.fixPreparing ? ")" : "): 正在準備網址 (the HTTPS certificate is probably being issued)"));
            r.fixPreparing = true;
        }
        updateFixedPanel();
        remoteChanged();
        return;
    }
    if (m && (kind == RemFixedUp || kind == RemFixedFail || kind == RemFixedCfUrl || kind == RemFixedCfEnd)) {
        if (m->gen != r.fixGen || !r.on) return;  // stopped meanwhile (stopFixed removed the port)
        if (kind == RemFixedUp && r.fixState == 1) {
            r.fixUrl = m->why, r.fixInfo.clear();  // 0.8.3.3: 已開啟 only once it answers (startFixedProbe)
            startFixedProbe();
        } else if (kind == RemFixedFail && r.fixState == 1) {
            // Said in plain words on the page, with 「改用臨時網址」 (cloudflared: the address
            // is not fixed, so it is the owner's choice, not automatic).
            r.fixState = 3, r.fixErr = funnelWhy(m->why), r.fixInfo.clear();
            // 0.8.3.2: the page to turn Funnel / HTTPS on (or the CLI's own help page): 開啟設定頁
            r.fixSetupUrl.clear();
            for (const char* key : {"needs-enable: funnel ", "needs-enable: https ", "no-funnel-attr: "})
                if (m->why.rfind(key, 0) == 0) r.fixSetupUrl = m->why.substr(strlen(key));
            if (r.fixSetupUrl.rfind("https://login.tailscale.com/", 0) != 0 && r.fixSetupUrl.rfind("https://tailscale.com/", 0) != 0) r.fixSetupUrl.clear();
            g.log->write("warn", "remote: 常駐外網: Funnel failed: " + m->why + " (the page offers a temporary address)");
        } else if (kind == RemFixedCfUrl && r.fixState == 1) {
            r.fixState = 2, r.fixUrl = m->why + "/";
            g.log->write("info", "remote: 常駐外網 ready at " + r.fixUrl + " (cloudflared fallback, not fixed)");
            g.window->showToast(pm::i18n::fmt(S::RemoteFixedOnToast, {w(r.fixUrl)}), 4000);
        } else if (kind == RemFixedCfEnd && (r.fixState == 1 || r.fixState == 2)) {
            r.fixState = 3;
            r.fixErr = (r.fixInfo.empty() ? std::wstring() : r.fixInfo + L"\n") + toWide(m->why.substr(0, 120));
            r.fixInfo.clear();
            g.log->write("warn", "remote: 常駐外網: cloudflared fallback ended: " + m->why);
        }
        updateFixedPanel();
        remoteChanged();
        return;
    }
    if (m && kind == RemGuestLogin) {  // 0.8.3.2: a 臨時帳號 login try (the successful one is logged when it connects)
        if (m->why != "ok") connLog("fixed", m->ip, "[" + m->label + "]", m->locked ? "blocked" : "wrong-password");
        g.log->write("info", "remote: 臨時帳號 login " + std::string(m->why == "ok" ? "right" : "WRONG") + " for \"" + m->label.substr(0, 40) + "\" from " + m->ip);
        return;
    }
    if (m && (kind == RemTunnelUrl || kind == RemTunnelEnd || kind == RemTunnelLink || kind == RemTunnelCheck)) {
        if (!r.on) return;
        if (kind == RemTunnelUrl && r.tunState == 1) {
            // The address is known but a fresh quick tunnel needs a few seconds before it
            // reaches this PC (measured 4-7 s): the QR code shows once the self-check gets through.
            r.tunUrl = m->why;
            g.log->write("info", "remote: 免安裝外網 address " + r.tunUrl + ", checking it from the internet");
            return;
        }
        if (kind == RemTunnelCheck && r.tunState == 1 && !r.tunUrl.empty()) {
            const bool reachable = m->why.rfind("1:", 0) == 0;
            if (!reachable) {
                g.log->write("warn", "remote: 免安裝外網 not reachable from the internet yet (QR code shown anyway)");
                g.window->showToast(tr(S::RemoteTunnelUnreachable), 6000);
            }
            r.tunState = 2;
#ifdef PM_APP_REMOTE
            g.log->write("info", "remote: 免安裝外網 ready at " + r.tunUrl + " (one-time link + 通行碼 on the panel)");
            if (g.dev) {  // --dev tests read them from the log
                wchar_t v[4] = {};
                if (GetEnvironmentVariableW(L"PM_REMOTE_TESTPASS", v, 4) > 0 && v[0] == L'1')
                    g.log->write("info", "remote: test tunnel link " + r.tunUrl + r.host.tunnelLinkPath() + " passcode " + r.host.tunnelPasscode());
            }
#endif
            if (reachable) g.window->showToast(tr(S::RemoteTunnelOnToast), 4000);
            if (!r.panel.isOpen() && !r.active) openRemotePanel(2);
            if (r.panel.isOpen()) r.panel.showTab(2);
        } else if (kind == RemTunnelEnd && (r.tunState == 1 || r.tunState == 2)) {
#ifdef PM_APP_REMOTE
            r.host.setTunnel(false);
#endif
            r.tunState = 3, r.tunErr = toWide(m->why.substr(0, 160));
            g.log->write("warn", "remote: 免安裝外網 failed: " + m->why);
            g.window->showToast(pm::i18n::fmt(S::RemoteTunnelFailed, {r.tunErr}), 5000);
        } else if (kind == RemTunnelLink) {
#ifdef PM_APP_REMOTE
            if (g.dev) {
                wchar_t v[4] = {};
                if (GetEnvironmentVariableW(L"PM_REMOTE_TESTPASS", v, 4) > 0 && v[0] == L'1')
                    g.log->write("info", "remote: test tunnel link " + r.tunUrl + m->why + " passcode " + r.host.tunnelPasscode());
            }
#endif
        }
        updateTunnelPanel();
        return;
    }
    if (!m || m->gen != r.gen || !r.on) return;
    switch (kind) {
    case RemUnlocked:  // 0.8.3: the phone lifted 僅觀看 for its session with the password
        g.log->write("info", "remote: " + m->label + " (" + m->ip + ") lifted 僅觀看 with the password (fixed link)");
        if (r.banner.visible()) r.banner.setViewOnly(false);
        g.window->showToast(pm::i18n::fmt(S::RemoteUnlockedToast, {w(m->label)}), 5000);
        if (!IsWindowVisible(g.hwnd) && !g.testOffscreen) trayBalloon(tr(S::RemotePanelTitle), pm::i18n::fmt(S::RemoteUnlockedToast, {w(m->label)}));
        setPanelStatus();
        break;
    case RemMutePc:  // 0.8.3.4: 電腦喇叭靜音 from the phone = 手機播聲音時電腦靜音 (kept like the menu item)
        setMutePcFrom(m->why == "1", "the phone (" + m->label + ")");
        break;
    case RemUploaded:  // 0.8.3: a file from the phone is in Downloads\自在投影遠端
        g.log->write("info", "remote: file from the phone saved: " + m->why);
        g.window->showToast(pm::i18n::fmt(S::RemoteUploadedToast, {w(m->why)}), 5000);
        if (!IsWindowVisible(g.hwnd) && !g.testOffscreen) trayBalloon(tr(S::RemotePanelTitle), pm::i18n::fmt(S::RemoteUploadedToast, {w(m->why)}));
        break;
    case RemRequest: {
        g.log->write("info", "remote: " + m->label + " from " + m->ip + (m->fixed ? " (常駐外網)" : m->tunnel ? " (免安裝外網)" : m->tailscale ? " (Tailscale)" : "") + " asks to connect" +
                                 (!m->guestId.empty() || (m->tunnel && !m->fixed) ? " (asked every time: no 記住)" : ""));
        r.askSession = m->session;
        r.askLabel = m->label;
        pm::ui::AskPanel::Info q;
        q.glyph = kIcoRemote;
        q.title = tr(S::RemoteAskTitle);
        q.body = tr(!m->guestId.empty() ? S::RemoteAskBodyGuest : m->tunnel && !m->fixed ? S::RemoteAskBodyTemp : S::RemoteAskBody);  // 0.8.3.2 / 0.8.3.3: someone else's phone
        q.rows.push_back({tr(S::RemoteAskDevice), w(m->label)});
        q.rows.push_back({tr(S::RemoteAskFrom),
                          pm::i18n::fmt(m->fixed ? S::RemoteAskViaFixed : m->tunnel ? S::RemoteAskViaTunnel : m->tailscale ? S::RemoteAskViaTs : S::RemoteAskViaLan, {w(m->ip)})});
        if (!m->guestId.empty()) {  // 0.8.3.2: a 臨時帳號: which one, what it may do; never remembered
            std::wstring what;
            for (const GuestRow& gr : guestRows())
                if (gr.id == m->guestId) what = guestSummary(gr);
            q.rows.push_back({tr(S::RemoteAskGuest), pm::i18n::fmt(S::RemoteAskGuestVal, {w(m->guest), what})});
        }
        q.primary = tr(S::RemoteAllow);
        q.secondary = tr(S::RemoteDeny);
        q.secondaryChoice = 2;
        // 0.8.3.3: a 臨時帳號 and the temporary address are asked every time: no 記住這支手機
        const bool guestAsk = !m->guestId.empty() || (m->tunnel && !m->fixed);
        if (!guestAsk) q.check = tr(S::RemoteRemember);
        q.checked = false;
        const std::string session = m->session, device = m->device, label = m->label;
        q.done = [session, device, label, guestAsk](int choice) {
            RemoteState& rs = R();
            if (rs.askSession == session) rs.askSession.clear();
            const bool allow = choice == 1, remember = allow && !guestAsk && rs.ask.checked();
            g.log->write("info", "remote: " + label + (allow ? " ALLOWED" : " denied") + (remember ? " (remembered)" : ""));
#ifdef PM_APP_REMOTE
            rs.host.approve(session, allow, remember);
#endif
            if (remember) rememberDevice(device, label);
            setPanelStatus();
        };
        // Never activated: whatever is typed right now keeps going where it went
        // (an Enter must not answer 允許); it is answered with a click.
        r.ask.open(g.hwnd, std::move(q), false);
        if (r.ask.hwnd()) {  // above everything, even with the app in the tray
            SetWindowPos(r.ask.hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            FLASHWINFO fi{sizeof(fi), r.ask.hwnd(), FLASHW_ALL | FLASHW_TIMERNOFG, 3, 0};
            FlashWindowEx(&fi);
        }
        if (!g.testOffscreen) MessageBeep(MB_ICONQUESTION);
        if (r.panel.isOpen()) r.panel.setStatus(pm::i18n::fmt(S::RemoteAsking, {w(m->label)}), false);
        break;
    }
    case RemActive:
        connLog(m->fixed ? "fixed" : m->tunnel ? "tunnel" : m->tailscale ? "ts" : "lan", m->ip, guestLabel(*m), "ok");
        r.activeGuestId = m->guestId;
        if (!m->guestId.empty()) {  // 0.8.3.2: 最後使用
            for (GuestRow& gr : guestRows())
                if (gr.id == m->guestId) gr.lastUsed = nowEpoch();
            saveGuests();
        }
        if (r.ask.isOpen() && r.askSession == m->session) r.ask.close(0);
        r.active = true;
        r.activeSession = m->session, r.activeLabel = m->label, r.activeIp = m->ip;
        g.log->write("info", "remote: " + m->label + " (" + m->ip + ") is connected" + (m->fixed ? " over the fixed link (password)" : ""));
        r.banner.show(w(m->label) + L"　" + w(m->ip) + (m->fixed ? std::wstring(L" · ") + tr(S::RemoteBannerFixed) : std::wstring()) +
                          (m->guest.empty() ? std::wstring() : std::wstring(L" · ") + tr(S::RemoteAskGuest) + L" " + w(m->guest)),
                      g.settings.remoteViewOnly,
                      pm::ui::RemoteBanner::Callbacks{[] { kick(); }, [](bool on) { setViewOnly(on); },
                                                      [] { refuseRemote("中斷 / 僅觀看 (banner)"); }});
        // 0.8.1: the QR panel goes once the phone is in. Left open it sits in the
        // middle of the streamed screen, and the phone's taps land on it
        // (owner's log 20:32:30: 關閉遠端操控 hit from the phone).
        if (r.panel.isOpen()) {
            r.panel.close();
            g.log->write("info", "remote: QR panel closed (a phone is connected)");
        }
        // Ctrl+D from anywhere ends it (the window's own Ctrl+D waits while this is registered).
        if (!r.hotkey) r.hotkey = RegisterHotKey(g.hwnd, kRemoteHotkeyId, MOD_CONTROL | MOD_NOREPEAT, 'D') != FALSE;
        if (!r.hotkey) g.log->write("warn", "remote: Ctrl+D is taken by another program (the bar's 中斷 still works)");
        setPanelStatus();
        remoteChanged();
        g.window->setIdleQuiet(true);  // 0.8.2: a still idle screen while the phone watches (CPU, and the encoder skips it)
        if (!IsWindowVisible(g.hwnd) && !g.testOffscreen)
            trayBalloon(tr(S::RemotePanelTitle), pm::i18n::fmt(S::RemoteActiveStatus, {w(m->label)}));
        break;
    case RemEnded:
        if (r.ask.isOpen() && r.askSession == m->session) {
            r.askSession.clear();
            r.ask.close(0);  // the phone went away / was replaced before an answer
        }
        if (m->why == "denied") connLog(m->fixed ? "fixed" : m->tunnel ? "tunnel" : m->tailscale ? "ts" : "lan", m->ip, guestLabel(*m), "denied");
        if (r.active && r.activeSession == m->session) {
            endActive(m->why);
            lockAfterSession(*m);  // 0.8.3 (after the bar is gone)
        }
        if (!m->guestId.empty()) guestSessionEnded(m->guestId);  // 0.8.3.2
        setPanelStatus();
        break;
    case RemBlocked:
        connLog(srcOfIp(m->ip), m->ip, "", "blocked");
        g.log->write("warn", "remote: " + m->ip + " blocked after too many wrong links");
        trayBalloon(tr(S::RemotePanelTitle), pm::i18n::fmt(S::RemoteBlocked, {w(m->ip)}));
        break;
    case RemTrouble:
        g.log->write("warn", "remote: " + m->ip + " cannot get the picture (" + m->why + ")");
        if (r.panel.isOpen()) r.panel.setStatus(pm::i18n::fmt(S::RemoteTrouble, {w(m->ip)}), false);
        else g.window->showToast(pm::i18n::fmt(S::RemoteTrouble, {w(m->ip)}), 6000);
        break;
    default: break;
    }
}

bool onRemoteHotkey(WPARAM id) {
    if (id != kRemoteHotkeyId) return false;
#ifdef PM_APP_REMOTE
    // Ctrl+D typed on the phone's keyboard (a browser's bookmark key...) reaches
    // the global hotkey too: it must not end the phone's own session.
    if (const int64_t ms = R().host.msSinceRemoteInput(); R().active && ms >= 0 && ms < 400) {
        g.log->write("info", "remote: Ctrl+D came from the phone -> ignored");
        return true;
    }
#endif
    g.log->write("info", "remote: Ctrl+D");
    kick();
    return true;
}

std::vector<MenuItem> remoteAdvancedItems();
void runMenu(const std::vector<MenuItem>& items, POINT pt, bool keyboard, bool tray);  // core/app/commands.cpp

// 手機播聲音時電腦靜音 (remote_mute_pc), from the PC's menu or (0.8.3.4) the phone's 電腦喇叭靜音 switch: kept in
// settings.ini; the speakers are muted only while a phone plays the sound and come back when it stops or
// the session ends (pm::remote::LoopbackAudio), whoever changed it.
void setMutePcFrom(bool on, const std::string& who) {
    const bool changed = g.settings.remoteMutePc != on;
    g.settings.remoteMutePc = on;
    if (changed) saveSettings();
#ifdef PM_APP_REMOTE
    if (R().on) R().host.setMutePc(on);  // 0.8.2: at once (0.8.1 did nothing until the session ended); the phone is told
#endif
    g.log->write("info", std::string("remote: 手機播聲音時電腦靜音 ") + (on ? "on" : "off") + " (from " + who + (changed ? ")" : ", unchanged)"));
    g.window->showToast(tr(on ? S::RemoteMutePcOnToast : S::RemoteMutePcOffToast), 2500);
    refreshIdleOptions();
}

void remoteCommand(UINT cmd) {
    struct After {  // 0.8.2: the 遠端控制 page and the title show every change
        ~After() {
            if (!g.quitting) remoteChanged();
        }
    } after;
    // Switches the phone must not reach through the PC's menus (it could end or lock itself out).
    if ((cmd == CmdRemoteToggle && R().on) || cmd == CmdRemoteKick || cmd == CmdRemoteViewOnly ||
        (cmd == CmdRemoteTunnel && R().tunState != 0) || cmd == CmdRemotePersistent || cmd == CmdRemotePassword ||
        cmd == CmdRemoteFixedApproval || cmd == CmdRemoteFixedToggle || cmd == CmdRemoteFixedTemp || cmd == CmdRemoteAdvanced ||
        cmd == CmdRemoteLockOutside || cmd == CmdRemoteLockLan || cmd == CmdRemoteLog ||
        (cmd >= CmdRemoteForgetDev0 && cmd <= CmdRemoteForgetDevLast) || cmd == CmdRemoteAudio || cmd == CmdRemoteMutePc ||
        cmd == CmdRemoteGuestNew || cmd == CmdRemoteAddrCopy || cmd == CmdRemoteTempShare || cmd == CmdRemoteFixedSetup || cmd == CmdRemoteTsDownload ||
        (cmd >= CmdRemoteGuestDel0 && cmd <= CmdRemoteGuestDelLast) || (cmd >= CmdRemoteGuestReset0 && cmd <= CmdRemoteGuestResetLast)) {
        if (fromPhone()) {
            refuseRemote(cmd == CmdRemoteToggle   ? "開啟遠端操控 (menu)"
                         : cmd == CmdRemoteKick   ? "中斷 (menu)"
                         : cmd == CmdRemoteTunnel ? "免安裝外網 (menu)"
                         : cmd == CmdRemotePersistent || cmd == CmdRemotePassword || cmd == CmdRemoteFixedApproval ||
                                   cmd == CmdRemoteFixedToggle || cmd == CmdRemoteFixedTemp ? "常駐外網 / 固定網址"
                         : cmd == CmdRemoteAdvanced || cmd == CmdRemoteAudio || cmd == CmdRemoteMutePc ? "遠端控制 settings"
                         : cmd >= CmdRemoteForgetDev0 && cmd <= CmdRemoteForgetDevLast ? "已記住的手機"
                         : cmd == CmdRemoteGuestNew || cmd == CmdRemoteAddrCopy || cmd == CmdRemoteTempShare || cmd >= CmdRemoteGuestDel0
                             ? "允許遠端控制 / 臨時帳號"
                                                  : "僅觀看 (menu)");
            return;
        }
    }
    switch (cmd) {
    case CmdRemoteToggle: setRemote(!R().on); break;
    case CmdRemoteGuestNew: openGuestNew(); break;  // 0.8.3.2
    case CmdRemoteAddrCopy: {
        std::string a;
#ifdef PM_APP_REMOTE
        if (R().fixState == 2) a = R().fixUrl;
        else if (R().tunState == 2) a = R().tunUrl + R().host.tunnelLinkPath();
#endif
        if (!a.empty()) copyToClipboard(toWide(a)), g.window->showToast(tr(S::RemoteCopied), 2000);
        break;
    }
    case CmdRemoteTempShare:  // 0.8.3.2 產生臨時網址: remote control on, the temporary address (the block shows it)
        if (!R().on) setRemote(true);
        if (R().tunState == 0 || R().tunState == 3) startTunnel();
        break;
    case CmdRemoteQr: showRemotePanel(); break;
    case CmdRemoteViewOnly: setViewOnly(!g.settings.remoteViewOnly); break;
    case CmdRemoteAudio:
        g.settings.remoteAudio = !g.settings.remoteAudio;
        saveSettings();
        if (R().on) R().host.setAudioOffered(g.settings.remoteAudio);  // 0.8.2: at once (0.8.1 waited for the session to end)
        break;
    case CmdRemoteMutePc:
        setMutePcFrom(!g.settings.remoteMutePc, "the PC");
        break;
    case CmdRemoteIdle15:
    case CmdRemoteIdle30:
    case CmdRemoteIdle60:
    case CmdRemoteIdleNever:
        g.settings.remoteIdleMinutes = cmd == CmdRemoteIdle15 ? 15 : cmd == CmdRemoteIdle30 ? 30 : cmd == CmdRemoteIdle60 ? 60 : 0;
        saveSettings();
        if (R().on) R().host.setIdleMinutes(g.settings.remoteIdleMinutes);
        break;
    case CmdRemoteForget: {
        {
            std::lock_guard<std::mutex> lk(R().dmu);
            R().devices.clear();
        }
        std::error_code ec;
        fs::remove(devicesFile(), ec);
        g.log->write("info", "remote: remembered phones forgotten");
        g.window->showToast(tr(S::RemoteForgotToast), 3000);
        break;
    }
    case CmdRemoteKick: kick(); break;
    case CmdRemoteHelp: openTutorial(L"remote"); break;
    case CmdRemoteLockOutside:
    case CmdRemoteLockLan:
        (cmd == CmdRemoteLockOutside ? g.settings.remoteLockOutside : g.settings.remoteLockLan) =
            !(cmd == CmdRemoteLockOutside ? g.settings.remoteLockOutside : g.settings.remoteLockLan);
        saveSettings();
#ifdef PM_APP_REMOTE
        if (R().on) R().host.setLockAfterSession(g.settings.remoteLockOutside, g.settings.remoteLockLan);
#endif
        g.log->write("info", std::string("remote: 遠端斷線後自動鎖定電腦: outside ") + (g.settings.remoteLockOutside ? "on" : "off") +
                                 ", LAN / Tailscale " + (g.settings.remoteLockLan ? "on" : "off"));
        break;
    case CmdRemoteLog:
        MessageBoxW(g.hwnd, connLogText().c_str(), tr(S::RemoteLogTitle), MB_OK | MB_ICONINFORMATION);
        break;
    case CmdRemotePersistent: setPersistent(!g.settings.remotePersistent); break;
    case CmdRemoteFixedToggle:
        if (R().fixState == 1 || R().fixState == 2) {
            stopFixed("關閉固定網址 on the page");
            g.window->showToast(tr(S::RemoteFixedOffToast), 3000);
        } else if (g.settings.remotePwHash.empty()) {
            openPasswordPanel(false);
        } else {
            if (!R().on) setRemoteImpl(true, true);
            stopFixed("retry");
            startFixed();
        }
        break;
    case CmdRemoteTsDownload:  // 0.8.3.3 下載 Tailscale (its own download page)
        if (g.dev || g.testOffscreen) g.log->write("info", "remote: dev: would open https://tailscale.com/download/windows (not opened)");
        else ShellExecuteW(nullptr, L"open", L"https://tailscale.com/download/windows", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case CmdRemoteFixedSetup:  // 0.8.3.2 開啟設定頁 (only a tailscale.com page from the CLI's answer)
        if (!R().fixSetupUrl.empty()) {
            if (g.dev || g.testOffscreen) g.log->write("info", "remote: dev: would open " + R().fixSetupUrl + " in the browser (not opened)");
            else ShellExecuteW(nullptr, L"open", toWide(R().fixSetupUrl).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    case CmdRemoteFixedCopy:
        if (R().fixState == 2) {
            copyToClipboard(toWide(R().fixUrl));
            g.window->showToast(tr(S::RemoteCopied), 2000);
        }
        break;
    case CmdRemoteFixedTemp:
        if (R().fixState == 3) {
            R().fixState = 1;
            startFixedFallback(R().fixErr);
            updateFixedPanel();
            remoteChanged();
        }
        break;
    case CmdRemoteLanQr:
    case CmdRemoteTsQr: {
        const int want = cmd == CmdRemoteLanQr ? 1 : 2;
        if (!R().on) {
            setRemoteImpl(true, true);
            R().pageQr = want;
        } else {
            R().pageQr = R().pageQr == want ? 0 : want;
        }
        remoteChanged();
        break;
    }
    case CmdRemoteAdvanced: {
        POINT pt{};
        GetCursorPos(&pt);
        runMenu(remoteAdvancedItems(), pt, false, false);
        break;
    }
    case CmdRemoteTsHelp: openTutorial(L"remote-tailscale"); break;
    case CmdRemotePassword: openPasswordPanel(false); break;
    case CmdRemoteFixedApproval:
        g.settings.remoteFixedNeedApproval = !g.settings.remoteFixedNeedApproval;
        saveSettings();
#ifdef PM_APP_REMOTE
        if (R().on) R().host.setFixedOptions(g.settings.remotePwHash, g.settings.remoteFixedSecret, g.settings.remoteFixedNeedApproval);
#endif
        g.log->write("info", std::string("remote: 外面的新手機也要電腦按允許 ") + (g.settings.remoteFixedNeedApproval ? "on" : "off"));
        break;
    case CmdRemoteTunnel:
        if (R().tunState == 0 || R().tunState == 3) {
            if (!R().on) setRemote(true);
            startTunnel();
            openRemotePanel(2);  // the link + 通行碼 show there (in ~10-20 s)
        } else {
            stopTunnel("menu");
            g.window->showToast(tr(S::RemoteTunnelOffToast), 2500);
        }
        break;
    default:
        if (cmd >= CmdRemoteGuestDel0 && cmd <= CmdRemoteGuestDelLast) deleteGuest(cmd - CmdRemoteGuestDel0);
        if (cmd >= CmdRemoteGuestReset0 && cmd <= CmdRemoteGuestResetLast) resetGuest(cmd - CmdRemoteGuestReset0);
#ifdef PM_APP_REMOTE
        if (cmd >= CmdRemoteForgetDev0 && cmd <= CmdRemoteForgetDevLast) {
            forgetDevice(cmd - CmdRemoteForgetDev0);
            remoteChanged();
            break;
        }
        if (cmd == CmdRemoteAudioAuto || cmd == CmdRemoteAudioDefault || (cmd >= CmdRemoteAudioDev0 && cmd <= CmdRemoteAudioDevLast)) {
            std::wstring name;
            if (cmd == CmdRemoteAudioAuto) g.settings.remoteAudioSource.clear(), name = tr(S::RemoteAudioAuto);
            else if (cmd == CmdRemoteAudioDefault) g.settings.remoteAudioSource = "default", name = tr(S::RemoteAudioDefault);
            else {
                const size_t i = cmd - CmdRemoteAudioDev0;
                if (i >= R().audioDevs.size()) break;
                g.settings.remoteAudioSource = R().audioDevs[i].id;
                name = w(R().audioDevs[i].name);
            }
            saveSettings();
            if (R().on) R().host.setAudioSource(g.settings.remoteAudioSource);
            g.log->write("info", "remote: sound from " + toUtf8(name));
            g.window->showToast(pm::i18n::fmt(S::RemoteAudioSrcToast, {name}), 2500);
        }
#endif
        break;
    }
}

// 遠端操控 ▸ (both menus; right: 「開」 / the connected phone).
MenuItem remoteItem() {
    RemoteState& r = R();
    std::vector<MenuItem> sub;
    sub.push_back(checkItem(CmdRemoteToggle, tr(S::MenuRemoteOn), kIcoRemote, r.on));
    if (r.on) sub.push_back(MenuItem::command(CmdRemoteQr, tr(S::MenuRemoteQr), kIcoQr));
    if (r.on) sub.push_back(checkItem(CmdRemoteTunnel, tr(S::MenuRemoteTunnel), kIcoRemote, r.tunState == 1 || r.tunState == 2));
    if (r.active) sub.push_back(MenuItem::command(CmdRemoteKick, tr(S::MenuRemoteKick), kIcoDisconnect, L"Ctrl+D"));
    sub.push_back(MenuItem::separator());
    // 常駐外網 (0.8.2)
    sub.push_back(checkItem(CmdRemotePersistent, tr(S::MenuRemotePersistent), kIcoRemote, g.settings.remotePersistent));
    {
        MenuItem pwItem = MenuItem::command(CmdRemotePassword, tr(S::MenuRemotePassword), kIcoSettings);
        sub.push_back(std::move(pwItem));
        MenuItem ap = checkItem(CmdRemoteFixedApproval, tr(S::MenuRemoteFixedApproval), kIcoShow, g.settings.remoteFixedNeedApproval);
        ap.enabled = !g.settings.remotePwHash.empty();
        sub.push_back(std::move(ap));
    }
    sub.push_back(MenuItem::separator());
    sub.push_back(checkItem(CmdRemoteViewOnly, tr(S::MenuRemoteViewOnly), kIcoShow, g.settings.remoteViewOnly));
    sub.push_back(checkItem(CmdRemoteAudio, tr(S::MenuRemoteAudio), kIcoCast, g.settings.remoteAudio));
    MenuItem mute = checkItem(CmdRemoteMutePc, tr(S::MenuRemoteMutePc), kIcoCast, g.settings.remoteMutePc);
    mute.enabled = g.settings.remoteAudio;
    sub.push_back(std::move(mute));
#ifdef PM_APP_REMOTE
    {
        // 聲音來源 ▸ 自動 / Windows 預設 / each active playback device (0.8.1).
        r.audioDevs = pm::remote::playbackDevices();
        if (r.audioDevs.size() > 8) r.audioDevs.resize(8);
        const std::string& src = g.settings.remoteAudioSource;
        std::vector<MenuItem> devs;
        auto radio = [&](UINT id, const std::wstring& label, bool on) {
            MenuItem m = MenuItem::command(id, label);
            m.radio = true;
            m.checked = on;
            m.enabled = g.settings.remoteAudio;
            devs.push_back(std::move(m));
        };
        radio(CmdRemoteAudioAuto, tr(S::RemoteAudioAuto), src.empty());
        radio(CmdRemoteAudioDefault, tr(S::RemoteAudioDefault), src == "default");
        std::wstring right = src.empty() ? tr(S::RemoteAudioAutoShort) : src == "default" ? tr(S::RemoteAudioDefaultShort) : L"";
        for (size_t i = 0; i < r.audioDevs.size(); ++i) {
            radio(CmdRemoteAudioDev0 + static_cast<UINT>(i), w(r.audioDevs[i].name), src == r.audioDevs[i].id);
            if (src == r.audioDevs[i].id) right = w(r.audioDevs[i].name);
        }
        if (right.empty()) right = tr(S::RemoteAudioDefaultShort);  // the chosen device is gone: the default is used
        if (right.size() > 18) right = right.substr(0, 17) + L"…";
        MenuItem srcItem = valueSubmenu(tr(S::MenuRemoteAudioSrc), kIcoCast, std::move(devs), right);
        srcItem.enabled = g.settings.remoteAudio;
        sub.push_back(std::move(srcItem));
    }
#endif
    {
        std::vector<MenuItem> idle;
        const int mins[3] = {15, 30, 60};
        for (int i = 0; i < 3; ++i) {
            MenuItem m = MenuItem::command(CmdRemoteIdle15 + i, pm::i18n::fmt(S::RemoteMinutes, {std::to_wstring(mins[i])}));
            m.radio = true;
            m.checked = g.settings.remoteIdleMinutes == mins[i];
            idle.push_back(std::move(m));
        }
        MenuItem never = MenuItem::command(CmdRemoteIdleNever, tr(S::MenuRemoteNever));
        never.radio = true;
        never.checked = g.settings.remoteIdleMinutes == 0;
        idle.push_back(std::move(never));
        sub.push_back(valueSubmenu(tr(S::MenuRemoteIdle), kIcoPower, std::move(idle),
                                   g.settings.remoteIdleMinutes ? pm::i18n::fmt(S::RemoteMinutes, {std::to_wstring(g.settings.remoteIdleMinutes)})
                                                                : std::wstring(tr(S::MenuRemoteNever))));
    }
    {
        // 0.8.3 遠端斷線後自動鎖定電腦 ▸ 從外網連線時 ✓ / 在家連線時
        std::vector<MenuItem> lock;
        lock.push_back(checkItem(CmdRemoteLockOutside, tr(S::MenuRemoteLockOutside), kIcoPower, g.settings.remoteLockOutside));
        lock.push_back(checkItem(CmdRemoteLockLan, tr(S::MenuRemoteLockLan), kIcoPower, g.settings.remoteLockLan));
        sub.push_back(valueSubmenu(tr(S::MenuRemoteLock), kIcoPower, std::move(lock),
                                   g.settings.remoteLockOutside || g.settings.remoteLockLan ? std::wstring(L"✓") : std::wstring()));
    }
    sub.push_back(MenuItem::command(CmdRemoteLog, tr(S::MenuRemoteLog), kIcoHelp));
    MenuItem forget = MenuItem::command(CmdRemoteForget, tr(S::MenuRemoteForget), kIcoDelete,
                                        r.on ? std::to_wstring(rememberedCount()) : std::wstring());
    sub.push_back(std::move(forget));
    sub.push_back(MenuItem::separator());
    sub.push_back(MenuItem::command(CmdRemoteHelp, tr(S::MenuRemoteHelp), kIcoHelp));
    std::wstring right;
    if (r.active) right = w(r.activeLabel);
    else if (r.on) right = tr(S::RemoteOnShort);
    return valueSubmenu(tr(S::MenuRemoteSub), kIcoRemote, std::move(sub), right);
}

// 進階設定: what the page keeps out of sight (聲音來源, 外面的新手機也要允許,
// 已記住的手機, 僅觀看, 自動中斷, 免安裝外網).
std::vector<MenuItem> remoteAdvancedItems() {
    RemoteState& r = R();
    std::vector<MenuItem> v;
    MenuItem src = remoteItem();  // its 聲音來源 ▸ / 自動中斷 ▸ rows are reused
    // 0.8.3.4 聲音 ▸ 電腦聲音傳到手機 ✓, 手機播聲音時電腦靜音 ✓ (off the page) and 聲音來源 (its choices inline)
    std::vector<MenuItem> sound;
    for (auto& m : src.sub)
        if (m.id == CmdRemoteAudio || m.id == CmdRemoteMutePc) sound.push_back(m);
    size_t at = std::wstring::npos;  // where 聲音來源 ▸ was
    for (auto& m : src.sub) {
        if (m.kind == MenuItem::Kind::Submenu && m.text == tr(S::MenuRemoteAudioSrc)) {
            at = v.size();
            sound.push_back(MenuItem::separator());
            sound.push_back(MenuItem::caption(m.text));
            for (auto& d : m.sub) sound.push_back(d);
            continue;
        }
        const bool keep = m.id == CmdRemoteViewOnly || m.id == CmdRemoteFixedApproval || m.id == CmdRemoteTunnel ||
                          m.id == CmdRemotePassword || m.id == CmdRemotePersistent || (m.kind == MenuItem::Kind::Submenu);
        if (m.id == CmdRemotePersistent && !g.settings.remotePwHash.empty())  // 0.8.3.4 (design B): off the page, here
            v.push_back(checkItem(CmdRemoteFixedToggle, tr(S::MenuRemoteFixedOn), kIcoRemote, r.fixState == 1 || r.fixState == 2));
        if (keep) v.push_back(m);
    }
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(std::min(at, v.size())),
             valueSubmenu(tr(S::MenuRemoteSound), kIcoCast, std::move(sound),
                          g.settings.remoteAudio ? std::wstring(tr(S::RemoteOnShort)) : std::wstring()));
    // 已記住的手機 ▸ each phone (click = forget it), 全部忘記
    std::vector<MenuItem> phones;
    const std::vector<Remembered> list = rememberedList();
    if (list.empty()) {
        MenuItem none = MenuItem::command(0, tr(S::RemoteRememberedNone));
        none.enabled = false;
        phones.push_back(std::move(none));
    } else {
        phones.push_back(MenuItem::caption(tr(S::RemoteForgetOneHint)));
        for (size_t i = 0; i < list.size() && i < 16; ++i)
            phones.push_back(MenuItem::command(CmdRemoteForgetDev0 + static_cast<UINT>(i), w(list[i].label.empty() ? list[i].id.substr(0, 8) : list[i].label),
                                               kIcoDelete, w(list[i].date)));
        phones.push_back(MenuItem::separator());
        phones.push_back(MenuItem::command(CmdRemoteForget, tr(S::MenuRemoteForget), kIcoDelete));
    }
    v.push_back(valueSubmenu(tr(S::MenuRemoteRemembered), kIcoPhone, std::move(phones), std::to_wstring(list.size())));
    (void)r;
    return v;
}

// The window title and the page follow every change of state.
void remoteChanged() {
    RemoteState& r = R();
    std::wstring path = L"Wi-Fi";
#ifdef PM_APP_REMOTE
    if (r.on && !r.host.tailscaleUrl().empty()) path += L" + Tailscale";
#endif
    if (r.fixState == 2) path += std::wstring(L" + ") + tr(S::CardRemotePathFixed);
    g.remoteTitle = !r.on ? std::wstring() : r.active ? pm::i18n::fmt(S::RemoteTitleActive, {w(r.activeLabel)})
                                                     : pm::i18n::fmt(S::RemoteTitleWaiting, {path});
    refreshTitles();
    if (!g.quitting && g.hwnd && liveSourceNow() == SrcNone) SetWindowTextW(g.hwnd, currentTitle().c_str());
    if (!g.quitting) refreshIdleHints();
}

void remoteIdlePage(std::vector<pm::VideoWindow::IdleCard>& cards, std::vector<pm::VideoWindow::IdleAction>& actions) {
    using IC = pm::VideoWindow::IdleCard;
    using IA = pm::VideoWindow::IdleAction;
    RemoteState& r = R();
    const HWND h = g.hwnd;  // posted: the window is inside its own click handling
    auto cmd = [h](UINT c) { return [h, c]() { PostMessageW(h, WM_PM_TOOL, c, 0); }; };
    auto act = [&](const std::wstring& label, bool primary, UINT c, int card, int kind = 0, bool on = false) {
        IA a{label, primary, c ? cmd(c) : std::function<void()>([] {}), card};
        a.kind = kind, a.on = on;
        actions.push_back(std::move(a));
    };
    auto strong = [&]() { actions.back().style = 1; };  // 0.8.3.4: the block's main action, filled
    // The big switch: 遠端控制 開 / 關 and where it stands.
    {
        IA sw{tr(S::RemoteSwLabel), false, cmd(CmdRemoteToggle), -1};
        sw.kind = 2, sw.on = r.on;
        sw.sub = r.on ? remoteStatusLine() : std::wstring(tr(S::RemoteSwOff));
        actions.push_back(std::move(sw));
    }
    std::string lan, ts;
#ifdef PM_APP_REMOTE
    if (r.on) lan = r.host.url(), ts = r.host.tailscaleUrl();
    // --dev screenshots: PM_REMOTE_SHOWLAN / PM_REMOTE_SHOWTS = made-up addresses shown on
    // this page (text and QR) instead of this PC's real ones; nothing else changes.
    if (g.dev && r.on) {
        auto shown = [](const wchar_t* var, std::string& url) {
            wchar_t v[256] = {};
            const DWORD n = GetEnvironmentVariableW(var, v, 256);
            if (n > 0 && n < 256 && !url.empty()) url = toUtf8(v);
        };
        shown(L"PM_REMOTE_SHOWLAN", lan);
        shown(L"PM_REMOTE_SHOWTS", ts);
    }
#endif
    const bool noTs = r.on ? ts.empty() : pm::remote::TailscaleFunnel::findCli().empty();
    auto qrOf = [](const std::string& url, IC& c) {
        if (url.empty()) return;
        std::vector<uint8_t> q;
        int n = 0;
        pm::share::renderQr(url, 1, 4, q, n);
        if (n > 0) c.qr = std::move(q), c.qrSize = n;
    };
    // 在家（同一個 Wi-Fi）
    auto lanCard = [&]() {
        const int i = static_cast<int>(cards.size());
        IC c{tr(S::RemoteCardLanTitle), tr(S::RemoteCardLanBody), L"", false};
        c.style = 2;  // 0.8.3.4 (design B): a compact row
        const bool show = r.on && r.pageQr == 1 && !lan.empty();
        if (show) qrOf(lan, c), c.note = w(lan);
        cards.push_back(std::move(c));
        act(tr(show ? S::RemoteHideQr : S::RemoteShowQr), true, CmdRemoteLanQr, i);
    };
    // 0.8.3.2 允許遠端控制 (at the top, like ToDesk's 本機 ID + 臨時密碼): 本機網址 big with its QR and
    // 複製網址, 新增臨時帳號 and the accounts (剩餘時間 / 權限 / 最後使用, 重設密碼 · 刪除). Without the
    // fixed address: a one-time temporary address (cloudflared) with its 通行碼, made with one click.
    auto allowCard = [&]() {
        const int i = static_cast<int>(cards.size());
        const bool hasPw = !g.settings.remotePwHash.empty();
        IC c{tr(S::RemoteAllowTitle), L"", L"", false};
        std::string tunLink, tunCode;
#ifdef PM_APP_REMOTE
        if (r.on && r.tunState == 2) tunLink = r.tunUrl + r.host.tunnelLinkPath(), tunCode = r.host.tunnelPasscode();
#endif
        auto tunnelRows = [&]() {  // the temporary address: ready / getting it / failed / one click
            if (!tunLink.empty()) {
                c.title = tr(S::RemoteTempAddrLabel), c.style = 1;  // 0.8.3.4 (design B)
                c.big = w(tunLink);
                c.note = pm::i18n::fmt(S::RemoteTunnelCode, {w(tunCode)});
                qrOf(tunLink, c);
                return 2;
            }
            if (r.tunState == 1) return c.note = tr(S::RemoteTunnelStarting), 1;
            if (r.tunState == 3) c.note = pm::i18n::fmt(S::RemoteTunnelFailed, {r.tunErr});
            return 0;
        };
        if (hasPw && r.fixState == 2) {
            // 0.8.3.4 (design B, like ToDesk's 本機識別碼): 「本機網址」 as a small label over the address, big
            c.title = tr(S::RemoteAllowAddr), c.style = 1;
            c.big = w(r.fixUrl);
            if (!r.fixInfo.empty()) c.note = r.fixInfo;
            qrOf(r.fixUrl, c);
            cards.push_back(std::move(c));
            act(tr(S::RemoteFixedCopy), true, CmdRemoteAddrCopy, i), strong();
            if (guestRows().size() < kMaxGuests) act(tr(S::RemoteGuestAdd), true, CmdRemoteGuestNew, i);
        } else if (hasPw && r.fixState == 1 && r.fixPreparing && !r.fixUrl.empty()) {
            // 0.8.3.3: the address is known but does not answer yet (the HTTPS certificate): no QR
            // code and no 臨時帳號 until it does; 已開啟 is said only then.
            c.body = tr(S::RemoteFixedPreparing);
            c.big = w(r.fixUrl);
            cards.push_back(std::move(c));
            act(tr(S::RemoteFixedClose), false, CmdRemoteFixedToggle, i);
            return;
        } else if (hasPw) {
            c.body = r.fixState == 1 ? (r.fixInfo.empty() ? std::wstring(tr(S::RemoteFixedStarting)) : r.fixInfo)
                     : r.fixState == 3 ? pm::i18n::fmt(S::RemoteFixedFailed, {r.fixErr})
                                       : std::wstring(tr(S::RemoteAllowFixedOff));
            const int t = tunnelRows();
            if (t == 2 && r.fixState != 3) c.body = tr(S::RemoteTempReadyBody);  // 0.8.3.4: what to do now, not how to make one
            cards.push_back(std::move(c));
            if (r.fixState == 0) act(tr(S::RemoteFixedOpen), true, CmdRemoteFixedToggle, i), strong();
            if (r.fixState == 3 && !r.fixSetupUrl.empty()) {  // 0.8.3.2: once on the web page, then again
                act(tr(S::RemoteFixedOpenSetup), true, CmdRemoteFixedSetup, i);
                act(tr(S::RemoteFixedRetryDone), true, CmdRemoteFixedToggle, i);
                act(tr(S::RemoteFixedTemp), false, CmdRemoteFixedTemp, i);
            } else if (r.fixState == 3) {
                act(tr(S::RemoteTunnelRetry), true, CmdRemoteFixedToggle, i), act(tr(S::RemoteFixedTemp), false, CmdRemoteFixedTemp, i);
            }
            if (t == 2) act(tr(S::RemoteFixedCopy), false, CmdRemoteAddrCopy, i), act(tr(S::RemoteTunnelStop), false, CmdRemoteTunnel, i);
            else if (t == 0 && r.fixState != 3) act(tr(S::RemoteTempShare), false, CmdRemoteTempShare, i);
        } else {
            c.body = tr(S::RemoteAllowTempBody);
            const int t = tunnelRows();
            if (t == 2) c.body = tr(S::RemoteTempReadyBody);  // 0.8.3.4
            cards.push_back(std::move(c));
            if (t == 2) act(tr(S::RemoteFixedCopy), true, CmdRemoteAddrCopy, i), strong(), act(tr(S::RemoteTunnelStop), false, CmdRemoteTunnel, i);
            else if (t == 0) act(tr(S::RemoteTempShare), true, CmdRemoteTempShare, i), strong();
            act(tr(S::RemoteAllowSetPw), false, CmdRemotePassword, i);
            return;
        }
        // 0.8.3.4 (design B): 變更密碼, 關閉固定網址 and 開機自動開啟固定網址 are in 進階設定 (kept out of sight).
        // The accounts (also while the fixed address is off: they can still be deleted). 0.8.3.4: a card of
        // their own under the address (the whole width: one line each; the QR code no longer beside a list).
        const std::vector<GuestRow>& gs = guestRows();
        if (!gs.empty()) {
            const int j = static_cast<int>(cards.size());
            cards.push_back(IC{pm::i18n::fmt(S::RemoteGuestsHead, {std::to_wstring(gs.size())}),
                               r.fixState == 2 ? std::wstring(tr(S::RemoteAllowFoot)) : std::wstring(), L"", false});
            for (size_t k = 0; k < gs.size() && k < kMaxGuests; ++k) {
                act(guestRowText(gs[k]), false, 0, j, 4);
                act(tr(S::RemoteGuestReset), false, CmdRemoteGuestReset0 + static_cast<UINT>(k), j, 5);
                act(tr(S::RemoteGuestDelete), false, CmdRemoteGuestDel0 + static_cast<UINT>(k), j, 5);
            }
        }
    };
    // 在外面：Tailscale
    auto tsCard = [&]() {
        const int i = static_cast<int>(cards.size());
        IC c{tr(S::RemoteCardTsTitle), tr(S::RemoteCardTsBody), noTs ? std::wstring(tr(S::RemoteNoTsShort)) : std::wstring(), false};
        c.style = 2;  // 0.8.3.4 (design B): a compact row
        const bool show = r.on && r.pageQr == 2 && !ts.empty();
        if (show) qrOf(ts, c), c.note = w(ts);
        cards.push_back(std::move(c));
        if (noTs) act(tr(S::RemoteTsDownload), true, CmdRemoteTsDownload, i), act(tr(S::RemoteTsGuide), false, CmdRemoteTsHelp, i);  // 0.8.3.3
        else act(tr(show ? S::RemoteHideQr : S::RemoteShowQr), true, CmdRemoteTsQr, i);
    };
    allowCard();  // 0.8.3.2: 允許遠端控制 first, then 在家 and Tailscale as before
    lanCard();
    tsCard();
    act(tr(S::RemoteAdvanced), false, CmdRemoteAdvanced, -1);
    actions.push_back(IA{tr(S::RemoteHowTo), false, []() { openTutorial(L"remote"); }, -1});
}

MenuItem remoteTrayToggle() {
    RemoteState& r = R();
    std::wstring right;
    if (r.active) right = w(r.activeLabel);
    else if (r.fixState == 2) right = tr(S::CardRemotePathFixed);
    if (right.size() > 18) right = right.substr(0, 17) + L"…";
    return checkItem(CmdRemoteToggle, tr(S::MenuRemoteTray), kIcoRemote, r.on, right);
}

std::wstring remoteStatusLine() {
    RemoteState& r = R();
    if (!r.on) return {};
    std::wstring path = L"Wi-Fi";
#ifdef PM_APP_REMOTE
    if (!r.host.tailscaleUrl().empty()) path += L" + Tailscale";
#endif
    if (r.fixState == 2) path += std::wstring(L" + ") + tr(S::CardRemotePathFixed);
    return r.active ? pm::i18n::fmt(S::CardRemoteOnActive, {path, L"1"}) : pm::i18n::fmt(S::CardRemoteOnIdle, {path});
}

// 0.8.3.1: 新功能 belongs to a feature release (x.y.z): a fix-up (0.8.3.1) neither shows 0.8.3's
// again nor calls it 0.8.3.1's.
static std::string newsVersion() {
    std::string v = kAppVersion;
    size_t p = 0;
    for (int dots = 0; (p = v.find('.', p)) != std::string::npos; ++p)
        if (++dots == 3) return v.substr(0, p);
    return v;
}

void showWhatsNew() {
    RemoteState& r = R();
    pm::ui::AskPanel::Info q;
    q.glyph = kIcoRemote;
    q.title = pm::i18n::fmt(S::WhatsNewTitle, {toWide(newsVersion())});
    q.body = tr(S::WhatsNewBody);
    // 0.8.3
    q.rows.push_back({tr(S::WhatsNewLabel1), tr(S::WhatsNewInk)});
    q.rows.push_back({tr(S::WhatsNewLabel2), tr(S::WhatsNewDrop)});
    q.rows.push_back({tr(S::WhatsNewLabel3), tr(S::WhatsNewAndroid)});
    q.rows.push_back({tr(S::WhatsNewLabel4), tr(S::WhatsNewRemote)});
    q.rows.push_back({tr(S::WhatsNewLabel5), tr(S::WhatsNewFixes)});
    q.primary = tr(S::WhatsNewOk);
    q.secondary = tr(S::WhatsNewTry);
    q.secondaryChoice = 2;
    q.done = [](int choice) {
        g.settings.whatsNewSeen = newsVersion();
        saveSettings();
        g.log->write("info", std::string("what's new ") + kAppVersion + " closed (" + std::to_string(choice) + ")");
        if (choice == 2) openTutorial(nullptr);  // 0.8.3: the guide from the top
    };
    r.news.open(g.hwnd, std::move(q), !g.testOffscreen);
}

void remoteStartup(bool background) {
    RemoteState& r = R();
    // 新功能: once, after an upgrade (a first start gets the tutorial instead), not from autostart.
    if (g.settings.whatsNewSeen != newsVersion()) {
        if (!g.settings.tutorialShown) {
            g.settings.whatsNewSeen = newsVersion();  // first start: nothing is "new"
            saveSettings();
        } else if (!background) {
            showWhatsNew();
        }
    }
    if (!remoteAvailable()) return;
#ifdef PM_APP_REMOTE
    if (g.settings.remotePersistent && !g.settings.remotePwHash.empty()) {
        g.log->write("info", "remote: 常駐外網 is on: remote control + the fixed link at start");
        setRemoteImpl(true, true);
        startFixed();
        return;
    }
    // A fixed link left behind (a crash, or turned off while the app was gone): 8443
    // pointing at this app's origin (127.0.0.2) is ours to remove. Nothing else is touched.
    // 0.8.2.1: never from a --dev instance (no tailscale CLI at all there), and only when
    // nothing answers at that origin (otherwise another running 自在投影 is serving it:
    // a --dev test once removed the installed app's live fixed link this way).
    if (g.dev) return;
    const HWND h = g.hwnd;
    ++r.cliRuns;
    r.fixThread = std::thread([h] {
        struct Done {
            ~Done() { --R().cliRuns; }
        } done;
        std::lock_guard<std::mutex> lk(R().cliMu);  // 0.8.3.2: one CLI job at a time
        const pm::remote::FunnelStatus st = pm::remote::TailscaleFunnel::status(kFixedPort);
        if (!st.hasPort || st.target.rfind("http://127.0.0.2:", 0) != 0) return;
        if (pm::remote::TailscaleFunnel::targetAnswers(st.target)) {
            auto* m = new RemoteMsg{0, "", "", "", "", std::string("常駐外網: Funnel :8443 -> ") + st.target + " is in use by another running app: left alone"};
            if (!PostMessageW(h, WM_PM_REMOTE, RemFixedStale, reinterpret_cast<LPARAM>(m))) delete m;
            return;
        }
        std::string err;
        const bool off = pm::remote::TailscaleFunnel::stop(kFixedPort, err);
        auto* m = new RemoteMsg{0, "", "", "", "", std::string("常駐外網: a stale Funnel :8443 -> ") + st.target + (off ? " removed" : " NOT removed: " + err)};
        if (!PostMessageW(h, WM_PM_REMOTE, RemFixedStale, reinterpret_cast<LPARAM>(m))) delete m;
    });
#else
    (void)r;
#endif
}

void stopRemote() {
    RemoteState& r = R();
    stopTunnel("quit");
    stopFixed("quit");  // the Funnel port goes with the app (nothing would answer behind it)
#ifdef PM_APP_REMOTE
    if (g.quitting) {  // 0.8.3.2: the window is gone: the port off (normally < 1 s), 4 s at most, then the CLI is ended
        for (int i = 0; i < 40 && r.cliRuns > 0; ++i) Sleep(100);
        if (r.cliRuns > 0) {
            g.log->write("warn", "remote: a tailscale CLI run still going at quit: ended");
            pm::remote::TailscaleFunnel::cancelAll();
            for (int i = 0; i < 20 && r.cliRuns > 0; ++i) Sleep(100);
        }
    }
#endif
    r.pw.close();
    r.guestPanel.close();
    r.news.close(0);
    if (!r.on && !r.banner.visible()) return;
    r.ask.close(0);
    r.on = false;
#ifdef PM_APP_REMOTE
    r.host.stop();
#endif
    r.active = false;
    r.panel.close();
    r.banner.hide();
    unregisterHotkey();
}

void remoteRetheme() {
    R().pw.retheme();
    R().guestPanel.retheme();
    R().news.retheme();
    R().panel.retheme();
    R().banner.retheme();
    R().ask.retheme();
}

void remoteRelabel() {
    R().pw.relabel();
    R().guestPanel.relabel();
    R().panel.relabel();
    R().banner.relabel();
}

void remoteDevShots() {
    RemoteState& r = R();
    const fs::path d = fs::temp_directory_path() / L"pmshots";
    std::error_code ec;
    fs::create_directories(d, ec);
    bool p = false, t = false, b = false, a = false;
    if (r.panel.isOpen()) {
        const int tab = r.panel.tab();
        r.panel.showTab(0);
        p = r.panel.renderPng((d / L"remote_panel.png").wstring());
        r.panel.showTab(1);
        t = r.panel.renderPng((d / L"remote_panel_ts.png").wstring());
        r.panel.showTab(2);
        r.panel.renderPng((d / L"remote_panel_tun.png").wstring());
        r.panel.showTab(tab);
    }
    {
        pm::ui::RemoteBanner probe;  // drawn whether or not a phone is connected
        b = (r.banner.visible() ? r.banner : probe).renderPng((d / L"remote_banner.png").wstring());
    }
    if (r.ask.isOpen()) a = r.ask.renderPng((d / L"remote_ask.png").wstring());
    g.log->write("info", std::string("dev remote shots: panel ") + (p ? "ok" : "-") + ", tailscale tab " + (t ? "ok" : "-") +
                             ", banner " + (b ? "ok" : "-") + ", ask " + (a ? "ok" : "-"));
}

void remoteDevClick(int how) {
    RemoteState& r = R();
    g.log->write("info", "dev remote click: " + std::to_string(how));
    if (how == 1 || how == 2) {
        if (!r.panel.isOpen()) openRemotePanel(0);
        r.panel.testClickStop(how == 1);
    } else if (how == 3 || how == 4) {
        r.banner.testClick(how == 3 ? 2 : 1, true);
    } else if (how == 5) {
        const LPARAM old = SetMessageExtraInfo(static_cast<LPARAM>(pm::ui::kRemoteInputTag));
        remoteCommand(CmdRemoteToggle);
        SetMessageExtraInfo(old);
    } else if (how == 6) {
#ifdef PM_APP_REMOTE
        // Armed: the menu's 開啟遠端操控 (off) runs right after the phone's next
        // click (a plain message, no tag): the timestamp guard must refuse it.
        static int64_t armedAt = 0;
        armedAt = r.host.msSinceRemoteInput();
        SetTimer(g.hwnd, 0x5244, 50, [](HWND h, UINT, UINT_PTR id, DWORD) {
            const int64_t ms = R().host.msSinceRemoteInput();
            if (ms < 0 || (armedAt >= 0 && ms >= armedAt)) return;  // no new phone click yet
            KillTimer(h, id);
            g.log->write("info", "dev remote click: menu toggle " + std::to_string(ms) + " ms after the phone's click");
            remoteCommand(CmdRemoteToggle);
        });
        return;
#endif
    }
    g.log->write("info", std::string("dev remote click done: remote ") + (r.on ? "on" : "off") + (r.active ? ", phone connected" : ""));
}

void remoteDevFixed(int how) {
    RemoteState& r = R();
    g.log->write("info", "dev remote fixed: " + std::to_string(how));
    if (how == 1) {
        openPasswordPanel(true);
    } else if (how == 2 || how == 3 || how == 7) {
        if (!r.pw.isOpen()) openPasswordPanel(true);
        r.pw.testFill(how == 7 ? L"short" : L"Zizai-Test-2026", how == 3 ? L"Zizai-Test-2027" : how == 7 ? L"short" : L"Zizai-Test-2026", true);
    } else if (how == 4) {
        setPersistent(false);
    } else if (how == 5) {
        const fs::path d = fs::temp_directory_path() / L"pmshots";
        std::error_code ec;
        fs::create_directories(d, ec);
        bool p = false, q = false, n = false;
        if (r.panel.isOpen()) {
            const int tab = r.panel.tab();
            r.panel.showTab(3);
            p = r.panel.renderPng((d / L"remote_fixed.png").wstring());
            r.panel.showTab(tab);
        }
        if (r.pw.isOpen()) q = r.pw.renderPng((d / L"remote_password.png").wstring());
        if (r.news.isOpen()) n = r.news.renderPng((d / L"whats_new.png").wstring());
        g.log->write("info", std::string("dev remote fixed shots: fixed tab ") + (p ? "ok" : "-") + ", password " + (q ? "ok" : "-") +
                                 ", what's new " + (n ? "ok" : "-"));
        return;
    } else if (how == 6) {
        showWhatsNew();
    } else if (how == 8) {  // 進階設定 as remote_advanced.png (no real menu window)
        const fs::path d = fs::temp_directory_path() / L"pmshots";
        std::error_code ec;
        fs::create_directories(d, ec);
        const bool ok = pm::ui::renderMenuPng(remoteAdvancedItems(), (d / L"remote_advanced.png").wstring());
        g.log->write("info", std::string("dev remote advanced shot: ") + (ok ? "ok" : "-"));
        // 0.8.3.4: and its 聲音 ▸ as remote_advanced_sound.png
        bool snd = false;
        for (const auto& m : remoteAdvancedItems())
            if (m.kind == MenuItem::Kind::Submenu && m.text == tr(S::MenuRemoteSound))
                snd = pm::ui::renderMenuPng(m.sub, (d / L"remote_advanced_sound.png").wstring());
        g.log->write("info", std::string("dev remote advanced sound shot: ") + (snd ? "ok" : "-"));
        return;
    } else if (how == 9) {
        r.news.close(1);
    } else if (how == 10) {  // the 「正在被遠端操控」 bar for a made-up phone on the fixed link (published shots)
        const fs::path d = fs::temp_directory_path() / L"pmshots";
        std::error_code ec;
        fs::create_directories(d, ec);
        pm::ui::RemoteBanner probe;
        probe.testDevice(std::wstring(L"Windows PC · Edge　203.0.113.24 · ") + tr(S::RemoteBannerFixed), GetDpiForWindow(g.hwnd));
        const bool ok = probe.renderPng((d / L"remote_banner.png").wstring());
        g.log->write("info", std::string("dev remote banner shot (example address): ") + (ok ? "ok" : "-"));
        return;
    }
    g.log->write("info", std::string("dev remote fixed done: state ") + std::to_string(r.fixState) + ", persistent " +
                             (g.settings.remotePersistent ? "1" : "0") + ", password " + (g.settings.remotePwHash.empty() ? "none" : "set"));
}

void remoteDevAnswer(int how) {
    RemoteState& r = R();
    if (!r.ask.isOpen()) {
        g.log->write("info", "dev remote answer: no dialog");
        return;
    }
    if (how == 3) r.ask.click(3), how = 1;  // tick 記住這支手機, then 允許
    g.log->write("info", std::string("dev remote answer: ") + (how == 1 ? "allow" : "deny"));
    r.ask.close(how == 1 ? 1 : 2);
}

// --dev DevCommand 965 (0.8.3): 1 = the 連線紀錄 as shown, into the log (no window).
void remoteDevLog(int how) {
    if (how == 2) {  // 中斷 on the PC (as the bar's / Ctrl+D)
        g.log->write("info", "dev remote: 中斷 from the PC");
        kick();
        return;
    }
    if (how != 1) return;
    std::string text = toUtf8(connLogText());
    for (char& c : text)
        if (c == '\n') c = '|';
    g.log->write("info", "dev remote connection log: " + text);
}

// 0.8.3.4 --dev DevCommand 988: a phone's 電腦喇叭靜音 switch, posted like the host's onMutePcRequest.
void remoteDevMutePc(int on) {
    auto* m = new RemoteMsg{R().gen, "", "", "", "--dev phone", on ? "1" : "0"};
    g.log->write("info", std::string("dev remote: 電腦喇叭靜音 ") + (on ? "on" : "off") + " as if from the phone");
    if (!PostMessageW(g.hwnd, WM_PM_REMOTE, RemMutePc, reinterpret_cast<LPARAM>(m))) delete m;
}

// 0.8.3.2 --dev hooks for the 臨時帳號 tests and screenshots (see remote.h). Never a password in the log.
void remoteDevGuest(int which, LPARAM lp) {
    RemoteState& r = R();
    const fs::path d = fs::temp_directory_path() / L"pmshots";
    std::error_code ec;
    if (which == 981 && r.guestPanel.isOpen()) {
        wchar_t nm[64] = {};
        const DWORD n = GetEnvironmentVariableW(L"PM_TEST_GUEST_NAME", nm, 64);
        r.guestPanel.testSet(n > 0 && n < 64 ? std::wstring(nm) : std::wstring(), static_cast<int>(lp & 3), (lp & 4) != 0, (lp & 8) != 0, (lp & 16) != 0);
        g.log->write("info", std::string("dev guest form: set") + ((lp & 16) ? " + 建立" : ""));
    } else if (which == 982) {
        fs::create_directories(d, ec);
        const UINT dpi = static_cast<UINT>(lp);
        const fs::path f = d / (dpi ? L"guest_panel_" + std::to_wstring(dpi) + L".png" : std::wstring(L"guest_panel.png"));
        g.log->write("info", std::string("dev guest panel shot: ") + (r.guestPanel.isOpen() && r.guestPanel.renderPng(f.wstring(), dpi) ? "ok" : "-"));
    } else if (which == 983 && r.guestPanel.isOpen()) {
        r.guestPanel.testCopy();
    } else if (which == 984) {
        r.guestPanel.close();
        remoteChanged();
    } else if (which == 985 && g.hwnd) {
        // As if the window moved to a monitor at this DPI: the same DIPs, or a client lp >> 16 px high
        // (a 1080p work area at 125 / 150 %), same shape.
        static UINT simDpi = 0;
        const UINT cur = simDpi ? simDpi : GetDpiForWindow(g.hwnd), dpi = static_cast<UINT>(lp & 0xFFFF), hPx = static_cast<UINT>((lp >> 16) & 0xFFFF);
        if (dpi < 96 || dpi > 288) return;
        RECT cr{}, wr{};
        GetClientRect(g.hwnd, &cr);
        GetWindowRect(g.hwnd, &wr);
        double cw = cr.right * double(dpi) / cur, ch = cr.bottom * double(dpi) / cur;
        if (hPx > 0 && ch > hPx) cw = cw * hPx / ch, ch = hPx;
        RECT nr{0, 0, static_cast<LONG>(std::lround(cw)), static_cast<LONG>(std::lround(ch))};
        AdjustWindowRectExForDpi(&nr, static_cast<DWORD>(GetWindowLongW(g.hwnd, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongW(g.hwnd, GWL_EXSTYLE)), dpi);
        RECT sug{wr.left, wr.top, wr.left + (nr.right - nr.left), wr.top + (nr.bottom - nr.top)};
        simDpi = dpi;
        SendMessageW(g.hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&sug));
        GetClientRect(g.hwnd, &cr);
        g.log->write("info", "dev dpi: " + std::to_string(dpi) + ", client " + std::to_string(cr.right) + "x" + std::to_string(cr.bottom) + " px (" +
                                 std::to_string(cr.right * 96 / dpi) + "x" + std::to_string(cr.bottom * 96 / dpi) + " DIPs)");
    } else if (which == 986) {
        std::string s;
        for (const GuestRow& gr : guestRows())
            s += (s.empty() ? "" : " | ") + gr.name + " " + toUtf8(guestSummary(gr)) + (gr.lastUsed ? " used" : " unused");
        g.log->write("info", "dev guests: " + std::to_string(guestRows().size()) + (s.empty() ? std::string() : ": " + s));
    }
}

}  // namespace pm_app
