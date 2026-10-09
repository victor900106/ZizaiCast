// Shared HTTPS downloader for the on-demand files (local LLM runtime / GPU
// backend / model, OCR GPU add-on, models).  WinHTTP, no window, no proxy
// prompts (the system's automatic proxy).
//
//  - Several files at once, each over parallel HTTP Range segments.  Items
//    with priority > 0 (the small files on a slow host - GitHub release
//    assets are throttled per connection, ~50-70 KB/s each) start first and
//    with more connections; a host measured slow (< 1.5 MB/s a connection)
//    gets its file's full share at once.  Up to 16 connections in all; when
//    an item finishes its share goes to the ones left (a single item left may
//    use all 16), and the remaining ranges are split finer near the end
//    (work stealing), so the last pieces do not crawl on one connection.
//    A server answering 200 instead of 206: one plain stream.
//  - Mirrors: Item::mirrors are tried first, the original last; a refused
//    request (404 …) moves to the next URL at once, repeated failures after
//    a few retries.  Every URL must serve the same bytes (same SHA-256 pin).
//  - Resumable: DEST.part (preallocated) + DEST.part.seg (the segments still
//    to fetch, rewritten every second); an interrupted / cancelled download
//    continues where it stopped.
//  - SHA-256 of the whole file before it is moved to DEST (a mismatch: both
//    deleted, error "SHA-256").
//  - Retries with backoff (0.5, 1, 2, 4, 8 s) per segment; fails after 6 in a
//    row without progress.
//  - Cancel: the flag is watched every 20 ms; the open requests are closed
//    (WinHttpCloseHandle aborts blocking calls) and the call returns within
//    ~0.2 s (a connection still stuck in the OS is left to finish on its own,
//    writing nothing).  The .part files are kept.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pm::translate::dl {

struct Url {
    std::string host;          // "victor900106.github.io"
    std::string path;          // "/ZizaiCast/addons/file.zip"
};

struct Item {
    std::string host;          // the original: "github.com"
    std::string path;          // "/ggml-org/…/file.zip"
    std::wstring dest;         // final path (written as dest + ".part" first)
    uint64_t size = 0;         // exact, from the pin
    std::string sha256;        // lowercase hex ("" = tests: not verified, left as .part)
    uint64_t offset = 0;       // > 0: the bytes [offset, offset + size) of the resource (a zip entry
                               // inside a package); the server must do Range
    std::vector<Url> mirrors;  // tried first, in order (same bytes; for a byte-range item: the same
                               // package, so the same offset)
    std::string label;         // what it is, for "still downloading: …" ("runtime", "gpu", "model")
    int priority = 0;          // > 0: started first, more connections (small files on a slow host)
};

struct Progress {
    uint64_t done = 0, total = 0;  // bytes over all items (including what earlier tries got)
    double bytesPerSec = 0;        // over the last ~3 s
    double secondsLeft = -1;       // -1: not known yet
    int connections = 0;           // open now
    bool verifying = false;        // every byte is in: the SHA-256 check runs (「驗證中…」)
    std::vector<std::string> pending;  // labels of the items still downloading (item order)
    double fraction() const { return total ? static_cast<double>(done) / total : 0; }
};

enum class Status { Ok, Cancelled, Network, Verify, Disk };

struct Result {
    Status status = Status::Ok;
    std::wstring detail;       // technical message ("" when ok)
    int http = 0;              // the HTTP status of a refused request (404, 403 …; 0 = none)
    int url = -1;              // the URL that delivered it: index into mirrors, or mirrors.size() = the original
};

struct Options {
    int startConnections = 2;  // a file (priority items: prioConnections)
    int prioConnections = 8;
    int maxConnections = 8;    // a file while other files are still downloading (priority items: more)
    int maxTotalConnections = 16;
    double slowBytesPerSec = 1.5e6;  // a connection below this: the host is "slow"
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
    std::vector<double> itemSeconds;    // when each item had all its bytes (from the start)
};
Stats lastStats();

}  // namespace pm::translate::dl
