#include "scrcpy_session.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>

#include "scrcpy_proto.h"

namespace pm::scrcpy {

namespace {
constexpr uintptr_t kNone = ~uintptr_t(0);

struct WsaInit {
    WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};

SOCKET connectTcp(const std::string& host, uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

void setTimeout(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

bool recvAll(SOCKET s, uint8_t* p, size_t n) {
    while (n) {
        int r = recv(s, reinterpret_cast<char*>(p), int(std::min<size_t>(n, 1 << 20)), 0);
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

bool sendAll(SOCKET s, const uint8_t* p, size_t n) {
    while (n) {
        int r = send(s, reinterpret_cast<const char*>(p), int(n), 0);
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

void closeSock(uintptr_t& s) {
    if (s != kNone) closesocket(SOCKET(s));
    s = kNone;
}
}  // namespace

Session::~Session() { stop(); }

int64_t Session::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool Session::connect(const Config& cfg, const std::function<bool()>& alive, std::string* err) {
    static WsaInit wsa;
    auto fail = [&](const std::string& e) {
        if (err) *err = e;
        closeSock(video_);
        closeSock(audio_);
        closeSock(control_);
        return false;
    };

    // Video socket: through adb forward the TCP connect succeeds even before
    // the server listens, so wait for the dummy byte.  Retries back off
    // exponentially (20 ms .. 400 ms): the server usually listens within
    // ~1 s, a slow phone may need several.
    SOCKET v = INVALID_SOCKET;
    const int64_t deadline = nowMs() + cfg.timeoutMs;
    int delay = std::max(1, cfg.delayMs);
    for (int i = 0; i < cfg.attempts && nowMs() < deadline; ++i) {
        if (alive && !alive()) return fail("cancelled");
        v = connectTcp(cfg.host, cfg.port);
        if (v != INVALID_SOCKET) {
            setTimeout(v, 2000);
            uint8_t b;
            if (recv(v, reinterpret_cast<char*>(&b), 1, 0) == 1) break;
            closesocket(v);
            v = INVALID_SOCKET;
        }
        Sleep(DWORD(delay));
        delay = std::min(delay * 2, std::max(delay, cfg.maxDelayMs));
    }
    if (alive && !alive() && v == INVALID_SOCKET) return fail("cancelled");
    if (v == INVALID_SOCKET) return fail("could not connect to scrcpy-server");
    video_ = v;

    if (cfg.audio) {
        SOCKET a = connectTcp(cfg.host, cfg.port);
        if (a == INVALID_SOCKET) return fail("audio socket connect failed");
        audio_ = a;
    }
    if (cfg.control) {
        SOCKET c = connectTcp(cfg.host, cfg.port);
        if (c == INVALID_SOCKET) return fail("control socket connect failed");
        BOOL nd = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nd), sizeof(nd));
        control_ = c;
    }

    // Device meta: 64-byte NUL-padded UTF-8 name on the first socket.
    setTimeout(v, 10000);
    uint8_t name[kDeviceNameLength];
    if (!recvAll(v, name, sizeof(name))) return fail("no device meta from scrcpy-server");
    name[kDeviceNameLength - 1] = 0;
    deviceName_ = reinterpret_cast<const char*>(name);
    setTimeout(v, 0);
    return true;
}

void Session::run(VideoSink* video, AudioSink* audio) {
    stopping_ = false;
    end_ = int(End::None);
    startedMs_ = nowMs();
    tv_ = std::thread([this, video] { videoLoop(video); });
    if (audio_ != kNone) ta_ = std::thread([this, audio] { audioLoop(audio); });
    if (control_ != kNone) {
        tcw_ = std::thread([this] { controlWriter(); });
        tcr_ = std::thread([this] { controlReader(); });
    }
}

void Session::stop() {
    {
        // Under qmu_: the writer checks stopping_ under the same lock, so this
        // wake-up cannot fall between its check and its wait (lost wake-up =
        // tcw_.join() below hanging the UI thread).
        std::lock_guard<std::mutex> lk(qmu_);
        stopping_ = true;
    }
    for (uintptr_t s : {video_, audio_, control_})
        if (s != kNone) shutdown(SOCKET(s), SD_BOTH);
    qcv_.notify_all();
    drained_.notify_all();
    for (std::thread* t : {&tv_, &ta_, &tcw_, &tcr_})
        if (t->joinable()) t->join();
    closeSock(video_);
    closeSock(audio_);
    closeSock(control_);
}

void Session::sendControl(std::vector<uint8_t> msg) {
    if (control_ == kNone || stopping_) return;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        if (queue_.size() >= 1024) return;  // device not reading: drop
        queue_.push_back(std::move(msg));
    }
    qcv_.notify_one();
}

void Session::videoLoop(VideoSink* sink) {
    VideoAdapter va(sink);
    va.log = log;
    va.onSize = [this](int w, int h) { if (onVideoSize) onVideoSize(w, h); };
    auto cb = va.callbacks();
    auto inner = cb.onPacket;
    cb.onPacket = [&](const uint8_t* p, size_t n, uint64_t pts, bool c, bool k) {
        ++videoPackets;
        lastVideoMs_ = nowMs();
        inner(p, n, pts, c, k);
    };
    StreamParser parser(cb);
    std::vector<uint8_t> buf(256 * 1024);
    End why = End::Eof;
    for (;;) {
        int r = recv(SOCKET(video_), reinterpret_cast<char*>(buf.data()), int(buf.size()), 0);
        if (r <= 0) break;
        if (!parser.feed(buf.data(), size_t(r))) {
            if (log) log("video stream error: " + parser.error());
            // Codec id 1 = the device could not capture / encode the screen.
            why = parser.codec() == kCodecError ? End::CaptureError : End::StreamError;
            break;
        }
        if (parser.disabled()) {  // video codec 0: the device gave up capturing
            why = End::CaptureError;
            break;
        }
    }
    if (!stopping_) {
        end_ = int(why);
        if (log) log(why == End::Eof ? "video stream ended" : why == End::CaptureError ? "video capture failed on the device" : "video stream broken");
        if (onEnded) onEnded();
    }
}

void Session::audioLoop(AudioSink* sink) {
    AudioAdapter aa(sink);
    aa.log = log;
    auto cb = aa.callbacks();
    auto inner = cb.onPacket;
    cb.onPacket = [&](const uint8_t* p, size_t n, uint64_t pts, bool c, bool k) {
        ++audioPackets;
        lastAudioMs_ = nowMs();
        inner(p, n, pts, c, k);
    };
    StreamParser parser(cb);
    std::vector<uint8_t> buf(64 * 1024);
    for (;;) {
        int r = recv(SOCKET(audio_), reinterpret_cast<char*>(buf.data()), int(buf.size()), 0);
        if (r <= 0) break;
        if (!parser.feed(buf.data(), size_t(r))) {
            if (log) log("audio stream error: " + parser.error() + " (audio off, video continues)");
            // Said once, not silently: codec error = capture failed on the device.
            if (!stopping_ && onAudioUnavailable) onAudioUnavailable(true);
            break;
        }
        if (parser.disabled()) {
            if (!stopping_ && onAudioUnavailable) onAudioUnavailable(false);
            break;
        }
    }
    // Audio ending never ends the session (video may continue).
}

void Session::controlWriter() {
    for (;;) {
        std::vector<uint8_t> m;
        {
            std::unique_lock<std::mutex> lk(qmu_);
            // Bounded: even a missed notify only delays the exit by 200 ms.
            while (!stopping_ && queue_.empty()) qcv_.wait_for(lk, std::chrono::milliseconds(200));
            if (stopping_) return;
            m = std::move(queue_.front());
            queue_.pop_front();
            writing_ = true;
        }
        const bool ok = sendAll(SOCKET(control_), m.data(), m.size());
        {
            std::lock_guard<std::mutex> lk(qmu_);
            writing_ = false;
            if (!ok) queue_.clear();
        }
        drained_.notify_all();
        if (!ok) return;
        ++controlSent;
    }
}

bool Session::drainControl(int timeoutMs) {
    if (control_ == kNone) return true;
    std::unique_lock<std::mutex> lk(qmu_);
    return drained_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                             [&] { return stopping_ || (queue_.empty() && !writing_); });
}

void Session::controlReader() {
    // Device messages (clipboard, acks, uhid output).  The socket is always
    // drained so the device never blocks writing, even after a parse error.
    DeviceMsgParser::Callbacks cb;
    cb.onClipboard = [this](const std::string& t) {
        if (onClipboard && !stopping_) onClipboard(t);
    };
    cb.onAckClipboard = [this](uint64_t s) {
        if (onAckClipboard && !stopping_) onAckClipboard(s);
    };
    DeviceMsgParser parser(cb);
    std::vector<uint8_t> buf(16 * 1024);
    bool reported = false;
    int r;
    while ((r = recv(SOCKET(control_), reinterpret_cast<char*>(buf.data()), int(buf.size()), 0)) > 0) {
        if (!parser.feed(buf.data(), size_t(r)) && !reported) {
            reported = true;
            if (log) log("device message error: " + parser.error() + " (clipboard sync off for this session)");
        }
    }
}

}  // namespace pm::scrcpy
