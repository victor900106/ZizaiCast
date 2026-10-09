// Online translation (opt-in, bring your own API key) - settings, keys, the
// 「測試連線」 checks for the settings page, and the picture-level batch call
// for the escalator.  The engine itself is the ITranslator returned by
// pm_create_online_translator() (pm/translator.h,
// translate/src/online_engine.cpp).
//
// Privacy promises (launch/_work/tr_arch/ARCHITECTURE.md §3.7.3):
//   - off by default; nothing is sent until the user turned it on, accepted
//     the consent dialog FOR THE CHOSEN PROVIDER (Settings::consented) and
//     gave a key;
//   - only the recognised text (and, for DeepL, the text next to it as
//     context) is sent - never the picture - over HTTPS, to the provider the
//     user picked; results are cached in memory only;
//   - the key is encrypted with Windows DPAPI (CryptProtectData, this
//     Windows user) in %LOCALAPPDATA%\PhoneMirror\online_translate.ini and
//     never written to a log or an error;
//   - never an automatic fallback: offline failures do not go online unless
//     the user turned it on.
//
// Localisation: nothing here returns text meant for the user.  Every outcome
// is an enum (Status, StoreError, Plan) with a stable id (statusId(), …) the
// UI maps to its own strings; `detail` fields are technical (for a
// 「詳細資料」 link only, English / the provider's words, never the key).
//
// Settings and the key-present flags are cached in memory and re-read only
// when the file changes (size / time stamp), so the engine's per-segment
// calls (id / supports / installed) do no parsing and no DPAPI work.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pm/translator.h"

namespace pm::translate::online {

enum class Provider { None, DeepL, Azure };
// Off: never sent.  Escalate: only segments that failed the quality checks
// (the escalator's step 4).  All: every segment goes online first (the
// Bergamot result is the fallback where the request fails) - see prefetch().
enum class Mode { Off, Escalate, All };

// Bump when the consent dialog text changes (the user is asked again).
constexpr int kConsentVersion = 1;

struct Settings {
    Mode mode = Mode::Off;
    Provider provider = Provider::None;
    // The consent dialog version the user accepted, per provider (0: none).
    // Consent is given to a company: switching provider needs it again.
    int consentDeepL = 0, consentAzure = 0;
    std::wstring azureRegion;      // Azure resource region ("eastasia", "japaneast"…); "" = global resource

