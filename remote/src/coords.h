#pragma once
// 0.8.3.5: where a point of the picture lands on the PC (header-only: also used by pm_input_test).
//
// The page sends a point of the streamed monitor as 0..1 (x, y) of the picture. Pixel i of a
// monitor w pixels wide covers [i / w, (i + 1) / w), so the pixel is floor(x * w), clamped to
// w - 1. 0.8.3.4 used left + x * (w - 1), rounded: right for the middle of a pixel, one pixel
// off on a boundary (0.75 of 1440 rows -> 1079, not 1080).
//
// SendInput absolute coordinates are 0..65535 over the whole virtual desktop (all monitors;
// MOUSEEVENTF_VIRTUALDESK). Windows does not document the rounding: a = ((x - left) * 65536 +
// 32768) / size (the middle of the pixel) gives back exactly x for every pixel of any desktop up
// to 32768 pixels under both readings, (a * size) >> 16 and a * (size - 1) / 65535 rounded.
// 0.8.3.4's lround((x - left) * 65535 / (size - 1)) is one pixel short under the first one on
// 30 of the 4880 rows + columns of a 3440x1440 desktop, 223 of 12000 on three monitors
// (pm_input_test).

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pm::remote {

// 0..1 of the streamed monitor (virtual-desktop pixels) -> the pixel.
inline POINT normToDesktop(double nx, double ny, const RECT& mon) {
    const LONG w = std::max(1L, mon.right - mon.left), h = std::max(1L, mon.bottom - mon.top);
    if (!std::isfinite(nx)) nx = 0;
    if (!std::isfinite(ny)) ny = 0;
    const LONG ix = std::clamp(LONG(std::floor(std::clamp(nx, 0.0, 1.0) * w)), 0L, w - 1);
    const LONG iy = std::clamp(LONG(std::floor(std::clamp(ny, 0.0, 1.0) * h)), 0L, h - 1);
    return POINT{mon.left + ix, mon.top + iy};
}

// A virtual-desktop pixel -> SendInput's 0..65535 (MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK).
inline LONG toAbsolute(LONG p, LONG origin, LONG size) {
    size = std::max(1L, size);
    const int64_t d = std::clamp<int64_t>(int64_t(p) - origin, 0, size - 1);
    return LONG(std::clamp<int64_t>((d * 65536 + 32768) / size, 0, 65535));
}
// What Windows does with it (the stub, the unit test).
inline LONG fromAbsolute(LONG a, LONG origin, LONG size) { return origin + LONG((int64_t(a) * std::max(1L, size)) >> 16); }

}  // namespace pm::remote
