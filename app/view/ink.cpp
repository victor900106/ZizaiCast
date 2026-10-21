// 自在投影 app: view/ink.cpp — 簡報畫筆 (0.8.3).
// Drawing itself is in video/ (VideoWindow ink API, ink_layer.*): strokes in
// the phone picture's own space, drawn with Direct2D, optionally burnt into
// screenshots / recordings.  Here: the toolbar group, the 畫筆 ▸ menu, the
// shortcuts (Ctrl+E, Ctrl+Shift+E, Ctrl+Z, Esc) and the settings.
// While ink mode is on the video window gives the pointer handler nothing,
// so an Android phone is not touched by the strokes.

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "menus/menus.h"
#include "view/ink.h"

namespace pm_app {

namespace {

constexpr uint32_t kInkColors[6] = {0xFF3B30, 0xFFD60A, 0x0A84FF, 0x30D158, 0xFFFFFF, 0x1C1C1E};
constexpr S kInkColorNames[6] = {S::InkRed, S::InkYellow, S::InkBlue, S::InkGreen, S::InkWhite, S::InkBlack};
constexpr float kInkWidths[3] = {3.f, 5.5f, 10.f};  // DIPs at zoom 1 (the highlighter is ~3x)
constexpr float kInkWidthDisc[3] = {0.25f, 0.55f, 1.f};
constexpr S kInkWidthNames[3] = {S::InkThin, S::InkMedium, S::InkThick};
constexpr S kInkToolNames[3] = {S::InkPen, S::InkHighlighter, S::InkArrow};
constexpr wchar_t kIcoInk = 0xE70F;    // Edit (pencil)
constexpr wchar_t kIcoUndo = 0xE7A7;   // Undo
constexpr wchar_t kIcoErase = 0xE75C;  // EraseTool
constexpr wchar_t kToolIcons[3] = {0xE70F, 0xE7E6, 0xE72A};  // Edit, Highlight, Forward (arrow)

void applyStyle() {
    pm::VideoWindow::InkStyle st;
    st.tool = static_cast<pm::VideoWindow::InkTool>(g.settings.inkTool % 3);
    st.color = kInkColors[g.settings.inkColor % 6];
    st.widthDip = kInkWidths[g.settings.inkWidth % 3];
    g.window->setInkStyle(st);
}

// 「螢光筆 · 黃 · 中」 after a change from the toolbar / menu.
void styleToast() {
    g.window->showToast(std::wstring(tr(kInkToolNames[g.settings.inkTool % 3])) + L" · " +
                            tr(kInkColorNames[g.settings.inkColor % 6]) + L" · " + tr(kInkWidthNames[g.settings.inkWidth % 3]),
                        1500);
}

// A tool / colour / thickness picked: saved, used for the next stroke, and
// (with a picture) ink mode on -- picking a pen means "I want to draw".
void styleChanged() {
    saveSettings();
    applyStyle();
    if (!inkOn() && viewAvailable()) setInkMode(true, false);
    styleToast();
    refreshToolbar();
}

}  // namespace

void inkInit() {
    applyStyle();
    g.window->setInkCapture(g.settings.inkInShots, g.settings.inkInRec);
    g.window->setInkHandler([](int strokes) {
        static int last = -1;
        if (strokes != last) g.log->write("info", "ink: " + std::to_string(strokes) + " stroke(s)");
        last = strokes;
        refreshToolbar();
    });
}

bool inkOn() { return g.window && g.window->inkMode(); }

void setInkMode(bool on, bool toast) {
    if (on && !viewAvailable()) {
        if (toast) g.window->showToast(tr(S::MenuNeedPicture));
        return;
    }
    if (on == inkOn()) return;
    g.window->setInkMode(on, on ? std::wstring(tr(S::InkBadge0834))  /* 0.8.3.4: Esc or Ctrl+E */ : std::wstring());
    g.log->write("info", std::string("ink: mode ") + (on ? "on" : "off") + " (" + std::to_string(g.window->inkCount()) +
                             " stroke(s)" + (on && androidLive() ? ", Android pointer input paused" : "") + ")");
    if (toast) {
        if (on) g.window->showToast(tr(androidLive() ? S::InkOnToastAndroid : S::InkOnToast), 3500);
        else g.window->showToast(tr(g.window->inkCount() > 0 ? S::InkOffToast : S::InkOffToastEmpty), 3000);
    }
    refreshToolbar();
}

void inkCommand(UINT cmd) {
    if (cmd == CmdInkToggle) {
        setInkMode(!inkOn());
        return;
    }
    if (cmd >= CmdInkToolPen && cmd <= CmdInkToolArrow) {
        g.settings.inkTool = static_cast<int>(cmd - CmdInkToolPen);
        styleChanged();
        return;
    }
    if (cmd == CmdInkToolCycle) {
        g.settings.inkTool = (g.settings.inkTool + 1) % 3;
        styleChanged();
        return;
    }
    if (cmd >= CmdInkColor0 && cmd <= CmdInkColorLast) {
        g.settings.inkColor = static_cast<int>(cmd - CmdInkColor0);
        styleChanged();
        return;
    }
    if (cmd == CmdInkColorCycle) {
        g.settings.inkColor = (g.settings.inkColor + 1) % 6;
        styleChanged();
        return;
    }
    if (cmd >= CmdInkWidth0 && cmd <= CmdInkWidthLast) {
        g.settings.inkWidth = static_cast<int>(cmd - CmdInkWidth0);
        styleChanged();
        return;
    }
    if (cmd == CmdInkWidthCycle) {
        g.settings.inkWidth = (g.settings.inkWidth + 1) % 3;
        styleChanged();
        return;
    }
    if (cmd == CmdInkUndo) {
        if (!g.window->inkUndo()) g.window->showToast(tr(S::InkNothing), 1500);
        refreshToolbar();
        return;
    }
    if (cmd == CmdInkClear) {
        if (g.window->inkCount() > 0) {
            g.window->inkClear();
            g.window->showToast(tr(S::InkCleared), 1500);
            g.log->write("info", "ink: cleared");
        } else {
            g.window->showToast(tr(S::InkNothing), 1500);
        }
        refreshToolbar();
        return;
    }
    if (cmd == CmdInkInShots || cmd == CmdInkInRec) {
        bool& v = cmd == CmdInkInShots ? g.settings.inkInShots : g.settings.inkInRec;
        v = !v;
        saveSettings();
        g.window->setInkCapture(g.settings.inkInShots, g.settings.inkInRec);
        g.window->showToast(fmt(v ? S::AndOptOn : S::AndOptOff, {tr(cmd == CmdInkInShots ? S::InkInShots : S::InkInRec)}), 2000);
        return;
    }
}

bool inkKey(WPARAM wp) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (ctrl && !alt && wp == 'E') {
        inkCommand(shift ? CmdInkClear : CmdInkToggle);
        return true;
    }
    // Ctrl+Z stays the phone's undo on an Android picture unless the pen is out.
    if (ctrl && !alt && !shift && wp == 'Z' && (inkOn() || (g.window->inkCount() > 0 && !androidLive()))) {
        inkCommand(CmdInkUndo);
        return true;
    }
    if (!ctrl && !alt && wp == VK_ESCAPE && inkOn()) {  // before Esc's fullscreen / phone meaning
        setInkMode(false);
        return true;
    }
    return false;
}

