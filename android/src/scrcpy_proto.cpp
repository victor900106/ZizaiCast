#include "scrcpy_proto.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pm::scrcpy {

namespace {
uint32_t rd32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t rd64(const uint8_t* p) { return (uint64_t(rd32(p)) << 32) | rd32(p + 4); }
void wr16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
void wr32(std::vector<uint8_t>& v, uint32_t x) { wr16(v, uint16_t(x >> 16)); wr16(v, uint16_t(x)); }
void wr64(std::vector<uint8_t>& v, uint64_t x) { wr32(v, uint32_t(x >> 32)); wr32(v, uint32_t(x)); }

std::string codecName(uint32_t id) {
    std::string s;
    for (int i = 3; i >= 0; --i) {
        char c = char((id >> (i * 8)) & 0xff);
        if (c) s.push_back(c >= 0x20 && c < 0x7f ? c : '?');
    }
    return s;
}

uint16_t floatToU16fp(float f) {
    f = std::clamp(f, 0.0f, 1.0f);
    uint32_t u = uint32_t(f * 65536.0f);
    return u >= 0xffff ? 0xffff : uint16_t(u);
}
int16_t floatToI16fp(float f) {
    f = std::clamp(f, -1.0f, 1.0f);
    int32_t i = int32_t(f * 32768.0f);
    if (i >= 0x7fff) i = 0x7fff;
    return int16_t(i);
}

// Index at which a UTF-8 string may be cut to at most maxLen bytes.
size_t utf8Truncation(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s.size();
    size_t len = maxLen;
    while (len > 0 && (uint8_t(s[len]) & 0xC0) == 0x80) --len;  // s[len] is a continuation byte
    return len;
}
}  // namespace

// ---------------------------------------------------------------- parser --

bool StreamParser::feed(const uint8_t* data, size_t len) {
    if (failed_) return false;
    while (len > 0) {
        if (st_ == St::Done) return true;  // stream disabled: ignore the rest
        size_t take = std::min(len, need_ - buf_.size());
        buf_.insert(buf_.end(), data, data + take);
        data += take;
        len -= take;
        if (buf_.size() < need_) break;

        switch (st_) {
            case St::Codec: {
                codec_ = rd32(buf_.data());
                gotCodec_ = true;
                if (cb_.onCodec) cb_.onCodec(codec_);
                if (codec_ == kCodecDisabled) { st_ = St::Done; buf_.clear(); return true; }
                if (codec_ == kCodecError) return fail("stream configuration error on the device");
                st_ = St::Header;
                need_ = 12;
                break;
            }
            case St::Header: {
                uint64_t v = rd64(buf_.data());
                if (v & kFlagSession) {
                    int w = int(rd32(&buf_[4])), h = int(rd32(&buf_[8]));
                    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return fail("invalid session size");
                    if (cb_.onSession) cb_.onSession(w, h, (buf_[3] & 1) != 0);
                    need_ = 12;
                } else {
                    uint32_t size = rd32(&buf_[8]);
                    if (size == 0) return fail("invalid packet length 0");
                    if (size > kMaxPacket) return fail("packet too large");
                    ptsFlags_ = v;
                    st_ = St::Payload;
                    need_ = size;
                }
                break;
            }
            case St::Payload: {
                bool config = (ptsFlags_ & kFlagConfig) != 0;
                bool key = (ptsFlags_ & kFlagKeyFrame) != 0;
                uint64_t pts = config ? 0 : (ptsFlags_ & kPtsMask);
                if (cb_.onPacket) cb_.onPacket(buf_.data(), buf_.size(), pts, config, key);
                st_ = St::Header;
                need_ = 12;
                break;
            }
            case St::Done:
                break;
        }
        buf_.clear();
    }
    return true;
}

