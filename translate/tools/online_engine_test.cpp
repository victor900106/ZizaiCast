// pm_online_engine_test: tests of the online translation engine
// (translate/src/online_engine*.cpp) against a fake provider on 127.0.0.1
// (loopback only: no firewall prompt, nothing leaves the PC, no key needed).
//
//   pm_online_engine_test            all tests, exit code 0 = pass
//
// Checks: off by default and nothing sent; DPAPI key storage (no plain key
// in the file); DeepL / Azure request format (path, headers, JSON, tag
// markup); placeholders, keep strings and glossary terms kept; batching and
// de-duplication; memory cache; retries (503, 429 + Retry-After, timeout);
// no retry on 403 / 456 / 403001; quota latch; the key never in an error;
// testKey; lost placeholder flagged; plain HTTP refused off loopback;
// consent per provider (and the older one-value file); settings cache; one
// request per picture (prefetch, mixed contexts); back-off after bad key /
// network / 429; testStoredKey; forgetAll resets the status.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "online_engine.h"
#include "pm/online_translate.h"
#include "pm/translator.h"

#pragma comment(lib, "ws2_32.lib")

using namespace pm::translate;
using namespace pm::translate::online;

namespace {

int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (c) ++g_pass;                                                        \
        else {                                                                  \
            ++g_fail;                                                           \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);          \
        }                                                                       \
    } while (0)

std::string u8(const std::wstring& s) { return toUtf8(s); }

// ---- Fake provider ----
struct Req {
    std::string method, path, body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string header(const std::string& name) const {
        for (const auto& [k, v] : headers)
            if (_stricmp(k.c_str(), name.c_str()) == 0) return v;
        return {};
    }
};
struct Resp {
    int status = 200;
    std::string body;
    std::string extraHeaders;  // "Retry-After: 1\r\n"
    int delayMs = 0;
};

class FakeServer {
public:
    std::function<Resp(const Req&)> handler;
    std::vector<Req> log;
    std::mutex m;

