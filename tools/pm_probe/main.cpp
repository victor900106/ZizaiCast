// pm_probe: console test harness for the PhoneMirror AirPlay core.
//   pm_probe.exe [name] [--h265] [--dynamic-ports] [--debug] [--key FILE] [--seconds N] [--pin]
//                [--quickack] [--al MS] [--maxfps N] [--size WxH]   (latency A/B knobs)
//                [--preset std|high|max]  (picture quality: 1920x1080 / 2560x1440 / 3840x2160)
//                [--refresh-at S]  (call refreshNetwork() S seconds after READY; also: press R)
//                [--keep-current]  (TakeoverPolicy::KeepCurrent: refuse a 2nd phone while one mirrors)
// Starts pm::AirPlayServer, dumps the mirrored video to probe.h264/probe.h265
// (Annex-B, playable with ffplay/VLC) and the audio packets to
// probe_audio.bin (each packet: uint32 little-endian length + raw bytes),
// and logs every callback with a timestamp. Ctrl+C to quit.
// GPL-3.0-or-later.

#include <windows.h>
#include <conio.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include "pm/airplay_server.h"

namespace {

std::mutex g_logMutex;
std::atomic<bool> g_quit{false};
const auto g_t0 = std::chrono::steady_clock::now();

void logLine(const char* tag, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    double rel = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::printf("%02d:%02d:%02d.%03d +%9.3fs [%s] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, rel,
                tag, msg);
    std::fflush(stdout);
}

const char* nalSummary(const uint8_t* p, size_t n, bool h265, char* out, size_t outLen) {
    // List NAL unit types in an Annex-B buffer, e.g. "7,8,5" (H.264) or "32,33,34,19" (H.265).
    size_t used = 0;
    out[0] = 0;
    int count = 0;
    for (size_t i = 0; i + 4 < n && count < 12; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            int t = h265 ? ((p[i + 4] >> 1) & 0x3f) : (p[i + 4] & 0x1f);
            int w = std::snprintf(out + used, outLen - used, "%s%d", count ? "," : "", t);
            if (w < 0 || static_cast<size_t>(w) >= outLen - used) break;
            used += w;
            ++count;
            i += 3;
        }
    }
    return out;
}

class ProbeVideoSink : public pm::VideoSink {
public:
    ~ProbeVideoSink() override { close(); }
    void onCodec(pm::VideoCodec codec) override {
        std::lock_guard<std::mutex> lk(m_);
        h265_ = codec == pm::VideoCodec::H265;
        const char* fn = h265_ ? "probe.h265" : "probe.h264";
        if (!f_ || fn != fn_) {
            close();
            f_ = std::fopen(fn, "ab");
            fn_ = fn;
        }
        logLine("VIDEO", "onCodec(%s) -> appending to %s", h265_ ? "H265" : "H264", fn);
    }
    void onFrame(const uint8_t* data, size_t len, uint64_t ntpLocalNs) override {
        std::lock_guard<std::mutex> lk(m_);
        if (!f_) {  // frame before onCodec: assume H.264 (the default codec)
            f_ = std::fopen("probe.h264", "ab");
            fn_ = "probe.h264";
        }
        if (f_) std::fwrite(data, 1, len, f_);
        ++frames_;
        bytes_ += len;
        int64_t ahead = static_cast<int64_t>(ntpLocalNs) - static_cast<int64_t>(pm::AirPlayServer::localTimeNs());
        char nals[96];
        if (frames_ <= 30 || (frames_ % 60) == 0) {
            logLine("VIDEO", "onFrame #%llu len=%zu nal=[%s] pts=%llu (%+.3f s vs now) total=%.1f MB",
                    (unsigned long long)frames_, len, nalSummary(data, len, h265_, nals, sizeof(nals)),
                    (unsigned long long)ntpLocalNs, ahead / 1e9, bytes_ / 1048576.0);
        }
    }
    void onSourceSize(int w, int h) override { logLine("VIDEO", "onSourceSize(%d x %d)", w, h); }
    void onReset() override {
        logLine("VIDEO", "onReset() after %llu frames", (unsigned long long)frames_);
        std::lock_guard<std::mutex> lk(m_);
        if (f_) std::fflush(f_);
    }
    unsigned long long frames() const { return frames_; }

private:
    void close() {
        if (f_) std::fclose(f_);
        f_ = nullptr;
    }
    std::mutex m_;
    FILE* f_ = nullptr;
    const char* fn_ = nullptr;
    bool h265_ = false;
    unsigned long long frames_ = 0, bytes_ = 0;
};

