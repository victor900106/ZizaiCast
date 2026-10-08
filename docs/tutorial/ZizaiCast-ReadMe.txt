Zizai Cast — Read me (short version)
====================================

Mirror the screen of an iPhone / iPad / Android phone to this Windows PC.
Full illustrated guide: "ZizaiCast-Guide.html" in the install folder ("How to connect?" on the waiting screen opens it too).

1. Before you start
  - The phone and the PC must be on the same Wi-Fi / local network (not a guest network, not mobile data).
  - On first run, when the Windows firewall asks, tick "Private networks" and click "Allow access".
  - Turn off VPNs on the PC and the phone first.
  - School, office and hotel networks may block device discovery, so the phone can't find the PC.

2. iPhone / iPad
  1. On the iPhone open "Control Center" → "Screen Mirroring" → choose "Zizai Cast".
  2. To stop: Control Center → "Screen Mirroring" → "Stop Mirroring" (or press Ctrl+D on the PC, see section 5).
  - Require a PIN to connect: when on, a new iPhone must enter the 4-digit PIN shown on the PC (phones that succeeded are remembered).
  - When a new phone connects: "Take over" = the new phone is shown; "Keep current" = other phones are refused.
  - Mirroring pauses while the iPhone screen is off and resumes after you unlock it.
  - The iPhone's volume buttons control the volume played on the PC.

3. Android option 1: Cast (Miracast, watch only, no control)
  Phone's Quick Settings casting tile → choose "Zizai Cast". The name depends on the brand:
    Samsung: Smart View      Xiaomi/Redmi: Cast, Wireless display
    OPPO/realme: Screencast, Multi-screen      vivo: Screen mirroring
    ASUS: Wireless display, PlayTo      Sony: Screen mirroring
    Google Pixel: not supported, use option 2.
  The PC's Wi-Fi adapter must support Wireless Display, and Wi-Fi must be on. To check:
    in Terminal type  netsh wlan show drivers
    it works only if you see "Wireless Display Supported: Yes".

4. Android option 2: Wireless debugging (control the phone with mouse and keyboard, needs Android 11 or later)
  1. Settings → About phone → tap "Build number" 7 times to turn on "Developer options".
     (Samsung: About phone → Software information → Build number; Xiaomi: All specs → MIUI version)
  2. Developer options → turn on "Wireless debugging" → tap it → "Pair device with QR code".
  3. PC: right-click the window → "Connect Android (scan QR)", and scan the QR code on screen with the phone.
  4. After that it connects automatically whenever wireless debugging is on (it often turns off after a restart; just turn it on again).
  Control: left-click = tap, drag = swipe, wheel = scroll, right-click = Back, middle-click = Home, keyboard = typing.
  Right-click is "Back", so to open the menu on an Android screen use "More" in the toolbar at the top (or Shift + right-click).
  Esc is sent to the phone (usually Back); in full screen, Esc leaves full screen.
  No response to the QR code after about 45 seconds: make sure the phone and the PC are on the same Wi-Fi, or click "Use pairing code".
  If the phone turns off wireless debugging or leaves the Wi-Fi, the PC returns to the waiting screen within seconds ("Android disconnected").
  Security: turn "Wireless debugging" off when you're not using it, and never turn it on on public Wi-Fi.

