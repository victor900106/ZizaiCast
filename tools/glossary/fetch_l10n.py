# Step 1c of the glossary build (English UI terms): zh_TW translations of free-software projects, cached in
# raw_l10n/<family>/<project>/<file>.  One request at a time, >= 0.5 s apart, project-only User-Agent, no other
# identifying data; a cached file is not fetched again (--refresh to force).
#
#   py fetch_l10n.py [--refresh] [gnome|kde|lo|moz ...]
#
# Licences (the translation files are under their project's licence; SOURCES.md):
#   GNOME  l10n.gnome.org POT/<module>.<branch>/<module>.<branch>.zh_TW.po   GPL-2.0+ / GPL-3.0+ / LGPL-2.1+ (per module, below)
#   KDE    websvn.kde.org trunk/l10n-kf6/zh_TW/messages/<module>/*.po        GPL-2.0+ / LGPL-2.0+ (KDE licensing policy)
#   LO     github.com/LibreOffice/translations source/zh-TW/<module>/messages.po  MPL-2.0
#   Mozilla github.com/mozilla-l10n/firefox-l10n zh-TW/*.ftl + firefox-l10n-source (en-US)  MPL-2.0
import json
import os
import re
import sys
import time

import requests

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "raw_l10n")
UA = "ZizaiCast-glossary/1.0 (github.com/victor900106/ZizaiCast)"
PAUSE = 0.5
_last = [0.0]
S = requests.Session()
S.headers.update({"User-Agent": UA})

# module -> licence (from the module's COPYING / SPDX headers)
GNOME = {
    "gtk": "LGPL-2.1-or-later", "glib": "LGPL-2.1-or-later", "libadwaita": "LGPL-2.1-or-later",
    "nautilus": "GPL-3.0-or-later", "gnome-control-center": "GPL-2.0-or-later", "gnome-text-editor": "GPL-3.0-or-later",
    "gedit": "GPL-2.0-or-later", "evince": "GPL-2.0-or-later", "eog": "GPL-2.0-or-later", "file-roller": "GPL-2.0-or-later",
    "gnome-software": "GPL-2.0-or-later", "gnome-shell": "GPL-2.0-or-later", "gnome-settings-daemon": "GPL-2.0-or-later",
    "gnome-calendar": "GPL-3.0-or-later", "epiphany": "GPL-3.0-or-later", "gnome-maps": "GPL-2.0-or-later",
    "gnome-weather": "GPL-2.0-or-later", "gnome-clocks": "GPL-2.0-or-later", "gnome-contacts": "GPL-2.0-or-later",
    "gnome-music": "GPL-2.0-or-later", "baobab": "GPL-2.0-or-later", "gnome-disk-utility": "GPL-2.0-or-later",
    "gnome-terminal": "GPL-3.0-or-later", "gnome-calculator": "GPL-3.0-or-later", "simple-scan": "GPL-3.0-or-later",
    "gnome-system-monitor": "GPL-2.0-or-later", "gnome-font-viewer": "GPL-2.0-or-later", "loupe": "GPL-3.0-or-later",
    "snapshot": "GPL-3.0-or-later", "console": "GPL-3.0-or-later", "totem": "GPL-2.0-or-later",
    "gnome-initial-setup": "GPL-2.0-or-later", "gnome-online-accounts": "LGPL-2.0-or-later", "evolution": "LGPL-2.0-or-later",
}
KDE = {
    "kxmlgui": "LGPL-2.0-or-later", "kio": "LGPL-2.0-or-later", "kwidgetsaddons": "LGPL-2.1-or-later",
    "kconfigwidgets": "LGPL-2.0-or-later", "ktextwidgets": "LGPL-2.0-or-later", "kcoreaddons": "LGPL-2.0-or-later",
    "knewstuff": "LGPL-2.1-or-later", "kbookmarks": "LGPL-2.0-or-later", "kconfig": "LGPL-2.0-or-later",
    "dolphin": "GPL-2.0-or-later", "okular": "GPL-2.0-or-later", "kate": "LGPL-2.0-or-later", "konsole": "GPL-2.0-or-later",
    "gwenview": "GPL-2.0-or-later", "ark": "GPL-2.0-or-later", "spectacle": "GPL-2.0-or-later", "elisa": "LGPL-3.0-or-later",
    "plasma-workspace": "GPL-2.0-or-later", "systemsettings": "GPL-2.0-or-later", "kcalc": "GPL-2.0-or-later",
    "plasma-desktop": "GPL-2.0-or-later", "kdeconnect-kde": "GPL-2.0-only OR GPL-3.0-only (KDE e.V. accepted)", "discover": "GPL-2.0-or-later",
    "kwrite": "LGPL-2.0-or-later", "kcharselect": "GPL-2.0-or-later",
}
LO = ["cui", "sfx2", "svx", "svtools", "vcl", "sw", "sc", "sd", "dbaccess", "basctl", "fpicker", "uui", "desktop",
      "extensions", "filter", "formula", "starmath", "chart2", "editeng", "avmedia", "framework", "xmlsecurity", "wizards"]
