// 自動更新: fetch a JSON manifest {version, url, sha256, notes} over
// HTTP(S) with WinHTTP, download the installer to %TEMP% and verify its
// SHA-256. Blocking functions: call them on a worker thread. See docs/app.md.
#pragma once

#include <string>

namespace pm::update {

struct Manifest {
    std::string version;  // "0.4.1"
    std::string url;      // installer download URL
    std::string sha256;   // 64 hex digits (any case)
    std::string notes;    // UTF-8, may be empty
};

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

}  // namespace pm::update
