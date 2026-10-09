// Online translation: WinHTTP transport, retries, JSON, placeholder markup.
// See online_engine.h.  Nothing here logs; error details never contain the
// request headers (where the key is).
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <regex>

#include "online_engine.h"

namespace pm::translate::online {

namespace {

std::atomic<uint16_t> g_loopbackPort{0};
std::atomic<unsigned> g_retryBaseMs{500};
std::atomic<unsigned> g_timeoutMs{0};

struct HInternet {
    HINTERNET h = nullptr;
    explicit HInternet(HINTERNET x) : h(x) {}
    ~HInternet() {
        if (h) WinHttpCloseHandle(h);
    }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
    operator HINTERNET() const { return h; }
};

}  // namespace

void setLoopbackPortForTest(uint16_t port) { g_loopbackPort = port; }
uint16_t loopbackPortForTest() { return g_loopbackPort; }
void setRetryBaseMsForTest(unsigned ms) { g_retryBaseMs = ms; }
void setTimeoutMsForTest(unsigned ms) { g_timeoutMs = ms; }

bool HttpResponse::timedOut() const { return status == 0 && winError == ERROR_WINHTTP_TIMEOUT; }

std::string toUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool httpSend(const HttpRequest& rq, HttpResponse& rs) {
    rs = {};
    // Plain HTTP only to this PC (the test server); the providers are HTTPS.
    if (!rq.https && rq.host != L"127.0.0.1") {
        rs.winError = ERROR_ACCESS_DENIED;
        return false;
    }
    HInternet session(WinHttpOpen(L"ZizaiCast-OnlineTranslate/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        rs.winError = GetLastError();
        return false;
    }
    const unsigned timeout = g_timeoutMs ? g_timeoutMs.load() : rq.timeoutMs;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, static_cast<int>(timeout));
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // older Windows without TLS 1.3 in WinHTTP
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    HInternet conn(WinHttpConnect(session, rq.host.c_str(), rq.port, 0));
    if (!conn) {
        rs.winError = GetLastError();
        return false;
    }
    HInternet req(WinHttpOpenRequest(conn, rq.method.c_str(), rq.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, rq.https ? WINHTTP_FLAG_SECURE : 0));
    if (!req) {
        rs.winError = GetLastError();
        return false;
    }
    // The wait for the answer's headers has its own option (default 90 s).
    WinHttpSetTimeouts(req, 5000, 5000, 10000, static_cast<int>(timeout));
    DWORD rrt = timeout;
    WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &rrt, sizeof rrt);
    // The key header must never follow a redirect to another host; no cookies.
    DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
    DWORD noCookies = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &noCookies, sizeof noCookies);

    std::wstring headers;
    for (const auto& [k, v] : rq.headers) headers += k + L": " + v + L"\r\n";
    BOOL ok = WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                 headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                 rq.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(rq.body.data()),
                                 static_cast<DWORD>(rq.body.size()), static_cast<DWORD>(rq.body.size()), 0);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    if (!ok || !WinHttpReceiveResponse(req, nullptr)) {
        rs.winError = GetLastError();
        return false;
    }
    DWORD code = 0, len = sizeof code;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &code, &len, WINHTTP_NO_HEADER_INDEX);
    rs.status = static_cast<int>(code);
    // Retry-After in seconds (an HTTP date is ignored: the default wait is used).
    wchar_t retry[64] = {};
    len = sizeof retry - sizeof(wchar_t);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"Retry-After", retry, &len, WINHTTP_NO_HEADER_INDEX))
        rs.retryAfterSec = static_cast<unsigned>(wcstoul(retry, nullptr, 10));
    constexpr size_t kMaxBody = 4u << 20;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            rs.winError = GetLastError();
            rs.status = 0;
            return false;
        }
        if (avail == 0) break;
        size_t at = rs.body.size();
        if (at + avail > kMaxBody) {
            rs.winError = ERROR_INSUFFICIENT_BUFFER;
            rs.status = 0;
            return false;
        }
        rs.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, rs.body.data() + at, avail, &got)) {
            rs.winError = GetLastError();
            rs.status = 0;
            return false;
        }
        rs.body.resize(at + got);
    }
    return true;
}

