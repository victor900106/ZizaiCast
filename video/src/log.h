#pragma once

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>

namespace pm::video {

// printf-style log to stderr, the debugger and VideoWindow::setLogHandler's
// handler, prefixed with "[video]" (wdlog: "[video-watchdog]", every
// self-healing action).
void log(const char* fmt, ...);
void wdlog(const char* fmt, ...);

// VideoWindow::setLogHandler's handler and its lock (defined in video_log.cpp).
extern std::mutex g_logM;
extern std::shared_ptr<std::function<void(const char*)>> g_logFn;

}  // namespace pm::video
