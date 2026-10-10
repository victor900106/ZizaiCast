// 自在投影 app: phonemirror.log 記錄器。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/base.h"

namespace pm_app {

class Log {
public:
    explicit Log(const fs::path& file) : out_((rotate(file), file), std::ios::app) {}
    // Over 8 MB at start: kept as phonemirror.old.log (the video module logs
    // a summary line every 5 s while mirroring).
    static void rotate(const fs::path& file) {
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        if (ec || size < (8u << 20)) return;
        fs::path old = file;
        old.replace_extension(L".old.log");
        fs::rename(file, old, ec);
    }
    void write(const char* level, const std::string& msg) {
        std::lock_guard<std::mutex> lock(mu_);
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char ts[32];
        std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
        std::string line = std::string(ts) + " [" + level + "] " + msg + "\n";
        out_ << line;
        out_.flush();
        OutputDebugStringA(line.c_str());
    }

private:
    std::mutex mu_;
    std::ofstream out_;
};

}  // namespace pm_app
