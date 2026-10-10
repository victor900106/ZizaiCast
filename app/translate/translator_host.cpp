// 自在投影 app: translate/translator_host.cpp — translate（放大鏡、翻譯的 UI 接線）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "translate/translate.h"

namespace pm_app {

// pm_translate callbacks (UI thread): themed dialogs instead of MessageBox.
void translatorAskDownload(pm::translate::Lang src, pm::translate::Lang tgt, double mb, std::function<void(bool)> answer) {
    wchar_t size[32];
    swprintf_s(size, L"%.0f MB", mb);
    pm::ui::AskPanel::Info a;
    a.glyph = 0xE896;  // Download
    const bool ocr = src == pm::translate::Lang::Unknown;  // the text recognition models (PaddleOCR)
    a.title = tr(ocr ? S::TrOcrConsentTitle : S::TrConsentTitle);
    a.body = ocr ? std::wstring(tr(S::TrOcrConsentBody))
                 : fmt(S::TrConsentBody, {pm::translate::langName(src), pm::translate::langName(tgt)});
    a.rows = {{tr(S::TrConsentSize), size},
              {tr(S::TrConsentSource), tr(ocr ? S::TrOcrConsentSourceVal : S::TrConsentSourceVal)},
              {tr(S::TrConsentLicense), tr(ocr ? S::TrOcrConsentLicenseVal : S::TrConsentLicenseVal)},
              {tr(S::TrConsentPlace), pm::translate::ModelStore::root()}};
    a.primary = tr(S::TrConsentYes);
    a.secondary = tr(S::TrConsentNo);
    a.done = [answer, mb](int choice) {
        g.log->write("info", "model download consent (" + std::to_string(static_cast<int>(mb)) + " MB): " +
                                 (choice == 1 ? "yes" : "no"));
        answer(choice == 1);
    };
    g.log->write("info", ocr ? std::string("model download consent asked: OCR (PaddleOCR)")
                             : "model download consent asked: " + toUtf8(pm::translate::langName(src)) + " -> " +
                                   toUtf8(pm::translate::langName(tgt)));
    g.askPanel.open(g.hwnd, std::move(a));
}

void translatorNotify(const std::wstring& title, const std::wstring& text, bool important) {
    g.log->write("info", "translate: " + toUtf8(title) + " | " + toUtf8(text));
    if (!important) {
        g.window->showToast(text, 4000);
        return;
    }
    pm::ui::AskPanel::Info a;
    a.title = title;
    a.body = text;
    if (title == tr(S::TrOcrMissingTitle)) {  // add the OCR language in Windows Settings
        a.glyph = 0xF2B7;  // LocaleLanguage
        a.primary = tr(S::TrOpenLangSettings);
        a.secondary = tr(S::AboutClose);
        a.done = [](int choice) {
            if (choice != 1) return;
            g.log->write("info", "open ms-settings:regionlanguage");
            if (!g.testOffscreen) ShellExecuteW(g.hwnd, L"open", L"ms-settings:regionlanguage", nullptr, nullptr, SW_SHOWNORMAL);
        };
    } else {
        a.glyph = 0xE7BA;  // Warning
        a.primary = tr(S::DlgOk);
    }
    g.askPanel.open(g.hwnd, std::move(a));
}

// A translation ended without a result: close it (the picture goes back to
// live).  The result callback is posted by the translator's worker before it
// clears busy(), so busy() may still be true here for the job that just
// ended; checking it once left the picture frozen with nothing on it.  Wait
// (a few ms, bounded) until the worker is done.  A new job cannot start while
// busy (translateScreen / translateRegion return), and live mode keeps going.
void closeEmptyTranslation(int tries) {
    if (!g.translator || g.translator->live()) return;
    if (g.translator->busy()) {
        if (tries < 100) {
            g.window->post([tries] {
                Sleep(1);
                closeEmptyTranslation(tries + 1);
            });
        } else {
            g.log->write("warn", "translate ended without a result but the translator stays busy: not closed");
        }
        return;
    }
    g.translator->close();
    returnToLive("ended without a result");
}

// 線上翻譯（選用）: the translator may go online only as the user saved it
// (online_translate.h activeMode() also checks consent, key and back-off);
// never in --dev --test-no-network runs. At start and after every save.
void applyOnlineAllowed() {
    if (g.translator) g.translator->setOnlineAllowed(!g.testNoNetwork && pm::ui::trset::onlineEnabled());
}

void createTranslator() {
    pm::translate::ScreenTranslator::Callbacks cb;
    cb.askDownload = translatorAskDownload;
    cb.notify = translatorNotify;
    cb.changed = []() { refreshToolbar(); };
    cb.finished = [](bool ok, const pm::translate::ScreenTranslator::Timing& t) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "translate%s %s: %d lines, %d blocks, %d translated, ocr %.0f ms, translate %.0f ms, total %.0f ms",
                      t.live ? " live run" : "", ok ? "done" : "ended", t.lines, t.blocks, t.translated, t.ocrMs, t.translateMs, t.totalMs);
        g.log->write("info", buf);
        // Nothing to show (declined download, no text, OCR language missing):
        // do not leave the picture frozen with an empty overlay.
        if (!ok) closeEmptyTranslation(0);
        if (ok && g.translator) {
            // 「線上」 badges on the blocks translated online (ARCHITECTURE.md §3.7.3).
            std::vector<std::wstring> online;
            for (const auto& it : g.translator->lastItems())
                if (it.online) online.push_back(it.original);
            if (!online.empty()) g.log->write("info", "translate: " + std::to_string(online.size()) + " block(s) translated online");
            g.window->setTextOverlayOnline(std::move(online));
            if (t.source != pm::translate::Lang::Unknown) {
                const std::string last = toUtf8(pm::translate::langTag(t.source));
                if (last != g.settings.translateLast) {
                    g.settings.translateLast = last;
                    saveSettings();
                }
            }
        }
        // An online failure (quota, bad key, no connection): said once per new state.
        if (const std::wstring t = pm::ui::trset::onlineToast(); !t.empty()) {
            g.log->write("warn", "translate: online " + toUtf8(t));
            g.window->showToast(t, 5000);
        }
    };
    g.translator = std::make_unique<pm::translate::ScreenTranslator>(*g.window, std::move(cb));
    g.window->setTextOverlayStyle(g.settings.translateLayout, g.settings.translateDark);
    g.translator->setTarget(translateTarget());
    applyOnlineAllowed();
    std::wstring err;
    if (!pm::translate::Engine::available(&err)) g.log->write("warn", "translate: engine unavailable: " + toUtf8(err));
}

}  // namespace pm_app
