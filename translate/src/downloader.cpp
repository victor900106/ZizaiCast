// downloader.h: parallel Range segments over WinHTTP, mirrors, resumable, SHA-256.
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
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

// Per host, the bytes a second one connection got last time (this process):
// a host known slow gets its file's full share of connections at once.
std::mutex gHostMu;
std::map<std::string, double> gHostRate;

double hostRate(const std::string& host) {
    std::lock_guard<std::mutex> l(gHostMu);
    auto i = gHostRate.find(host);
    return i == gHostRate.end() ? -1 : i->second;
}
void noteHostRate(const std::string& host, double perConn) {
    std::lock_guard<std::mutex> l(gHostMu);
    auto i = gHostRate.find(host);
    gHostRate[host] = i == gHostRate.end() ? perConn : 0.7 * i->second + 0.3 * perConn;
}

struct Seg {
    uint64_t cur = 0, end = 0;  // [cur, end) still to fetch
    bool taken = false;
};

struct Conn;

struct File {
    Item it;
    std::vector<Url> urls;      // mirrors, then the original
    std::wstring part, side;
    HANDLE h = INVALID_HANDLE_VALUE;
    std::mutex mu;              // everything below except the atomics
    std::vector<Seg> segs;
    size_t url = 0;             // the URL in use
    bool single = false;        // the server ignores Range: one stream from 0
    Conn* owner = nullptr;      // ... its connection
    bool ranged = false;        // a 206 was seen
    std::atomic<uint64_t> done{0};
    std::atomic<int> fails{0};  // unused (per connection now)
    std::atomic<double> lastProgress{0};  // ms of the last byte written (any connection)
    std::atomic<bool> failed{false};
    std::wstring err;
    int http = 0;               // a refused request's status
    Status failStatus = Status::Network;
    bool there = false;         // dest was already complete
    double doneAt = -1;         // ms from the start when all bytes were in
    int maxSeen = 0;

    uint64_t remaining() const {
        uint64_t r = 0;
        for (const auto& s : segs) r += s.end > s.cur ? s.end - s.cur : 0;
        return r;
    }
    bool complete() const { return remaining() == 0; }
    void save() {  // the sidecar (caller holds mu)
        std::ofstream o(side, std::ios::trunc);
        o << "pm-dl 1 " << it.size << "\n";
        for (const auto& s : segs)
            if (s.end > s.cur) o << s.cur << " " << s.end << "\n";
    }
    // The next URL after `from` failed (caller holds mu): false when it was the last.
    bool nextUrl(size_t from) {
        if (url != from) return true;  // another connection moved on already
        if (url + 1 >= urls.size()) return false;
        ++url;
        fails = 0;
        lastProgress = nowMs();  // the next URL gets its own time
        if (single) {  // the next server may do Range
            single = false;
            owner = nullptr;
        }
        return true;
    }
    void fail(const std::wstring& why, int code, Status st = Status::Network) {  // caller holds mu
        if (failed) return;
        err = why;
        http = code;
        failStatus = st;
        failed = true;
    }
};

struct Conn {
    std::shared_ptr<File> f;
    size_t seg = 0;
    std::atomic<HINTERNET> req{nullptr};
    std::atomic<uint64_t> bytes{0};
    std::atomic<bool> exited{false};
    std::thread th;
    int idle = 0;               // watcher: seconds without a byte
    uint64_t lastBytes = 0;     // watcher
};

struct State {
    const std::atomic<bool>* cancel = nullptr;
    std::atomic<bool> stop{false};
    Options opt;
    bool stopping() const { return stop.load() || (cancel && cancel->load()); }
};

void closeReq(Conn& c) {
    if (HINTERNET r = c.req.exchange(nullptr)) WinHttpCloseHandle(r);
}

// Sleeps ms unless stopping (checked every 20 ms).
void nap(State& st, double ms) {
    const double t = nowMs() + ms;
    while (!st.stopping() && nowMs() < t) Sleep(20);
}

uint64_t minSplitFor(const File& f, int cap) {
    // Pieces of remaining / (2 x connections), 64 KB .. 2 MB: fine near the end.
    const uint64_t r = f.remaining() / std::max(2, 2 * cap);
    return std::clamp<uint64_t>(r, 64ull << 10, 2ull << 20);
}

