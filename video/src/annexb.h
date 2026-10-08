// Minimal Annex-B helpers (header-only): NAL iteration, keyframe detection and
// an access-unit splitter used by the test tool.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pm/media.h"

namespace pm::annexb {

struct Nal {
    const uint8_t* data;  // first byte of the NAL header (after start code)
    size_t size;
    size_t startCodeOffset;  // offset of the start code in the buffer
};

// Calls fn(Nal) for each NAL unit in an Annex-B buffer.
template <class Fn>
inline void forEachNal(const uint8_t* p, size_t n, Fn&& fn) {
    auto findStart = [&](size_t from, size_t& scLen) -> size_t {
        for (size_t i = from; i + 3 <= n; ++i) {
            if (p[i] == 0 && p[i + 1] == 0) {
                if (p[i + 2] == 1) { scLen = 3; return i; }
                if (i + 4 <= n && p[i + 2] == 0 && p[i + 3] == 1) { scLen = 4; return i; }
            }
        }
        scLen = 0;
        return n;
    };
    size_t sc = 0;
    size_t pos = findStart(0, sc);
    while (pos < n) {
        size_t begin = pos + sc;
        size_t nsc = 0;
        size_t next = findStart(begin, nsc);
        size_t end = next;
        // Trailing zero bytes belong to the next start code (zero_byte).
        while (end > begin && p[end - 1] == 0 && next < n) --end;
        if (end > begin) fn(Nal{p + begin, end - begin, pos});
        pos = next;
        sc = nsc;
    }
}

inline int nalType(VideoCodec c, const uint8_t* nal) {
    return c == VideoCodec::H264 ? (nal[0] & 0x1F) : ((nal[0] >> 1) & 0x3F);
}

inline bool isVcl(VideoCodec c, int t) {
    return c == VideoCodec::H264 ? (t >= 1 && t <= 5) : (t >= 0 && t <= 31);
}

inline bool isParamSet(VideoCodec c, int t) {
    return c == VideoCodec::H264 ? (t == 7 || t == 8) : (t >= 32 && t <= 34);
}

inline bool isIrap(VideoCodec c, int t) {
    return c == VideoCodec::H264 ? (t == 5) : (t >= 16 && t <= 23);
}

// True if the access unit contains an IDR/IRAP picture or parameter sets
// (i.e. it must never be dropped).
inline bool isKeyAccessUnit(VideoCodec c, const uint8_t* p, size_t n) {
    bool key = false;
    forEachNal(p, n, [&](const Nal& nal) {
        int t = nalType(c, nal.data);
        if (isIrap(c, t) || isParamSet(c, t)) key = true;
    });
    return key;
}

// Splits a whole Annex-B elementary stream into access units.
// Returns [offset, length) pairs into the input buffer.
inline std::vector<std::pair<size_t, size_t>> splitAccessUnits(VideoCodec c, const uint8_t* p, size_t n) {
    std::vector<std::pair<size_t, size_t>> aus;
    size_t auStart = SIZE_MAX;
    bool haveVcl = false;
    forEachNal(p, n, [&](const Nal& nal) {
        int t = nalType(c, nal.data);
        bool startsNew = false;
        if (isVcl(c, t)) {
            size_t hdr = c == VideoCodec::H264 ? 1 : 2;
            // H.264: first_mb_in_slice == 0 -> ue(v) '1' bit.
            // HEVC: first_slice_segment_in_pic_flag.
            bool firstSlice = nal.size > hdr && (nal.data[hdr] & 0x80);
            if (firstSlice && haveVcl) startsNew = true;
            if (startsNew || auStart == SIZE_MAX) {
                if (startsNew) aus.emplace_back(auStart, nal.startCodeOffset - auStart);
                auStart = nal.startCodeOffset;
                haveVcl = false;
            }
            haveVcl = true;
        } else {
            // AUD, SPS/PPS/VPS, prefix SEI start a new AU once a picture was seen.
            bool prefix = c == VideoCodec::H264 ? (t == 6 || t == 7 || t == 8 || t == 9)
                                                : (t == 32 || t == 33 || t == 34 || t == 35 || t == 39);
            if (prefix && haveVcl) {
                aus.emplace_back(auStart, nal.startCodeOffset - auStart);
                auStart = nal.startCodeOffset;
                haveVcl = false;
            } else if (auStart == SIZE_MAX) {
                auStart = nal.startCodeOffset;
            }
        }
    });
    if (auStart != SIZE_MAX && auStart < n) aus.emplace_back(auStart, n - auStart);
    return aus;
}

}  // namespace pm::annexb
