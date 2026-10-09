// Online translation engine (opt-in, bring your own key): the ITranslator
// behind pm_create_online_translator() (pm/translator.h, escalation step 4),
// the picture-level prefetch() (Mode::All, and batching the escalation of a
// picture), plus the settings / key functions of pm/online_translate.h.
//
// - Only TrRequest::text (+ context when DeepL is used) leaves the PC; never
//   a picture.  installed() is false unless the user turned it on, accepted
//   the consent dialog for that provider and gave a key, so the escalator
//   never sends anything by itself.
// - Placeholders (ZQA..ZQZ), TrRequest::keep strings and glossary terms are
//   sent inside markup the provider does not translate (DeepL tag_handling
//   xml + ignore_tags, Azure textType html + class="notranslate") and put
//   back unchanged; TrHypothesis::flags gets "placeholder" when one was lost.
// - Results are cached in memory only (4,096 entries, cleared with the key).
//   A call whose texts are all cached sends nothing and decrypts no key.
// - Settings are cached (Snapshot below): the file is re-read only when its
//   size / time stamp changes; each stored key is DPAPI-decrypted once per
//   re-read to know it is usable, and again only for a request that sends.
// - Timeouts: resolve / connect 5 s, send 10 s, answer 10 s; network
//   errors, 429 and 5xx are retried twice (a timeout once) (0.5 s, 1 s, or the
//   server's Retry-After up to 5 s); key / quota errors are not.
// - Back-off (circuit breaker), per provider: see pm/online_translate.h.
// - The key is wiped after use; it is never part of an error text (scrub()).
#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

#include "online_engine.h"

#pragma comment(lib, "crypt32.lib")

namespace pm::translate::online {

namespace {

std::atomic<int64_t> g_clockOffsetMs{0};

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() +
           g_clockOffsetMs.load();
}

int pi(Provider p) { return static_cast<int>(p); }

// ---- Settings file ----

std::wstring configPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"PM_ONLINE_CONFIG", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    if (dir.empty()) return {};
    return dir + L"\\PhoneMirror\\online_translate.ini";
}

using Kv = std::map<std::string, std::string>;

Kv readKv(const std::wstring& path) {
    Kv kv;
    if (path.empty()) return kv;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

bool writeKv(const std::wstring& path, const Kv& kv, StoreError* err) {
    if (path.empty()) {
        if (err) *err = StoreError::NoSettingsFolder;
        return false;
    }
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) SHCreateDirectoryExW(nullptr, path.substr(0, slash).c_str(), nullptr);
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (err) *err = StoreError::WriteFailed;
            return false;
        }
        f << "# 自在投影 Zizai Cast - online translation (opt-in). Keys are DPAPI-encrypted for this Windows user.\n"
             "[online]\n";
        for (const auto& [k, v] : kv) f << k << '=' << v << '\n';
        if (!f) {
            if (err) *err = StoreError::WriteFailed;
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        if (err) *err = StoreError::WriteFailed;
        return false;
    }
    if (err) *err = StoreError::None;
    return true;
}

// Azure region as stored: trimmed, lower case ("EastAsia " -> "eastasia").
std::wstring normRegion(const std::wstring& r) {
    size_t a = r.find_first_not_of(L" \t\r\n"), b = r.find_last_not_of(L" \t\r\n");
    std::wstring t = a == std::wstring::npos ? std::wstring() : r.substr(a, b - a + 1);
    for (auto& ch : t) ch = static_cast<wchar_t>(towlower(ch));
    return t;
}

const char* modeId(Mode m) { return m == Mode::All ? "all" : m == Mode::Escalate ? "escalate" : "off"; }
std::string keyName(Provider p) { return std::string("key_") + providerId(p); }
std::string consentName(Provider p) { return std::string("consent_") + providerId(p); }

Settings fromKv(const Kv& kv) {
    Settings s;
    auto get = [&](const std::string& k) {
        auto it = kv.find(k);
        return it == kv.end() ? std::string() : it->second;
    };
    std::string m = get("mode"), p = get("provider");
    s.mode = m == "all" ? Mode::All : m == "escalate" ? Mode::Escalate : Mode::Off;
    s.provider = p == "deepl" ? Provider::DeepL : p == "azure" ? Provider::Azure : Provider::None;
    for (Provider q : {Provider::DeepL, Provider::Azure})
        if (kv.count(consentName(q))) s.setConsent(q, atoi(get(consentName(q)).c_str()));
    // Older files: one "consent" value, given for the provider saved with it.
    if (kv.count("consent") && !kv.count(consentName(s.provider))) s.setConsent(s.provider, atoi(get("consent").c_str()));
    s.azureRegion = normRegion(fromUtf8(get("azure_region")));
    return s;
}

