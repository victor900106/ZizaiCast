#pragma once
// 0.8.3.7: a file from the phone keeps the name the browser gave it, but an image's extension follows what the
// bytes are. iPhone Safari may hand the page a converted copy of a photo (HEIC -> JPEG when the page's file
// picker asks for "most compatible", PNG / JPEG -> HEIC with some accept lists: WebKit bugs 267277 / 292350 /
// 303803); the name usually follows, but a mismatch (IMG_0001.HEIC holding a JPEG) would not open in Windows'
// Photos by its extension. Only the image / video extensions below are ever changed; anything else is kept.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>

namespace pm::remote {

// What the first bytes say: L".jpg", L".png", L".gif", L".heic", L".avif", L".webp", L".mov", L".mp4", or L"" (unknown).
inline std::wstring sniffExt(const uint8_t* b, size_t n) {
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return L".jpg";
    if (n >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8)) return L".png";
    if (n >= 6 && (!memcmp(b, "GIF87a", 6) || !memcmp(b, "GIF89a", 6))) return L".gif";
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) return L".webp";
    if (n >= 12 && !memcmp(b + 4, "ftyp", 4)) {
        const std::string brand(reinterpret_cast<const char*>(b + 8), 4);
        if (brand == "heic" || brand == "heix" || brand == "heim" || brand == "heis" || brand == "hevc" || brand == "hevx" || brand == "mif1" || brand == "msf1")
            return L".heic";
        if (brand == "avif" || brand == "avis") return L".avif";
        if (brand == "qt  ") return L".mov";
        if (brand == "isom" || brand == "iso2" || brand == "mp41" || brand == "mp42" || brand == "M4V " || brand == "avc1") return L".mp4";
    }
    return L"";
}

// The name with its extension corrected when it is an image / video one that does not match the bytes.
inline std::wstring fixMediaExt(const std::wstring& name, const uint8_t* head, size_t n) {
    const std::wstring kind = sniffExt(head, n);
    if (kind.empty()) return name;
    const size_t dot = name.rfind(L'.');
    std::wstring ext = dot == std::wstring::npos || dot == 0 ? L"" : name.substr(dot);
    for (wchar_t& c : ext) c = wchar_t(towlower(c));
    auto same = [&](const wchar_t* const* list) { for (; *list; ++list) if (ext == *list) return true; return false; };
    static const wchar_t* const kJpg[] = {L".jpg", L".jpeg", L".jpe", nullptr}, *const kHeic[] = {L".heic", L".heif", L".hif", nullptr};
    static const wchar_t* const kMedia[] = {L".jpg", L".jpeg", L".jpe", L".png", L".gif", L".webp", L".heic", L".heif", L".hif", L".avif", L".mov", L".mp4", L".m4v", nullptr};
    if (kind == L".jpg" ? same(kJpg) : kind == L".heic" ? same(kHeic) : kind == L".mp4" ? (ext == L".mp4" || ext == L".m4v") : ext == kind) return name;
    if (!ext.empty() && !same(kMedia)) return name;  // another kind of file that happens to start like an image: left alone
    const std::wstring stem = ext.empty() ? name : name.substr(0, dot);
    return stem + kind;
}

}  // namespace pm::remote
