// Internals of the online translation engine (translate/src/online_engine*.cpp):
// HTTP over WinHTTP, a small JSON reader / writer, placeholder markup, and the
// provider adapters (DeepL, Azure Translator).  Public side:
// pm/online_translate.h (settings / keys / testKey), pm/translator.h (engine).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pm/online_translate.h"
#include "pm/translate.h"
#include "pm/translator.h"

namespace pm::translate::online {

// ---- HTTP ----
struct HttpRequest {
    std::wstring method = L"POST";
    std::wstring host;              // "api-free.deepl.com"
    uint16_t port = 443;
    bool https = true;              // plain http only for 127.0.0.1 (tests; refused otherwise)
    std::wstring path;              // "/v2/translate?…"
    std::vector<std::pair<std::wstring, std::wstring>> headers;  // may hold the key: never logged
    std::string body;               // UTF-8
    unsigned timeoutMs = 10000;     // answer timeout (resolve / connect 5 s, send 10 s)
};
struct HttpResponse {
    int status = 0;                 // 0: no HTTP answer (see winError)
    std::string body;
    unsigned winError = 0;          // WinHTTP / Win32 error when status == 0
    unsigned retryAfterSec = 0;     // Retry-After header (0: none)
    bool timedOut() const;
};
bool httpSend(const HttpRequest& rq, HttpResponse& rs);

// Test hook: send every provider request to http://127.0.0.1:port instead
// of the provider's HTTPS host (0: off).  Only loopback is possible.
void setLoopbackPortForTest(uint16_t port);
uint16_t loopbackPortForTest();
// Test hook: shorter retry waits (ms per step; default 500).
void setRetryBaseMsForTest(unsigned ms);
// Test hook: answer timeout in ms for every request (0: the request's own).
void setTimeoutMsForTest(unsigned ms);
// Test hook: moves the back-off clock forward (ms added to "now").
void setClockOffsetMsForTest(int64_t ms);

// ---- JSON (only what the providers' answers need) ----
struct Json {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::vector<Json> a;
    std::vector<std::pair<std::wstring, Json>> o;
    const Json* get(const wchar_t* key) const;  // object member or nullptr
    const Json& at(size_t i) const;             // array element (Null when out of range)
    std::wstring str() const { return t == T::Str ? s : std::wstring(); }
};
bool parseJson(const std::string& utf8, Json& out);
std::string jsonString(const std::wstring& s);  // "…" (UTF-8, escaped)

std::string toUtf8(const std::wstring& s);
std::wstring fromUtf8(const std::string& s);

// ---- Placeholders / protected text ----
// The source with every protected piece (ZQA..ZQZ placeholders, the request's
// keep strings, glossary terms) replaced by markup the provider leaves alone,
// and the rest escaped for XML / HTML.
struct Marked {
    std::wstring text;
    std::vector<std::wstring> pieces;  // piece i is <k>i</k> / <span class="notranslate">…</span>
};
enum class Markup { DeepLXml, Html };
Marked markUp(const TrRequest& r, Markup m);
// Back to plain text: markup -> pieces, entities unescaped.  *lost = pieces
// that are missing or doubled in the answer.
std::wstring unmark(const std::wstring& answer, const Marked& mk, Markup m, int* lost = nullptr);

// ---- Providers ----
// Azure Text Translation API version.  "3.0" is still supported and
// documented (Learn, checked 2026-10-09: no retirement date announced).  The
// GA "2026-06-06" version is NOT a drop-in replacement (body "inputs" /
// "targets", answer "value", Detect / BreakSentence removed); migrate in its
// own change with a full validation pass once v3.0 gets a retirement date.
// https://learn.microsoft.com/azure/ai-services/translator/text-translation/how-to/migrate-to-2026-06-06
constexpr const wchar_t* kAzureApiVersion = L"3.0";

struct Batch {
    Lang src = Lang::Unknown, tgt = Lang::Unknown;
    std::wstring context;           // DeepL: the batch's combined context (not translated, not billed)
    std::vector<Marked> texts;
};
class Adapter {
public:
    virtual ~Adapter() = default;
    virtual Provider provider() const = 0;
    virtual const char* engineId() const = 0;          // "online-deepl"
    virtual Markup markup() const = 0;
    virtual bool supports(Lang src, Lang tgt) const = 0;
    virtual size_t maxTexts() const = 0;               // per request
    virtual size_t maxChars() const = 0;               // per request (UTF-16 units of the marked text)
    virtual HttpRequest translateRequest(const Batch& b, const std::wstring& key, const std::wstring& region) const = 0;
    // Answers in the batch's order (marked text, before unmark).
    virtual Status parseTranslate(const HttpResponse& rs, size_t count, std::vector<std::wstring>& out,
                                  std::wstring* detail) const = 0;
    virtual TestResult testKey(const std::wstring& key, const std::wstring& region) const = 0;
    // Status of an error answer (status != 200).
    virtual Status errorStatus(const HttpResponse& rs, std::wstring* detail) const = 0;
};
std::unique_ptr<Adapter> makeAdapter(Provider p);

// Removes the key (and any long prefix of it) from a text before it is
// shown or logged.
std::wstring scrub(std::wstring text, const std::wstring& key);
// Engine-internal: the plain key (DPAPI decrypted), "" if none.
std::wstring loadKey(Provider p);
void setLastStatus(Status s);
// Retries: transient failures (timeout, network, 429, 5xx) are tried again
// up to 2 more times; key / quota / bad-request errors are not.
bool retryable(Status s);
HttpResponse sendWithRetry(const HttpRequest& rq, const Adapter& ad, Status* st, std::wstring* detail);

}  // namespace pm::translate::online
