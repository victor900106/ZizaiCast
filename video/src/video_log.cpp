// pm::video log lines ("[video]", "[video-watchdog]") to stderr, the debugger and
// VideoWindow::setLogHandler's handler.
// 拆檔 0.7.9：自 video_window.cpp 原樣搬出（g_logM / g_logFn 改成外部連結，見 log.h）。
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>

namespace pm::video {

std::mutex g_logM;
std::shared_ptr<std::function<void(const char*)>> g_logFn;  // VideoWindow::setLogHandler

namespace {

void vlog(const char* tag, const char* fmt, va_list ap) {
    char buf[1024];
    int n = std::snprintf(buf, sizeof(buf), "%s ", tag);
    int m = std::vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    size_t end = std::min(sizeof(buf) - 2, static_cast<size_t>(n) + static_cast<size_t>(std::max(m, 0)));
    buf[end] = 0;
    std::shared_ptr<std::function<void(const char*)>> fn;
    {
        std::lock_guard lk(g_logM);
        fn = g_logFn;
    }
    if (fn && *fn) (*fn)(buf);  // the app's log file (no newline)
    buf[end] = '\n';
    buf[end + 1] = 0;
    std::fputs(buf, stderr);
    OutputDebugStringA(buf);
}

}  // namespace

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("[video]", fmt, ap);
    va_end(ap);
}

void wdlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("[video-watchdog]", fmt, ap);
    va_end(ap);
}

}  // namespace pm::video
