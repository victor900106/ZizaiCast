// pm_translate's log lines: stderr, and setLogHandler's handler (the app's log file).
#include "tr_log.h"

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "pm/translate.h"

namespace pm::translate {

namespace {
std::mutex gLogMu;
std::shared_ptr<std::function<void(const char*, const std::string&)>> gLogFn;
}  // namespace

void setLogHandler(std::function<void(const char* level, const std::string& line)> fn) {
    std::lock_guard lk(gLogMu);
    gLogFn = fn ? std::make_shared<std::function<void(const char*, const std::string&)>>(std::move(fn)) : nullptr;
}

void trLog(const char* level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    std::shared_ptr<std::function<void(const char*, const std::string&)>> fn;
    {
        std::lock_guard lk(gLogMu);
        fn = gLogFn;
    }
    std::fprintf(stderr, "[tr-%s] %s\n", level, buf);
    if (fn && *fn) (*fn)(level, buf);
}

}  // namespace pm::translate
