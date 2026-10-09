// scrcpy v5 wire protocol (client side): stream demuxing and control
// messages. Pure code (no sockets) so it can be unit-tested.
// Reference: Genymobile/scrcpy v5.0 app/src/demuxer.c, control_msg.c,
// server/.../device/Streamer.java (Apache-2.0).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pm/media.h"
#include "pm/video_window.h"

namespace pm::scrcpy {

constexpr const char* kServerVersion = "5.0";

constexpr uint32_t kCodecH264 = 0x68323634;  // "h264"
constexpr uint32_t kCodecH265 = 0x68323635;  // "h265"
constexpr uint32_t kCodecAv1 = 0x00617631;
constexpr uint32_t kCodecOpus = 0x6f707573;
constexpr uint32_t kCodecAac = 0x00616163;   // "aac"
constexpr uint32_t kCodecFlac = 0x666c6163;
constexpr uint32_t kCodecRaw = 0x00726177;
constexpr uint32_t kCodecDisabled = 0;       // device could not capture (e.g. audio < Android 11)
constexpr uint32_t kCodecError = 1;          // configuration error on the device

constexpr uint64_t kFlagSession = 1ull << 63;
constexpr uint64_t kFlagConfig = 1ull << 62;
constexpr uint64_t kFlagKeyFrame = 1ull << 61;
constexpr uint64_t kPtsMask = kFlagKeyFrame - 1;

constexpr size_t kDeviceNameLength = 64;
constexpr uint32_t kMaxPacket = 32u << 20;  // sanity limit

// Incremental parser for one scrcpy media socket after the (optional) dummy
// byte and device-meta header: codec id (4) then 12-byte headers, each
// either a session packet (video size) or a media packet + payload.
class StreamParser {
public:
    struct Callbacks {
        std::function<void(uint32_t codecId)> onCodec;
        std::function<void(int width, int height, bool clientResized)> onSession;
        // pts in microseconds (0 for config packets).
        std::function<void(const uint8_t* data, size_t len, uint64_t ptsUs, bool config, bool keyFrame)> onPacket;
    };
    explicit StreamParser(Callbacks cb) : cb_(std::move(cb)) {}
    // Returns false on a protocol error (then the parser stays failed).
    bool feed(const uint8_t* data, size_t len);
    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }
    uint32_t codec() const { return codec_; }
    bool disabled() const { return codec_ == kCodecDisabled && gotCodec_; }

private:
    bool fail(std::string why) { failed_ = true; error_ = std::move(why); return false; }
    Callbacks cb_;
    enum class St { Codec, Header, Payload, Done } st_ = St::Codec;
    std::vector<uint8_t> buf_;
    size_t need_ = 4;
    uint64_t ptsFlags_ = 0;
    uint32_t codec_ = 0;
    bool gotCodec_ = false;
    bool failed_ = false;
    std::string error_;
};

// Video: codec id → onCodec; session → onSourceSize; config packets (SPS/PPS)
// are prepended to the next frame (Annex-B, as VideoWindow expects).
class VideoAdapter {
public:
    explicit VideoAdapter(VideoSink* sink) : sink_(sink) {}
    StreamParser::Callbacks callbacks();
    int width() const { return w_; }
    int height() const { return h_; }
    std::function<void(int, int)> onSize;  // also notify the input mapper
    std::function<void(const std::string&)> log;

private:
    VideoSink* sink_;
    std::vector<uint8_t> config_, merged_;
    int w_ = 0, h_ = 0;
};

// Audio: aac → onFormat(AAC_LC, 48000, 2, 1024); raw AAC frames → onPacket.
class AudioAdapter {
public:
    explicit AudioAdapter(AudioSink* sink) : sink_(sink) {}
    StreamParser::Callbacks callbacks();
    bool active() const { return active_; }
    std::function<void(const std::string&)> log;

private:
    AudioSink* sink_;
    bool active_ = false;
};

// ---- control messages (client → device) ----
enum class MsgType : uint8_t {
    InjectKeycode = 0, InjectText = 1, InjectTouch = 2, InjectScroll = 3, BackOrScreenOn = 4,
    ExpandNotificationPanel = 5, ExpandSettingsPanel = 6, CollapsePanels = 7, GetClipboard = 8,
    SetClipboard = 9, SetDisplayPower = 10, RotateDevice = 11,
};
constexpr uint64_t kPointerMouse = ~0ull;            // -1
constexpr uint64_t kPointerGenericFinger = ~0ull - 1; // -2
enum KeyAction : uint8_t { kKeyDown = 0, kKeyUp = 1 };
enum MotionAction : uint8_t { kMotionDown = 0, kMotionUp = 1, kMotionMove = 2 };
constexpr uint32_t kMetaShift = 0x41, kMetaCtrl = 0x3000, kMetaAlt = 0x12;
// Android keycodes we use directly.
constexpr uint32_t kKeyHome = 3, kKeyBack = 4, kKeyAppSwitch = 187;

std::vector<uint8_t> msgKeycode(uint8_t action, uint32_t keycode, uint32_t repeat, uint32_t meta);
std::vector<uint8_t> msgText(const std::string& utf8);  // truncated to 300 bytes on a UTF-8 boundary
std::vector<uint8_t> msgTouch(uint8_t action, uint64_t pointerId, int32_t x, int32_t y, uint16_t w,
                              uint16_t h, float pressure, uint32_t actionButton, uint32_t buttons);
std::vector<uint8_t> msgScroll(int32_t x, int32_t y, uint16_t w, uint16_t h, float hscroll, float vscroll,
                               uint32_t buttons);
std::vector<uint8_t> msgBackOrScreenOn(uint8_t action);
std::vector<uint8_t> msgSimple(MsgType t);

// Ctrl / Alt / Shift as held on the calling thread right now (GetKeyState),
// as scrcpy meta flags.
uint32_t keyboardMeta();

// Win32 VK → Android keycode (0 = unmapped).
uint32_t vkToAndroidKeycode(unsigned vk);

// Turns VideoWindow pointer/key events into control messages ("mixed" key
// mode like scrcpy: printable characters as INJECT_TEXT, everything else and
// Ctrl/Alt chords as INJECT_KEYCODE). Not thread-safe; callers serialise.
class InputTranslator {
public:
    using Out = std::function<void(std::vector<uint8_t>)>;
    explicit InputTranslator(Out out) : out_(std::move(out)) {}
    void setVideoSize(int w, int h);
    void pointer(const VideoWindow::PointerEvent& e);
    void key(unsigned vk, bool down, wchar_t ch);
    // Modifiers held when a key arrives.  VideoWindow never forwards Ctrl /
    // Alt themselves (its contract: "modifier state: GetKeyState() inside the
    // handler"), so by default keyboardMeta() is read on the calling (UI)
    // thread; added to the modifier events key() tracks itself.  nullptr =
    // tracked events only (tests).
    using Modifiers = std::function<uint32_t()>;
    void setModifierSource(Modifiers m) { mods_ = std::move(m); }
    void reset();  // release a held touch

private:
    void pos(float nx, float ny, int32_t& x, int32_t& y) const;
    Out out_;
    int w_ = 0, h_ = 0;
    bool touching_ = false;
    int32_t lastX_ = 0, lastY_ = 0;
    bool shift_ = false, ctrl_ = false, alt_ = false;
    Modifiers mods_ = keyboardMeta;
    bool keyDownSent_[256] = {};
    wchar_t highSurrogate_ = 0;
};

std::string utf16ToUtf8(const std::wstring& s);

}  // namespace pm::scrcpy
