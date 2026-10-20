// Online translation providers: DeepL API (Free / Pro), Microsoft Azure
// Translator (Text Translation v3.0, kAzureApiVersion) and Google Cloud
// Translation Basic (v2).  All translate ja / ko / en / zh directly (no
// English pivot) into Traditional Chinese.  Request formats:
//   DeepL  POST https://api(-free).deepl.com/v2/translate   (JSON body,
//          Authorization: DeepL-Auth-Key …, tag_handling xml, ignore_tags k)
//          GET  /v2/usage  (the key check: bills no characters)
//   Azure  POST https://api.cognitive.microsofttranslator.com/translate
//          ?api-version=3.0&from=ja&to=zh-Hant&textType=html
//          (Ocp-Apim-Subscription-Key / -Region, <span class="notranslate">)
//   Google POST https://translation.googleapis.com/language/translate/v2
//          JSON {"q":[...],"target":"zh-TW","source":"ja","format":"html"}
//          (X-Goog-Api-Key header, <span class="notranslate">)
#include "online_engine.h"

namespace pm::translate::online {

namespace {

std::wstring trimmed(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

// Short readable detail of an error answer (provider message, no key).
std::wstring messageOf(const HttpResponse& rs, const Json& j) {
    std::wstring m;
    if (const Json* e = j.get(L"error")) {
        if (const Json* mm = e->get(L"message")) m = mm->str();
        if (const Json* c = e->get(L"code"); c && c->t == Json::T::Num) m = std::to_wstring(static_cast<long long>(c->n)) + L" " + m;
    } else if (const Json* mm = j.get(L"message")) {
        m = mm->str();
    }
    if (m.size() > 200) m.resize(200);
    return L"HTTP " + std::to_wstring(rs.status) + (m.empty() ? L"" : L": " + m);
}

void applyLoopback(HttpRequest& rq) {
    if (uint16_t port = loopbackPortForTest()) {
        rq.host = L"127.0.0.1";
        rq.port = port;
        rq.https = false;
    }
}

// ---------------- DeepL ----------------

class DeepL final : public Adapter {
public:
    Provider provider() const override { return Provider::DeepL; }
    const char* engineId() const override { return "online-deepl"; }
    Markup markup() const override { return Markup::DeepLXml; }
    static const wchar_t* srcCode(Lang l) {
        switch (l) {
        case Lang::Ja: return L"JA";
        case Lang::Ko: return L"KO";
        case Lang::En: return L"EN";
        case Lang::ZhHans:
        case Lang::ZhHant: return L"ZH";
        default: return nullptr;  // Unknown: let DeepL detect it
        }
    }
    static const wchar_t* tgtCode(Lang l) {
        switch (l) {
        case Lang::Ja: return L"JA";
        case Lang::Ko: return L"KO";
        case Lang::En: return L"EN-US";
        case Lang::ZhHans: return L"ZH-HANS";
        case Lang::ZhHant: return L"ZH-HANT";
        default: return nullptr;
        }
    }
    bool supports(Lang src, Lang tgt) const override {
        return tgtCode(tgt) && (src == Lang::Unknown || srcCode(src)) && src != tgt;
    }
    size_t maxTexts() const override { return 50; }      // DeepL: up to 50 texts per request
    size_t maxChars() const override { return 30000; }   // well under the 128 KiB request limit (UTF-8)
    static std::wstring host(const std::wstring& key) {
        // Free-plan keys end in ":fx" and only work on api-free.deepl.com.
        return key.size() > 3 && key.compare(key.size() - 3, 3, L":fx") == 0 ? L"api-free.deepl.com" : L"api.deepl.com";
    }
    static HttpRequest base(const std::wstring& key) {
        HttpRequest rq;
        rq.host = host(key);
        rq.headers.push_back({L"Authorization", L"DeepL-Auth-Key " + key});
        applyLoopback(rq);
        return rq;
    }
    HttpRequest translateRequest(const Batch& b, const std::wstring& key, const std::wstring&) const override {
        HttpRequest rq = base(trimmed(key));
        rq.path = L"/v2/translate";
        rq.headers.push_back({L"Content-Type", L"application/json"});
        std::string j = "{\"text\":[";
        for (size_t i = 0; i < b.texts.size(); ++i) j += (i ? "," : "") + jsonString(b.texts[i].text);
        j += "]";
        if (const wchar_t* s = srcCode(b.src)) j += ",\"source_lang\":" + jsonString(s);
        j += ",\"target_lang\":" + jsonString(tgtCode(b.tgt));
        j += ",\"tag_handling\":\"xml\",\"ignore_tags\":[\"k\"]";
        if (!b.context.empty()) j += ",\"context\":" + jsonString(b.context);  // not billed by DeepL
        j += "}";
        rq.body = std::move(j);
        return rq;
    }
    Status parseTranslate(const HttpResponse& rs, size_t count, std::vector<std::wstring>& out,
                          std::wstring* detail) const override {
        Json j;
        const Json* tr = parseJson(rs.body, j) ? j.get(L"translations") : nullptr;
        if (!tr || tr->t != Json::T::Arr || tr->a.size() != count) {
            if (detail) *detail = L"unexpected DeepL answer";
            return Status::BadResponse;
        }
        out.clear();
        for (const auto& t : tr->a) out.push_back(t.get(L"text") ? t.get(L"text")->str() : std::wstring());
        return Status::Ok;
    }
    Status errorStatus(const HttpResponse& rs, std::wstring* detail) const override {
        Json j;
        parseJson(rs.body, j);
        if (detail) *detail = messageOf(rs, j);
        switch (rs.status) {
        case 401:
        case 403: return Status::BadKey;  // also a Pro key on the Free host and back
        case 429: return Status::RateLimited;
        case 456: return Status::QuotaExceeded;  // character limit of the plan reached
        default: break;
        }
        if (rs.status >= 500) return Status::ServerError;  // incl. 503 / 529 "too many requests"
        // DeepL says "Value for 'target_lang' not supported." for a pair it lacks.
        if (rs.status == 400 && detail && detail->find(L"not supported") != std::wstring::npos) return Status::Unsupported;
        return Status::BadResponse;
    }
    TestResult testKey(const std::wstring& rawKey, const std::wstring&) const override {
        TestResult r;
        std::wstring key = trimmed(rawKey);
        if (key.empty()) return r;
        HttpRequest rq = base(key);
        rq.method = L"GET";
        rq.path = L"/v2/usage";
        Status st;
        std::wstring detail;
        HttpResponse rs = sendWithRetry(rq, *this, &st, &detail);
        r.status = st;
        r.http = rs.status;
        r.detail = scrub(detail, key);
        if (st == Status::Ok) {
            Json j;
            if (!parseJson(rs.body, j) || !j.get(L"character_count")) {
                r.status = Status::BadResponse;
                r.detail = L"unexpected DeepL answer";
                return r;
            }
            r.used = static_cast<int64_t>(j.get(L"character_count")->n);
            if (const Json* l = j.get(L"character_limit")) r.limit = static_cast<int64_t>(l->n);
            // The key works but the plan's character allowance is used up: say so now.
            if (r.limit > 0 && r.used >= r.limit) r.status = Status::QuotaExceeded;
            const bool free = host(key) == L"api-free.deepl.com";
            r.plan = free ? Plan::DeepLFree : Plan::DeepLPro;
            r.detail = free ? L"DeepL API Free" : L"DeepL API Pro";
        }
        return r;
    }
};

// ---------------- Microsoft Azure Translator ----------------

class Azure final : public Adapter {
public:
    Provider provider() const override { return Provider::Azure; }
    const char* engineId() const override { return "online-azure"; }
    Markup markup() const override { return Markup::Html; }
    static const wchar_t* code(Lang l) {
        switch (l) {
        case Lang::Ja: return L"ja";
        case Lang::Ko: return L"ko";
        case Lang::En: return L"en";
        case Lang::ZhHans: return L"zh-Hans";
        case Lang::ZhHant: return L"zh-Hant";
        default: return nullptr;
        }
    }
    bool supports(Lang src, Lang tgt) const override { return code(tgt) && (src == Lang::Unknown || code(src)) && src != tgt; }
    size_t maxTexts() const override { return 1000; }    // Azure: 1,000 elements, 50,000 characters per request
    size_t maxChars() const override { return 40000; }
    static HttpRequest base(const std::wstring& key, const std::wstring& region) {
        HttpRequest rq;
        rq.host = L"api.cognitive.microsofttranslator.com";
        rq.headers.push_back({L"Ocp-Apim-Subscription-Key", key});
        if (!region.empty()) rq.headers.push_back({L"Ocp-Apim-Subscription-Region", region});
        rq.headers.push_back({L"Content-Type", L"application/json; charset=UTF-8"});
        applyLoopback(rq);
        return rq;
    }
    static std::string body(const std::vector<std::wstring>& texts) {
        std::string j = "[";
        for (size_t i = 0; i < texts.size(); ++i) j += (i ? ",{\"Text\":" : "{\"Text\":") + jsonString(texts[i]) + "}";
        return j + "]";
    }
    HttpRequest translateRequest(const Batch& b, const std::wstring& key, const std::wstring& region) const override {
        HttpRequest rq = base(trimmed(key), trimmed(region));
        rq.path = std::wstring(L"/translate?api-version=") + kAzureApiVersion;
        if (const wchar_t* s = code(b.src)) rq.path += std::wstring(L"&from=") + s;
        rq.path += std::wstring(L"&to=") + code(b.tgt) + L"&textType=html";
        std::vector<std::wstring> texts;
        for (const auto& t : b.texts) texts.push_back(t.text);
        rq.body = body(texts);  // Azure has no context field: each text stands alone
        return rq;
    }
    Status parseTranslate(const HttpResponse& rs, size_t count, std::vector<std::wstring>& out,
                          std::wstring* detail) const override {
        Json j;
        if (!parseJson(rs.body, j) || j.t != Json::T::Arr || j.a.size() != count) {
            if (detail) *detail = L"unexpected Azure answer";
            return Status::BadResponse;
        }
        out.clear();
        for (const auto& e : j.a) {
            const Json* tr = e.get(L"translations");
            const Json& first = tr ? tr->at(0) : e;
            out.push_back(first.get(L"text") ? first.get(L"text")->str() : std::wstring());
        }
        return Status::Ok;
    }
    Status errorStatus(const HttpResponse& rs, std::wstring* detail) const override {
        Json j;
        parseJson(rs.body, j);
        if (detail) *detail = messageOf(rs, j);
        long long code = 0;
        if (const Json* e = j.get(L"error"))
            if (const Json* c = e->get(L"code"); c && c->t == Json::T::Num) code = static_cast<long long>(c->n);
        if (rs.status == 401) return Status::BadKey;       // 401000 wrong key / region / endpoint
        if (rs.status == 403) return code == 403001 ? Status::QuotaExceeded : Status::BadKey;  // 403001: free tier used up
        if (rs.status == 429) return Status::RateLimited;  // 429000 / 429001 / 429002
        if (rs.status >= 500) return Status::ServerError;
        if (rs.status == 400 && (code == 400035 || code == 400036 || code == 400019 || code == 400023))
            return Status::Unsupported;  // source / target language not valid
        return Status::BadResponse;
    }
    TestResult testKey(const std::wstring& rawKey, const std::wstring& region) const override {
        TestResult r;
        std::wstring key = trimmed(rawKey);
        if (key.empty()) return r;
        // /languages needs no key, so a real (2-character) translation checks it.
        HttpRequest rq = base(key, trimmed(region));
        rq.path = std::wstring(L"/translate?api-version=") + kAzureApiVersion + L"&from=en&to=zh-Hant";
        rq.body = body({L"OK"});
        Status st;
        std::wstring detail;
        HttpResponse rs = sendWithRetry(rq, *this, &st, &detail);
        r.status = st;
        r.http = rs.status;
        r.detail = scrub(detail, key);
        if (st == Status::Ok) {
            std::vector<std::wstring> out;
            if (parseTranslate(rs, 1, out, &r.detail) != Status::Ok) {
                r.status = Status::BadResponse;
                return r;
            }
            r.sample = out[0];
            r.plan = Plan::Azure;
            r.detail = L"Microsoft Azure Translator";
        }
        return r;
    }
};

// ---------------- Google Cloud Translation Basic (v2) ----------------
//
// Docs (checked 2026-10-10):
//   https://docs.cloud.google.com/translate/docs/reference/rest/v2/translate  (q: at most 128 strings)
//   https://docs.cloud.google.com/translate/quotas  (request at most 100K bytes, 5K characters recommended;
//                                                   quota exceeded = HTTP 403 "Daily Limit Exceeded" /
//                                                   "User Rate Limit Exceeded")
//   https://docs.cloud.google.com/docs/authentication/api-keys-use  (X-Goog-Api-Key header preferred: a key in
//                                                                 the URL can leak through URL scans)
// format is "html" (not "text") on purpose: only html honours <span class="notranslate">, which keeps the
// placeholders / keep strings / glossary terms; markUp() has escaped & < > already.  Answers are unmarked
// like Azure's, so HTML entities in them (&#39; &quot; &amp; ...) are decoded.

// Text of an error answer that tells the cases apart: message, status and reason of every entry of
// "errors" / "details" (not the help links), lower-cased by the caller.
void collectErrorText(const Json& j, std::wstring& out, int depth = 0) {
    if (depth > 6) return;
    if (j.t == Json::T::Str) {
        out += j.s + L'\n';
    } else if (j.t == Json::T::Arr) {
        for (const auto& e : j.a) collectErrorText(e, out, depth + 1);
    } else if (j.t == Json::T::Obj) {
        for (const auto& [k, v] : j.o)
            if (k != L"@type" && k != L"links" && k != L"url" && k != L"domain" && k != L"locationType" && k != L"location")
                collectErrorText(v, out, depth + 1);
    }
}

class Google final : public Adapter {
public:
    Provider provider() const override { return Provider::Google; }
    const char* engineId() const override { return "online-google"; }
    Markup markup() const override { return Markup::Html; }
    static const wchar_t* code(Lang l) {
        switch (l) {
        case Lang::Ja: return L"ja";
        case Lang::Ko: return L"ko";
        case Lang::En: return L"en";
        case Lang::ZhHans: return L"zh-CN";
        case Lang::ZhHant: return L"zh-TW";
        default: return nullptr;  // Unknown: Google detects the source
        }
    }
    bool supports(Lang src, Lang tgt) const override { return code(tgt) && (src == Lang::Unknown || code(src)) && src != tgt; }
    size_t maxTexts() const override { return 128; }     // Google: at most 128 strings in q
    size_t maxChars() const override { return 5000; }    // Google's recommended maximum per request (hard limit: 100K bytes)
    static HttpRequest base(const std::wstring& key) {
        HttpRequest rq;
        rq.host = L"translation.googleapis.com";
        rq.path = L"/language/translate/v2";
        rq.headers.push_back({L"X-Goog-Api-Key", key});  // header, not ?key=: the URL never carries the key
        rq.headers.push_back({L"Content-Type", L"application/json; charset=UTF-8"});
        applyLoopback(rq);
        return rq;
    }
    static std::string body(const std::vector<std::wstring>& texts, Lang src, Lang tgt) {
        std::string j = "{\"q\":[";
        for (size_t i = 0; i < texts.size(); ++i) j += (i ? "," : "") + jsonString(texts[i]);
        j += "],\"target\":" + jsonString(code(tgt));
        if (const wchar_t* s = code(src)) j += ",\"source\":" + jsonString(s);
        return j + ",\"format\":\"html\"}";
    }
    HttpRequest translateRequest(const Batch& b, const std::wstring& key, const std::wstring&) const override {
        HttpRequest rq = base(trimmed(key));
        std::vector<std::wstring> texts;
        for (const auto& t : b.texts) texts.push_back(t.text);
        rq.body = body(texts, b.src, b.tgt);  // no context field: each text stands alone
        return rq;
    }
    Status parseTranslate(const HttpResponse& rs, size_t count, std::vector<std::wstring>& out,
                          std::wstring* detail) const override {
        Json j;
        const Json* d = parseJson(rs.body, j) ? j.get(L"data") : nullptr;
        const Json* tr = d ? d->get(L"translations") : nullptr;
        if (!tr || tr->t != Json::T::Arr || tr->a.size() != count) {
            if (detail) *detail = L"unexpected Google answer";
            return Status::BadResponse;
        }
        out.clear();
        for (const auto& t : tr->a) out.push_back(t.get(L"translatedText") ? t.get(L"translatedText")->str() : std::wstring());
        return Status::Ok;
    }
    Status errorStatus(const HttpResponse& rs, std::wstring* detail) const override {
        Json j;
        parseJson(rs.body, j);
        if (detail) *detail = messageOf(rs, j);
        std::wstring t;
        if (const Json* e = j.get(L"error")) collectErrorText(*e, t);
        for (auto& ch : t) ch = static_cast<wchar_t>(towlower(ch));
        auto has = [&](const wchar_t* s) { return t.find(s) != std::wstring::npos; };
        if (rs.status >= 500) return Status::ServerError;
        if (rs.status == 429) return has(L"daily") || has(L"per day") ? Status::QuotaExceeded : Status::RateLimited;
        if (rs.status == 401) return Status::BadKey;
        if (rs.status == 403) {
            // Quota: 403 "User Rate Limit Exceeded" (per minute) / "Daily Limit Exceeded" (daily).
            if (has(L"rate limit") || has(L"per minute") || has(L"ratelimitexceeded")) return Status::RateLimited;
            if (has(L"daily") || has(L"quota") || has(L"resource_exhausted")) return Status::QuotaExceeded;
            // API not enabled / key limited to other APIs / no billing account.
            if (has(L"service_disabled") || has(L"accessnotconfigured") || has(L"has not been used") || has(L"is disabled") ||
                has(L"api_key_service_blocked") || has(L"billing"))
                return Status::ApiDisabled;
            return Status::BadKey;  // other key restrictions (IP address, referrer, app), no permission
        }
        if (rs.status == 400) {
            if (has(L"api key not valid") || has(L"api_key_invalid") || has(L"keyinvalid") || has(L"api key expired") ||
                has(L"api key not found"))
                return Status::BadKey;
            if (has(L"language") || has(L"invalid value")) return Status::Unsupported;
        }
        return Status::BadResponse;
    }
    TestResult testKey(const std::wstring& rawKey, const std::wstring&) const override {
        TestResult r;
        std::wstring key = trimmed(rawKey);
        if (key.empty()) return r;
        // A real (2-character) translation: it also shows whether the API is enabled for the key.
        HttpRequest rq = base(key);
        rq.body = body({L"OK"}, Lang::En, Lang::ZhHant);
        Status st;
        std::wstring detail;
        HttpResponse rs = sendWithRetry(rq, *this, &st, &detail);
        r.status = st;
        r.http = rs.status;
        r.detail = scrub(detail, key);
        if (st == Status::Ok) {
            std::vector<std::wstring> out;
            if (parseTranslate(rs, 1, out, &r.detail) != Status::Ok) {
                r.status = Status::BadResponse;
                return r;
            }
            r.sample = unmark(out[0], Marked{}, Markup::Html);
            r.plan = Plan::Google;
            r.detail = L"Google Cloud Translation";
        }
        return r;
    }
};

}  // namespace

std::unique_ptr<Adapter> makeAdapter(Provider p) {
    switch (p) {
    case Provider::DeepL: return std::make_unique<DeepL>();
    case Provider::Azure: return std::make_unique<Azure>();
    case Provider::Google: return std::make_unique<Google>();
    default: return nullptr;
    }
}

}  // namespace pm::translate::online
