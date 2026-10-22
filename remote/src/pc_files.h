#pragma once
// 0.8.4 電腦的檔案: the phone browses a few folders of this PC and downloads files from them.
//
// Only "roots" are ever reachable: by default 桌面 / 文件 / 下載 / 圖片 / 影片 and the app's 截圖 / 錄影
// folders; anything else only when the PC's owner added it (允許清單). The phone names a root by id and
// a path *relative* to it ("a/b/c.txt"); nothing it sends is ever used as a path as it is:
//   1. cleanRelPath(): '/'-separated parts only; no "", ".", "..", dots / spaces only, a trailing dot or
//      space, '\\', ':' (drive letters, alternate data streams), wildcards, control characters,
//      reserved device names (CON, NUL, COM1 ...); at most 32 parts / 1024 bytes.
//   2. the joined path is opened and its *final* path asked from Windows (GetFinalPathNameByHandleW:
//      junctions, symbolic links, 8.3 names, mapped drives all resolved), and
//   3. that canonical path must be the root's own canonical path or below it (case-insensitive,
//      on a '\\' boundary: "C:\Users\a\Docs2" is not under "C:\Users\a\Docs").
// A link inside a root that points outside it is therefore refused (and left out of listings).
// Hidden + system entries (desktop.ini, thumbs) are not listed.
//
// Transport-neutral: the host turns a resolved file into a one-time download key (HTTP dl/<key>
// today; a DataChannel transfer can take the same canonical path later).

#include <cstdint>
#include <string>
#include <vector>

namespace pm::remote {

struct PcRoot {
    std::string id;     // "desktop", "documents", "downloads", "pictures", "videos", "shots", "recordings", "extra0".."extra15"
    std::string kind;   // the same without the number (the page names it in its own language)
    std::string label;  // the folder's own name (UTF-8), for extra folders and the 連線紀錄
    std::wstring path;  // canonical (finalPath) at the time the list was made
};

// The default folders (that exist) + extra ones, duplicates (same canonical path) dropped.
// shots / recordings: the app's folders ("" = none). testRoots (tests): "id=path" pairs replace
// everything (no default folder of this PC is then used).
std::vector<PcRoot> pcRoots(const std::wstring& shotDir, const std::wstring& recDir, const std::vector<std::wstring>& extra,
                            const std::vector<std::wstring>& testRoots = {});

// Step 1 above. out: the parts joined with '\\' ("" = the root itself).
bool cleanRelPath(const std::string& relUtf8, std::wstring& out);
// The canonical final path of an existing file / folder ("\\?\" removed; "\\?\UNC\" -> "\\"), "" if it cannot be opened.
std::wstring finalPath(const std::wstring& path);
// canonPath == canonRoot or below it (both canonical).
bool underRoot(const std::wstring& canonRoot, const std::wstring& canonPath);

enum class PcPathErr { Ok, BadPath, NoRoot, NotFound, Escape, NotFile, NotDir };
const char* pcPathErrName(PcPathErr e);

// Steps 1-3: root + rel -> canonical path (a folder when wantDir, else a regular file).
PcPathErr resolvePcPath(const std::vector<PcRoot>& roots, const std::string& rootId, const std::string& rel, bool wantDir,
                        std::wstring& canon, const PcRoot** rootOut = nullptr);

struct PcEntry {
    std::string name;   // UTF-8
    bool dir = false;
    uint64_t size = 0;
    int64_t mtime = 0;  // Unix seconds
};
// One folder's entries (folders first, then by name), at most maxEntries (truncated = there were more).
PcPathErr listPcDir(const std::vector<PcRoot>& roots, const std::string& rootId, const std::string& rel, std::vector<PcEntry>& out,
                    bool& truncated, size_t maxEntries = 2000);

}  // namespace pm::remote
