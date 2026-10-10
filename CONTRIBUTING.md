# 參與貢獻 / Contributing

謝謝你願意幫忙自在投影！(English below.)

## 回報問題與建議

- **問題回報**：請用 [Issues → 問題回報](https://github.com/victor900106/ZizaiCast/issues/new/choose)，附上版本、連線方式、手機與 Windows 版本，以及 `%LOCALAPPDATA%\PhoneMirror\phonemirror.log` 中出問題那段時間的記錄。
- **功能建議**：請先搜尋是否有人提過；有的話按 👍 就好，我們依需求數量排序。
- 先看 README 的「常見問題」與「目前的限制」：例如 iPhone 無法從電腦操控、DRM 影片黑畫面，都是平台限制，不是錯誤。
- 請勿在公開 Issue 貼出個人資料、私人畫面或配對 QR 碼。

## 開發環境

- Windows 10 / 11 x64、Visual Studio 2026 Build Tools（MSVC v145）、CMake ≥ 3.25、vcpkg classic mode（triplet `x64-windows`）、Inno Setup 6（打包安裝程式才需要）。
- 精確版本與指令：source zip 裡的 `BUILD.md`、`docs/licenses/SOURCE.md`。

```bat
vcpkg install openssl libplist pthreads alac "ffmpeg[core,avcodec]" --triplet x64-windows --overlay-ports=tools/vcpkg-overlay
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## 程式碼結構

| 目錄 | 內容 | 說明文件 |
|---|---|---|
| `core/` | AirPlay 接收端（移植自 UxPlay，修改處標 `PM:`） | `docs/core.md` |
| `video/` | 解碼（Media Foundation）、Direct3D 11 / Direct2D 視窗與等待畫面 | `docs/video.md` |
| `audio/` | AAC / ALAC 解碼與播放 | `docs/audio.md` |
| `recorder/` | MP4 錄影 | `docs/recorder.md` |
| `miracast/` | Android 投放（Windows Miracast 接收） | `docs/miracast.md` |
| `android/` | 無線偵錯配對、adb、scrcpy-server 串流與操控 | `docs/android.md` |
| `app/` | 主程式：選單、系統匣、設定、更新、來源切換 | `docs/app.md` |

## 測試

- 每個模組都有自己的測試工具（`pm_video_test`、`pm_audio_test`、`pm_recorder_test`、`pm_android_test`…），用法寫在對應的 `docs/*.md`。
- 主程式有開發者模式：`自在投影.exe --dev`（可與安裝版並存），加上 `--test-feed 檔案.h264` 可以不用真的手機就模擬連線；`--test-offscreen` 讓腳本截圖時視窗不搶焦點。詳見 `docs/app.md`「Command line」。
- 改到連線或解碼時，請盡量用真的 iPhone / Android 手機再試一次，並在 PR 說明測了哪些裝置。

## Pull Request

1. 一個 PR 做一件事；說明「為什麼」和「怎麼測的」。
2. 跟著周圍程式碼的風格（C++20、`pm::` 命名空間、4 格縮排）；使用者看得到的文字用繁體中文，並與教學、README 一致。
3. 改了行為就同步更新對應的 `docs/*.md`。
4. 修改 `core/` 中來自 UxPlay 的檔案時，請加上 `PM:` 註解。
5. 不要加入任何會連上網際網路的功能（更新檢查以外），也不要加入遙測或分析。
6. 新的第三方程式碼或函式庫：必須是與 GPL-3.0 相容的授權，並更新 `docs/licenses/` 的授權說明。
7. 美術素材只能使用原創或授權清楚的圖（吉祥物「投投 Toutou」在 `assets/public/toutou/`）。

提交的程式碼將以 GPL-3.0 授權釋出。

---

## English

Thanks for helping! **Bugs**: use the bug-report form (version, connection type, phone + Windows version, and the relevant part of `%LOCALAPPDATA%\PhoneMirror\phonemirror.log`). **Ideas**: search first and 👍 existing requests. Please check the README FAQ / Known limits (e.g. the iPhone can't be controlled from a PC; DRM video is black — platform limits, not bugs).

**Build**: Windows x64, VS 2026 Build Tools, CMake ≥ 3.25, vcpkg `x64-windows` (commands above; exact versions in `BUILD.md` / `docs/licenses/SOURCE.md`). Each module has a `docs/*.md` and a test tool; the app has a `--dev` mode with `--test-feed file.h264` to simulate a phone.

**Pull requests**: one change per PR, explain why and how you tested (with real phones when touching connection or decoding code), follow the surrounding style (C++20, `pm::`, 4-space indent), keep user-facing text in Traditional Chinese, update the matching `docs/*.md`, mark changes to UxPlay-derived files in `core/` with `PM:`, no network features beyond the update check and no telemetry, only GPL-3.0-compatible code and original / clearly licensed artwork. Contributions are released under GPL-3.0.
