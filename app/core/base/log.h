// 自在投影 app: phonemirror.log 記錄器。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/base.h"
#include "core/base/log_throttle.h"

namespace pm_app {

class Log {
public:
    // 0.8.3: phonemirror.log is rotated while the program runs too (it used to be checked only at
    // start): over kMaxBytes it becomes phonemirror.old.log, the older ones shift to .old2.log and
    // .old3.log, the oldest is deleted. At most kKeep + 1 files of kMaxBytes each (4 x 4 MB). (0.8.4: unchanged; lines are thinned out first, see log_throttle.h.)
    static constexpr std::uintmax_t kMaxBytes = 4u << 20;
    static constexpr int kKeep = 3;

    explicit Log(const fs::path& file) : file_(file) {
        rotate(file_);  // (the video module logs a summary line every 5 s while mirroring)
        std::error_code ec;
        const auto size = fs::file_size(file_, ec);
        bytes_ = ec ? 0 : size;
        out_.open(file_, std::ios::app);
    }
    // phonemirror.log -> .old.log -> .old2.log -> .old3.log when the file is over kMaxBytes.
    static void rotate(const fs::path& file, bool force = false) {
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        if (ec || (!force && size < kMaxBytes)) return;
        for (int i = kKeep; i >= 1; --i) {
            const fs::path to = oldName(file, i);
            if (i == kKeep) fs::remove(to, ec);
            else fs::rename(oldName(file, i), oldName(file, i + 1), ec);
        }
        fs::rename(file, oldName(file, 1), ec);
        if (ec) fs::remove(file, ec);  // cannot keep it (locked?): start empty rather than grow forever
    }
    static fs::path oldName(const fs::path& file, int n) {
        fs::path old = file;
        old.replace_extension(n == 1 ? L".old.log" : (L".old" + std::to_wstring(n) + L".log"));
        return old;
    }
    void write(const char* level, const std::string& msg) {
        std::lock_guard<std::mutex> lock(mu_);
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char ts[32];
        std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
        // 0.8.4: chatty / identical lines are thinned out (see log_throttle.h)
        const LogThrottle::Verdict v = throttle_.check(level, msg, static_cast<int64_t>(t));
        if (!v.write) return;
        std::string line;
        if (v.flushDup)
            line += std::string(ts) + " [info] (the line above was repeated " + std::to_string(v.flushDup) + " more times)\n";
        line += std::string(ts) + " [" + level + "] " + msg;
        if (v.suppressed)
            line += " (+" + std::to_string(v.suppressed) + " similar lines left out in the last " +
                    std::to_string(v.ruleSeconds) + " s)";
        else if (v.dupNow)
            line += " (+" + std::to_string(v.dupNow) + " identical lines left out)";
        line += "\n";
        if (bytes_ + line.size() >= kMaxBytes && out_.is_open()) {  // rotate now, then this line opens the new file
            out_.close();
            rotate(file_, true);
            out_.open(file_, std::ios::app);
            bytes_ = 0;
        }
        out_ << line;
        out_.flush();
        bytes_ += line.size();
        OutputDebugStringA(line.c_str());
    }

private:
    fs::path file_;
    std::uintmax_t bytes_ = 0;
    std::mutex mu_;
    std::ofstream out_;
    LogThrottle throttle_;
};

}  // namespace pm_app
