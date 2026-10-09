// downloader.h: parallel Range segments over WinHTTP, resumable, SHA-256.
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "downloader.h"
#include "text_util.h"

namespace pm::translate::dl {

namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool fileSize(const std::wstring& path, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return true;
}

// SHA-256 of a file, the cancel flag checked between 1 MB chunks ("" = cancelled / unreadable).
std::string sha256File(const std::wstring& path, const std::atomic<bool>* cancel) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    std::string out;
    if (h != INVALID_HANDLE_VALUE && hash) {
        std::vector<char> buf(1 << 20);
        DWORD got = 0;
        bool stopped = false;
        while (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got) {
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), got, 0);
            if (cancel && cancel->load()) {
                stopped = true;
                break;
            }
        }
        unsigned char d[32] = {};
        if (!stopped && BCryptFinishHash(hash, d, sizeof(d), 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", d[i]);
            out.assign(hex, 64);
        }
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

struct Seg {
    uint64_t cur = 0, end = 0;  // [cur, end) still to fetch
    bool taken = false;
};

struct Conn;

struct File {
    const Item* it = nullptr;
    std::wstring part, side;
    HANDLE h = INVALID_HANDLE_VALUE;
    std::mutex mu;  // segs, single, owner, writes
    std::vector<Seg> segs;
    bool single = false;        // the server ignores Range: one stream from 0
    Conn* owner = nullptr;      // ... its connection
    bool ranged = false;        // a 206 was seen
    std::atomic<uint64_t> done{0};
    std::atomic<int> fails{0};  // in a row, without progress
    std::atomic<bool> failed{false};
    std::wstring err;
    int http = 0;               // a refused request's status
    int conns = 0, maxSeen = 0;
    Result res;
    bool finished = false;      // transfer complete (verify pending / done)

    uint64_t remaining() {
        uint64_t r = 0;
        for (const auto& s : segs) r += s.end > s.cur ? s.end - s.cur : 0;
        return r;
    }
    bool complete() { return remaining() == 0; }
    void save() {  // the sidecar (caller holds mu)
        std::ofstream o(side, std::ios::trunc);
        o << "pm-dl 1 " << it->size << "\n";
        for (const auto& s : segs)
            if (s.end > s.cur) o << s.cur << " " << s.end << "\n";
    }
};

struct Conn {
    File* f = nullptr;
    size_t seg = 0;
    std::atomic<HINTERNET> req{nullptr};
    std::atomic<uint64_t> bytes{0};
    std::atomic<bool> exited{false};
    std::thread th;
    int idle = 0;  // watcher: seconds without a byte
};

struct Shared {
    const std::atomic<bool>* cancel = nullptr;
    std::atomic<bool> stop{false};
    const Options* opt = nullptr;
    bool stopping() const { return stop.load() || (cancel && cancel->load()); }
};

void closeReq(Conn& c) {
    if (HINTERNET r = c.req.exchange(nullptr)) WinHttpCloseHandle(r);
}

// Sleeps ms unless stopping (checked every 20 ms).
void nap(Shared& sh, double ms) {
    const double t = nowMs() + ms;
    while (!sh.stopping() && nowMs() < t) Sleep(20);
}

// Takes an untaken segment with something left (caller holds f.mu); false if none.
bool takeSeg(File& f, Conn& c) {
    for (size_t i = 0; i < f.segs.size(); ++i)
        if (!f.segs[i].taken && f.segs[i].end > f.segs[i].cur) {
            f.segs[i].taken = true;
            c.seg = i;
            return true;
        }
    return false;
}

void worker(Shared& sh, File& f, Conn& c) {
    const Item& it = *f.it;
    const std::wstring whost = fromUtf8(it.host), wpath = fromUtf8(it.path);
    std::vector<char> buf(256 * 1024);
    for (;;) {
        if (sh.stopping() || f.failed) break;
        uint64_t from = 0, to = 0;
        bool singleMe = false;
        {
            std::lock_guard<std::mutex> l(f.mu);
            if (f.single && f.owner != &c) break;
            Seg& s = f.segs[c.seg];
            if (s.cur >= s.end) {
                s.taken = false;
                if (f.single || !takeSeg(f, c)) break;
                continue;
            }
            from = s.cur;
            to = s.end;
            singleMe = f.single;
        }
        HINTERNET ses = WinHttpOpen(sh.opt->userAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET con = ses ? WinHttpConnect(ses, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
        HINTERNET rq = con ? WinHttpOpenRequest(con, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                           : nullptr;
        bool progressed = false;
        std::wstring why;
        if (ses) WinHttpSetTimeouts(ses, 10000, 10000, 15000, 20000);
        if (rq) {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
            WinHttpSetOption(rq, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
            c.req = rq;
            if (sh.stopping()) closeReq(c);  // the closer may have run before the store
        }
        // Range always (also from 0: a server that does Range answers 206).
        const uint64_t off = it.offset;
        const std::wstring range = L"Range: bytes=" + std::to_wstring(off + from) + L"-" +
                                   (singleMe && !off ? L"" : std::to_wstring(off + to - 1)) + L"\r\n";
        DWORD code = 0, len = sizeof(code);
        const bool sent = rq && WinHttpSendRequest(rq, range.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                          WinHttpReceiveResponse(rq, nullptr) &&
                          WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                              &code, &len, WINHTTP_NO_HEADER_INDEX);
        if (!sent) {
            why = L"cannot reach " + whost + L" (error " + std::to_wstring(GetLastError()) + L")";
        } else if (code == 200 && off) {
            why = L"HTTP 200 without Range from " + whost;  // would send the whole package
            std::lock_guard<std::mutex> l(f.mu);
            f.err = why;
            f.http = 200;
            f.res.status = Status::Network;
            f.failed = true;
        } else if (code == 200) {
            // No Range: one stream from 0, owned by this connection.
            std::lock_guard<std::mutex> l(f.mu);
            if (!f.single) {
                f.single = true;
                f.owner = &c;
                f.segs.assign(1, Seg{0, it.size, true});
                c.seg = 0;
                f.done = 0;
            }
            if (f.owner == &c) from = f.segs[0].cur = 0, f.done = 0;
            else code = 0;  // another connection has it: this one stops below
            if (code == 0) {
                closeReq(c);
                WinHttpCloseHandle(con);
                WinHttpCloseHandle(ses);
                break;
            }
        } else if (code == 206) {
            std::lock_guard<std::mutex> l(f.mu);
            f.ranged = true;
        } else {
            why = L"HTTP " + std::to_wstring(code) + L" from " + whost;
            if (code >= 400 && code < 500 && code != 408 && code != 429) {  // refused: no retries
                std::lock_guard<std::mutex> l(f.mu);
                f.err = why;
                f.http = static_cast<int>(code);
                f.res.status = Status::Network;
                f.failed = true;
            }
        }
        if (why.empty()) {
            for (;;) {
                DWORD n = 0;
                HINTERNET live = c.req.load();  // null: closed by cancel
                if (!live || !WinHttpReadData(live, buf.data(), static_cast<DWORD>(buf.size()), &n)) {
                    if (!sh.stopping()) why = L"download interrupted (error " + std::to_wstring(GetLastError()) + L")";
                    break;
                }
                if (n == 0) {
                    std::lock_guard<std::mutex> l(f.mu);
                    if (f.segs[c.seg].cur < f.segs[c.seg].end) why = L"download interrupted (connection closed)";
                    break;
                }
                bool segDone = false, lost = false;
                {
                    std::lock_guard<std::mutex> l(f.mu);
                    if (f.single && f.owner != &c) {
                        lost = true;
                    } else {
                        Seg& s = f.segs[c.seg];
                        const uint64_t take = std::min<uint64_t>(n, s.end > s.cur ? s.end - s.cur : 0);
                        if (take) {
                            OVERLAPPED ov{};
                            ov.Offset = static_cast<DWORD>(s.cur);
                            ov.OffsetHigh = static_cast<DWORD>(s.cur >> 32);
                            DWORD w = 0;
                            if (!WriteFile(f.h, buf.data(), static_cast<DWORD>(take), &w, &ov) || w != take) {
                                f.err = L"cannot write " + f.part + L" (disk full?)";
                                f.res.status = Status::Disk;
                                f.failed = true;
                                lost = true;
                            } else {
                                s.cur += take;
                                f.done += take;
                                c.bytes += take;
                                progressed = true;
                            }
                        }
                        segDone = s.cur >= s.end;
                    }
                }
                if (lost || segDone || sh.stopping()) break;
            }
        }
        closeReq(c);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
        if (sh.stopping() || f.failed) break;
        if (!why.empty()) {
            if (progressed) f.fails = 0;
            const int k = ++f.fails;
            if (k > 6) {
                std::lock_guard<std::mutex> l(f.mu);
                f.err = why;
                if (f.res.status == Status::Ok) f.res.status = Status::Network;
                f.failed = true;
                break;
            }
            nap(sh, 500.0 * (1 << std::min(k - 1, 4)));
        } else if (progressed) {
            f.fails = 0;
        }
    }
    {
        std::lock_guard<std::mutex> l(f.mu);
        if (c.seg < f.segs.size()) f.segs[c.seg].taken = false;
    }
    c.exited = true;
}

// Loads the sidecar / an old single-stream .part, or starts new segments.
bool prepare(File& f, int startConns) {
    const Item& it = *f.it;
    uint64_t have = 0;
    const bool partThere = fileSize(f.part, have);
    std::vector<Seg> segs;
    std::ifstream in(f.side);
    std::string tag;
    int ver = 0;
    uint64_t sz = 0;
    if (partThere && have == it.size && in >> tag >> ver >> sz && tag == "pm-dl" && sz == it.size) {
        uint64_t a, b;
        while (in >> a >> b)
            if (a < b && b <= it.size) segs.push_back(Seg{a, b, false});
    } else if (partThere && have < it.size && have > 0 && !in.is_open()) {
        segs.push_back(Seg{have, it.size, false});  // an older single-stream .part: its start is good
    } else {
        DeleteFileW(f.part.c_str());
        have = 0;
    }
    in.close();
    if (segs.empty() && !(partThere && have == it.size && sz == it.size)) {
        const int n = it.size >= 8ull << 20 ? std::max(1, startConns) : 1;
        for (int i = 0; i < n; ++i) segs.push_back(Seg{it.size * i / n, it.size * (i + 1) / n, false});
    }
    f.h = CreateFileW(f.part.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) {
        f.err = L"cannot write " + f.part;
        f.res.status = Status::Disk;
        return false;
    }
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(it.size);
    if (!SetFilePointerEx(f.h, li, nullptr, FILE_BEGIN) || !SetEndOfFile(f.h)) {
        f.err = L"cannot write " + f.part + L" (disk full?)";
        f.res.status = Status::Disk;
        return false;
    }
    f.segs = std::move(segs);
    f.done = it.size - f.remaining();
    std::lock_guard<std::mutex> l(f.mu);
    f.save();
    return true;
}

std::mutex gStatsMu;
Stats gStats;

}  // namespace

uint64_t partialBytes(const Item& item) {
    const std::wstring part = item.dest + L".part", side = part + L".seg";
    uint64_t have = 0;
    if (!fileSize(part, have)) return 0;
    std::ifstream in(side);
    std::string tag;
    int ver = 0;
    uint64_t sz = 0;
    if (in >> tag >> ver >> sz && tag == "pm-dl" && sz == item.size) {
        uint64_t left = 0, a, b;
        while (in >> a >> b)
            if (a < b) left += b - a;
        return item.size > left ? item.size - left : 0;
    }
    return in.is_open() || have > item.size ? 0 : have;  // a sidecar we cannot read: starts over
}

Stats lastStats() {
    std::lock_guard<std::mutex> l(gStatsMu);
    return gStats;
}

std::vector<Result> fetchAll(const std::vector<Item>& items, const std::function<void(const Progress&)>& progress,
                             const std::atomic<bool>* cancel, const Options& o) {
    const double t0 = nowMs();
    Shared sh;
    sh.cancel = cancel;
    sh.opt = &o;
    std::vector<std::unique_ptr<File>> files;
    std::deque<Conn> conns;  // stable addresses
    uint64_t total = 0;
    for (const auto& it : items) {
        auto f = std::make_unique<File>();
        f->it = &it;
        f->part = it.dest + L".part";
        f->side = f->part + L".seg";
        total += it.size;
        files.push_back(std::move(f));
    }
    for (auto& f : files) {
        uint64_t sz = 0;
        if (fileSize(f->it->dest, sz) && sz == f->it->size) {  // there already (the caller re-checks the hash)
            f->finished = true;
            f->done = f->it->size;
            continue;
        }
        if (!prepare(*f, o.startConnections)) {
            f->failed = true;
            f->res.detail = f->err;
        }
    }
    auto spawn = [&](File& f, bool take) {
        int live = 0;
        for (auto& c : conns) live += !c.exited && c.f == &f ? 1 : 0;
        if (live >= o.maxConnections) return;
        conns.emplace_back();
        Conn& c = conns.back();
        c.f = &f;
        {
            std::lock_guard<std::mutex> l(f.mu);
            if (take && !takeSeg(f, c)) {
                c.exited = true;
                return;
            }
            if (f.single) f.owner = &c;  // the stream's connection is gone: this one continues it
        }
        f.conns = live + 1;
        f.maxSeen = std::max(f.maxSeen, f.conns);
        c.th = std::thread([&sh, &f, &c] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            worker(sh, f, c);
        });
    };
    int open = 0;
    for (auto& f : files) {
        if (f->finished || f->failed) continue;
        const int n = std::min<int>(static_cast<int>(f->segs.size()), std::max(1, o.startConnections));
        for (int i = 0; i < n && open < o.maxTotalConnections; ++i, ++open) spawn(*f, true);
    }
    // Watch: cancel (20 ms), progress (100 ms), sidecars + more connections (1 s).
    std::deque<std::pair<double, uint64_t>> samples;
    double lastProgress = 0, lastTick = nowMs(), cancelAt = -1;
    auto doneAll = [&] {
        uint64_t d = 0;
        for (auto& f : files) d += f->done.load();
        return d;
    };
    bool verifying = false;
    auto report = [&](bool force) {
        const double t = nowMs();
        if (!progress || (!force && t - lastProgress < 100)) return;
        lastProgress = t;
        Progress p;
        p.done = doneAll();
        p.total = total;
        samples.push_back({t, p.done});
        while (samples.size() > 2 && t - samples.front().first > 3000) samples.pop_front();
        if (samples.size() >= 2 && t - samples.front().first > 400) {
            p.bytesPerSec = (p.done - samples.front().second) * 1000.0 / (t - samples.front().first);
            if (p.bytesPerSec > 1) p.secondsLeft = (total - p.done) / p.bytesPerSec;
        }
        for (auto& c : conns) p.connections += c.exited ? 0 : 1;
        p.verifying = verifying;
        progress(p);
    };
    std::vector<uint64_t> lastBytes;
    for (;;) {
        bool running = false;
        for (auto& c : conns) running = running || !c.exited;
        if (!running) {
            // Segments left without a connection (a split nobody took): start one.
            bool more = false;
            if (!sh.stopping())
                for (auto& f : files) {
                    std::lock_guard<std::mutex> l(f->mu);
                    if (!f->failed && !f->finished && !f->complete()) more = true;
                }
            if (!more) break;
            for (auto& f : files)
                if (!f->failed && !f->finished && !f->complete()) spawn(*f, true);
            continue;
        }
        if (sh.stopping() && cancelAt < 0) {
            cancelAt = nowMs();
            sh.stop = true;
            for (auto& c : conns) closeReq(c);  // aborts blocking sends / reads
        }
        Sleep(20);
        report(false);
        if (nowMs() - lastTick >= 1000 && !sh.stopping()) {
            const double dt = (nowMs() - lastTick) / 1000.0;
            lastTick = nowMs();
            lastBytes.resize(conns.size(), 0);
            int openNow = 0;
            for (auto& c : conns) openNow += c.exited ? 0 : 1;
            size_t ci = 0;
            std::vector<double> rate(files.size(), 0);
            std::vector<int> live(files.size(), 0);
            for (auto& c : conns) {
                const uint64_t b = c.bytes.load();
                // A stalled connection (no byte for 8 s): closed, its worker reconnects.
                c.idle = c.exited || b != lastBytes[ci] ? 0 : c.idle + 1;
                if (c.idle >= 8) {
                    closeReq(c);
                    c.idle = 0;
                }
                for (size_t k = 0; k < files.size(); ++k)
                    if (files[k].get() == c.f && !c.exited) {
                        rate[k] += (b - lastBytes[ci]) / dt;
                        ++live[k];
                    }
                lastBytes[ci++] = b;
            }
            for (size_t k = 0; k < files.size(); ++k) {
                File& f = *files[k];
                std::lock_guard<std::mutex> l(f.mu);
                if (f.failed || f.finished) continue;
                f.save();
                f.conns = live[k];
                // More connections while each is slow (a throttled host).
                const bool slow = live[k] > 0 && rate[k] / live[k] < 1.5e6;
                if (!f.single && f.ranged && slow && live[k] < o.maxConnections && openNow < o.maxTotalConnections) {
                    size_t best = 0;
                    uint64_t left = 0;
                    for (size_t i = 0; i < f.segs.size(); ++i)
                        if (f.segs[i].end - std::min(f.segs[i].cur, f.segs[i].end) > left) {
                            left = f.segs[i].end - f.segs[i].cur;
                            best = i;
                        }
                    // Halves of >= 1 MB (256 KB near the end, so the last pieces do not crawl on one connection).
                    const uint64_t minSplit = f.remaining() < (8ull << 20) ? (512ull << 10) : (2ull << 20);
                    if (left >= minSplit) {
                        Seg& s = f.segs[best];
                        const uint64_t mid = s.cur + left / 2;
                        f.segs.push_back(Seg{mid, s.end, false});
                        f.segs[best].end = mid;
                    }
                }
            }
            // Spawn outside the file locks for the new segments.
            for (auto& f : files) {
                if (f->failed || f->finished || f->single) continue;
                bool untaken = false;
                {
                    std::lock_guard<std::mutex> l(f->mu);
                    for (const auto& s : f->segs) untaken = untaken || (!s.taken && s.end > s.cur);
                }
                if (untaken && openNow < o.maxTotalConnections) {
                    spawn(*f, true);
                    ++openNow;
                }
            }
        }
    }
    for (auto& c : conns)
        if (c.th.joinable()) c.th.join();
    // Close, verify, move.
    std::vector<Result> out(items.size());
    for (size_t k = 0; k < files.size(); ++k) {
        File& f = *files[k];
        if (f.h != INVALID_HANDLE_VALUE) {
            std::lock_guard<std::mutex> l(f.mu);
            if (!f.finished) f.save();
            CloseHandle(f.h);
            f.h = INVALID_HANDLE_VALUE;
        }
        Result& r = out[k];
        if (f.finished) continue;  // was there already
        if (f.failed) {
            r.status = f.res.status == Status::Ok ? Status::Network : f.res.status;
            r.detail = f.err;
            r.http = f.http;
            continue;
        }
        if (cancel && cancel->load()) {
            r.status = Status::Cancelled;
            r.detail = L"cancelled";
            continue;
        }
        if (!f.complete()) {
            r.status = Status::Network;
            r.detail = L"download interrupted";
            continue;
        }
        if (f.it->sha256.empty()) {  // tests only (a byte range of a big file): kept as .part
            DeleteFileW(f.side.c_str());
            continue;
        }
        if (!verifying) {
            verifying = true;
            report(true);
        }
        const std::string h = sha256File(f.part, cancel);
        if (h.empty() && cancel && cancel->load()) {
            r.status = Status::Cancelled;
            r.detail = L"cancelled";
            continue;
        }
        if (h != f.it->sha256) {
            DeleteFileW(f.part.c_str());
            DeleteFileW(f.side.c_str());
            r.status = Status::Verify;
            r.detail = L"SHA-256";
            continue;
        }
        DeleteFileW(f.side.c_str());
        DeleteFileW(f.it->dest.c_str());
        if (!MoveFileExW(f.part.c_str(), f.it->dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            r.status = Status::Disk;
            r.detail = L"cannot move " + f.part;
        }
    }
    report(true);
    {
        std::lock_guard<std::mutex> l(gStatsMu);
        gStats = Stats{};
        gStats.seconds = (nowMs() - t0) / 1000;
        gStats.cancelMs = cancelAt >= 0 ? nowMs() - cancelAt : -1;
        for (auto& f : files) {
            gStats.connectionsPerItem.push_back(f->maxSeen);
            gStats.ranged.push_back(f->ranged);
            gStats.maxConnections = std::max(gStats.maxConnections, f->maxSeen);
        }
    }
    return out;
}

}  // namespace pm::translate::dl
