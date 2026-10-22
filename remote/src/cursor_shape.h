#pragma once
// 0.8.4 游標: the PC pointer's picture, for the phone page to draw over the video itself (the pointer is
// left out of the picture: no blur, a size a finger can see, no frame to encode when only the pointer
// moves). From an HCURSOR (GetIconInfo): colour pixels with alpha, and the pixels that invert the screen
// under them (monochrome / masked pointers: the I-beam); the hot spot. PNG via WIC for the DataChannel.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pm::remote {

struct CursorShape {
    int w = 0, h = 0, hotX = 0, hotY = 0;
    std::vector<uint32_t> color;  // BGRA, straight alpha (0 = transparent)
    std::vector<uint32_t> invert; // 0xFFFFFFFF where the screen is inverted, else 0 (empty: none)
    bool anyInvert() const { return !invert.empty(); }
};

// The pointer's picture (false: none / not readable).
bool cursorShapeOf(HCURSOR h, CursorShape& out);
// The same from a DXGI pointer shape buffer (capture.cpp: Desktop Duplication's own pointer).
void cursorShapeFromDxgi(const uint8_t* buf, int type, int width, int height, int pitch, int hotX, int hotY, CursorShape& out);
// PNG of BGRA pixels (WIC), "" on failure.
std::string pngOf(const std::vector<uint32_t>& bgra, int w, int h);
std::string base64(const std::string& bytes);
// A stable id for a shape (FNV-1a of its pixels and hot spot): the page keeps each one once.
uint32_t cursorShapeId(const CursorShape& s);

}  // namespace pm::remote