5. Features and shortcuts (right-click the picture for every feature)
  Disconnect (any phone): Ctrl+D, or move the mouse over the picture → the red button at the right end of the toolbar,
    or right-click the tray icon → "Disconnect (phone name)". An Android phone disconnected from the PC won't reconnect
    automatically until you pair it again with the QR code or restart Zizai Cast.
  Toolbar: appears when the mouse moves over the picture while mirroring (hides after 2 seconds): (Android: Back, Home,
    Recent apps) Screenshot, Record (red while recording), Rotate, Full screen, More (full menu), Disconnect.
  F11 / double-click the picture: Full screen (Esc to leave)      Ctrl+T: Always on top
  Ctrl+S: Screenshot (截圖 / Screenshots folder)      Ctrl+R: Start/stop recording (錄影 / Recordings folder, MP4 with sound)
  Ctrl+→ / Ctrl+←: rotate 90°   Ctrl+H: flip horizontally   Ctrl+0: reset   Ctrl+F: iPhone frame
  Quality: Standard (1080p) / High · recommended (1440p) / Highest (4K); High and Highest need "HEVC Video Extensions" from the Microsoft Store.
  Themes: Sakura, Mint, Night Sky, Milk Tea.
  Language: Settings → "Language / 語言" (Automatic (Windows) / 繁體中文 / English / 日本語 / 한국어).
  Closing the window sends the app to the system tray at the bottom right, where it keeps waiting; to quit, right-click the tray icon → "Quit".
  Start with Windows, Check for updates: in Settings or the tray menu (updates are also checked at every start).
  Updates: when a new version is out, the "A new version of Zizai Cast" window offers "Update now", "Remind me later"
  or "Skip this version"; "Update to vX.Y.Z" also appears at the top of the menu, on the waiting screen and in the toolbar.
  It installs only when you click it (never by itself) and restarts afterwards. A newer installer placed in the 安裝檔 (Installers) folder is found too.

  New in 0.7:
  - Magnifier: right-click menu → "Magnifier". Ctrl+= zoom in, Ctrl+- zoom out (up to 8×), Ctrl+Shift+0 back to 1×;
    Ctrl+wheel zooms where the pointer is; drag to move around (Ctrl+drag while controlling an Android phone).
    High contrast (Ctrl+K cycles): Original colours / More contrast / Greyscale / Invert colours / Yellow on black.
    Freeze picture (Ctrl+P): holds the picture still on the PC while the phone carries on.
  - Translate: right-click menu → "Translate". Translate the whole screen (Ctrl+L, again to close), Translate a selected area
    (Ctrl+Shift+L), Show original (Ctrl+O), Keep translating (every 5 s), Translate into: 繁體中文 / English / 日本語 / 한국어.
    Runs offline on this PC, nothing is sent to the internet; the first time it asks, then downloads a model (about 50 MB per direction).
    Japanese/Korean not recognised: Windows Settings → Time & language → Language & region → Add a language, tick only "Optical character recognition".
  - Send to phone: after a screenshot or recording click "Send to phone". Android (wireless debugging): straight into the gallery;
    iPhone / iPad / Android casting: scan the QR code on the PC with the phone's camera (same Wi-Fi, works for 10 minutes).

6. FAQ
  - Zizai Cast doesn't appear: check same network, firewall allowed, VPN off;
    only one AirPlay receiver can run on a PC (close AirServer, UxPlay, etc. first).
  - Clicked Cancel on the firewall: search "Allow an app through Windows Firewall" → Change settings → tick "Private" for Zizai Cast.
  - Picture but no sound: turn up the iPhone volume, check the Ring/Silent switch and the Windows volume / output device.
  - Lag, stutter: use 5 GHz Wi-Fi or connect the PC by cable, and set the quality to "Standard".
  - Blurry picture: set the quality to "High" or "Highest" (applies from the next connection).
  - Someone else's phone took over: set "When a new phone connects" to "Keep current", or turn on the PIN.
  - Miracast won't connect: check that the PC supports Wireless Display, Wi-Fi is on and Mobile hotspot is off; otherwise use option 2.
  - Wireless debugging keeps dropping: turn wireless debugging on again; turn off the phone's battery saver; if that fails, remove the pairing and scan the QR code again.

Settings and log: %LOCALAPPDATA%\PhoneMirror (settings.ini, phonemirror.log).
Uninstall: Settings → Apps → Zizai Cast → Uninstall (screenshots and recordings are kept).
License: GPL-3.0 (see LICENSE.txt). The AirPlay part is ported from UxPlay.