StreamParser::Callbacks VideoAdapter::callbacks() {
    StreamParser::Callbacks cb;
    cb.onCodec = [this](uint32_t id) {
        if (id == kCodecH264 || id == kCodecH265) {
            if (log) log("video codec " + codecName(id));
            if (sink_) sink_->onCodec(id == kCodecH265 ? VideoCodec::H265 : VideoCodec::H264);
        } else if (id == kCodecDisabled) {
            if (log) log("video stream disabled by the device");
        } else if (id != kCodecError) {
            if (log) log("unsupported video codec 0x" + codecName(id));
        }
    };
    cb.onSession = [this](int w, int h, bool) {
        w_ = w;
        h_ = h;
        if (log) log("video size " + std::to_string(w) + "x" + std::to_string(h));
        if (sink_) sink_->onSourceSize(w, h);
        if (onSize) onSize(w, h);
    };
    cb.onPacket = [this](const uint8_t* p, size_t n, uint64_t, bool config, bool) {
        if (config) {
            // SPS/PPS: keep and prepend to the next frame (replaces any older one).
            config_.assign(p, p + n);
            return;
        }
        if (!sink_) return;
        if (!config_.empty()) {
            merged_.resize(config_.size() + n);
            memcpy(merged_.data(), config_.data(), config_.size());
            memcpy(merged_.data() + config_.size(), p, n);
            config_.clear();
            sink_->onFrame(merged_.data(), merged_.size(), 0);
        } else {
            sink_->onFrame(p, n, 0);  // 0 = show ASAP (latency first, like scrcpy)
        }
    };
    return cb;
}

StreamParser::Callbacks AudioAdapter::callbacks() {
    StreamParser::Callbacks cb;
    cb.onCodec = [this](uint32_t id) {
        if (id == kCodecAac) {
            active_ = true;
            if (log) log("audio codec aac (48000 Hz, 2 ch)");
            if (sink_) sink_->onFormat(AudioCodec::AAC_LC, 48000, 2, 1024);
        } else if (id == kCodecDisabled) {
            if (log) log("audio disabled by the device (needs Android 11+, or capture refused) - video only");
        } else if (id != kCodecError) {
            if (log) log("unsupported audio codec " + codecName(id) + " - ignored");
        }
    };
    cb.onPacket = [this](const uint8_t* p, size_t n, uint64_t, bool config, bool) {
        if (!active_ || config || !sink_) return;  // config = AudioSpecificConfig (implied by onFormat)
        sink_->onPacket(p, n, 0);
    };
    return cb;
}

// --------------------------------------------------------- control msgs --

std::vector<uint8_t> msgKeycode(uint8_t action, uint32_t keycode, uint32_t repeat, uint32_t meta) {
    std::vector<uint8_t> v{uint8_t(MsgType::InjectKeycode), action};
    wr32(v, keycode);
    wr32(v, repeat);
    wr32(v, meta);
    return v;
}

std::vector<uint8_t> msgText(const std::string& utf8) {
    size_t n = utf8Truncation(utf8, 300);
    std::vector<uint8_t> v{uint8_t(MsgType::InjectText)};
    wr32(v, uint32_t(n));
    v.insert(v.end(), utf8.begin(), utf8.begin() + n);
    return v;
}

static void writePosition(std::vector<uint8_t>& v, int32_t x, int32_t y, uint16_t w, uint16_t h) {
    wr32(v, uint32_t(x));
    wr32(v, uint32_t(y));
    wr16(v, w);
    wr16(v, h);
}

std::vector<uint8_t> msgTouch(uint8_t action, uint64_t pointerId, int32_t x, int32_t y, uint16_t w, uint16_t h,
                              float pressure, uint32_t actionButton, uint32_t buttons) {
    std::vector<uint8_t> v{uint8_t(MsgType::InjectTouch), action};
    wr64(v, pointerId);
    writePosition(v, x, y, w, h);
    wr16(v, floatToU16fp(pressure));
    wr32(v, actionButton);
    wr32(v, buttons);
    return v;
}

std::vector<uint8_t> msgScroll(int32_t x, int32_t y, uint16_t w, uint16_t h, float hscroll, float vscroll,
                               uint32_t buttons) {
    std::vector<uint8_t> v{uint8_t(MsgType::InjectScroll)};
    writePosition(v, x, y, w, h);
    wr16(v, uint16_t(floatToI16fp(std::clamp(hscroll / 16.0f, -1.0f, 1.0f))));
    wr16(v, uint16_t(floatToI16fp(std::clamp(vscroll / 16.0f, -1.0f, 1.0f))));
    wr32(v, buttons);
    return v;
}

