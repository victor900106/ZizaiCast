#!/usr/bin/env bash
# Rebuilds the public snapshot as one commit per module (same final tree), so the
# GitHub file list shows a meaningful message per folder. Run inside the public
# snapshot repo (e.g. %TEMP%\zc-publish) that already has the single snapshot commit.
set -euo pipefail
SNAP=$(git rev-parse HEAD)
git checkout -q --orphan modular
git rm -rq --cached . >/dev/null
NAME="victor900106"; EMAIL="victor900106@users.noreply.github.com"
TRAILER="Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
c() { # c "<message>" "<body>" paths...
  local msg="$1" body="$2"; shift 2
  git add -- "$@"
  printf '%s\n\n%s\n\n%s\n' "$msg" "$body" "$TRAILER" > /tmp/split_msg.txt
  git -c user.name="$NAME" -c user.email="$EMAIL" commit -q -F /tmp/split_msg.txt
  echo "  $(git log --oneline -1)"
}
c "專案設定：CMake 建置、共用介面與 GPL-3.0 授權" "根目錄 CMake（各模組各自一個資料夾）、include/pm 共用的影音介面與中英文字串表、建置說明 BUILD.md。" \
  CMakeLists.txt .gitignore BUILD.md LICENSE include
c "core：AirPlay 協定（UxPlay 移植到 MSVC）" "iPhone / iPad 螢幕鏡像的協定核心：配對、FairPlay、RTSP、時間同步、mDNS 廣播、多手機接手、PIN 碼；tools/pm_probe 是協定測試工具。" \
  core tools/pm_probe
c "video：Media Foundation 硬體解碼與 D3D11 視窗" "H.264 / H.265 硬體解碼、低延遲顯示、等待畫面與投投動畫、主題、iPhone 外框、旋轉、工具列、截圖。" \
  video
c "audio：AAC-ELD / AAC-LC（FFmpeg）與 ALAC 解碼、WASAPI 播放" "低延遲聲音播放、音量跟隨手機、暫停後恢復不卡頓。" \
  audio
c "recorder：錄影成 MP4（硬體 H.264 + AAC）" "Media Foundation Sink Writer，影音對齊、轉向不壞檔。" \
  recorder
c "miracast：Android 投放（Windows.Media.Miracast）" "手機的「投放／Smart View」直接投到自在投影。" \
  miracast
c "android：無線偵錯投影與電腦操控（adb + scrcpy-server）" "掃 QR 碼配對、自動重連、滑鼠鍵盤操控手機。" \
  android
c "assets：吉祥物「投投」與 App 圖示原始檔" "原創雲朵吉祥物的向量原始檔與各尺寸匯出。" \
  assets
c "app：主程式（選單、系統匣、設定、配對面板、自動更新）" "把各模組接起來的 Windows 主程式：自訂主題選單、浮動工具列、語言切換、關於視窗、一鍵更新。" \
  app
c "installer：Inno Setup 安裝程式（中英文）" "免系統管理員權限安裝到桌面「手機投影」資料夾，附授權聲明與教學。" \
  installer
c "docs：各模組技術文件、使用教學與第三方授權" "中英文使用教學、第三方授權清單、原始碼提供方式（SOURCE.md）。" \
  docs/android.md docs/app.md docs/audio.md docs/core.md docs/miracast.md docs/recorder.md docs/video.md \
  docs/tutorial docs/licenses docs/mascot
c "tools：發布與原始碼打包腳本" "原始碼壓縮檔、GitHub 發布流程。" \
  tools/release
c "README：中英文介紹、截圖與社群檔案" "專案首頁 README（繁中／English）、功能截圖與示範動畫、問題回報範本、貢獻指南。" \
  README.md README.en.md docs/readme .github CONTRIBUTING.md
echo "--- leftover (must be empty):"; git status --short
echo "--- tree equal to snapshot? $(git diff --quiet "$SNAP" HEAD && echo YES || echo NO)"
