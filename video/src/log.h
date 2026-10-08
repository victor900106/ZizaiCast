#pragma once

#include <cstdarg>
#include <cstdio>

namespace pm::video {

// printf-style log to stderr and the debugger, prefixed with "[video]".
void log(const char* fmt, ...);

}  // namespace pm::video