class ProbeAudioSink : public pm::AudioSink {
public:
    ProbeAudioSink() { f_ = std::fopen("probe_audio.bin", "ab"); }
    ~ProbeAudioSink() override {
        if (f_) std::fclose(f_);
    }
    void onFormat(pm::AudioCodec codec, int rate, int ch, int spf) override {
        const char* n = codec == pm::AudioCodec::AAC_ELD ? "AAC-ELD" : codec == pm::AudioCodec::AAC_LC ? "AAC-LC" : "ALAC";
        logLine("AUDIO", "onFormat(%s ct=%d, %d Hz, %d ch, %d samples/frame)", n, static_cast<int>(codec), rate, ch,
                spf);
    }
    void onPacket(const uint8_t* data, size_t len, uint64_t ntpLocalNs) override {
        std::lock_guard<std::mutex> lk(m_);
        if (f_) {
            uint32_t l = static_cast<uint32_t>(len);
            std::fwrite(&l, 4, 1, f_);
            std::fwrite(data, 1, len, f_);
        }
        ++packets_;
        if (packets_ <= 10 || (packets_ % 500) == 0) {
            int64_t ahead = static_cast<int64_t>(ntpLocalNs) - static_cast<int64_t>(pm::AirPlayServer::localTimeNs());
            logLine("AUDIO", "onPacket #%llu len=%zu first=%02x pts=%llu (%+.3f s vs now)",
                    (unsigned long long)packets_, len, len ? data[0] : 0, (unsigned long long)ntpLocalNs, ahead / 1e9);
        }
    }
    void onVolume(float db) override { logLine("AUDIO", "onVolume(%.2f dB)", db); }
    void onFlush() override {
        logLine("AUDIO", "onFlush() after %llu packets", (unsigned long long)packets_);
        std::lock_guard<std::mutex> lk(m_);
        if (f_) std::fflush(f_);
    }

private:
    std::mutex m_;
    FILE* f_ = nullptr;
    unsigned long long packets_ = 0;
};

// Print a symbolised stack on crashes so field logs are actionable
// (symbols need the .pdb next to the exe, e.g. a RelWithDebInfo build).
LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    std::printf("\n*** CRASH: exception 0x%08lx at %p\n", ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f{};
    f.AddrPC.Offset = ctx.Rip;
    f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp;
    f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp;
    f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !f.AddrPC.Offset)
            break;
        char buf[sizeof(SYMBOL_INFO) + 256];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD ldisp = 0;
        char mod[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(f.AddrPC.Offset), &hm))
            GetModuleFileNameA(hm, mod, sizeof(mod));
        const char* modName = std::strrchr(mod, '\\') ? std::strrchr(mod, '\\') + 1 : mod;
        bool haveSym = SymFromAddr(proc, f.AddrPC.Offset, &disp, sym);
        bool haveLine = SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &line);
        std::printf("  #%02d %s!%s+0x%llx %s:%lu\n", i, modName, haveSym ? sym->Name : "?",
                    (unsigned long long)disp, haveLine ? line.FileName : "", haveLine ? line.LineNumber : 0);
    }
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

