// 0.8.3.7: a file from the phone keeps its name; an image's extension follows its bytes (file_kind.h).
//   pm_filekind_test.exe -> each check, exit code 0 when all pass
#include <cstdio>
#include <string>
#include <vector>

#include "file_kind.h"

using namespace pm::remote;
static int fails = 0, passes = 0;

static std::vector<uint8_t> ftyp(const char* brand) {
    std::vector<uint8_t> b{0, 0, 0, 24, 'f', 't', 'y', 'p'};
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(brand[i]));
    b.resize(24, 0);
    return b;
}

static void expect(const wchar_t* name, const std::vector<uint8_t>& head, const wchar_t* want, const char* what) {
    const std::wstring got = fixMediaExt(name, head.data(), head.size());
    const bool ok = got == want;
    printf("%s  %-58s %ls -> %ls\n", ok ? "PASS" : "FAIL", what, name, got.c_str());
    (ok ? passes : fails)++;
}

int main() {
    const std::vector<uint8_t> jpg{0xFF, 0xD8, 0xFF, 0xE1, 0, 0, 'E', 'x', 'i', 'f'}, png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13},
        txt{'h', 'e', 'l', 'l', 'o'}, gif{'G', 'I', 'F', '8', '9', 'a'};
    expect(L"IMG_9575.png", png, L"IMG_9575.png", "owner's screenshot (PNG, as sent on 2026-10-22): kept");
    expect(L"IMG_0001.HEIC", ftyp("heic"), L"IMG_0001.HEIC", "HEIC original: kept as is (Windows opens HEIC)");
    expect(L"IMG_0001.HEIC", jpg, L"IMG_0001.jpg", "named HEIC, holds JPEG (converted on the phone)");
    expect(L"IMG_0002.JPG", ftyp("mif1"), L"IMG_0002.heic", "named JPG, holds HEIF (Safari 17 accept=image/heic case)");
    expect(L"IMG_0003.jpeg", jpg, L"IMG_0003.jpeg", ".jpeg holding JPEG: kept");
    expect(L"IMG_0004.png", jpg, L"IMG_0004.jpg", "named PNG, holds JPEG");
    expect(L"image", jpg, L"image.jpg", "no extension, JPEG: .jpg added");
    expect(L"IMG_0005.MOV", ftyp("qt  "), L"IMG_0005.MOV", "QuickTime video: kept");
    expect(L"clip.mp4", ftyp("isom"), L"clip.mp4", "MP4: kept");
    expect(L"report.pdf", jpg, L"report.pdf", "not an image extension: never changed");
    expect(L"notes.txt", txt, L"notes.txt", "unknown bytes: kept");
    expect(L"anim.gif", gif, L"anim.gif", "GIF: kept");
    expect(L"x.heif", ftyp("heic"), L"x.heif", ".heif holding HEIC: kept");
    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