// Splits the segment with the most left in two (caller holds f.mu); the new
// half is untaken.  False when nothing is big enough.
bool splitLargest(File& f, uint64_t minSplit) {
    size_t best = 0;
    uint64_t left = 0;
    for (size_t i = 0; i < f.segs.size(); ++i) {
        const Seg& s = f.segs[i];
        const uint64_t l = s.end > s.cur ? s.end - s.cur : 0;
        if (l > left) left = l, best = i;
    }
    if (left < 2 * minSplit || f.single) return false;
    const uint64_t mid = f.segs[best].cur + left / 2;
    f.segs.push_back(Seg{mid, f.segs[best].end, false});
    f.segs[best].end = mid;
    return true;
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

void worker(std::shared_ptr<State> stp, std::shared_ptr<File> fp, std::shared_ptr<Conn> cp) {
    State& st = *stp;
    File& f = *fp;
    Conn& c = *cp;
    std::vector<char> buf(256 * 1024);
    int fails = 0;  // this connection's, in a row without progress
    for (;;) {
        if (st.stopping() || f.failed) break;
        uint64_t from = 0, to = 0;
        bool singleMe = false;
        size_t urlIdx = 0;
        {
            std::lock_guard<std::mutex> l(f.mu);
            if (f.single && f.owner != &c) break;
            Seg& s = f.segs[c.seg];
            if (s.cur >= s.end) {
                s.taken = false;
                // Done: another untaken piece, or half of the largest one (work stealing).
                if (f.single) break;
                if (!takeSeg(f, c) && !(splitLargest(f, minSplitFor(f, st.opt.maxTotalConnections)) && takeSeg(f, c))) break;
                continue;
            }
            from = s.cur;
            to = s.end;
            singleMe = f.single;
            urlIdx = f.url;
        }
        const Url u = f.urls[urlIdx];
        const bool mirror = urlIdx + 1 < f.urls.size();
        const std::wstring whost = fromUtf8(u.host), wpath = fromUtf8(u.path);
        HINTERNET ses = WinHttpOpen(st.opt.userAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET con = ses ? WinHttpConnect(ses, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
        HINTERNET rq = con ? WinHttpOpenRequest(con, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                           : nullptr;
        bool progressed = false;
        std::wstring why;
        // A mirror that does not answer gives way quickly (resolve / connect 5 s).
        if (ses) WinHttpSetTimeouts(ses, mirror ? 5000 : 10000, mirror ? 5000 : 10000, 15000, 20000);
        if (rq) {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
            WinHttpSetOption(rq, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
            c.req = rq;
            if (st.stopping()) closeReq(c);  // the closer may have run before the store
        }
        // Range always (also from 0: a server that does Range answers 206).
        const uint64_t off = f.it.offset;
        const std::wstring range = L"Range: bytes=" + std::to_wstring(off + from) + L"-" +
                                   (singleMe && !off ? L"" : std::to_wstring(off + to - 1)) + L"\r\n";
        DWORD code = 0, len = sizeof(code);
        const bool sent = rq && WinHttpSendRequest(rq, range.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                          WinHttpReceiveResponse(rq, nullptr) &&
                          WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                              &code, &len, WINHTTP_NO_HEADER_INDEX);
        bool refused = false;  // this URL will not serve it: the next one, no retries
        if (!sent) {
            why = L"cannot reach " + whost + L" (error " + std::to_wstring(GetLastError()) + L")";
            refused = mirror;  // an unreachable mirror: straight to the next URL
        } else if (code == 200 && off) {
            why = L"HTTP 200 without Range from " + whost;  // would send the whole package
            refused = true;
        } else if (code == 200) {
            // No Range: one stream from 0, owned by this connection.
            std::lock_guard<std::mutex> l(f.mu);
            if (!f.single) {
                f.single = true;
                f.owner = &c;
                f.segs.assign(1, Seg{0, f.it.size, true});
                c.seg = 0;
                f.done = 0;
            }
            if (f.owner == &c) {
                f.segs[0].cur = 0;
                f.done = 0;
            } else {
                code = 0;  // another connection has it
            }
        } else if (code == 206) {
            std::lock_guard<std::mutex> l(f.mu);
            f.ranged = true;
        } else {
            why = L"HTTP " + std::to_wstring(code) + L" from " + whost;
            refused = code >= 400 && code < 500 && code != 408 && code != 429;
        }
        if (code == 0 && why.empty()) {  // lost the single stream to another connection
            closeReq(c);
            if (con) WinHttpCloseHandle(con);
            if (ses) WinHttpCloseHandle(ses);
            break;
        }
        if (why.empty()) {
            for (;;) {
                DWORD n = 0;
                HINTERNET live = c.req.load();  // null: closed by cancel / the stall watch
                if (!live || !WinHttpReadData(live, buf.data(), static_cast<DWORD>(buf.size()), &n)) {
                    if (!st.stopping()) why = L"download interrupted (error " + std::to_wstring(GetLastError()) + L")";
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
                    if (st.stop || (f.single && f.owner != &c) || f.url != urlIdx || f.h == INVALID_HANDLE_VALUE) {
                        lost = true;  // stopped (the file may be closed), or moved to another URL
                    } else {
                        Seg& s = f.segs[c.seg];
                        const uint64_t take = std::min<uint64_t>(n, s.end > s.cur ? s.end - s.cur : 0);
                        if (take) {
                            OVERLAPPED ov{};
                            ov.Offset = static_cast<DWORD>(s.cur);
                            ov.OffsetHigh = static_cast<DWORD>(s.cur >> 32);
                            DWORD w = 0;
                            if (!WriteFile(f.h, buf.data(), static_cast<DWORD>(take), &w, &ov) || w != take) {
                                f.fail(L"cannot write " + f.part + L" (disk full?)", 0, Status::Disk);
                                lost = true;
                            } else {
                                s.cur += take;
                                f.done += take;
                                f.lastProgress = nowMs();
                                c.bytes += take;
                                progressed = true;
                            }
                        }
                        segDone = s.cur >= s.end;
                    }
                }
                if (lost || segDone || st.stopping()) break;
            }
        }
        closeReq(c);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
        if (st.stopping() || f.failed) break;
        if (progressed) fails = 0;
        if (!why.empty()) {
            const int k = progressed ? 0 : ++fails;
            // A mirror gets 2 tries, the original 6 - and only while no other
            // connection of the file got a byte for 20 s (one busy host
            // refusing some of 16 connections is not a failure) - then the
            // next URL / failure.
            const bool stuck = nowMs() - f.lastProgress.load() > 20000;
            if (refused || (k > (mirror ? 2 : 6) && (mirror || stuck))) {
                std::lock_guard<std::mutex> l(f.mu);
                if (!f.nextUrl(urlIdx)) {
                    f.fail(why, refused && sent ? static_cast<int>(code) : 0);
                    break;
                }
                continue;  // the next URL right away
            }
            if (k > 0) nap(st, 500.0 * (1 << std::min(k - 1, 4)));
        }
    }
    {
        std::lock_guard<std::mutex> l(f.mu);
        if (c.seg < f.segs.size() && (!f.single || f.owner == &c)) f.segs[c.seg].taken = false;
    }
    c.exited = true;
}

// Loads the sidecar / an old single-stream .part, or starts new segments.
bool prepare(File& f, int startConns) {
    const Item& it = f.it;
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
        const int n = std::clamp<int>(static_cast<int>(it.size / (512ull << 10)), 1, std::max(1, startConns));
        for (int i = 0; i < n; ++i) segs.push_back(Seg{it.size * i / n, it.size * (i + 1) / n, false});
    }
    // Resumed with fewer pieces than connections wanted: split them now.
    f.segs = std::move(segs);
    while (static_cast<int>(f.segs.size()) < startConns && splitLargest(f, 256ull << 10)) {
    }
    f.h = CreateFileW(f.part.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) {
        f.fail(L"cannot write " + f.part, 0, Status::Disk);
        return false;
    }
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(it.size);
    if (!SetFilePointerEx(f.h, li, nullptr, FILE_BEGIN) || !SetEndOfFile(f.h)) {
        f.fail(L"cannot write " + f.part + L" (disk full?)", 0, Status::Disk);
        return false;
    }
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
    // Shared with the connection threads (a straggler after a cancel keeps it alive).
    auto stp = std::make_shared<State>();
    State& st = *stp;
    st.cancel = cancel;
    st.opt = o;
    std::vector<std::shared_ptr<File>> files;
    std::deque<std::shared_ptr<Conn>> conns;
    uint64_t total = 0;
    for (const auto& it : items) {
        auto f = std::make_shared<File>();
        f->it = it;
        f->urls = it.mirrors;
        f->urls.push_back(Url{it.host, it.path});
        f->part = it.dest + L".part";
        f->lastProgress = nowMs();
        f->side = f->part + L".seg";
        total += it.size;
        files.push_back(std::move(f));
    }
    auto isSlow = [&](const File& f) {
        const double r = hostRate(f.urls[f.url].host);
        return r >= 0 && r < o.slowBytesPerSec;
    };
    // Initial connections: priority / known-slow items first, the others keep startConnections.
    int nonPrio = 0, prio = 0;
    for (auto& f : files) (f->it.priority > 0 || isSlow(*f) ? prio : nonPrio)++;
    const int prioEach = prio ? std::clamp((o.maxTotalConnections - nonPrio * o.startConnections) / prio, 1, o.prioConnections) : 0;
    for (auto& f : files) {
        uint64_t sz = 0;
        if (fileSize(f->it.dest, sz) && sz == f->it.size) {  // there already (the caller re-checks the hash)
            f->there = true;
            f->done = f->it.size;
            continue;
        }
        prepare(*f, f->it.priority > 0 || isSlow(*f) ? prioEach : o.startConnections);
    }
    auto liveOf = [&](const File* f) {
        int n = 0;
        for (auto& c : conns) n += !c->exited && c->f.get() == f ? 1 : 0;
        return n;
    };
    auto openAll = [&] {
        int n = 0;
        for (auto& c : conns) n += c->exited ? 0 : 1;
        return n;
    };
    auto activeFiles = [&] {
        int n = 0;
        for (auto& f : files) {
            std::lock_guard<std::mutex> l(f->mu);
            n += !f->there && !f->failed && !f->complete() ? 1 : 0;
        }
        return n;
    };
    // A file's connection cap: all of them when it is the last one; priority /
    // slow-host files most of them while others run; the rest maxConnections.
    auto capOf = [&](File& f, int active) {
        if (active <= 1) return o.maxTotalConnections;
        if (f.it.priority > 0 || isSlow(f)) return std::max(o.maxConnections, o.maxTotalConnections - 2 * (active - 1));
        return o.maxConnections;
    };
    auto spawn = [&](const std::shared_ptr<File>& f) {
        auto c = std::make_shared<Conn>();
        c->f = f;
        {
            std::lock_guard<std::mutex> l(f->mu);
            if (f->failed || !takeSeg(*f, *c)) return false;
            if (f->single) f->owner = c.get();  // the stream's connection is gone: this one continues it
        }
        conns.push_back(c);
        f->maxSeen = std::max(f->maxSeen, liveOf(f.get()) + 1);
        c->th = std::thread([stp, f, c] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            worker(stp, f, c);
        });
        return true;
    };
    // Untaken pieces get connections: priority files first, within the caps.
    auto fill = [&] {
        const int active = activeFiles();
        std::vector<std::shared_ptr<File>> order(files.begin(), files.end());
        std::stable_sort(order.begin(), order.end(), [&](const std::shared_ptr<File>& a, const std::shared_ptr<File>& b) {
            return (a->it.priority > 0 || isSlow(*a)) > (b->it.priority > 0 || isSlow(*b));
        });
        for (auto& f : order) {
            if (f->there || f->failed) continue;
            const int cap = capOf(*f, active);
            while (openAll() < o.maxTotalConnections && liveOf(f.get()) < cap && spawn(f)) {
            }
        }
    };
    fill();
    // Watch: cancel (20 ms), progress (100 ms), sidecars + connections (1 s).
    std::deque<std::pair<double, uint64_t>> samples;
    double lastProgress = 0, lastTick = nowMs(), cancelAt = -1;
    bool verifying = false;
    auto doneAll = [&] {
        uint64_t d = 0;
        for (auto& f : files) d += f->done.load();
        return d;
    };
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
        p.connections = openAll();
        p.verifying = verifying;
        for (auto& f : files) {
            std::lock_guard<std::mutex> l(f->mu);
            if (!f->there && !f->failed && !f->complete() && !f->it.label.empty()) p.pending.push_back(f->it.label);
        }
        progress(p);
    };
    for (;;) {
        if (openAll() == 0) {
            bool more = false;
            if (!st.stopping())
                for (auto& f : files) {
                    std::lock_guard<std::mutex> l(f->mu);
                    if (!f->failed && !f->there && !f->complete()) more = true;
                }
            if (!more) break;
            fill();
            if (openAll() == 0) Sleep(20);  // pieces all taken by exiting connections: next round
            continue;
        }
        if (st.stopping()) {
            cancelAt = nowMs();
            st.stop = true;
            for (auto& c : conns) closeReq(*c);  // aborts blocking sends / reads
            break;
        }
        Sleep(20);
        for (auto& f : files)
            if (f->doneAt < 0 && !f->there) {
                std::lock_guard<std::mutex> l(f->mu);
                if (f->complete()) f->doneAt = nowMs() - t0;
            }
        report(false);
        if (nowMs() - lastTick < 1000) continue;
        const double dt = (nowMs() - lastTick) / 1000.0;
        lastTick = nowMs();
        // Rates, stalls, the host memory.
        std::map<File*, std::pair<double, int>> rate;  // bytes/s, live connections
        for (auto& c : conns) {
            const uint64_t b = c->bytes.load();
            if (!c->exited) {
                auto& r = rate[c->f.get()];
                r.first += (b - c->lastBytes) / dt;
                ++r.second;
                // A stalled connection (no byte for 8 s): closed, its worker reconnects.
                c->idle = b != c->lastBytes ? 0 : c->idle + 1;
                if (c->idle >= 8) {
                    closeReq(*c);
                    c->idle = 0;
                }
            }
            c->lastBytes = b;
        }
        const int active = activeFiles();
        for (auto& f : files) {
            std::lock_guard<std::mutex> l(f->mu);
            if (f->failed || f->there) continue;
            f->save();
            if (f->complete()) continue;
            const auto [bps, live] = rate[f.get()];
            if (live > 0 && bps > 0) noteHostRate(f->urls[f->url].host, bps / live);
            const bool slow = live > 0 && bps / live < o.slowBytesPerSec;
            if (!f->ranged || f->single || !slow) continue;
            // Slow connections: up to the file's cap at once, by splitting the largest pieces.
            const int cap = capOf(*f, active);
            int untaken = 0;
            for (const auto& s : f->segs) untaken += !s.taken && s.end > s.cur ? 1 : 0;
            const uint64_t minSplit = minSplitFor(*f, cap);
            for (int want = cap - live - untaken; want > 0 && splitLargest(*f, minSplit); --want) {
            }
        }
        fill();
        // Exited connections: joined and dropped.
        for (auto i = conns.begin(); i != conns.end();)
            if ((*i)->exited) {
                if ((*i)->th.joinable()) (*i)->th.join();
                i = conns.erase(i);
            } else {
                ++i;
            }
    }
    for (auto& f : files)
        if (f->doneAt < 0 && !f->there) {
            std::lock_guard<std::mutex> l(f->mu);
            if (f->complete()) f->doneAt = nowMs() - t0;
        }
    // Wait for the connections: briefly after a cancel (a straggler stuck in the
    // OS is detached - it holds the shared state and writes nothing), fully otherwise.
    const double waitUntil = nowMs() + 150;
    for (auto& c : conns) {
        while (cancelAt >= 0 && !c->exited && nowMs() < waitUntil) Sleep(5);
        if (!c->th.joinable()) continue;
        if (c->exited || cancelAt < 0) c->th.join();
        else c->th.detach();
    }
    // Close, verify, move.
    std::vector<Result> out(items.size());
    for (size_t k = 0; k < files.size(); ++k) {
        File& f = *files[k];
        {
            std::lock_guard<std::mutex> l(f.mu);
            if (f.h != INVALID_HANDLE_VALUE) {
                if (!f.there) f.save();
                CloseHandle(f.h);
                f.h = INVALID_HANDLE_VALUE;
            }
        }
        Result& r = out[k];
        r.url = static_cast<int>(f.url);
        if (f.there) continue;
        if (f.failed) {
            r.status = f.failStatus == Status::Ok ? Status::Network : f.failStatus;
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
        if (f.it.sha256.empty()) {  // tests only (a byte range of a big file): kept as .part
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
        if (h != f.it.sha256) {
            DeleteFileW(f.part.c_str());
            DeleteFileW(f.side.c_str());
            r.status = Status::Verify;
            r.detail = L"SHA-256";
            continue;
        }
        DeleteFileW(f.side.c_str());
        DeleteFileW(f.it.dest.c_str());
        if (!MoveFileExW(f.part.c_str(), f.it.dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
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
            gStats.itemSeconds.push_back(f->doneAt / 1000);
            gStats.maxConnections = std::max(gStats.maxConnections, f->maxSeen);
        }
    }
    return out;
}

}  // namespace pm::translate::dl