// ---- DPAPI ----

const char kEntropy[] = "ZizaiCast online translation key v1";

std::string protect(const std::wstring& key) {
    std::string plain = toUtf8(key);
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(plain.data())};
    DATA_BLOB ent{static_cast<DWORD>(sizeof kEntropy - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    std::string b64;
    if (CryptProtectData(&in, L"Zizai Cast online translation", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        DWORD n = 0;
        CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
        b64.resize(n);
        if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &n))
            b64.resize(n);
        else
            b64.clear();
        LocalFree(out.pbData);
    }
    SecureZeroMemory(plain.data(), plain.size());
    return b64;
}

std::wstring unprotect(const std::string& b64) {
    if (b64.empty()) return {};
    DWORD n = 0;
    if (!CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr))
        return {};
    std::string blob(n, '\0');
    if (!CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(blob.data()), &n, nullptr, nullptr))
        return {};
    DATA_BLOB in{n, reinterpret_cast<BYTE*>(blob.data())};
    DATA_BLOB ent{static_cast<DWORD>(sizeof kEntropy - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    std::wstring key;
    if (CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
        key = fromUtf8(plain);
        SecureZeroMemory(plain.data(), plain.size());
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
    return key;
}

struct WipedKey {
    std::wstring k;
    ~WipedKey() { SecureZeroMemory(k.data(), k.size() * sizeof(wchar_t)); }
};

// ---- Settings cache ----
// The parsed file, valid while the file's path / size / write time are the
// same.  keyOk: the stored key decrypts (checked once per re-read).  stamp:
// changes whenever a key blob or the Azure region changes (ends a BadKey
// back-off).
struct Snapshot {
    bool valid = false;
    std::wstring path;
    bool exists = false;
    uint64_t size = 0;
    FILETIME mtime{};
    Kv kv;
    Settings s;
    bool keyOk[3] = {false, false, false};
    uint64_t stamp[3] = {0, 0, 0};
};

std::mutex g_fileMutex;  // the file and g_snap
Snapshot g_snap;

uint64_t fnv(const std::string& a, const std::wstring& b) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : a) h = (h ^ c) * 1099511628211ull;
    h = (h ^ 0xff) * 1099511628211ull;
    for (wchar_t c : b) h = (h ^ static_cast<uint64_t>(c)) * 1099511628211ull;
    return h | 1;  // never 0
}

// Caller holds g_fileMutex.  The file is looked at (path, size, time stamp)
// at most once a second; this library's own writes invalidate at once, an
// edit by another process is seen within a second.
const Snapshot& snapLocked() {
    static ULONGLONG lastCheck = 0;
    const ULONGLONG tick = GetTickCount64();
    if (g_snap.valid && tick - lastCheck < 1000) return g_snap;
    lastCheck = tick;
    std::wstring path = configPath();
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    bool exists = !path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa);
    uint64_t size = exists ? (static_cast<uint64_t>(fa.nFileSizeHigh) << 32 | fa.nFileSizeLow) : 0;
    if (g_snap.valid && g_snap.path == path && g_snap.exists == exists &&
        (!exists || (g_snap.size == size && CompareFileTime(&g_snap.mtime, &fa.ftLastWriteTime) == 0)))
        return g_snap;
    Snapshot n;
    n.valid = true;
    n.path = path;
    n.exists = exists;
    n.size = size;
    if (exists) n.mtime = fa.ftLastWriteTime;
    n.kv = exists ? readKv(path) : Kv{};
    n.s = fromKv(n.kv);
    for (Provider p : {Provider::DeepL, Provider::Azure}) {
        auto it = n.kv.find(keyName(p));
        std::string blob = it == n.kv.end() ? std::string() : it->second;
        if (!blob.empty()) {
            // Same blob as before: no need to decrypt again.
            auto old = g_snap.kv.find(keyName(p));
            // (also after invalidateLocked(): the old kv is still there)
            if (g_snap.path == path && old != g_snap.kv.end() && old->second == blob) {
                n.keyOk[pi(p)] = g_snap.keyOk[pi(p)];
            } else {
                WipedKey k{unprotect(blob)};
                n.keyOk[pi(p)] = !k.k.empty();
            }
        }
        n.stamp[pi(p)] = fnv(blob, p == Provider::Azure ? n.s.azureRegion : std::wstring());
    }
    g_snap = std::move(n);
    return g_snap;
}

