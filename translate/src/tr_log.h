// pm_translate's log (pm/translate.h setLogHandler): stderr + the app's log file.
#pragma once

namespace pm::translate {

// printf-style; level "info" / "warn" / "error".  Any thread.
void trLog(const char* level, const char* fmt, ...);

}  // namespace pm::translate