bool retryable(Status s) {
    return s == Status::Timeout || s == Status::Network || s == Status::RateLimited || s == Status::ServerError;
}

HttpResponse sendWithRetry(const HttpRequest& rq, const Adapter& ad, Status* st, std::wstring* detail) {
    HttpResponse rs;
    Status s = Status::Network;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            unsigned wait = g_retryBaseMs.load() << (attempt - 1);  // 0.5 s, 1 s
            if (rs.retryAfterSec) wait = std::min(rs.retryAfterSec, 5u) * 1000;
            Sleep(wait);
        }
        httpSend(rq, rs);
        if (rs.status == 200) {
            s = Status::Ok;
            break;
        }
        if (rs.status == 0) {
            s = rs.timedOut() ? Status::Timeout : Status::Network;
            if (detail) *detail = L"WinHTTP error " + std::to_wstring(rs.winError);
        } else {
            s = ad.errorStatus(rs, detail);
        }
        if (!retryable(s)) break;
        if (s == Status::Timeout && attempt >= 1) break;  // a slow provider: one more try only (~25 s worst case)
    }
    if (st) *st = s;
    return rs;
}

std::wstring scrub(std::wstring text, const std::wstring& key) {
    if (key.size() < 4) return text;
    for (size_t p = text.find(key); p != std::wstring::npos; p = text.find(key, p)) text.replace(p, key.size(), L"***");
    return text;
}

// ---- JSON ----

const Json* Json::get(const wchar_t* key) const {
    if (t != T::Obj) return nullptr;
    for (const auto& [k, v] : o)
        if (k == key) return &v;
    return nullptr;
}

const Json& Json::at(size_t i) const {
    static const Json kNull;
    return t == T::Arr && i < a.size() ? a[i] : kNull;
}

