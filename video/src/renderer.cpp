#include "renderer.h"

#include <d3dcompiler.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "pm/i18n.h"
#include "renderer_internal.h"

namespace pm::video {

using namespace detail;  // 拆檔 0.7.9: was this file's anonymous namespace

double clockMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

void Renderer::screenToPicture(int rot, bool mirror, float tx, float ty, float& sx, float& sy) {
    float u[4], v[4];
    pictureTransform(rot, mirror, u, v);
    sx = u[0] * tx + u[1] * ty + u[2];
    sy = v[0] * tx + v[1] * ty + v[2];
}

}  // namespace pm::video
