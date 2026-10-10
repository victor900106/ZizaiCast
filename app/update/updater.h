// 自動更新: fetch a JSON manifest {version, url, sha256, notes [, date, size,
// changes_zh, changes_en]} over HTTP(S) with WinHTTP, download the installer
// to %TEMP% and verify its SHA-256. Blocking functions: call them on a worker
// thread. See docs/app.md.
#pragma once

#include <string>
#include <vector>

namespace pm::update {

struct Manifest {
    std::string version;  // "0.4.1"
    std::string url;      // installer download URL
    std::string sha256;   // 64 hex digits (any case)
    std::string notes;    // UTF-8, may be empty (one-line summary, read by every client)
    // Optional (0.7.0+; 0.6.x apps skip these keys): release date "2026-10-08",
    // installer size in bytes (0 = unknown), 「這次更新了什麼」 bullets (UTF-8).
    std::string date;
    unsigned long long size = 0;
    std::vector<std::string> changesZh, changesEn;
    std::vector<std::string> changesJa, changesKo;  // 0.7.0, optional (else changes_en)
};

// The strings of a top-level array `key` (["a", "b"]); other elements are
// skipped. Empty if missing or not an array.
std::vector<std::string> jsonStringArray(const std::string& json, const std::string& key);

// A top-level non-negative integer `key` (a string of digits is accepted
// too). 0 if missing.
unsigned long long jsonNumber(const std::string& json, const std::string& key);

// Fills every manifest field found in `json` (a leading UTF-8 BOM is
// skipped; blank bullets dropped, at most 40 kept); no validation. Also used
// for a local installer's sidecar <installer>.json (same schema).
void parseManifest(std::string json, Manifest& m);

// Reads a sidecar file (<= 64 KB) into `m`; false if missing / unreadable.
bool readManifestFile(const std::wstring& file, Manifest& m);

// Compares dotted numeric versions ("0.4.0" < "0.4.10"). Missing parts are 0;
// a non-numeric suffix in a part is ignored.
int compareVersions(const std::string& a, const std::string& b);

// Minimal flat-JSON reader: the string value of `key` in a top-level object
// (handles \" \\ \/ \n \t \uXXXX incl. surrogate pairs). Empty if missing.
std::string jsonString(const std::string& json, const std::string& key);

// GET `url` into memory (at most maxBytes). Returns false with `error` set.
bool httpGet(const std::wstring& url, std::string& body, std::string& error, size_t maxBytes = 1 << 20);

// Fetches and parses the manifest; false if unreachable or incomplete
// (version / url / sha256 missing, sha256 not 64 hex digits).
bool fetchManifest(const std::wstring& url, Manifest& m, std::string& error);

// Downloads `url` to `file` (overwritten). `progress(done, total)` may be
// null; total is 0 when unknown. Returns false (and removes the partial file)
// on failure.
bool download(const std::wstring& url, const std::wstring& file, std::string& error,
              void (*progress)(unsigned long long done, unsigned long long total, void* ctx) = nullptr,
              void* ctx = nullptr);

// 本機更新: the version of a 自在投影 installer from its VERSIONINFO
// (ProductName must be 「自在投影」; ProductVersion, else FileVersion, else
// the fixed file version; a trailing ".0" fourth part is dropped, so
// "0.5.3.0" -> "0.5.3"). False if unreadable or another product.
bool installerVersion(const std::wstring& file, std::string& version);

// Lower-case hex SHA-256 of a file ("" on error).
std::string sha256File(const std::wstring& file);
// Same, from an open handle (read from the start; the position moves).
std::string sha256Handle(void* fileHandle);

// Update URLs (manifest and installer) must be https://. Plain http:// is
// accepted only for this PC (localhost, 127.x.x.x, [::1]: a dev / test
// server); anything else is refused with `why` set. request() enforces it
// (and allows no redirect away from a loopback http URL).
bool urlAllowed(const std::wstring& url, std::string& why);

// Authenticode of an installer (WinVerifyTrust, no UI, no network
// retrieval). The app's installers are not code-signed today: Unsigned is
// fine (SHA-256 + https are the check); a file that IS signed must verify.
enum class Signature { Unsigned, Valid, Invalid };
Signature checkSignature(const std::wstring& file, void* fileHandle, std::string& detail);

}  // namespace pm::update