namespace {

struct Parser {
    const std::wstring& s;
    size_t i = 0;
    int depth = 0;
    void ws() {
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\n' || s[i] == L'\r')) ++i;
    }
    bool lit(const wchar_t* w) {
        size_t n = wcslen(w);
        if (s.compare(i, n, w) != 0) return false;
        i += n;
        return true;
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            wchar_t c = s[i++];
            v <<= 4;
            if (c >= L'0' && c <= L'9') v |= c - L'0';
            else if (c >= L'a' && c <= L'f') v |= c - L'a' + 10;
            else if (c >= L'A' && c <= L'F') v |= c - L'A' + 10;
            else return false;
        }
        return true;
    }
    bool str(std::wstring& out) {
        if (i >= s.size() || s[i] != L'"') return false;
        ++i;
        while (i < s.size()) {
            wchar_t c = s[i++];
            if (c == L'"') return true;
            if (c != L'\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            wchar_t e = s[i++];
            switch (e) {
            case L'"': out += L'"'; break;
            case L'\\': out += L'\\'; break;
            case L'/': out += L'/'; break;
            case L'b': out += L'\b'; break;
            case L'f': out += L'\f'; break;
            case L'n': out += L'\n'; break;
            case L'r': out += L'\r'; break;
            case L't': out += L'\t'; break;
            case L'u': {
                unsigned v;
                if (!hex4(v)) return false;
                out += static_cast<wchar_t>(v);  // UTF-16: surrogate pairs come as two \u
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Json& v) {
        if (++depth > 64) return false;
        ws();
        if (i >= s.size()) return false;
        wchar_t c = s[i];
        bool ok = true;
        if (c == L'{') {
            v.t = Json::T::Obj;
            ++i;
            ws();
            if (i < s.size() && s[i] == L'}') ++i;
            else
                for (;;) {
                    ws();
                    std::wstring k;
                    Json m;
                    if (!str(k)) return false;
                    ws();
                    if (i >= s.size() || s[i++] != L':') return false;
                    if (!value(m)) return false;
                    v.o.emplace_back(std::move(k), std::move(m));
                    ws();
                    if (i < s.size() && s[i] == L',') { ++i; continue; }
                    if (i < s.size() && s[i] == L'}') { ++i; break; }
                    return false;
                }
        } else if (c == L'[') {
            v.t = Json::T::Arr;
            ++i;
            ws();
            if (i < s.size() && s[i] == L']') ++i;
            else
                for (;;) {
                    Json m;
                    if (!value(m)) return false;
                    v.a.push_back(std::move(m));
                    ws();
                    if (i < s.size() && s[i] == L',') { ++i; continue; }
                    if (i < s.size() && s[i] == L']') { ++i; break; }
                    return false;
                }
        } else if (c == L'"') {
            v.t = Json::T::Str;
            ok = str(v.s);
        } else if (lit(L"true")) {
            v.t = Json::T::Bool;
            v.b = true;
        } else if (lit(L"false")) {
            v.t = Json::T::Bool;
        } else if (lit(L"null")) {
            v.t = Json::T::Null;
        } else {
            size_t st = i;
            while (i < s.size() && wcschr(L"+-0123456789.eE", s[i])) ++i;
            if (i == st) return false;
            v.t = Json::T::Num;
            v.n = wcstod(s.substr(st, i - st).c_str(), nullptr);
        }
        --depth;
        return ok;
    }
};

}  // namespace

bool parseJson(const std::string& utf8, Json& out) {
    std::wstring w = fromUtf8(utf8);
    if (!w.empty() && w[0] == 0xFEFF) w.erase(0, 1);
    Parser p{w};
    out = {};
    if (!p.value(out)) return false;
    p.ws();
    return p.i == w.size();
}

std::string jsonString(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        switch (c) {
        case L'"': o += L"\\\""; break;
        case L'\\': o += L"\\\\"; break;
        case L'\n': o += L"\\n"; break;
        case L'\r': o += L"\\r"; break;
        case L'\t': o += L"\\t"; break;
        default:
            if (c < 0x20) {
                wchar_t b[8];
                swprintf(b, 8, L"\\u%04x", static_cast<unsigned>(c));
                o += b;
            } else {
                o += c;
            }
        }
    }
    o += L"\"";
    return toUtf8(o);
}

// ---- Markup ----

namespace {

void escapeTo(std::wstring& o, const std::wstring& s, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
        wchar_t c = s[i];
        if (c == L'&') o += L"&amp;";
        else if (c == L'<') o += L"&lt;";
        else if (c == L'>') o += L"&gt;";
        else o += c;
    }
}

std::wstring escaped(const std::wstring& s) {
    std::wstring o;
    escapeTo(o, s, 0, s.size());
    return o;
}

std::wstring unescape(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'&') {
            o += s[i];
            continue;
        }
        size_t semi = s.find(L';', i);
        if (semi == std::wstring::npos || semi - i > 10) {
            o += s[i];
            continue;
        }
        std::wstring e = s.substr(i + 1, semi - i - 1);
        wchar_t r = 0;
        if (e == L"amp") r = L'&';
        else if (e == L"lt") r = L'<';
        else if (e == L"gt") r = L'>';
        else if (e == L"quot") r = L'"';
        else if (e == L"apos") r = L'\'';
        else if (e == L"nbsp") r = L' ';
        else if (e.size() > 1 && e[0] == L'#') {
            unsigned long v = (e[1] == L'x' || e[1] == L'X') ? wcstoul(e.c_str() + 2, nullptr, 16) : wcstoul(e.c_str() + 1, nullptr, 10);
            if (v > 0 && v < 0x10000) r = static_cast<wchar_t>(v);
        }
        if (!r) {
            o += s[i];
            continue;
        }
        o += r;
        i = semi;
    }
    return o;
}

struct Range {
    size_t pos, len;
    std::wstring piece;  // what goes back into the translation
};

const wchar_t* openTag(Markup m) { return m == Markup::DeepLXml ? L"<k>" : L"<span class=\"notranslate\">"; }
const wchar_t* closeTag(Markup m) { return m == Markup::DeepLXml ? L"</k>" : L"</span>"; }

}  // namespace