void appendInkToolbar(std::vector<pm::VideoWindow::ToolbarItem>& v) {
    if (!viewAvailable()) return;
    using TI = pm::VideoWindow::ToolbarItem;
    const bool on = inkOn();
    TI pen{CmdInkToggle, kIcoInk, tr(on ? S::TipInkOff : S::TipInkOn)};
    pen.toggled = on;
    pen.label = tr(S::ToolLabelInk);
    v.push_back(pen);
    if (!on) return;
    const int t = g.settings.inkTool % 3, c = g.settings.inkColor % 6, w = g.settings.inkWidth % 3;
    TI tool{CmdInkToolCycle, kToolIcons[t], fmt(S::TipInkTool, {tr(kInkToolNames[t])})};
    tool.groupStart = true;
    v.push_back(tool);
    TI col{CmdInkColorCycle, 0, fmt(S::TipInkColor, {tr(kInkColorNames[c])})};
    col.swatch = static_cast<int>(kInkColors[c]);
    col.swatchSize = 0.8f;
    v.push_back(col);
    TI wid{CmdInkWidthCycle, 0, fmt(S::TipInkWidth, {tr(kInkWidthNames[w])})};
    wid.swatch = 0xD9D2D3;  // neutral: the size is what it says
    wid.swatchSize = kInkWidthDisc[w];
    v.push_back(wid);
    v.push_back({CmdInkUndo, kIcoUndo, tr(S::TipInkUndo)});
    v.push_back({CmdInkClear, kIcoErase, tr(S::TipInkClear)});
}