MOZ_PREFIX = ("toolkit/toolkit/", "browser/browser/")


def get(url, path, refresh=False, tries=4):
    if os.path.exists(path) and not refresh:
        return open(path, "rb").read()
    wait = 5.0
    for _ in range(tries):
        dt = time.time() - _last[0]
        if dt < PAUSE:
            time.sleep(PAUSE - dt)
        try:
            r = S.get(url, timeout=60)
        except requests.RequestException as e:
            print(f"  {url}: {e!r}", file=sys.stderr)
            _last[0] = time.time()
            time.sleep(wait)
            wait *= 2
            continue
        _last[0] = time.time()
        if r.status_code == 200:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "wb").write(r.content)
            return r.content
        if r.status_code == 404:
            return None
        ra = r.headers.get("Retry-After")
        time.sleep(float(ra) if ra and ra.isdigit() else wait)
        wait *= 2
    return None


def gnome(refresh):
    for m in GNOME:
        path = os.path.join(RAW, "gnome", m, m + ".zh_TW.po")
        ok = None
        for br in ("main", "master", "gnome-49", "gnome-48"):
            ok = get(f"https://l10n.gnome.org/POT/{m}.{br}/{m}.{br}.zh_TW.po", path, refresh)
            if ok:
                break
        print(f"gnome {m}: {'ok' if ok else 'MISSING'}", file=sys.stderr)


def kde(refresh):
    for m in KDE:
        idx = get(f"https://websvn.kde.org/trunk/l10n-kf6/zh_TW/messages/{m}/", os.path.join(RAW, "kde", m, "_index.html"), refresh)
        if not idx:
            print(f"kde {m}: MISSING", file=sys.stderr)
            continue
        names = sorted(set(re.findall(rf"messages/{re.escape(m)}/([A-Za-z0-9_.-]+\.po)\?view=log", idx.decode("utf-8", "replace"))))
        names = [n for n in names if "._desktop_" not in n and "._json_" not in n and "appdata" not in n and "metainfo" not in n]
        for n in names:
            get(f"https://websvn.kde.org/trunk/l10n-kf6/zh_TW/messages/{m}/{n}?view=co", os.path.join(RAW, "kde", m, n), refresh)
        print(f"kde {m}: {len(names)} files", file=sys.stderr)


def lo(refresh):
    for m in LO:
        ok = get(f"https://raw.githubusercontent.com/LibreOffice/translations/master/source/zh-TW/{m}/messages.po",
                 os.path.join(RAW, "lo", m, "messages.po"), refresh)
        print(f"lo {m}: {'ok' if ok else 'MISSING'}", file=sys.stderr)


def moz(refresh):
    tree = get("https://api.github.com/repos/mozilla-l10n/firefox-l10n/git/trees/main?recursive=1",
               os.path.join(RAW, "moz", "_tree.json"), refresh)
    files = [t["path"][len("zh-TW/"):] for t in json.loads(tree)["tree"]
             if t["path"].startswith("zh-TW/") and t["path"].endswith(".ftl")]
    files = [f for f in files if f.startswith(MOZ_PREFIX)]
    n = 0
    for f in files:
        a = get(f"https://raw.githubusercontent.com/mozilla-l10n/firefox-l10n/main/zh-TW/{f}", os.path.join(RAW, "moz", "zh-TW", f), refresh)
        b = get(f"https://raw.githubusercontent.com/mozilla-l10n/firefox-l10n-source/main/{f}", os.path.join(RAW, "moz", "en-US", f), refresh)
        n += bool(a and b)
    print(f"moz: {n} of {len(files)} file pairs", file=sys.stderr)


if __name__ == "__main__":
    refresh = "--refresh" in sys.argv
    which = [a for a in sys.argv[1:] if not a.startswith("--")] or ["gnome", "kde", "lo", "moz"]
    for w in which:
        globals()[w](refresh)