void invalidateLocked() { g_snap.valid = false; }

// ---- Memory cache ----

class Cache {
public:
    bool get(const std::wstring& k, std::wstring& v) {
        std::lock_guard lk(m_);
        auto it = map_.find(k);
        if (it == map_.end()) return false;
        v = it->second;
        return true;
    }
    void put(const std::wstring& k, const std::wstring& v) {
        std::lock_guard lk(m_);
        if (map_.emplace(k, v).second) {
            order_.push_back(k);
            while (order_.size() > kMax) {
                map_.erase(order_.front());
                order_.pop_front();
            }
        }
    }
    void clear() {
        std::lock_guard lk(m_);
        map_.clear();
        order_.clear();
    }

private:
    static constexpr size_t kMax = 4096;
    std::mutex m_;
    std::unordered_map<std::wstring, std::wstring> map_;
    std::deque<std::wstring> order_;
};

Cache& cache() {
    static Cache c;
    return c;
}

// ---- Last error / back-off ----

std::mutex g_stateMutex;  // g_last, g_block
LastError g_last;
struct Block {
    Status status = Status::Ok;
    int64_t until = 0;      // nowMs(); LLONG_MAX: until the stamp changes
    uint64_t stamp = 0;     // Snapshot::stamp when it was set
};
Block g_block[3];

void setLast(Status s, Provider p, const std::wstring& detail) {
    std::lock_guard lk(g_stateMutex);
    g_last = {s, p, detail};
}

int64_t backoffMs(Status s, unsigned retryAfterSec) {
    switch (s) {
    case Status::BadKey: return LLONG_MAX;
    case Status::QuotaExceeded: return 3600 * 1000;
    case Status::Network:
    case Status::Timeout: return 60 * 1000;
    case Status::RateLimited: return (std::max<int64_t>)(30 * 1000, static_cast<int64_t>(retryAfterSec) * 1000);
    case Status::ServerError: return 30 * 1000;
    default: return 0;  // Ok, NotConfigured, Unsupported, BadResponse: per request, not latched
    }
}

void setBlock(Provider p, Status s, uint64_t stamp, unsigned retryAfterSec = 0) {
    if (p == Provider::None) return;
    int64_t d = backoffMs(s, retryAfterSec);
    if (d == 0) return;
    std::lock_guard lk(g_stateMutex);
    g_block[pi(p)] = {s, d == LLONG_MAX ? LLONG_MAX : nowMs() + d, stamp};
}

void clearBlock(Provider p) {
    std::lock_guard lk(g_stateMutex);
    if (p == Provider::None)
        for (auto& b : g_block) b = {};
    else
        g_block[pi(p)] = {};
}

// Caller passes the current stamp of the provider.
Status blockedNow(Provider p, uint64_t stamp, int64_t* remaining) {
    if (remaining) *remaining = 0;
    if (p == Provider::None) return Status::Ok;
    std::lock_guard lk(g_stateMutex);
    Block& b = g_block[pi(p)];
    if (b.status == Status::Ok) return Status::Ok;
    // A key / region change ends a BadKey or quota back-off.
    if ((b.status == Status::BadKey || b.status == Status::QuotaExceeded) && b.stamp != stamp) {
        b = {};
        return Status::Ok;
    }
    if (b.until != LLONG_MAX && b.until <= nowMs()) {
        b = {};
        return Status::Ok;
    }
    if (remaining) *remaining = b.until == LLONG_MAX ? -1 : b.until - nowMs();
    return b.status;
}

bool fatal(Status s) {
    // Stop sending the rest of the call: it would fail the same way (and
    // each network failure already waited through its retries).
    return s == Status::BadKey || s == Status::QuotaExceeded || s == Status::Timeout || s == Status::Network ||
           s == Status::RateLimited || s == Status::ServerError || s == Status::NotConfigured;
}