Marked markUp(const TrRequest& r, Markup m) {
    const std::wstring& t = r.text;
    std::vector<Range> rs;
    // Placeholders of the preprocessor (ZQA..ZQZ).
    for (size_t p = t.find(L"ZQ"); p != std::wstring::npos; p = t.find(L"ZQ", p + 1))
        if (p + 2 < t.size() && t[p + 2] >= L'A' && t[p + 2] <= L'Z') rs.push_back({p, 3, t.substr(p, 3)});
    for (const auto& k : r.keep)
        if (!k.empty())
            for (size_t p = t.find(k); p != std::wstring::npos; p = t.find(k, p + k.size())) rs.push_back({p, k.size(), k});
    // Glossary terms: the target term is put in place and kept.
    for (const auto& [src, dst] : r.terms)
        if (!src.empty() && !dst.empty())
            for (size_t p = t.find(src); p != std::wstring::npos; p = t.find(src, p + src.size())) rs.push_back({p, src.size(), dst});
    std::sort(rs.begin(), rs.end(), [](const Range& a, const Range& b) { return a.pos != b.pos ? a.pos < b.pos : a.len > b.len; });
    Marked mk;
    size_t at = 0;
    for (const auto& x : rs) {
        if (x.pos < at) continue;  // overlaps an earlier (longer) piece
        escapeTo(mk.text, t, at, x.pos);
        mk.text += openTag(m);
        mk.text += escaped(x.piece);
        mk.text += closeTag(m);
        mk.pieces.push_back(x.piece);
        at = x.pos + x.len;
    }
    escapeTo(mk.text, t, at, t.size());
    return mk;
}

std::wstring unmark(const std::wstring& answer, const Marked& mk, Markup m, int* lost) {
    static const std::wregex kDeepL(L"<k(?:\\s[^>]*)?>([\\s\\S]*?)</k>");
    static const std::wregex kHtml(L"<span\\s+class\\s*=\\s*[\"']notranslate[\"']\\s*>([\\s\\S]*?)</span>",
                                   std::regex::icase);
    // Markup -> private-use sentinels (U+E000 + piece), then entities, then pieces.
    std::vector<int> seen(mk.pieces.size(), 0);
    std::wstring tmp;
    size_t at = 0;
    const std::wregex& re = m == Markup::DeepLXml ? kDeepL : kHtml;
    for (std::wsregex_iterator it(answer.begin(), answer.end(), re), end; it != end; ++it) {
        const auto& mt = *it;
        tmp.append(answer, at, static_cast<size_t>(mt.position(0)) - at);
        at = static_cast<size_t>(mt.position(0) + mt.length(0));
        std::wstring content = unescape(mt[1].str());
        // The first piece with this text not used yet (else any with it).
        size_t idx = SIZE_MAX, any = SIZE_MAX;
        for (size_t i = 0; i < mk.pieces.size(); ++i)
            if (mk.pieces[i] == content) {
                if (any == SIZE_MAX) any = i;
                if (!seen[i]) {
                    idx = i;
                    break;
                }
            }
        if (idx == SIZE_MAX) idx = any;
        if (idx == SIZE_MAX || idx >= 0x1000) {
            tmp += escaped(content);  // something the provider made up: keep its text
            continue;
        }
        ++seen[idx];
        tmp += static_cast<wchar_t>(0xE000 + idx);
    }
    tmp.append(answer, at, std::wstring::npos);
    // Any other tag the provider added (should not happen) goes.
    static const std::wregex kTag(L"</?(?:k|span|b|i|br)\\b[^>]*>", std::regex::icase);
    tmp = std::regex_replace(tmp, kTag, L"");
    std::wstring plain = unescape(tmp);
    std::wstring out;
    out.reserve(plain.size());
    for (wchar_t c : plain) {
        size_t i = static_cast<size_t>(c) - 0xE000;
        if (c >= 0xE000 && i < mk.pieces.size()) out += mk.pieces[i];
        else out += c;
    }
    if (lost) {
        *lost = 0;
        for (int n : seen)
            if (n != 1) ++*lost;
    }
    return out;
}

}  // namespace pm::translate::online
