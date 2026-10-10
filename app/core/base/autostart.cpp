// 自在投影 app: core/base/autostart.cpp — core/base（第 0 層：共用型別、狀態與原語）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// Autostart: HKCU\...\Run\自在投影 = "<exe>" --background. The registry is the
// source of truth (the installer's optional task writes the same value). The
// value name is an identifier shared with the installer, not a UI string.
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"自在投影";

std::wstring autostartCommand() { return L"\"" + exePath().wstring() + L"\" --background"; }

std::wstring readAutostart() {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return {};
    std::wstring v(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, v.data(), &bytes) != ERROR_SUCCESS)
        return {};
    v.resize(wcslen(v.c_str()));
    return v;
}

bool setAutostart(bool on) {
    if (!on) {
        LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
    }
    const std::wstring cmd = autostartCommand();
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// The entry points at an exe that no longer exists (folder moved): repoint it
// at this exe. An entry for another existing copy is left alone.
void repairAutostart() {
    std::wstring v = readAutostart();
    if (v.empty()) return;
    std::wstring path = v;
    if (!path.empty() && path[0] == L'"') {
        auto end = path.find(L'"', 1);
        path = path.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
    }
    std::error_code ec;
    if (!fs::exists(path, ec)) setAutostart(true);
}

}  // namespace pm_app