// What a call needs from the settings, read once.
struct Config {
    Settings s;
    bool keyOk = false;
    uint64_t stamp = 0;
};
Config config() {
    std::lock_guard lk(g_fileMutex);
    const Snapshot& n = snapLocked();
    Config c;
    c.s = n.s;
    if (n.s.provider != Provider::None) {
        c.keyOk = n.keyOk[pi(n.s.provider)];
        c.stamp = n.stamp[pi(n.s.provider)];
    }
    return c;
}

uint64_t stampOf(Provider p) {
    if (p == Provider::None) return 0;
    std::lock_guard lk(g_fileMutex);
    return snapLocked().stamp[pi(p)];
}

// ---- Engine ----

std::wstring wid(const char* s) { return std::wstring(s, s + strlen(s)); }

// The translation of a list of requests (ITranslator::translate, prefetch).
bool translateAll(const std::vector<TrRequest>& in, std::vector<TrHypothesis>& out, std::wstring* err) {
    auto t0 = std::chrono::steady_clock::now();
    out.assign(in.size(), {});
    const Config c = config();
    const Provider prov = c.s.provider;
    auto ad = makeAdapter(prov);
    auto fail = [&](Status st, const wchar_t* why) {
        setLast(st, prov, why);
        if (err) *err = std::wstring(L"online: ") + fromUtf8(statusId(st)) + L" - " + why;
        return false;
    };
    if (!c.s.enabled() || !ad || !c.keyOk) return fail(Status::NotConfigured, L"online translation is off");
    if (Status b = blockedNow(prov, c.stamp, nullptr); b != Status::Ok) return fail(b, L"backed off after an earlier failure");
    for (auto& h : out) h.engine = ad->engineId();
    const bool deepl = prov == Provider::DeepL;

    // Marked text and cache lookups; misses grouped by language pair.
    struct Item {
        Marked mk;
        std::wstring cacheKey, ctx;
    };
    std::vector<Item> items(in.size());
    std::map<std::pair<int, int>, std::vector<size_t>> groups;
    size_t wanted = 0, done = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const TrRequest& r = in[i];
        if (r.text.find_first_not_of(L" \t\r\n") == std::wstring::npos || !ad->supports(r.src, r.tgt)) continue;
        ++wanted;
        items[i].mk = markUp(r, ad->markup());
        items[i].ctx = deepl ? r.context : std::wstring();
        items[i].cacheKey = wid(ad->engineId()) + L'\x1f' + static_cast<wchar_t>(L'0' + static_cast<int>(r.src)) +
                            static_cast<wchar_t>(L'0' + static_cast<int>(r.tgt)) + L'\x1f' + items[i].ctx + L'\x1f' +
                            items[i].mk.text;
        std::wstring hit;
        if (cache().get(items[i].cacheKey, hit)) {
            int lost = 0;
            out[i].text = unmark(hit, items[i].mk, ad->markup(), &lost);
            if (lost) out[i].flags.push_back("placeholder");
            ++done;
            continue;
        }
        groups[{static_cast<int>(r.src), static_cast<int>(r.tgt)}].push_back(i);
    }

    Status worst = Status::Ok;
    std::wstring detail;
    if (!groups.empty()) {
        WipedKey key{loadKey(prov)};
        if (key.k.empty()) return fail(Status::NotConfigured, L"the stored key cannot be read");
        // DeepL's context is per request: a batch sends the distinct contexts
        // of its texts together (capped), so a picture is not split into one
        // request per segment.
        constexpr size_t kMaxContext = 4000;
        for (auto& [g, idx] : groups) {
            if (fatal(worst)) break;
            // Identical texts (same context) are sent once.
            std::vector<size_t> uniq;
            std::unordered_map<std::wstring, size_t> seen;
            std::vector<std::vector<size_t>> users;
            for (size_t i : idx) {
                auto [it, fresh] = seen.emplace(items[i].ctx + L'\x1f' + items[i].mk.text, uniq.size());
                if (fresh) {
                    uniq.push_back(i);
                    users.emplace_back();
                }
                users[it->second].push_back(i);
            }
            for (size_t at = 0; at < uniq.size() && !fatal(worst);) {
                Batch b;
                b.src = static_cast<Lang>(g.first);
                b.tgt = static_cast<Lang>(g.second);
                std::set<std::wstring> ctxSeen;
                size_t chars = 0, from = at;
                while (at < uniq.size() && b.texts.size() < ad->maxTexts()) {
                    const Item& it = items[uniq[at]];
                    const bool newCtx = !it.ctx.empty() && !ctxSeen.count(it.ctx);
                    const size_t addCtx = newCtx ? it.ctx.size() + 1 : 0;
                    if (!b.texts.empty() && (chars + b.context.size() + addCtx + it.mk.text.size() > ad->maxChars() ||
                                             b.context.size() + addCtx > kMaxContext))
                        break;
                    if (newCtx) {
                        ctxSeen.insert(it.ctx);
                        b.context += (b.context.empty() ? L"" : L"\n") + it.ctx;
                    }
                    chars += it.mk.text.size();
                    b.texts.push_back(it.mk);
                    ++at;
                }
                if (b.context.size() > kMaxContext) b.context.resize(kMaxContext);
                HttpRequest rq = ad->translateRequest(b, key.k, c.s.azureRegion);
                Status st;
                std::wstring d;
                HttpResponse rs = sendWithRetry(rq, *ad, &st, &d);
                for (auto& [hk, hv] : rq.headers) SecureZeroMemory(hv.data(), hv.size() * sizeof(wchar_t));
                std::vector<std::wstring> answers;
                if (st == Status::Ok) st = ad->parseTranslate(rs, b.texts.size(), answers, &d);
                if (st != Status::Ok) {
                    if (worst == Status::Ok || fatal(st)) {
                        worst = st;
                        detail = scrub(d, key.k);
                    }
                    setBlock(prov, st, c.stamp, rs.retryAfterSec);
                    continue;
                }
                for (size_t k = 0; k < answers.size(); ++k) {
                    size_t u = from + k;
                    if (answers[k].empty()) continue;
                    cache().put(items[uniq[u]].cacheKey, answers[k]);
                    for (size_t i : users[u]) {
                        int lost = 0;
                        out[i].text = unmark(answers[k], items[i].mk, ad->markup(), &lost);
                        if (lost) out[i].flags.push_back("placeholder");
                        ++done;
                    }
                }
            }
        }
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for (auto& h : out) h.ms = in.empty() ? 0 : ms / static_cast<double>(in.size());
    setLast(worst, prov, detail);
    // Technical text (logs / 「詳細資料」); the UI uses lastStatus() / lastError().
    if (worst != Status::Ok && err) *err = std::wstring(L"online (") + fromUtf8(providerId(prov)) + L"): " +
                                           fromUtf8(statusId(worst)) + (detail.empty() ? L"" : L" - " + detail);
    return done > 0 || wanted == 0;
}

