# Building ZizaiCast 0.6.2 from source

This archive is the Corresponding Source of the ZizaiCast 0.6.2 installer
(GPL-3.0, see `LICENSE`, `docs/licenses/SOURCE.md` and
`docs/licenses/THIRD_PARTY_NOTICES.txt`).

* git commit: `81ab06e4e46bb15d2e11a038b35178d87a39c8fb` (2026-10-08T15:30:54+08:00)
* vcpkg commit used for the release build: `f451d04d496aa089e294a1a2d799a269788d47ba`
* Library sources (OpenSSL, libplist, pthreads4w, ALAC, FFmpeg + vcpkg port
  scripts/patches): `ZizaiCast-0.6.2-deps-source.zip` on the same release page.

## 1. Tools (Windows 10/11 x64)

| Tool | Version used for the release |
|---|---|
| Visual Studio 2026 Build Tools ("Visual Studio 18"), workload *Desktop development with C++* (MSVC v145, Windows SDK) | MSVC 14.51.36231 |
| CMake (the one bundled with VS Build Tools is fine) | 4.3.1 (>= 3.25 required) |
| vcpkg (classic mode) | commit `f451d04d496aa089e294a1a2d799a269788d47ba` |
| Inno Setup (only for the installer) | 6.7.3 |
| Windows PowerShell 5.1 (for `fetch_tools.ps1`) | |

## 2. Libraries (vcpkg, triplet x64-windows)

| Port | Version | Port-version | Upstream |
|---|---|---|---|
| `openssl` | 3.6.5 | 1 | https://github.com/openssl/openssl |
| `libplist` | 2.8.0 | 0 | https://github.com/libimobiledevice/libplist |
| `pthreads` | 3.0.0 | 14 | https://sourceforge.net/projects/pthreads4w/ |
| `alac` | 2017-11-03-c38887c5 | 4 | https://github.com/macosforge/alac |
| `ffmpeg[core,avcodec]` | 9.0.2 | 1 | https://ffmpeg.org/ (git: https://git.ffmpeg.org/ffmpeg.git) |

```
git clone https://github.com/microsoft/vcpkg C:\vcpkg
git -C C:\vcpkg checkout f451d04d496aa089e294a1a2d799a269788d47ba
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install openssl libplist pthreads alac "ffmpeg[core,avcodec]" --triplet x64-windows
```

FFmpeg must stay an LGPL build: do not enable the `gpl`, `version3` or
`nonfree` features. `fdk-aac` is *not* needed by the shipped program
(at most by the optional `pm_audio_test`).

## 3. Android helper binaries (not in the archive, pinned by SHA-256)

```
powershell -ExecutionPolicy Bypass -File android\third_party\fetch_tools.ps1
```

Downloads adb (Android SDK Platform-Tools r37.0.1, from dl.google.com) and
scrcpy-server v5.0 (github.com/Genymobile/scrcpy). CMake also runs it at
configure time if the files are missing (`-DPM_ANDROID_FETCH_TOOLS=OFF` to
disable).

## 4. Artwork

The mascot 投投 (Toutou) and the app icon are original artwork of this
project and are included: sources in `assets/public/toutou/` (SVG + PNG
exports, generator `docs/mascot/toutou/*.mjs`), the layers embedded by
`pm_video` in `video/res/toutou/`, the icon in `app/res/app.ico`.
Nothing has to be added to build.

## 5. Configure and build

```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Result: `build\bin\Release\自在投影.exe` plus the vcpkg DLLs (copied by
vcpkg's applocal step), the VC++ runtime DLLs and `android-tools\`.
Individual targets: `pm_core pm_video pm_audio pm_recorder pm_miracast
pm_android PhoneMirror` (the app), tests `pm_probe pm_video_test
pm_audio_test pm_recorder_test pm_miracast_test pm_android_test`.

## 6. Installer

```
"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" /DBuildDir=..\build\bin\Release installer\zizai.iss
```

(`installer\zizai.iss` packages the user guides from `docs\tutorial\` and
the notices from `docs\licenses\`, both in this archive. Output:
`installer\Output\自在投影-安裝程式-0.6.2.exe`, Traditional Chinese +
English.)

## 7. Replacing the LGPL libraries

`plist-2.0.dll` (libplist) and `avcodec-*.dll` / `avutil-*.dll`
(FFmpeg) are ordinary DLLs next to `自在投影.exe`; you may replace them with
your own ABI-compatible builds (e.g. from the deps-source zip) without
rebuilding the program.