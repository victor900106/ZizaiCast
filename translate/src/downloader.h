// Shared HTTPS downloader for the on-demand files (local LLM runtime / GPU
// backend / model, OCR GPU add-on, models).  WinHTTP, no window, no proxy
// prompts (the system's automatic proxy).
//
//  - Several files at once, each over parallel HTTP Range segments: starts
//    with 2 connections a file, adds one (splitting the largest remaining
//    segment) every second while a connection gets < 1.5 MB/s, up to 8 a
//    file (GitHub release assets are throttled per connection: ~73 KB/s).
//    A server answering 200 instead of 206: one plain stream.
//  - Resumable: DEST.part (preallocated) + DEST.part.seg (the segments still
//    to fetch, rewritten every second); an interrupted / cancelled download
//    continues where it stopped.
//  - SHA-256 of the whole file before it is moved to DEST (a mismatch: both
//    deleted, error "SHA-256").
//  - Retries with backoff (0.5, 1, 2, 4, 8 s) per segment; fails after 6 in a
//    row without progress.
//  - Cancel: the flag is watched every 20 ms; the open requests are closed
//    (WinHttpCloseHandle aborts blocking calls), so the call returns within
//    ~100 ms.  The .part files are kept.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pm::translate::dl {

struct Item {
    std::string host;          // "github.com"
    std::string path;          // "/ggml-org/…/file.zip"
    std::wstring dest;         // final path (written as dest + ".part" first)
    uint64_t size = 0;         // exact, from the pin
    std::string sha256;        // lowercase hex ("" = tests: not verified, left as .part)
    uint64_t offset = 0;       // > 0: the bytes [offset, offset + size) of the resource (a zip entry
                               // inside a package); the server must do Range
};

struct Progress {
    uint64_t done = 0, total = 0;  // bytes over all items (including what earlier tries got)
    double bytesPerSec = 0;        // over the last ~3 s
    double secondsLeft = -1;       // -1: not known yet
    int connections = 0;           // open now
    bool verifying = false;        // every byte is in: the SHA-256 check runs (「驗證中…」)
    double fraction() const { return total ? static_cast<double>(done) / total : 0; }
};

enum class Status { Ok, Cancelled, Network, Verify, Disk };

struct Result {
    Status status = Status::Ok;
    std::wstring detail;       // technical message ("" when ok)
    int http = 0;              // the HTTP status of a refused request (404, 403 …; 0 = none)
};

struct Options {
    int startConnections = 2;  // a file
    int maxConnections = 8;    // a file
    int maxTotalConnections = 16;
    const wchar_t* userAgent = L"ZizaiCast/0.7";
};

// Downloads every item (already complete + verified ones are skipped).
// progress: on the calling thread, at most every ~100 ms and at the end.
// Blocking; returns one Result per item.  cancel: set from any thread.
std::vector<Result> fetchAll(const std::vector<Item>& items, const std::function<void(const Progress&)>& progress,
                             const std::atomic<bool>* cancel, const Options& o = {});

// Bytes already on disk for item (its .part), for "繼續下載（約 …）".
uint64_t partialBytes(const Item& item);

// Test hook: the measured numbers of the last fetchAll (pm_llm_eval --dl-test).
struct Stats {
    double seconds = 0, cancelMs = -1;  // cancelMs: from the flag to the return
    int maxConnections = 0;
    std::vector<int> connectionsPerItem;
    std::vector<bool> ranged;           // the server did Range
};
Stats lastStats();

}  // namespace pm::translate::dl