std::vector<uint8_t> msgBackOrScreenOn(uint8_t action) { return {uint8_t(MsgType::BackOrScreenOn), action}; }
std::vector<uint8_t> msgSimple(MsgType t) { return {uint8_t(t)}; }

// ------------------------------------------------------------------ keys --

uint32_t vkToAndroidKeycode(unsigned vk) {
    if (vk >= 'A' && vk <= 'Z') return 29 + (vk - 'A');        // KEYCODE_A..Z
    if (vk >= '0' && vk <= '9') return 7 + (vk - '0');         // KEYCODE_0..9
    if (vk >= 0x60 && vk <= 0x69) return 144 + (vk - 0x60);    // NUMPAD_0..9
    if (vk >= 0x70 && vk <= 0x7B) return 131 + (vk - 0x70);    // F1..F12
    switch (vk) {
        case 0x08: return 67;   // VK_BACK → DEL
        case 0x09: return 61;   // TAB
        case 0x0D: return 66;   // ENTER
        case 0x1B: return 111;  // ESCAPE
        case 0x20: return 62;   // SPACE
        case 0x21: return 92;   // PAGE_UP
        case 0x22: return 93;   // PAGE_DOWN
        case 0x23: return 123;  // MOVE_END
        case 0x24: return 122;  // MOVE_HOME
        case 0x25: return 21;   // DPAD_LEFT
        case 0x26: return 19;   // DPAD_UP
        case 0x27: return 22;   // DPAD_RIGHT
        case 0x28: return 20;   // DPAD_DOWN
        case 0x2D: return 124;  // INSERT
        case 0x2E: return 112;  // FORWARD_DEL
        case 0x6A: return 155;  // NUMPAD_MULTIPLY
        case 0x6B: return 157;  // NUMPAD_ADD
        case 0x6D: return 156;  // NUMPAD_SUBTRACT
        case 0x6E: return 158;  // NUMPAD_DOT
        case 0x6F: return 154;  // NUMPAD_DIVIDE
        case 0xAD: return 164;  // VOLUME_MUTE
        case 0xAE: return 25;   // VOLUME_DOWN
        case 0xAF: return 24;   // VOLUME_UP
        case 0xB0: return 87;   // MEDIA_NEXT
        case 0xB1: return 88;   // MEDIA_PREVIOUS
        case 0xB2: return 86;   // MEDIA_STOP
        case 0xB3: return 85;   // MEDIA_PLAY_PAUSE
        case 0xBA: return 74;   // ;
        case 0xBB: return 70;   // =
        case 0xBC: return 55;   // ,
        case 0xBD: return 69;   // -
        case 0xBE: return 56;   // .
        case 0xBF: return 76;   // /
        case 0xC0: return 68;   // `
        case 0xDB: return 71;   // [
        case 0xDC: return 73;   // backslash
        case 0xDD: return 72;   // ]
        case 0xDE: return 75;   // '
        default: return 0;
    }
}