class OnlineTranslator final : public ITranslator {
public:
    const char* id() const override {
        auto ad = makeAdapter(config().s.provider);
        return ad ? ad->engineId() : "online";
    }
    bool supports(Lang src, Lang tgt, bool* direct) const override {
        if (direct) *direct = true;  // the providers translate every pair directly (no English pivot)
        if (auto ad = makeAdapter(config().s.provider)) return ad->supports(src, tgt);
        return makeAdapter(Provider::DeepL)->supports(src, tgt) || makeAdapter(Provider::Azure)->supports(src, tgt);
    }
    bool installed(Lang src, Lang tgt) const override {
        const Config c = config();
        if (!c.s.enabled() || !c.keyOk) return false;
        if (blockedNow(c.s.provider, c.stamp, nullptr) != Status::Ok) return false;
        auto ad = makeAdapter(c.s.provider);
        return ad && ad->supports(src, tgt);
    }
    bool isOnline() const override { return true; }
    TrCost cost() const override { return {2.0, 400, 1}; }  // network: ~0.3-1 s per request
    bool translate(const std::vector<TrRequest>& in, std::vector<TrHypothesis>& out, std::wstring* err) override {
        return translateAll(in, out, err);
    }
};

TestResult runStoredTest(Provider p, const std::wstring& region) {
    auto ad = makeAdapter(p);
    if (!ad) return {};
    WipedKey key{loadKey(p)};
    if (key.k.empty()) return {};
    TestResult r = ad->testKey(key.k, region);
    r.detail = scrub(r.detail, key.k);
    // The stored key's back-off follows what the test found (only when the
    // test used the saved region: another region says nothing about it).
    if (p != Provider::Azure || normRegion(region) == loadSettings().azureRegion) {
        const uint64_t stamp = stampOf(p);
        if (r.status == Status::Ok) clearBlock(p);
        else if (r.status == Status::BadKey || r.status == Status::QuotaExceeded) setBlock(p, r.status, stamp);
    }
    return r;
}

}  // namespace