BOOL WINAPI ctrlHandler(DWORD) {
    g_quit = true;
    return TRUE;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetUnhandledExceptionFilter(crashFilter);
    std::string name = "PhoneMirror";
    pm::AirPlayServer::Options opts;
    int runSeconds = 0;  // 0 = until Ctrl+C
    int refreshAt = 0;   // >0: refreshNetwork() after this many seconds
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--h265") opts.h265 = true;
        else if (a == "--dynamic-ports") opts.legacyPorts = false;
        else if (a == "--debug") opts.debugLog = true;
        else if (a == "--pin") opts.requirePin = true;
        else if (a == "--keep-current") opts.takeoverPolicy = pm::AirPlayServer::TakeoverPolicy::KeepCurrent;
        else if (a == "--quickack") opts.mirrorQuickAck = true;
        else if (a == "--al" && i + 1 < argc) opts.reportedAudioLatencyMs = std::atoi(argv[++i]);
        else if (a == "--maxfps" && i + 1 < argc) opts.maxFps = std::atoi(argv[++i]);
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &opts.width, &opts.height);
        else if (a == "--key" && i + 1 < argc) opts.keyFile = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) runSeconds = std::atoi(argv[++i]);
        else if (a == "--refresh-at" && i + 1 < argc) refreshAt = std::atoi(argv[++i]);
        else if (a == "--preset" && i + 1 < argc) {
            std::string p = argv[++i];
            pm::AirPlayServer::applyQualityPreset(
                opts, p == "max" ? pm::AirPlayServer::QualityPreset::Highest
                      : p == "high" ? pm::AirPlayServer::QualityPreset::High
                                    : pm::AirPlayServer::QualityPreset::Standard);
        }
        else if (a == "-h" || a == "--help") {
            std::printf("usage: pm_probe [name] [--h265] [--dynamic-ports] [--debug] [--key FILE] [--seconds N] "
                        "[--pin] [--quickack] [--al MS] [--maxfps N] [--size WxH] [--preset std|high|max] "
                        "[--refresh-at S] [--keep-current]\n");
            return 0;
        } else name = a;
    }

    ProbeVideoSink video;
    ProbeAudioSink audio;
    pm::AirPlayServer server;
    server.setOptions(opts);
    server.setLogCallback([](pm::AirPlayServer::LogLevel l, const std::string& msg) {
        static const char* tags[] = {"ERROR", "WARN ", "info ", "debug"};
        // multi-line library messages: keep them readable
        logLine(tags[static_cast<int>(l)], "%s", msg.c_str());
    });
    pm::AirPlayServer::Events ev;
    ev.onClientConnecting = [&server](const std::string& n, const std::string& m) {
        logLine("EVENT", "onClientConnecting(\"%s\", \"%s\") current=\"%s\"", n.c_str(), m.c_str(),
                server.currentClientName().c_str());
    };
    ev.onClientDisconnected = [&server] {
        logLine("EVENT", "onClientDisconnected() current=\"%s\"", server.currentClientName().c_str());
    };
    ev.onTakeover = [](const std::string& o, const std::string& n) {
        logLine("EVENT", "onTakeover(\"%s\" -> \"%s\")", o.c_str(), n.c_str());
    };
    ev.onPin = [](const std::string& pin) {
        if (pin.empty()) logLine("EVENT", "onPin(\"\") -> hide PIN");
        else logLine("EVENT", "onPin(\"%s\") -> show PIN", pin.c_str());
    };
    server.setEvents(std::move(ev));

    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    logLine("PROBE", "starting AirPlay receiver \"%s\" (h265=%s, ports=%s, key=%s)", name.c_str(),
            opts.h265 ? "on" : "off", opts.legacyPorts ? "legacy fixed" : "dynamic",
            opts.keyFile.empty() ? "(derived from MAC)" : opts.keyFile.c_str());
    if (!server.start(name, &video, &audio)) {
        logLine("PROBE", "FAILED to start server");
        return 1;
    }
    logLine("PROBE", "READY: deviceid %s, TCP port %u. On the iPhone: Control Center -> Screen Mirroring -> \"%s\".",
            server.deviceId().c_str(), server.port(), name.c_str());
    logLine("PROBE", "Ctrl+C to stop. Output: probe.h264 / probe.h265 (Annex-B), probe_audio.bin (u32le len + data)");

    for (const auto& f : server.advertisedInterfaces()) logLine("PROBE", "mDNS interface: %s", f.c_str());
    logLine("PROBE", "press R to call refreshNetwork() (re-scan interfaces, re-announce mDNS), C to print "
            "currentClientName()");
    std::string lastClient;
    for (int ms = 0; !g_quit && (runSeconds <= 0 || ms < runSeconds * 1000); ms += 200) {
        Sleep(200);
        std::string cur = server.currentClientName();
        if (cur != lastClient) {
            logLine("PROBE", "currentClientName() changed: \"%s\" -> \"%s\"", lastClient.c_str(), cur.c_str());
            lastClient = cur;
        }
        bool refresh = refreshAt > 0 && ms == refreshAt * 1000;
        while (_kbhit()) {
            int c = _getch();
            if (c == 'r' || c == 'R') refresh = true;
            if (c == 'c' || c == 'C')
                logLine("PROBE", "currentClientName() = \"%s\"", server.currentClientName().c_str());
        }
        if (refresh) {
            logLine("PROBE", "calling refreshNetwork()");
            server.refreshNetwork();
        }
    }
    logLine("PROBE", "stopping (%llu video frames received)", video.frames());
    server.stop();
    logLine("PROBE", "bye");
    return 0;
}