std::string utf16ToUtf8(const std::wstring& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = uint16_t(s[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && uint16_t(s[i + 1]) >= 0xDC00 &&
            uint16_t(s[i + 1]) <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (uint16_t(s[i + 1]) - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out.push_back(char(c));
        } else if (c < 0x800) {
            out.push_back(char(0xC0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(char(0xE0 | (c >> 12)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (c >> 18)));
            out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// ----------------------------------------------------- input translator --

void InputTranslator::setVideoSize(int w, int h) {
    if (w == w_ && h == h_) return;
    // The device ignores events for a stale size; a held touch cannot be
    // released on the new size, so just forget it.
    touching_ = false;
    w_ = w;
    h_ = h;
}

void InputTranslator::pos(float nx, float ny, int32_t& x, int32_t& y) const {
    nx = std::clamp(nx, 0.0f, 1.0f);
    ny = std::clamp(ny, 0.0f, 1.0f);
    x = std::min(int32_t(std::lround(nx * w_)), int32_t(w_ - 1));
    y = std::min(int32_t(std::lround(ny * h_)), int32_t(h_ - 1));
}

void InputTranslator::reset() {
    if (touching_ && w_ > 0 && h_ > 0)
        out_(msgTouch(kMotionUp, kPointerGenericFinger, lastX_, lastY_, uint16_t(w_), uint16_t(h_), 0.0f, 0, 0));
    touching_ = false;
}

void InputTranslator::pointer(const VideoWindow::PointerEvent& e) {
    using K = VideoWindow::PointerEvent::Kind;
    if (w_ <= 0 || h_ <= 0) return;
    int32_t x, y;
    pos(e.x, e.y, x, y);
    const uint16_t w = uint16_t(w_), h = uint16_t(h_);
    switch (e.kind) {
        case K::Down:
            if (e.button == 0) {
                if (touching_) out_(msgTouch(kMotionUp, kPointerGenericFinger, lastX_, lastY_, w, h, 0.0f, 0, 0));
                touching_ = true;
                lastX_ = x, lastY_ = y;
                out_(msgTouch(kMotionDown, kPointerGenericFinger, x, y, w, h, 1.0f, 0, 0));
            } else if (e.button == 1) {
                out_(msgBackOrScreenOn(kKeyDown));  // right click = back (turns the screen on if off)
            } else if (e.button == 2) {
                out_(msgKeycode(kKeyDown, kKeyHome, 0, 0));
            }
            break;
        case K::Move:
            if (touching_ && (x != lastX_ || y != lastY_)) {
                lastX_ = x, lastY_ = y;
                out_(msgTouch(kMotionMove, kPointerGenericFinger, x, y, w, h, 1.0f, 0, 0));
            }
            break;
        case K::Up:
            if (e.button == 0) {
                if (touching_) {
                    touching_ = false;
                    out_(msgTouch(kMotionUp, kPointerGenericFinger, x, y, w, h, 0.0f, 0, 0));
                }
            } else if (e.button == 1) {
                out_(msgBackOrScreenOn(kKeyUp));
            } else if (e.button == 2) {
                out_(msgKeycode(kKeyUp, kKeyHome, 0, 0));
            }
            break;
        case K::Wheel:
            if (e.wheelX != 0 || e.wheelY != 0) out_(msgScroll(x, y, w, h, e.wheelX, e.wheelY, 0));
            break;
    }
}

void InputTranslator::key(unsigned vk, bool down, wchar_t ch) {
    // Modifiers: tracked, never sent on their own.
    switch (vk) {
        case 0x10: case 0xA0: case 0xA1: shift_ = down; return;  // SHIFT / LSHIFT / RSHIFT
        case 0x11: case 0xA2: case 0xA3: ctrl_ = down; return;   // CONTROL
        case 0x12: case 0xA4: case 0xA5: alt_ = down; return;    // MENU (Alt)
        case 0x5B: case 0x5C: return;                            // Win keys
        default: break;
    }

    // Text (IME commits arrive with vk 0 / VK_PROCESSKEY, or a printable ch).
    if (down && ch != 0 && (vk == 0 || vk == 0xE5 || (ch >= 0x20 && ch != 0x7f && !ctrl_ && !alt_))) {
        std::wstring s;
        if (ch >= 0xD800 && ch <= 0xDBFF) { highSurrogate_ = ch; return; }
        if (ch >= 0xDC00 && ch <= 0xDFFF) {
            if (!highSurrogate_) return;
            s.push_back(highSurrogate_);
        }
        highSurrogate_ = 0;
        s.push_back(ch);
        out_(msgText(utf16ToUtf8(s)));
        return;
    }
    if (vk == 0 || vk >= 256) return;

    uint32_t kc = vkToAndroidKeycode(vk);
    if (!kc) return;
    uint32_t meta = (shift_ ? kMetaShift : 0) | (ctrl_ ? kMetaCtrl : 0) | (alt_ ? kMetaAlt : 0);
    if (down) {
        bool repeat = keyDownSent_[vk];
        keyDownSent_[vk] = true;
        out_(msgKeycode(kKeyDown, kc, repeat ? 1 : 0, meta));
    } else if (keyDownSent_[vk]) {
        keyDownSent_[vk] = false;
        out_(msgKeycode(kKeyUp, kc, 0, meta));
    }
}

}  // namespace pm::scrcpy