    bool start() {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        ls_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only
        a.sin_port = 0;
        if (bind(ls_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(ls_, 8) != 0) return false;
        int len = sizeof a;
        getsockname(ls_, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        stop_ = true;
        closesocket(ls_);
        if (th_.joinable()) th_.join();
        WSACleanup();
    }
    size_t count() {
        std::lock_guard lk(m);
        return log.size();
    }
    Req last() {
        std::lock_guard lk(m);
        return log.empty() ? Req{} : log.back();
    }
    void reset() {
        std::lock_guard lk(m);
        log.clear();
    }
    uint16_t port = 0;

private:
    SOCKET ls_ = INVALID_SOCKET;
    std::thread th_;
    std::atomic<bool> stop_{false};

    void loop() {
        while (!stop_) {
            SOCKET c = accept(ls_, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            serve(c);
            closesocket(c);
        }
    }
    void serve(SOCKET c) {
        std::string data;
        char buf[8192];
        size_t headEnd = std::string::npos, need = 0;
        for (;;) {
            int n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) return;
            data.append(buf, static_cast<size_t>(n));
            if (headEnd == std::string::npos) {
                headEnd = data.find("\r\n\r\n");
                if (headEnd != std::string::npos) {
                    std::string h = data.substr(0, headEnd);
                    size_t p = 0;
                    std::string lower = h;
                    for (auto& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                    if ((p = lower.find("content-length:")) != std::string::npos) need = strtoul(h.c_str() + p + 15, nullptr, 10);
                }
            }
            if (headEnd != std::string::npos && data.size() >= headEnd + 4 + need) break;
        }
        Req r;
        std::istringstream hs(data.substr(0, headEnd));
        std::string line;
        std::getline(hs, line);
        {
            std::istringstream fl(line);
            fl >> r.method >> r.path;
        }
        while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string v = line.substr(colon + 1);
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            r.headers.push_back({line.substr(0, colon), v});
        }
        r.body = data.substr(headEnd + 4, need);
        {
            std::lock_guard lk(m);
            log.push_back(r);
        }
        Resp rs = handler ? handler(r) : Resp{};
        if (rs.delayMs) Sleep(rs.delayMs);
        std::string out = "HTTP/1.1 " + std::to_string(rs.status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
                          std::to_string(rs.body.size()) + "\r\nConnection: close\r\n" + rs.extraHeaders + "\r\n" + rs.body;
        send(c, out.data(), static_cast<int>(out.size()), 0);
        shutdown(c, SD_SEND);
    }
};

// Echo translators: the answer for each text is "譯[" + text + "]", the
// markup moved to the front (as a real engine reorders words).
std::wstring echo(const std::wstring& t, const wchar_t* open, const wchar_t* close) {
    std::wstring tags, rest = t;
    for (size_t p; (p = rest.find(open)) != std::wstring::npos;) {
        size_t e = rest.find(close, p);
        if (e == std::wstring::npos) break;
        e += wcslen(close);
        tags += rest.substr(p, e - p);
        rest.erase(p, e - p);
    }
    return tags + L"譯[" + rest + L"]";
}

Resp deeplEcho(const Req& r) {
    Json j;
    if (!parseJson(r.body, j) || !j.get(L"text")) return {400, "{\"message\":\"bad body\"}"};
    std::string o = "{\"translations\":[";
    for (size_t i = 0; i < j.get(L"text")->a.size(); ++i)
        o += std::string(i ? "," : "") + "{\"detected_source_language\":\"JA\",\"text\":" +
             jsonString(echo(j.get(L"text")->a[i].s, L"<k>", L"</k>")) + "}";
    return {200, o + "]}"};
}

Resp azureEcho(const Req& r) {
    Json j;
    if (!parseJson(r.body, j) || j.t != Json::T::Arr) return {400, "{\"error\":{\"code\":400000,\"message\":\"bad\"}}"};
    std::string o = "[";
    for (size_t i = 0; i < j.a.size(); ++i)
        o += std::string(i ? "," : "") + "{\"translations\":[{\"text\":" +
             jsonString(echo(j.a[i].get(L"Text")->s, L"<span class=\"notranslate\">", L"</span>")) + ",\"to\":\"zh-Hant\"}]}";
    return {200, o + "]"};
}

TrRequest req(const std::wstring& text, Lang src = Lang::Ja, Lang tgt = Lang::ZhHant) {
    TrRequest r;
    r.text = text;
    r.src = src;
    r.tgt = tgt;
    return r;
}

std::string fileText(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

void enable(Provider p, Mode m = Mode::Escalate, const std::wstring& region = L"") {
    Settings s;
    s.mode = m;
    s.provider = p;
    s.setConsent(p);
    s.azureRegion = region;
    saveSettings(s);
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    // Private settings file for the run (never the user's real one).
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring cfg = std::wstring(tmp) + L"pm_online_test_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    DeleteFileW(cfg.c_str());
    SetEnvironmentVariableW(L"PM_ONLINE_CONFIG", cfg.c_str());

    FakeServer srv;
    if (!srv.start()) {
        std::printf("cannot listen on 127.0.0.1\n");
        return 2;
    }
    setLoopbackPortForTest(srv.port);
    setRetryBaseMsForTest(20);
    std::printf("fake provider on 127.0.0.1:%u\n", srv.port);

    std::unique_ptr<ITranslator> eng(pm_create_online_translator());
    CHECK(eng != nullptr);
    CHECK(eng->isOnline());

    const std::wstring kDeepLKey = L"0123abcd-ffff-4444-9999-deadbeef5678:fx";
    const std::wstring kAzureKey = L"AZUREKEY0123456789abcdefSECRET";

    // 1. Markup unit checks.
    std::printf("[markup]\n");
    {
        TrRequest r = req(L"ZQAを直射日光&高温<多湿>を避けて1.5g保存");
        r.keep = {L"1.5g"};
        r.terms = {{L"直射日光", L"陽光直射"}};
        Marked mk = markUp(r, Markup::DeepLXml);
        CHECK(mk.text == L"<k>ZQA</k>を<k>陽光直射</k>&amp;高温&lt;多湿&gt;を避けて<k>1.5g</k>保存");
        CHECK(mk.pieces.size() == 3);
        int lost = -1;
        std::wstring back = unmark(L"<k>1.5g</k> 請避免 <k>陽光直射</k>&amp;高溫&lt;潮濕&gt; <k>ZQA</k>", mk, Markup::DeepLXml, &lost);
        CHECK(back == L"1.5g 請避免 陽光直射&高溫<潮濕> ZQA");
        CHECK(lost == 0);
        back = unmark(L"請避免 <k>ZQA</k>", mk, Markup::DeepLXml, &lost);
        CHECK(lost == 2);
        Marked mh = markUp(req(L"ZQBとZQC"), Markup::Html);
        CHECK(mh.text == L"<span class=\"notranslate\">ZQB</span>と<span class=\"notranslate\">ZQC</span>");
        back = unmark(L"<span class='notranslate'>ZQC</span>和<span class=\"notranslate\">ZQB</span>", mh, Markup::Html, &lost);
        CHECK(back == L"ZQC和ZQB" && lost == 0);
        Json j;
        CHECK(parseJson("{\"a\":[1,\"\\u7e41\\n\",true,null],\"b\":{\"c\":-2.5e1}}", j));
        CHECK(j.get(L"a")->at(1).s == L"繁\n" && j.get(L"b")->get(L"c")->n == -25);
        CHECK(!parseJson("{\"a\":", j));
        CHECK(jsonString(L"\"x\"\n\x01") == "\"\\\"x\\\"\\n\\u0001\"");
    }

    // 2. Off by default: nothing is sent.
    std::printf("[off by default]\n");
    {
        Settings s = loadSettings();
        CHECK(s.mode == Mode::Off && !s.enabled());
        CHECK(!eng->installed(Lang::Ja, Lang::ZhHant));
        std::vector<TrHypothesis> out;
        std::wstring err;
        CHECK(!eng->translate({req(L"テスト")}, out, &err));
        CHECK(out.size() == 1 && out[0].text.empty());
        CHECK(lastStatus() == Status::NotConfigured);
        // A key alone (no consent / mode) still sends nothing.
        setKey(Provider::DeepL, kDeepLKey);
        CHECK(!eng->installed(Lang::Ja, Lang::ZhHant));
        CHECK(!eng->translate({req(L"テスト")}, out, &err));
        Settings half;
        half.mode = Mode::Escalate;
        half.provider = Provider::DeepL;  // consent not given
        saveSettings(half);
        CHECK(!eng->installed(Lang::Ja, Lang::ZhHant));
        CHECK(!eng->translate({req(L"テスト")}, out, &err));
        CHECK(srv.count() == 0);
    }

    // 3. Key storage.
    std::printf("[key storage]\n");
    {
        std::string file = fileText(cfg);
        CHECK(file.find("key_deepl=") != std::string::npos);
        CHECK(file.find(u8(kDeepLKey)) == std::string::npos);
        CHECK(file.find("deadbeef") == std::string::npos);
        CHECK(hasKey(Provider::DeepL) && !hasKey(Provider::Azure));
        CHECK(loadKey(Provider::DeepL) == kDeepLKey);
        CHECK(keyHint(Provider::DeepL) == L"••••5678:fx");
        setKey(Provider::Azure, L"  " + kAzureKey + L"\r\n");  // pasted with spaces
        CHECK(loadKey(Provider::Azure) == kAzureKey);
        CHECK(fileText(cfg).find(u8(kAzureKey)) == std::string::npos);
    }

    // 4. DeepL request format, placeholders, de-duplication, cache.
    std::printf("[deepl]\n");
    enable(Provider::DeepL);
    CHECK(eng->installed(Lang::Ja, Lang::ZhHant));
    CHECK(std::string(eng->id()) == "online-deepl");
    bool direct = false;
    CHECK(eng->supports(Lang::Ko, Lang::ZhHant, &direct) && direct);
    {
        srv.handler = deeplEcho;
        srv.reset();
        TrRequest a = req(L"ZQAを直射日光、高温多湿を避けて保存");
        a.keep = {L"42kcal"};
        TrRequest b = req(L"熱量 42kcal & 脂質 1.5g");
        b.keep = {L"42kcal", L"1.5g"};
        std::vector<TrRequest> in = {a, b, a, req(L"   "), req(L"hello", Lang::En, Lang::ZhHant)};
        std::vector<TrHypothesis> out;
        std::wstring err;
        CHECK(eng->translate(in, out, &err));
        CHECK(out.size() == 5);
        CHECK(srv.count() == 2);  // ja->zh-Hant (2 unique texts) + en->zh-Hant
        Req r = srv.log[0];
        CHECK(r.method == "POST" && r.path == "/v2/translate");
        CHECK(r.header("Authorization") == "DeepL-Auth-Key " + u8(kDeepLKey));
        CHECK(r.header("Content-Type") == "application/json");
        Json j;
        CHECK(parseJson(r.body, j));
        CHECK(j.get(L"target_lang") && j.get(L"target_lang")->s == L"ZH-HANT");
        CHECK(j.get(L"source_lang") && j.get(L"source_lang")->s == L"JA");
        CHECK(j.get(L"tag_handling") && j.get(L"tag_handling")->s == L"xml");
        CHECK(j.get(L"ignore_tags") && j.get(L"ignore_tags")->at(0).s == L"k");
        CHECK(j.get(L"text") && j.get(L"text")->a.size() == 2);
        CHECK(j.get(L"text")->at(1).s == L"熱量 <k>42kcal</k> &amp; 脂質 <k>1.5g</k>");
        CHECK(out[0].text == L"ZQA譯[を直射日光、高温多湿を避けて保存]");
        CHECK(out[1].text == L"42kcal1.5g譯[熱量  & 脂質 ]");
        CHECK(out[2].text == out[0].text);
        CHECK(out[3].text.empty());
        CHECK(out[4].text == L"譯[hello]");
        CHECK(out[0].engine == "online-deepl" && !out[0].pivot && out[0].flags.empty());
        CHECK(parseJson(srv.log[1].body, j) && j.get(L"source_lang")->s == L"EN");
        CHECK(lastStatus() == Status::Ok);
        // Cached: nothing sent again.
        CHECK(eng->translate({a, b}, out, &err));
        CHECK(srv.count() == 2 && out[1].text == L"42kcal1.5g譯[熱量  & 脂質 ]");
        // Context goes along (and is part of the cache key).
        TrRequest c = a;
        c.context = L"保存方法";
        CHECK(eng->translate({c}, out, &err));
        CHECK(srv.count() == 3 && parseJson(srv.last().body, j) && j.get(L"context") && j.get(L"context")->s == L"保存方法");
        // A lost placeholder is flagged.
        srv.handler = [](const Req&) { return Resp{200, "{\"translations\":[{\"text\":\"沒有佔位\"}]}"}; };
        CHECK(eng->translate({req(L"ZQBを開封後")}, out, &err));
        CHECK(out[0].text == L"沒有佔位" && out[0].flags.size() == 1 && out[0].flags[0] == "placeholder");
        // Pro key -> api.deepl.com, free key -> api-free (host chosen by the suffix).
        CHECK(makeAdapter(Provider::DeepL) != nullptr);
    }

    // 5. Retries.
    std::printf("[retries]\n");
    {
        std::vector<TrHypothesis> out;
        std::wstring err;
        int n = 0;
        srv.handler = [&](const Req& r) { return ++n == 1 ? Resp{503, "{\"message\":\"busy\"}"} : deeplEcho(r); };
        srv.reset();
        CHECK(eng->translate({req(L"再試行1")}, out, &err));
        CHECK(srv.count() == 2 && out[0].text == L"譯[再試行1]");
        n = 0;
        srv.handler = [&](const Req& r) {
            return ++n == 1 ? Resp{429, "{\"message\":\"Too many requests\"}", "Retry-After: 1\r\n"} : deeplEcho(r);
        };
        srv.reset();
        DWORD t0 = GetTickCount();
        CHECK(eng->translate({req(L"再試行2")}, out, &err));
        std::printf("  retry-after: %zu requests, %lu ms\n", srv.count(), GetTickCount() - t0);
        CHECK(srv.count() == 2 && GetTickCount() - t0 >= 900);  // waited for Retry-After
        // Three 5xx in a row: gives up after 3 tries.
        srv.handler = [](const Req&) { return Resp{500, "{\"message\":\"oops\"}"}; };
        srv.reset();
        CHECK(!eng->translate({req(L"再試行3")}, out, &err));
        CHECK(srv.count() == 3 && lastStatus() == Status::ServerError && out[0].text.empty());
        CHECK(blockedStatus(Provider::DeepL) == Status::ServerError);  // backed off (30 s)
        resetBackoff();
        // Timeout (answer after 4 s, timeout 1 s): one request alone, then the engine (2 tries, Timeout).
        setTimeoutMsForTest(1000);
        srv.handler = [](const Req& r) {
            Resp x = deeplEcho(r);
            x.delayMs = 4000;
            return x;
        };
        srv.reset();
        {
            HttpRequest one = makeAdapter(Provider::DeepL)->translateRequest(Batch{Lang::Ja, Lang::ZhHant, L"", {markUp(req(L"一回"), Markup::DeepLXml)}}, L"k:fx", L"");
            HttpResponse rs;
            t0 = GetTickCount();
            httpSend(one, rs);
            DWORD el = GetTickCount() - t0;
            std::printf("  one request: timedOut=%d err=%u %lu ms\n", rs.timedOut(), rs.winError, el);
            CHECK(rs.timedOut() && el < 3800);  // WinHTTP notices ~1-2 s after the set timeout
            Sleep(4200);
            std::printf("  server saw %zu requests\n", srv.count());
            srv.reset();
        }
        t0 = GetTickCount();
        bool slowOk = eng->translate({req(L"遅い")}, out, &err);
        std::printf("  timeout: ok=%d status=%s %lu ms\n", slowOk, statusId(lastStatus()), GetTickCount() - t0);
        CHECK(!slowOk);
        CHECK(lastStatus() == Status::Timeout);
        Sleep(4000 * 2 + 500);  // let the fake server finish the late answers
        CHECK(srv.count() == 2);
        setTimeoutMsForTest(0);
        CHECK(blockedStatus(Provider::DeepL) == Status::Timeout);
        resetBackoff();
        // Garbage answer (not latched: per request).
        srv.handler = [](const Req&) { return Resp{200, "<html>proxy login</html>"}; };
        CHECK(!eng->translate({req(L"壊れた")}, out, &err));
        CHECK(lastStatus() == Status::BadResponse);
        CHECK(blockedStatus(Provider::DeepL) == Status::Ok);
    }

    // 6. Errors that are not retried; the key never in an error.
    std::printf("[errors]\n");
    {
        std::vector<TrHypothesis> out;
        std::wstring err;
        srv.handler = [&](const Req&) {
            return Resp{403, "{\"message\":\"Authorization failure for key " + u8(kDeepLKey) + "\"}"};
        };
        srv.reset();
        CHECK(!eng->translate({req(L"鍵1"), req(L"鍵2", Lang::Ko)}, out, &err));
        CHECK(srv.count() == 1);  // no retry, and the second group is not tried
        CHECK(lastStatus() == Status::BadKey);
        CHECK(err.find(kDeepLKey) == std::wstring::npos && err.find(L"deadbeef") == std::wstring::npos);
        CHECK(err.find(L"bad-key") != std::wstring::npos);
        // Latched until the key changes: nothing sent, not installed.
        int64_t left = 0;
        CHECK(blockedStatus(Provider::DeepL, &left) == Status::BadKey && left == -1);
        CHECK(!eng->installed(Lang::Ja, Lang::ZhHant));
        CHECK(activeMode() == Mode::Off);
        CHECK(!eng->translate({req(L"鍵3")}, out, &err));
        CHECK(srv.count() == 1 && lastStatus() == Status::BadKey);
        setClockOffsetMsForTest(24LL * 3600 * 1000);  // time alone does not end it
        CHECK(blockedStatus(Provider::DeepL) == Status::BadKey);
        setClockOffsetMsForTest(0);
        saveSettings(loadSettings());  // same provider / region: still latched
        CHECK(blockedStatus(Provider::DeepL) == Status::BadKey);
        setKey(Provider::DeepL, kDeepLKey);  // a (new) key ends it
        CHECK(blockedStatus(Provider::DeepL) == Status::Ok && eng->installed(Lang::Ja, Lang::ZhHant));
        srv.handler = [](const Req&) { return Resp{456, "{\"message\":\"Quota exceeded\"}"}; };
        srv.reset();
        CHECK(!eng->translate({req(L"額度")}, out, &err));
        CHECK(srv.count() == 1 && lastStatus() == Status::QuotaExceeded);
        // Quota latch: not installed, nothing sent until the key changes.
        CHECK(!eng->installed(Lang::Ja, Lang::ZhHant));
        CHECK(!eng->translate({req(L"額度2")}, out, &err));
        CHECK(srv.count() == 1);
        setKey(Provider::DeepL, kDeepLKey);
        CHECK(eng->installed(Lang::Ja, Lang::ZhHant));
        srv.handler = [](const Req&) { return Resp{400, "{\"message\":\"Value for 'target_lang' not supported.\"}"}; };
        CHECK(!eng->translate({req(L"言語")}, out, &err));
        CHECK(lastStatus() == Status::Unsupported);
    }

    // 7. Azure.
    std::printf("[azure]\n");
    enable(Provider::Azure, Mode::All, L"eastasia");
    CHECK(loadSettings().mode == Mode::All && loadSettings().azureRegion == L"eastasia");
    CHECK(std::string(eng->id()) == "online-azure");
    {
        std::vector<TrHypothesis> out;
        std::wstring err;
        srv.handler = azureEcho;
        srv.reset();
        TrRequest a = req(L"ZQAは<b>1.5g</b>");
        a.keep = {L"1.5g"};
        a.context = L"ignored by Azure";
        CHECK(eng->translate({a, req(L"주의 ZQB", Lang::Ko)}, out, &err));
        CHECK(srv.count() == 2);
        Req r = srv.log[0];
        CHECK(r.method == "POST");
        CHECK(r.path == "/translate?api-version=3.0&from=ja&to=zh-Hant&textType=html");
        CHECK(r.header("Ocp-Apim-Subscription-Key") == u8(kAzureKey));
        CHECK(r.header("Ocp-Apim-Subscription-Region") == "eastasia");
        Json j;
        CHECK(parseJson(r.body, j) && j.t == Json::T::Arr && j.a.size() == 1);
        CHECK(j.at(0).get(L"Text")->s ==
              L"<span class=\"notranslate\">ZQA</span>は&lt;b&gt;<span class=\"notranslate\">1.5g</span>&lt;/b&gt;");
        CHECK(r.body.find("ignored") == std::string::npos);
        CHECK(out[0].text == L"ZQA1.5g譯[は<b></b>]");
        CHECK(out[1].text == L"ZQB譯[주의 ]" && out[1].engine == "online-azure");
        CHECK(srv.log[1].path.find("from=ko") != std::string::npos);
        srv.handler = [](const Req&) {
            return Resp{403, "{\"error\":{\"code\":403001,\"message\":\"The operation is not allowed because the subscription has exceeded its free quota.\"}}"};
        };
        srv.reset();
        CHECK(!eng->translate({req(L"無料枠")}, out, &err));
        CHECK(srv.count() == 1 && lastStatus() == Status::QuotaExceeded);
        CHECK(!eng->installed(Lang::Ja, Lang::ZhHant));
        setKey(Provider::Azure, kAzureKey);
        srv.handler = [](const Req&) { return Resp{429, "{\"error\":{\"code\":429001,\"message\":\"rate\"}}"}; };
        srv.reset();
        CHECK(!eng->translate({req(L"多すぎ")}, out, &err));
        CHECK(srv.count() == 3 && lastStatus() == Status::RateLimited);
    }

    // 8. testKey.
    std::printf("[testKey]\n");
    {
        srv.handler = [](const Req& r) {
            if (r.path == "/v2/usage" && r.method == "GET") return Resp{200, "{\"character_count\":1234,\"character_limit\":500000}"};
            return Resp{404, "{}"};
        };
        srv.reset();
        TestResult t = testKey(Provider::DeepL, L" " + kDeepLKey + L" ");
        CHECK(t.status == Status::Ok && t.used == 1234 && t.limit == 500000);
        CHECK(srv.count() == 1 && srv.last().header("Authorization") == "DeepL-Auth-Key " + u8(kDeepLKey));
        CHECK(t.detail == L"DeepL API Free" && t.plan == Plan::DeepLFree);
        srv.handler = [](const Req&) { return Resp{200, "{\"character_count\":500000,\"character_limit\":500000}"}; };
        CHECK(testKey(Provider::DeepL, kDeepLKey).status == Status::QuotaExceeded);
        srv.handler = [&](const Req&) { return Resp{403, "{\"message\":\"Wrong key " + u8(kDeepLKey) + "\"}"}; };
        t = testKey(Provider::DeepL, kDeepLKey);
        CHECK(t.status == Status::BadKey && t.http == 403);
        CHECK(t.detail.find(kDeepLKey) == std::wstring::npos);
        srv.handler = [](const Req& r) {
            if (r.path == "/translate?api-version=3.0&from=en&to=zh-Hant" && r.header("Ocp-Apim-Subscription-Region").empty())
                return Resp{200, "[{\"translations\":[{\"text\":\"好\",\"to\":\"zh-Hant\"}]}]"};
            return Resp{401, "{\"error\":{\"code\":401000,\"message\":\"bad\"}}"};
        };
        t = testKey(Provider::Azure, kAzureKey);  // global resource: no region header
        CHECK(t.status == Status::Ok && t.sample == L"好" && t.plan == Plan::Azure);
        t = testKey(Provider::Azure, kAzureKey, L"japaneast");
        CHECK(t.status == Status::BadKey && t.http == 401);
        CHECK(testKey(Provider::Azure, L"").status == Status::NotConfigured);
    }

    // 9. Plain HTTP refused off loopback (no request is made).
    std::printf("[transport]\n");
    {
        HttpRequest rq;
        rq.host = L"example.com";
        rq.https = false;
        rq.port = 80;
        rq.path = L"/";
        HttpResponse rs;
        CHECK(!httpSend(rq, rs) && rs.status == 0 && rs.winError == ERROR_ACCESS_DENIED);
        CHECK(scrub(L"x SECRETKEY y", L"SECRETKEY") == L"x *** y");
    }

    // 10. Consent belongs to a provider.
    std::printf("[consent per provider]\n");
    {
        resetBackoff();
        srv.handler = deeplEcho;
        enable(Provider::DeepL);
        Settings s = loadSettings();
        CHECK(s.enabled() && s.consented(Provider::DeepL) && !s.consented(Provider::Azure));
        s.provider = Provider::Azure;  // switched in the panel: Azure was never agreed to
        CHECK(s.needsConsent() && !s.enabled());
        saveSettings(s);
        CHECK(!loadSettings().enabled() && loadSettings().needsConsent());
        CHECK(!eng->installed(Lang::Ja, Lang::ZhHant) && activeMode() == Mode::Off);
        srv.reset();
        std::vector<TrHypothesis> out;
        std::wstring err;
        CHECK(!eng->translate({req(L"同意")}, out, &err));
        CHECK(srv.count() == 0 && lastStatus() == Status::NotConfigured);
        s.setConsent(Provider::Azure);
        saveSettings(s);
        CHECK(loadSettings().enabled() && loadSettings().consented(Provider::DeepL));
        std::string f = fileText(cfg);
        CHECK(f.find("consent_deepl=1") != std::string::npos && f.find("consent_azure=1") != std::string::npos);
        CHECK(f.find("\nconsent=") == std::string::npos);
        // A file of the earlier build: one "consent" = the saved provider only.
        std::string keys;
        for (const char* k : {"key_deepl=", "key_azure="}) {
            size_t p = f.find(k);
            if (p != std::string::npos) keys += f.substr(p, f.find('\n', p) - p + 1);
        }
        {
            std::ofstream o(cfg, std::ios::binary | std::ios::trunc);
            o << "[online]\nmode=escalate\nprovider=deepl\nconsent=1\nazure_region=EastAsia\n" << keys;
        }
        Sleep(1100);         // another process wrote it: seen within a second
        s = loadSettings();
        CHECK(s.mode == Mode::Escalate && s.provider == Provider::DeepL);
        CHECK(s.consentDeepL == 1 && s.consentAzure == 0 && s.enabled());
        CHECK(s.azureRegion == L"eastasia");
        CHECK(hasKey(Provider::DeepL) && hasKey(Provider::Azure));
        s.provider = Provider::Azure;
        CHECK(!s.enabled());
    }

    // 11. Settings cache: per-segment calls do no file / DPAPI work.
    std::printf("[settings cache]\n");
    {
        enable(Provider::DeepL);
        DWORD t0 = GetTickCount();
        int yes = 0;
        for (int i = 0; i < 20000; ++i) yes += eng->installed(Lang::Ja, Lang::ZhHant) && hasKey(Provider::DeepL) ? 1 : 0;
        DWORD el = GetTickCount() - t0;
        std::printf("  20000 installed()+hasKey(): %lu ms\n", el);
        CHECK(yes == 20000 && el < 2000);
        // An edit by another process is still seen.
        std::string f = fileText(cfg);
        size_t m = f.find("mode=escalate");
        CHECK(m != std::string::npos);
        if (m != std::string::npos) f.replace(m, 13, "mode=all");
        {
            std::ofstream o(cfg, std::ios::binary | std::ios::trunc);
            o << f;
        }
        Sleep(1100);
        CHECK(loadSettings().mode == Mode::All && activeMode() == Mode::All);
        StoreError se = StoreError::None;
        CHECK(!setKey(Provider::None, L"x", &se) && se == StoreError::InvalidProvider);
        CHECK(std::string(storeErrorId(se)) == "invalid-provider");
        CHECK(saveSettings(loadSettings(), &se) && se == StoreError::None);
    }

    // 12. Batching: different contexts still go in one request; prefetch fills
    //     the cache so the escalator's per-segment calls send nothing.
    std::printf("[batch / prefetch]\n");
    {
        srv.handler = deeplEcho;
        srv.reset();
        clearCache();
        std::vector<TrRequest> pic;
        for (int i = 0; i < 5; ++i) {
            TrRequest r = req(L"段落" + std::to_wstring(i));
            r.context = i ? L"段落" + std::to_wstring(i - 1) : L"";
            pic.push_back(r);
        }
        pic.push_back(pic[1]);  // a duplicate
        std::vector<TrHypothesis> out;
        CHECK(prefetch(pic, &out) == Status::Ok);
        CHECK(srv.count() == 1);
        Json j;
        CHECK(parseJson(srv.last().body, j) && j.get(L"text") && j.get(L"text")->a.size() == 5);
        CHECK(j.get(L"context") && j.get(L"context")->s == L"段落0\n段落1\n段落2\n段落3");
        CHECK(out.size() == 6 && out[3].text == L"譯[段落3]" && out[5].text == out[1].text);
        std::wstring err;
        for (const auto& r : pic) {
            std::vector<TrHypothesis> one;
            CHECK(eng->translate({r}, one, &err) && one[0].text == L"譯[" + r.text + L"]");
        }
        CHECK(srv.count() == 1);  // all cache hits
        // 60 texts: DeepL takes 50 per request.
        std::vector<TrRequest> many;
        for (int i = 0; i < 60; ++i) many.push_back(req(L"多数" + std::to_wstring(i)));
        srv.reset();
        CHECK(prefetch(many) == Status::Ok && srv.count() == 2);
        // Off: nothing sent.
        Settings s = loadSettings();
        s.mode = Mode::Off;
        saveSettings(s);
        srv.reset();
        CHECK(activeMode() == Mode::Off && prefetch({req(L"オフ")}) == Status::NotConfigured && srv.count() == 0);
        enable(Provider::DeepL, Mode::All);
        CHECK(activeMode() == Mode::All);
    }

    // 13. Network back-off: one failed picture, then nothing waits.
    std::printf("[network back-off]\n");
    {
        // A loopback port nobody listens on: connection refused.
        SOCKET tmpS = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(tmpS, reinterpret_cast<sockaddr*>(&a), sizeof a);
        int len = sizeof a;
        getsockname(tmpS, reinterpret_cast<sockaddr*>(&a), &len);
        closesocket(tmpS);
        setLoopbackPortForTest(ntohs(a.sin_port));
        std::vector<TrHypothesis> out;
        std::wstring err;
        CHECK(!eng->translate({req(L"ネット1")}, out, &err));
        CHECK(lastStatus() == Status::Network);
        int64_t left = 0;
        CHECK(blockedStatus(Provider::DeepL, &left) == Status::Network && left > 50000 && left <= 60000);
        CHECK(!eng->installed(Lang::Ja, Lang::ZhHant) && activeMode() == Mode::Off);
        DWORD t0 = GetTickCount();
        CHECK(!eng->translate({req(L"ネット2")}, out, &err) && prefetch({req(L"ネット3")}) == Status::Network);
        CHECK(GetTickCount() - t0 < 100);
        CHECK(lastStatus() == Status::Network && lastError().provider == Provider::DeepL);
        setLoopbackPortForTest(srv.port);
        setClockOffsetMsForTest(61 * 1000);  // a minute later: tried again
        CHECK(blockedStatus(Provider::DeepL) == Status::Ok && eng->installed(Lang::Ja, Lang::ZhHant));
        srv.handler = deeplEcho;
        CHECK(eng->translate({req(L"ネット4")}, out, &err) && lastStatus() == Status::Ok);
        // 429 with Retry-After: 30 s at least.
        srv.handler = [](const Req&) { return Resp{429, "{\"message\":\"slow down\"}", "Retry-After: 1\r\n"}; };
        CHECK(!eng->translate({req(L"多い")}, out, &err));
        CHECK(blockedStatus(Provider::DeepL, &left) == Status::RateLimited && left > 25000 && left <= 30000);
        setClockOffsetMsForTest(0);
        resetBackoff();
        CHECK(blockedStatus(Provider::DeepL) == Status::Ok);
    }

    // 14. testStoredKey: the saved key, never typed again.
    std::printf("[testStoredKey]\n");
    {
        srv.handler = [](const Req& r) {
            if (r.path == "/v2/usage") return Resp{200, "{\"character_count\":7,\"character_limit\":500000}"};
            return Resp{404, "{}"};
        };
        srv.reset();
        TestResult t = testStoredKey(Provider::DeepL);
        CHECK(t.status == Status::Ok && t.used == 7 && t.plan == Plan::DeepLFree);
        CHECK(srv.count() == 1 && srv.last().header("Authorization") == "DeepL-Auth-Key " + u8(kDeepLKey));
        srv.handler = [&](const Req&) { return Resp{403, "{\"message\":\"no " + u8(kDeepLKey) + "\"}"}; };
        t = testStoredKey(Provider::DeepL);
        CHECK(t.status == Status::BadKey && t.detail.find(kDeepLKey) == std::wstring::npos);
        CHECK(blockedStatus(Provider::DeepL) == Status::BadKey);  // the engine will not try it either
        srv.handler = [](const Req&) { return Resp{200, "{\"character_count\":7,\"character_limit\":500000}"}; };
        CHECK(testStoredKey(Provider::DeepL).status == Status::Ok);
        CHECK(blockedStatus(Provider::DeepL) == Status::Ok);  // fixed on the provider's side: usable again
        // Azure: saved region, or the one in the box.
        Settings s = loadSettings();
        s.azureRegion = L"japaneast";
        saveSettings(s);
        srv.handler = [](const Req& r) {
            if (r.header("Ocp-Apim-Subscription-Region") == "japaneast")
                return Resp{200, "[{\"translations\":[{\"text\":\"好\",\"to\":\"zh-Hant\"}]}]"};
            return Resp{401, "{\"error\":{\"code\":401000,\"message\":\"bad\"}}"};
        };
        srv.reset();
        t = testStoredKey(Provider::Azure);
        CHECK(t.status == Status::Ok && t.sample == L"好");
        CHECK(srv.last().header("Ocp-Apim-Subscription-Key") == u8(kAzureKey));
        t = testStoredKey(Provider::Azure, L"westeurope");  // unsaved region: does not latch the stored one
        CHECK(t.status == Status::BadKey && blockedStatus(Provider::Azure) == Status::Ok);
        setKey(Provider::Azure, L"");
        srv.reset();
        CHECK(testStoredKey(Provider::Azure).status == Status::NotConfigured && srv.count() == 0);
        CHECK(testStoredKey(Provider::None).status == Status::NotConfigured);
    }

    // 15. Display data.
    std::printf("[display]\n");
    {
        CHECK(std::wstring(providerPrivacyUrl(Provider::Azure)).find(
                  L"learn.microsoft.com/azure/foundry/responsible-ai/translator/data-privacy-security") != std::wstring::npos);
        CHECK(std::wstring(providerName(Provider::Azure)) == L"Microsoft Azure Translator");
        CHECK(std::wstring(kAzureApiVersion) == L"3.0");
        CHECK(std::string(planId(Plan::DeepLPro)) == "deepl-pro" && std::string(providerId(Provider::Azure)) == "azure");
    }

    // 16. forgetAll: off, keys gone, status and back-off cleared.
    std::printf("[forget]\n");
    srv.handler = [](const Req&) { return Resp{456, "{\"message\":\"Quota exceeded\"}"}; };
    {
        std::vector<TrHypothesis> out;
        std::wstring err;
        CHECK(!eng->translate({req(L"最後")}, out, &err) && lastStatus() == Status::QuotaExceeded);
    }
    forgetAll();
    CHECK(!loadSettings().enabled() && !hasKey(Provider::DeepL) && !hasKey(Provider::Azure));
    CHECK(!loadSettings().consented(Provider::DeepL) && !loadSettings().consented(Provider::Azure));
    CHECK(!eng->installed(Lang::Ja, Lang::ZhHant));
    CHECK(lastStatus() == Status::Ok && blockedStatus(Provider::DeepL) == Status::Ok);

    setLoopbackPortForTest(0);
    srv.stop();
    DeleteFileW(cfg.c_str());
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
