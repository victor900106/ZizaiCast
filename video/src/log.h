#pragma once

#include <cstdarg>
#include <cstdio>

namespace pm::video {

// printf-style log to stderr, the debugger and VideoWindow::setLogHandler's
// handler, prefixed with "[video]" (wdlog: "[video-watchdog]", every
// self-healing action).
void log(const char* fmt, ...);
void wdlog(const char* fmt, ...);

}  // namespace pm::video
