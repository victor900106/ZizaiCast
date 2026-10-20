// PC side of the clipboard sync (AndroidSource::setClipboardSync): reads /
// writes the Windows clipboard as UTF-8 text.  An interface so tests use an
// in-memory clipboard (never the owner's real one).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace pm::clip {

class Backend {
public:
    virtual ~Backend() = default;
    // Changes whenever the clipboard content changes (any app, or setText).
    virtual uint32_t sequence() = 0;
    // CF_UNICODETEXT as UTF-8; false if the clipboard holds no text.
    virtual bool getText(std::string& utf8) = 0;
    // Replaces the clipboard content with the text.  Returns the sequence
    // number the clipboard has right after the change (0 on failure), so
    // the caller can tell its own change from the user's next copy.
    virtual uint32_t setText(const std::string& utf8) = 0;
};

// The real Windows clipboard.  Owns a hidden message-only window (HWND_MESSAGE,
// never visible) on its own thread: clipboard ownership needs a window and
// its owner must keep pumping messages.
std::unique_ptr<Backend> makeWindowsBackend();

// In-memory clipboard (tests; thread-safe).
class MemoryBackend : public Backend {
public:
    uint32_t sequence() override {
        std::lock_guard<std::mutex> l(m_);
        return seq_;
    }
    bool getText(std::string& utf8) override {
        std::lock_guard<std::mutex> l(m_);
        if (!has_) return false;
        utf8 = text_;
        return true;
    }
    uint32_t setText(const std::string& utf8) override {
        std::lock_guard<std::mutex> l(m_);
        text_ = utf8;
        has_ = true;
        ++writes_;
        return ++seq_;
    }
    // A "user" copy on the PC.
    void userCopy(const std::string& utf8) { setText(utf8); }
    int writes() {
        std::lock_guard<std::mutex> l(m_);
        return writes_;
    }

private:
    std::mutex m_;
    int writes_ = 0;
    std::string text_;
    bool has_ = false;
    uint32_t seq_ = 1;
};

}  // namespace pm::clip