void setClockOffsetMsForTest(int64_t ms) { g_clockOffsetMs = ms; }

// ---- pm/online_translate.h ----

Settings loadSettings() {
    std::lock_guard lk(g_fileMutex);
    return snapLocked().s;
}

bool saveSettings(const Settings& s, StoreError* err) {
    std::lock_guard lk(g_fileMutex);
    const std::wstring path = configPath();
    Kv kv = snapLocked().kv;
    Settings before = fromKv(kv);
    kv["mode"] = modeId(s.mode);
    kv["provider"] = providerId(s.provider);
    kv.erase("consent");  // replaced by one value per provider
    kv[consentName(Provider::DeepL)] = std::to_string(s.consentDeepL);
    kv[consentName(Provider::Azure)] = std::to_string(s.consentAzure);
    const std::wstring region = normRegion(s.azureRegion);
    kv["azure_region"] = toUtf8(region);
    if (before.provider != s.provider || s.mode == Mode::Off) clearCache();
    if (before.provider != s.provider || before.azureRegion != region) clearBlock(Provider::None);
    bool ok = writeKv(path, kv, err);
    invalidateLocked();
    return ok;
}

bool setKey(Provider p, const std::wstring& key, StoreError* err) {
    if (p == Provider::None) {
        if (err) *err = StoreError::InvalidProvider;
        return false;
    }
    std::lock_guard lk(g_fileMutex);
    const std::wstring path = configPath();
    Kv kv = snapLocked().kv;
    std::wstring k = key;
    size_t a = k.find_first_not_of(L" \t\r\n"), b = k.find_last_not_of(L" \t\r\n");
    std::wstring t = a == std::wstring::npos ? std::wstring() : k.substr(a, b - a + 1);
    SecureZeroMemory(k.data(), k.size() * sizeof(wchar_t));
    if (t.empty()) {
        kv.erase(keyName(p));
    } else {
        std::string blob = protect(t);
        SecureZeroMemory(t.data(), t.size() * sizeof(wchar_t));
        if (blob.empty()) {
            if (err) *err = StoreError::EncryptFailed;
            return false;
        }
        kv[keyName(p)] = blob;
    }
    clearBlock(p);
    clearCache();
    bool ok = writeKv(path, kv, err);
    invalidateLocked();
    return ok;
}

std::wstring loadKey(Provider p) {
    std::string blob;
    {
        std::lock_guard lk(g_fileMutex);
        const Snapshot& n = snapLocked();
        auto it = n.kv.find(keyName(p));
        if (it != n.kv.end()) blob = it->second;
    }
    return unprotect(blob);
}

bool hasKey(Provider p) {
    if (p == Provider::None) return false;
    std::lock_guard lk(g_fileMutex);
    return snapLocked().keyOk[pi(p)];
}

std::wstring keyHint(Provider p) {
    WipedKey k{loadKey(p)};
    if (k.k.empty()) return {};
    std::wstring core = k.k;
    std::wstring suffix;
    if (core.size() > 3 && core.compare(core.size() - 3, 3, L":fx") == 0) {
        suffix = L":fx";
        core.resize(core.size() - 3);
    }
    std::wstring hint = L"••••" + (core.size() > 8 ? core.substr(core.size() - 4) : std::wstring()) + suffix;
    SecureZeroMemory(core.data(), core.size() * sizeof(wchar_t));
    return hint;
}

void forgetAll() {
    {
        std::lock_guard lk(g_fileMutex);
        Kv kv;
        kv["mode"] = "off";
        kv["provider"] = "none";
        kv[consentName(Provider::DeepL)] = "0";
        kv[consentName(Provider::Azure)] = "0";
        writeKv(configPath(), kv, nullptr);
        invalidateLocked();
    }
    clearBlock(Provider::None);
    setLast(Status::Ok, Provider::None, {});
    clearCache();
}

const char* storeErrorId(StoreError e) {
    switch (e) {
    case StoreError::None: return "ok";
    case StoreError::NoSettingsFolder: return "no-folder";
    case StoreError::WriteFailed: return "write-failed";
    case StoreError::EncryptFailed: return "encrypt-failed";
    case StoreError::InvalidProvider: return "invalid-provider";
    }
    return "?";
}