MenuItem inkMenuItem() {
    const bool av = viewAvailable(), on = inkOn();
    const int strokes = g.window->inkCount();
    std::vector<MenuItem> sub;
    if (!av) sub.push_back(MenuItem::note(tr(S::MenuNeedPicture)));
    MenuItem mode = checkItem(CmdInkToggle, tr(S::InkModeToggle), kIcoInk, on, L"Ctrl+E");
    mode.enabled = av;
    sub.push_back(std::move(mode));
    sub.push_back(MenuItem::separator());
    for (int t = 0; t < 3; ++t) {
        MenuItem m = MenuItem::command(CmdInkToolPen + t, tr(kInkToolNames[t]), kToolIcons[t]);
        m.radio = true;
        m.checked = g.settings.inkTool % 3 == t;
        sub.push_back(std::move(m));
    }
    sub.push_back(MenuItem::caption(tr(S::InkColor)));
    for (int c = 0; c < 6; ++c) {
        MenuItem m = MenuItem::command(CmdInkColor0 + c, tr(kInkColorNames[c]));
        m.radio = true;
        m.checked = g.settings.inkColor % 6 == c;
        m.hasSwatch = true;
        for (uint32_t& s : m.swatch) s = kInkColors[c];
        sub.push_back(std::move(m));
    }
    sub.push_back(MenuItem::caption(tr(S::InkWidth)));
    for (int w = 0; w < 3; ++w) {
        MenuItem m = MenuItem::command(CmdInkWidth0 + w, tr(kInkWidthNames[w]));
        m.radio = true;
        m.checked = g.settings.inkWidth % 3 == w;
        sub.push_back(std::move(m));
    }
    sub.push_back(MenuItem::separator());
    MenuItem undo = MenuItem::command(CmdInkUndo, tr(S::InkUndo), kIcoUndo, L"Ctrl+Z");
    undo.enabled = strokes > 0;
    sub.push_back(std::move(undo));
    MenuItem clr = MenuItem::command(CmdInkClear, tr(S::InkClear), kIcoErase, L"Ctrl+Shift+E");
    clr.enabled = strokes > 0;
    sub.push_back(std::move(clr));
    sub.push_back(MenuItem::separator());
    sub.push_back(checkItem(CmdInkInShots, tr(S::InkInShots), kIcoCamera, g.settings.inkInShots));
    sub.push_back(checkItem(CmdInkInRec, tr(S::InkInRec), kIcoRecord, g.settings.inkInRec));
    MenuItem m = MenuItem::submenu(tr(S::InkMenu), kIcoInk, std::move(sub));
    m.right = av ? std::wstring(L"Ctrl+E") : std::wstring(tr(S::MenuNeedPhone));
    return m;
}

void inkSyncPicture() {
    if (!g.window) return;
    // The picture went away, or another phone took the window: ink mode off,
    // the strokes (drawn on that phone's screen) go.
    static std::wstring lastPhone;
    const bool av = viewAvailable();
    const std::wstring phone = av ? std::to_wstring(g_active.load()) + L"|" + g.sourceName + L"|" + g.peerName : std::wstring();
    if (phone == lastPhone) return;
    const bool had = !lastPhone.empty();
    lastPhone = phone;
    if (!had) return;
    if (inkOn()) setInkMode(false, false);
    if (g.window->inkCount() > 0) {
        g.window->inkClear();
        g.log->write("info", "ink: strokes cleared (the picture ended / another phone)");
    }
}

}  // namespace pm_app
