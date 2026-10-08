#include "wireless_display.h"

#include <windows.h>

#include <cwchar>

namespace pm::miracast {

FeatureState wirelessDisplayFeatureState() {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\Packages", 0,
                      KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS)
        return FeatureState::Unknown;
    static const wchar_t kPrefix[] = L"Microsoft-Windows-WirelessDisplay-FOD-Package~";
    // Language-neutral package names look like "...~amd64~~10.0.26100.1"
    // (language packs have "~zh-TW~"): look at those only.
    FeatureState best = FeatureState::NotInstalled;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = ARRAYSIZE(name);
        const LONG r = RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        if (_wcsnicmp(name, kPrefix, ARRAYSIZE(kPrefix) - 1) != 0 || !wcsstr(name, L"~~")) continue;
        DWORD state = 0, size = sizeof state;
        if (RegGetValueW(root, name, L"CurrentState", RRF_RT_REG_DWORD, nullptr, &state, &size) != ERROR_SUCCESS)
            continue;
        // CBS states: 0x00 Absent, 0x40 Staged, 0x50 Superseded, 0x60 Install
        // Pending (reboot), 0x65 Partially Installed, 0x70 Installed, 0x80 Permanent.
        if (state == 0x70 || state == 0x80) {
            best = FeatureState::Installed;
            break;
        }
        if (state == 0x60 || state == 0x65) best = FeatureState::PendingReboot;
    }
    RegCloseKey(root);
    return best;
}

}  // namespace pm::miracast