    int consentFor(Provider p) const { return p == Provider::DeepL ? consentDeepL : p == Provider::Azure ? consentAzure : 0; }
    void setConsent(Provider p, int version = kConsentVersion) {
        if (p == Provider::DeepL) consentDeepL = version;
        if (p == Provider::Azure) consentAzure = version;
    }
    bool consented(Provider p) const { return consentFor(p) >= kConsentVersion; }
    // The UI shows the consent dialog before saving when this is true.
    bool needsConsent() const { return mode != Mode::Off && provider != Provider::None && !consented(provider); }
    bool enabled() const { return mode != Mode::Off && provider != Provider::None && consented(provider); }
};

// Why storing settings / a key failed (the UI maps it to its own text).
enum class StoreError {
    None,
    NoSettingsFolder,   // %LOCALAPPDATA% not found
    WriteFailed,        // could not write / replace the file
    EncryptFailed,      // DPAPI refused to encrypt the key
    InvalidProvider,    // setKey(Provider::None, …)
};
const char* storeErrorId(StoreError e);  // "ok", "no-folder", "write-failed", "encrypt-failed", "invalid-provider"

// %LOCALAPPDATA%\PhoneMirror\online_translate.ini (PM_ONLINE_CONFIG overrides
// the path, for tests).  Missing / unreadable file: defaults (Off).  Cached.
// A file written by an older build (one "consent" value) counts as consent
// for the provider that was saved with it only.
Settings loadSettings();
bool saveSettings(const Settings& s, StoreError* err = nullptr);

// Keys (one per provider), DPAPI-encrypted in the same file.  setKey("")
// removes it; spaces / line breaks around a pasted key are dropped.  The
// plain key is never returned except to the engine.
bool setKey(Provider p, const std::wstring& key, StoreError* err = nullptr);
bool hasKey(Provider p);  // a key is stored and decrypts for this Windows user (cached)
// "••••3f9a:fx" style hint for the settings page (last 4 characters, and
// DeepL's ":fx" free-plan suffix), "" without a key.
std::wstring keyHint(Provider p);
// Turns it all off: mode Off, consent and keys removed, cache, back-off and
// lastStatus() cleared.
void forgetAll();

// Outcome of a request (testKey, lastStatus, prefetch).
enum class Status {
    Ok,
    NotConfigured,   // off, no consent for this provider, or no key
    BadKey,          // 401 / 403 (wrong key, wrong Azure region, key of another product)
    QuotaExceeded,   // DeepL 456 (character allowance of the plan used up), Azure 403001 (free tier used up)
    RateLimited,     // 429 after the retries
    Timeout,         // no answer in time
    Network,         // no connection / DNS / TLS / proxy
    ServerError,     // 5xx after the retries
    Unsupported,     // language pair not offered by the provider
    BadResponse,     // unexpected answer
};
// "ok", "not-configured", "bad-key", "quota", "rate-limited", "timeout",
// "network", "server-error", "unsupported", "bad-response".
const char* statusId(Status s);

// What the key test found out about the account (UI text from this, not from detail).
enum class Plan { Unknown, DeepLFree, DeepLPro, Azure };
const char* planId(Plan p);  // "unknown", "deepl-free", "deepl-pro", "azure"

struct TestResult {
    Status status = Status::NotConfigured;
    int http = 0;                   // HTTP status (0: none)
    Plan plan = Plan::Unknown;      // set when the key works
    std::wstring detail;            // technical detail for 「詳細資料」 (never contains the key)
    std::wstring sample;            // a short sample translation (Azure; DeepL: "")
    int64_t used = -1, limit = -1;  // characters used / allowed (DeepL /v2/usage; -1 unknown).
                                    // Not necessarily "this month": DeepL's current free
                                    // plan is a one-time allowance - do not say "monthly".
};
// Checks a key typed in (before it is saved) with one tiny request: DeepL
// GET /v2/usage (no characters billed), Azure a 2-character translation.
// Blocking (up to ~25 s): call from a worker thread.  Sends no screen text.
// region: Azure only.  Does not touch the back-off of the stored key.
TestResult testKey(Provider p, const std::wstring& key, const std::wstring& region = L"");
// The same with the stored key, decrypted inside the library (the UI never
// sees it).  The first form uses the saved Azure region, the second the one
// in the region box (may be unsaved).  No stored key: NotConfigured, nothing
// sent.  The result also updates the back-off: Ok clears it, BadKey /
// QuotaExceeded latch it like a failed translation.
TestResult testStoredKey(Provider p);
TestResult testStoredKey(Provider p, const std::wstring& region);

// Status of the engine's last request (Ok before any), for a toast like
// 「線上翻譯額度已用完，已改用離線翻譯」.
Status lastStatus();
struct LastError {
    Status status = Status::Ok;
    Provider provider = Provider::None;
    std::wstring detail;            // technical (「詳細資料」), never the key
};
LastError lastError();

// Back-off (circuit breaker), per provider: after a failure that would hit
// every later segment too, the provider is not asked again for a while and
// installed() is false, so the escalator skips the step at once instead of
// waiting through timeouts / retries for each segment:
//   BadKey         until the key or the Azure region changes (setKey,
//                  saveSettings, a successful testStoredKey, resetBackoff)
//   QuotaExceeded  1 hour (or until the key changes)
//   Network / Timeout   60 s;  RateLimited / ServerError   30 s
// blockedStatus: Ok = not blocked, else the cause; *remainingMs = -1 for
// "until the key changes".
Status blockedStatus(Provider p, int64_t* remainingMs = nullptr);
void resetBackoff();  // 「重試」: forget every back-off now

// The mode in effect right now: Off unless enabled(), a key is stored and
// the provider is not backed off.  The escalator reads this once per
// picture (cheap: cached).
Mode activeMode();

// Picture-level batch: translates the segments in as few requests as the
// provider allows (DeepL 50 texts, Azure 1,000 texts per request; one per
// language pair, contexts of a batch combined) and keeps the results in the
// memory cache, so later per-segment ITranslator::translate calls with the
// same TrRequest (text, src, tgt, context, keep, terms) are cache hits and
// send nothing.  Blocking; worker thread only.  Nothing is sent when
// activeMode() is Off.  out (optional): one hypothesis per request, text ""
// where it failed.  Use:
//   Mode::All:      every segment of the picture, before Bergamot; take
//                   out[i] where it passes the checks, Bergamot for the rest.
//   Mode::Escalate: the segments that failed the checks after the Bergamot
//                   pass of a picture, before step 4 runs for each of them.
Status prefetch(const std::vector<TrRequest>& in, std::vector<TrHypothesis>* out = nullptr);

// Display data for the settings page / consent dialog (URLs and the
// provider's own name; not translated).
const wchar_t* providerName(Provider p);       // "DeepL", "Microsoft Azure Translator"
const char* providerId(Provider p);            // "deepl", "azure", "none"
const wchar_t* providerTermsUrl(Provider p);   // terms of service
const wchar_t* providerPrivacyUrl(Provider p); // privacy / data handling
const wchar_t* providerSignupUrl(Provider p);  // where the user gets a key

// Clears the in-memory translation cache (also done by setKey / forgetAll).
void clearCache();

}  // namespace pm::translate::online
