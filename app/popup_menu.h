// Custom-drawn popup menu in the app's theme (warm dark card, blanket-pink
// accent), used for the window's right-click menu and the tray menu instead
// of plain Win32 HMENUs. Layered top-level windows rendered with
// Direct2D/DirectWrite; per-monitor DPI aware. See docs/app.md.
#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ID2D1RenderTarget;

namespace pm::ui {

struct MenuItem {
    enum class Kind { Command, Submenu, Separator, Caption, Note, Header };
    Kind kind = Kind::Command;
    UINT id = 0;              // returned by trackMenu for a Command
    std::wstring text;
    std::wstring right;       // right-aligned shortcut / detail text
    wchar_t icon = 0;         // Segoe Fluent Icons / Segoe MDL2 Assets code point (0 = none)
    bool checkable = false;   // pink check mark when checked
    bool radio = false;       // pink radio dot when checked, ring otherwise
    bool checked = false;
    bool enabled = true;
    bool bold = false;        // default item
    // Colour swatch drawn in the icon column instead of a glyph (主題 items):
    // background, card and accent as 0xRRGGBB (VideoWindow::themeSwatch).
    bool hasSwatch = false;
    uint32_t swatch[3] = {0, 0, 0};
    std::vector<MenuItem> sub;  // Kind::Submenu

    static MenuItem command(UINT id, std::wstring text, wchar_t icon = 0, std::wstring right = {}) {
        MenuItem m;
        m.id = id;
        m.text = std::move(text);
        m.icon = icon;
        m.right = std::move(right);
        return m;
    }
    static MenuItem separator() {
        MenuItem m;
        m.kind = Kind::Separator;
        return m;
    }
    static MenuItem caption(std::wstring text) {  // small dim group title
        MenuItem m;
        m.kind = Kind::Caption;
        m.text = std::move(text);
        return m;
    }
    static MenuItem note(std::wstring text) {  // small dim hint line, not selectable
        MenuItem m;
        m.kind = Kind::Note;
        m.text = std::move(text);
        return m;
    }
    static MenuItem header(std::wstring text, std::wstring right = {}) {  // app icon + name
        MenuItem m;
        m.kind = Kind::Header;
        m.text = std::move(text);
        m.right = std::move(right);
        return m;
    }
    static MenuItem submenu(std::wstring text, wchar_t icon, std::vector<MenuItem> items) {
        MenuItem m;
        m.kind = Kind::Submenu;
        m.text = std::move(text);
        m.icon = icon;
        m.sub = std::move(items);
        return m;
    }
};

struct MenuOptions {
    bool selectFirst = false;       // opened from the keyboard: focus the first item
    HINSTANCE iconInstance = nullptr;
    int headerIconId = 0;           // icon resource drawn by a Header item
};

// Re-themes every menu opened from now on, derived from a theme swatch
// (background, card, accent as 0xRRGGBB): card gradient from the card colour,
// text light or dark depending on the card, highlights in the accent.
void setPalette(uint32_t background, uint32_t card, uint32_t accent);

// The colours derived by setPalette (0xRRGGBB), for other themed windows
// (the Android pairing panel) to match the menus.
struct Colors {
    uint32_t cardTop, cardBottom, fg, dim, accent, shadow;
    bool light;  // light card (dark text)
};
Colors currentColors();

// Creates the shared Direct2D / DirectWrite objects ahead of the first menu.
void warmUp();

// Shows the menu with its top-left corner at `pt` (screen pixels; flipped /
// clamped to stay on the monitor's work area) and runs a modal loop until
// an item is chosen (returns its id) or the menu is dismissed (returns 0).
// Call SetForegroundWindow(owner) first for tray menus. Not re-entrant: a
// call while a menu is open returns 0 at once.
UINT trackMenu(HWND owner, const std::vector<MenuItem>& items, POINT pt, const MenuOptions& options = {});

// Time from trackMenu() to the menu being on screen, for the log.
double lastOpenMs();
// Development screenshots: draws a menu (root level only, row hotRow
// highlighted, -1 none) into a 32-bit PNG without showing a window.
bool renderMenuPng(const std::vector<MenuItem>& items, const std::wstring& path, const MenuOptions& options = {},
                   int hotRow = -1);
// Development screenshots of the other themed windows (pairing panel, About):
// `draw` paints a widthDip x heightDip DIP card at `dpi` into a software
// Direct2D target (BeginDraw / EndDraw are done here), saved as a 32-bit PNG.
bool renderToPng(float widthDip, float heightDip, UINT dpi, const std::wstring& path,
                 const std::function<void(ID2D1RenderTarget*)>& draw);

}  // namespace pm::ui