const char* statusId(Status s) {
    switch (s) {
    case Status::Ok: return "ok";
    case Status::NotConfigured: return "not-configured";
    case Status::BadKey: return "bad-key";
    case Status::QuotaExceeded: return "quota";
    case Status::RateLimited: return "rate-limited";
    case Status::Timeout: return "timeout";
    case Status::Network: return "network";
    case Status::ServerError: return "server-error";
    case Status::Unsupported: return "unsupported";
    case Status::BadResponse: return "bad-response";
    }
    return "?";
}

const char* planId(Plan p) {
    switch (p) {
    case Plan::Unknown: return "unknown";
    case Plan::DeepLFree: return "deepl-free";
    case Plan::DeepLPro: return "deepl-pro";
    case Plan::Azure: return "azure";
    }
    return "?";
}

TestResult testKey(Provider p, const std::wstring& key, const std::wstring& region) {
    auto ad = makeAdapter(p);
    if (!ad) return {};
    TestResult r = ad->testKey(key, region);
    r.detail = scrub(r.detail, key);
    return r;
}

TestResult testStoredKey(Provider p) { return runStoredTest(p, loadSettings().azureRegion); }
TestResult testStoredKey(Provider p, const std::wstring& region) { return runStoredTest(p, region); }

Status lastStatus() {
    std::lock_guard lk(g_stateMutex);
    return g_last.status;
}
LastError lastError() {
    std::lock_guard lk(g_stateMutex);
    return g_last;
}
void setLastStatus(Status s) { setLast(s, Provider::None, {}); }

Status blockedStatus(Provider p, int64_t* remainingMs) { return blockedNow(p, stampOf(p), remainingMs); }
void resetBackoff() { clearBlock(Provider::None); }

Mode activeMode() {
    const Config c = config();
    if (!c.s.enabled() || !c.keyOk) return Mode::Off;
    if (blockedNow(c.s.provider, c.stamp, nullptr) != Status::Ok) return Mode::Off;
    return c.s.mode;
}

Status prefetch(const std::vector<TrRequest>& in, std::vector<TrHypothesis>* out) {
    std::vector<TrHypothesis> tmp;
    std::vector<TrHypothesis>& o = out ? *out : tmp;
    const Config c = config();
    if (!c.s.enabled() || !c.keyOk) {
        o.assign(in.size(), {});
        setLast(Status::NotConfigured, c.s.provider, {});
        return Status::NotConfigured;
    }
    // Backed off: nothing sent, the cause (and lastStatus()) kept.
    if (Status b = blockedNow(c.s.provider, c.stamp, nullptr); b != Status::Ok) {
        o.assign(in.size(), {});
        return b;
    }
    translateAll(in, o, nullptr);
    return lastStatus();
}

const char* providerId(Provider p) { return p == Provider::DeepL ? "deepl" : p == Provider::Azure ? "azure" : "none"; }

const wchar_t* providerName(Provider p) {
    switch (p) {
    case Provider::DeepL: return L"DeepL";
    case Provider::Azure: return L"Microsoft Azure Translator";
    default: return L"";
    }
}
const wchar_t* providerTermsUrl(Provider p) {
    switch (p) {
    case Provider::DeepL: return L"https://www.deepl.com/en/pro-license";
    case Provider::Azure: return L"https://www.microsoft.com/licensing/terms/productoffering/MicrosoftAzure/MCA";
    default: return L"";
    }
}
const wchar_t* providerPrivacyUrl(Provider p) {
    switch (p) {
    case Provider::DeepL: return L"https://www.deepl.com/privacy";
    // Learn: "Data, privacy, and security for Azure Translator in Foundry
    // Tools" (checked 2026-10-09; no locale in the path: Learn picks it).
    case Provider::Azure: return L"https://learn.microsoft.com/azure/foundry/responsible-ai/translator/data-privacy-security";
    default: return L"";
    }
}
const wchar_t* providerSignupUrl(Provider p) {
    switch (p) {
    case Provider::DeepL: return L"https://www.deepl.com/pro-api";
    case Provider::Azure: return L"https://portal.azure.com/#create/Microsoft.CognitiveServicesTextTranslation";
    default: return L"";
    }
}

void clearCache() { cache().clear(); }

}  // namespace pm::translate::online

// pm/translator.h factory: always an object (settings can be turned on while
// the app runs); installed() says whether it may be used now.
extern "C" pm::translate::ITranslator* pm_create_online_translator() {
    return new pm::translate::online::OnlineTranslator();
}
