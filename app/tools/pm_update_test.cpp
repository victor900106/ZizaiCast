// pm_update_test: 自動更新 safety checks without network or UI.
//   URL policy (https only; http only for this PC), SHA-256 from a locked
//   handle, the lock (no write / rename / delete while held), launching an exe
//   while it is locked, and Authenticode (unsigned ok, signed ok, tampered
//   signed refused).
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../updater.h"

namespace fs = std::filesystem;
using namespace pm::update;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what) {
    (ok ? g_pass : g_fail)++;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

static std::string u8(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? char(c) : '?';
    return s;
}

static HANDLE lockFile(const fs::path& p) {  // what the app holds from verification to launch
    return CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
}

int wmain() {
    std::printf("== URL policy\n");
    struct U {
        const wchar_t* url;
        bool ok;
    } urls[] = {
        {L"https://github.com/victor900106/ZizaiCast/releases/latest/download/update.json", true},
        {L"https://example.com:8443/x.exe", true},
        {L"http://127.0.0.1:8765/update.json", true},
        {L"http://localhost/update.json", true},
        {L"http://LOCALHOST:99/a", true},
        {L"http://[::1]:8765/update.json", true},
        {L"http://example.com/update.json", false},
        {L"http://192.168.1.10/update.json", false},
        {L"http://127.evil.example/update.json", false},
        {L"http://127.0.0.1.evil.example/x", false},
        {L"ftp://example.com/x.exe", false},
        {L"file:///C:/x.exe", false},
        {L"", false},
        {L"not a url", false},
    };
    for (const U& u : urls) {
        std::string why;
        const bool got = urlAllowed(u.url, why);
        check(got == u.ok, std::string(u.ok ? "allowed: " : "refused: ") + u8(u.url) + (got ? "" : " (" + why + ")"));
    }
    {
        std::string body, err;
        const bool got = httpGet(L"http://example.com/update.json", body, err);
        check(!got && err.find("http") != std::string::npos, "httpGet refuses plain http before connecting (" + err + ")");
    }

    std::printf("\n== SHA-256 and the installer lock\n");
    const fs::path dir = fs::temp_directory_path() / L"pm_update_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path f = dir / L"installer.bin";
    { std::ofstream(f, std::ios::binary) << "abc"; }
    check(sha256File(f.wstring()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256File(\"abc\")");
    HANDLE h = lockFile(f);
    check(h != INVALID_HANDLE_VALUE, "lock opened");
    check(sha256Handle(h) == sha256File(f.wstring()), "sha256Handle == sha256File (others may still read)");
    check(sha256Handle(h) == sha256Handle(h), "sha256Handle twice (rewinds)");
    HANDLE w = CreateFileW(f.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    check(w == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "locked: no one can open it for writing");
    if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
    check(!MoveFileW(f.c_str(), (dir / L"moved.bin").c_str()), "locked: cannot be renamed away");
    check(!DeleteFileW(f.c_str()), "locked: cannot be deleted");
    CloseHandle(h);
    check(DeleteFileW(f.c_str()) != 0, "unlocked: can be deleted again");

    std::printf("\n== launching a locked exe\n");
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const fs::path exe = dir / L"tool.exe";
    fs::copy_file(fs::path(sys) / L"where.exe", exe, fs::copy_options::overwrite_existing, ec);
    h = lockFile(exe);
    const std::string before = sha256Handle(h);
    std::wstring cmd = L"\"" + exe.wstring() + L"\" /?";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    const bool started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                        dir.c_str(), &si, &pi) != 0;
    check(started, "an exe held with FILE_SHARE_READ still starts");
    if (started) {
        WaitForSingleObject(pi.hProcess, 10000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        check(code == 0, "and runs (exit " + std::to_string(code) + ")");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    check(sha256Handle(h) == before && !before.empty(), "re-hash right before launch matches");
    std::string detail;
    { const Signature r = checkSignature(exe.wstring(), h, detail); check(r != Signature::Invalid, "a Windows tool copy: not refused (" + detail + ")"); }
    CloseHandle(h);

    std::printf("\n== Authenticode\n");
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const fs::path un = dir / L"unsigned.exe";
        fs::copy_file(self, un, fs::copy_options::overwrite_existing, ec);
        h = lockFile(un);
        { const Signature r = checkSignature(un.wstring(), h, detail); check(r == Signature::Unsigned, "our unsigned build: Unsigned (" + detail + ")"); }
        CloseHandle(h);
    }
    // An exe with an embedded signature (any of these that exists here).
    fs::path signedExe;
    for (const wchar_t* c : {L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
                             L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
                             L"C:\\Program Files\\Git\\cmd\\git.exe", L"C:\\Program Files\\nodejs\\node.exe",
                             L"C:\\Program Files\\dotnet\\dotnet.exe"}) {
        if (fs::exists(c, ec) && checkSignature(c, nullptr, detail) == Signature::Valid) {
            signedExe = c;
            break;
        }
    }
    if (signedExe.empty()) {
        std::printf("skip  no embedded-signed exe found for the signed / tampered checks\n");
    } else {
        const fs::path s = dir / L"signed.exe", t = dir / L"tampered.exe";
        fs::copy_file(signedExe, s, fs::copy_options::overwrite_existing, ec);
        h = lockFile(s);
        { const Signature r = checkSignature(s.wstring(), h, detail); check(r == Signature::Valid, "signed copy of " + u8(signedExe.filename().wstring()) + ": Valid"); }
        CloseHandle(h);
        fs::copy_file(signedExe, t, fs::copy_options::overwrite_existing, ec);
        {
            std::fstream io(t, std::ios::in | std::ios::out | std::ios::binary);
            io.seekg(0x400);
            char b = 0;
            io.read(&b, 1);
            io.seekp(0x400);
            b = char(b ^ 0x5A);
            io.write(&b, 1);
        }
        h = lockFile(t);
        { const Signature r = checkSignature(t.wstring(), h, detail); check(r == Signature::Invalid, "one byte changed: Invalid (" + detail + ")"); }
        CloseHandle(h);
    }
    fs::remove_all(dir, ec);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
