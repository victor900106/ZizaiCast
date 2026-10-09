# Building ZizaiCast 0.7.7 from source

This archive is the Corresponding Source of the ZizaiCast 0.7.7 installer
(GPL-3.0, see `LICENSE`, `docs/licenses/SOURCE.md` and
`docs/licenses/THIRD_PARTY_NOTICES.txt`).

* git commit: `11de1bf1ee49911350d46b075861f597e2ac78ff` (2026-10-10T00:05:27+08:00)
* vcpkg commit used for the release build: `f451d04d496aa089e294a1a2d799a269788d47ba`
* Library sources (OpenSSL, libplist, pthreads4w, ALAC, FFmpeg, PCRE2 + vcpkg port scripts/patches; the complete source of `bergamot.dll`; licence and provenance of `onnxruntime.dll`):
  `ZizaiCast-0.7.7-deps-source.zip` on the same release page.

## 1. Tools (Windows 10/11 x64)

| Tool | Version used for the release |
|---|---|
| Visual Studio 2026 Build Tools ("Visual Studio 18"), workload *Desktop development with C++* (MSVC v145, Windows SDK) | MSVC 14.51.36231 |
| CMake (the one bundled with VS Build Tools is fine) | 4.3.1 (>= 3.25 required) |
| vcpkg (classic mode) | commit `f451d04d496aa089e294a1a2d799a269788d47ba` |
| Inno Setup (only for the installer) | 6.7.3 |
| Windows PowerShell 5.1 (for `fetch_tools.ps1`) | |

## 2. Libraries (vcpkg; triplet x64-windows, pcre2 x64-windows-static)

| Port | Triplet | Version | Port-version | Upstream |
|---|---|---|---|---|
| `openssl` | x64-windows | 3.6.5 | 1 | https://github.com/openssl/openssl |
| `libplist` | x64-windows | 2.8.0 | 0 | https://github.com/libimobiledevice/libplist |
| `pthreads` | x64-windows | 3.0.0 | 14 | https://sourceforge.net/projects/pthreads4w/ |
| `alac` | x64-windows | 2017-11-03-c38887c5 | 4 | https://github.com/macosforge/alac |
| `ffmpeg[core,avcodec]` | x64-windows | 9.0.2 | 1 | https://ffmpeg.org/ (git: https://git.ffmpeg.org/ffmpeg.git) |
| `pcre2` | x64-windows-static | 10.49 | 0 | https://github.com/PCRE2Project/pcre2 |

```
git clone https://github.com/microsoft/vcpkg C:\vcpkg
git -C C:\vcpkg checkout f451d04d496aa089e294a1a2d799a269788d47ba
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install openssl libplist pthreads alac "ffmpeg[core,avcodec]" --triplet x64-windows
```

`pcre2` (static) is only needed for `bergamot.dll`, see section 5.


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

## 5. Translation engine and OCR runtime (`bergamot.dll`, `onnxruntime.dll`)

Both DLLs are loaded at run time from the program folder. CMake copies them
next to `自在投影.exe` when they exist at the cache paths `PM_BERGAMOT_DLL`
(default `build-translate/bergamot/bin/bergamot.dll`) and
`PM_ONNXRUNTIME_DLL` (default `build-translate/ort/onnxruntime.dll`).
Without them the program still builds and runs (翻譯 then reports that the
engine is missing / falls back to Windows OCR, with a CMake warning). Make
them before step 6, or re-run the build afterwards.

### 5.1 `bergamot.dll` — built from source (MPL-2.0 + MIT / Apache-2.0 / BSD components)

Needs Git Bash (Git for Windows), the same Visual Studio / CMake (`cmake` on
`PATH`, e.g. start `bash` from a Developer Command Prompt) and PCRE2 as a
static vcpkg library:

```
C:\vcpkg\vcpkg install pcre2 --triplet x64-windows-static
```

**Online** — `translate/tools/build_bergamot.sh` clones
BergamotTranslatorSharp at the pinned commit `e084db279f0d4314b31c7730cfc61ab03f235604` with all its
submodules, applies this project's build patches (Eigen / ONNX sgemm instead of
Intel MKL or BLAS, `translate/bergamot/lapack_stubs.cpp`) and builds:

```
VCPKG_ROOT=C:/vcpkg bash translate/tools/build_bergamot.sh
```

Output: `build-translate/bergamot/bin/bergamot.dll`.

**Offline, from `ZizaiCast-0.7.7-deps-source.zip`** — its
`bergamot/BergamotTranslatorSharp/` is exactly that tree (every submodule at
the commit recorded by its parent, unmodified; list in `DEPS-README.txt`).
BergamotTranslatorSharp's own CMake applies its patches from
`cmake/patches/` at configure time. `build_bergamot.sh` expects a git
checkout, so run its patch + build steps on the extracted tree (Git Bash, at the
root of this archive, deps zip next to it; no download from GitHub needed):

```
mkdir -p build-translate/x
unzip -q ../ZizaiCast-0.7.7-deps-source.zip -d build-translate/x
mv build-translate/x/bergamot/BergamotTranslatorSharp build-translate/bts-src
M=build-translate/bts-src/bergamot-translator/3rd_party/marian-dev   # Marian's CMake reads its revision from git:
git -C $M init -q
git -C $M -c user.name=build -c user.email=build@localhost commit -q --allow-empty -m "marian-dev from the deps-source zip"
sed -n '/^# 1)/,$p' translate/tools/build_bergamot.sh > build-translate/steps.sh
HERE="$PWD/translate" OUT="$PWD/build-translate" SRC="$PWD/build-translate/bts-src" \
  BLD="$PWD/build-translate/bergamot-build" VCPKG=C:/vcpkg bash -euo pipefail build-translate/steps.sh
```

### 5.2 `onnxruntime.dll` — official Microsoft binary, not rebuilt (MIT)

```
bash translate/tools/get_onnxruntime.sh
```

downloads `https://files.pythonhosted.org/packages/a6/13/0f1699f6de549c9324bc9112a2a85b14c517904cd11b562a654643b755a1/onnxruntime-1.30.0-cp312-cp312-win_amd64.whl`,
checks SHA-256 `f3501472571f1b1eee50e017851e7929f5ea37312d2d8c2494a19e8fc58b4a38` and extracts `onnxruntime.dll` (ONNX Runtime
1.30.0, CPU; the same DLL as `onnxruntime-win-x64-1.30.0.zip` of the GitHub
release) with its `LICENSE` and `ThirdPartyNotices.txt` into
`build-translate/ort/`. It is shipped unmodified; upstream source:
https://github.com/microsoft/onnxruntime/tree/v1.30.0 . The C API headers it
is used through are in `translate/third_party/onnxruntime/`, its notices in
`docs/licenses/onnxruntime/` (installed to `licenses\onnxruntime\`).

### 5.3 Models (not in the installer or in these archives)

The Firefox Translations models (MPL-2.0, from Mozilla's CDN) and the PaddleOCR
text detection / recognition models (Apache-2.0, ONNX conversions of the
RapidOCR project on ModelScope) are downloaded by the program itself, after the
user agrees, into the user's profile. They are data, not part of the program.
The same holds for the optional add-ons of 0.7.4+: the llama.cpp runtime b11514
(MIT, from its GitHub release) with the Qwen GGUF models (Apache-2.0, from
Hugging Face) for local AI translation, and the DirectML build of ONNX Runtime
with `DirectML.dll` (from Microsoft's NuGet feed) for text recognition on the
GPU. Pinned names, sizes and SHA-256: `translate/src/llm_engine_models.inc`,
`translate/src/ocr_gpu.inc`; notices: `docs/licenses/THIRD_PARTY_NOTICES.txt`
part 2d.

## 6. Configure and build

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

## 7. Installer

```
"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" /DBuildDir=..\build\bin\Release installer\zizai.iss
```

(`installer\zizai.iss` packages the user guides from `docs\tutorial\` and
the notices from `docs\licenses\`, both in this archive. Output:
`installer\Output\自在投影-安裝程式-0.7.7.exe`, Traditional Chinese +
English.)

## 8. Replacing the LGPL libraries

`plist-2.0.dll` (libplist) and `avcodec-*.dll` / `avutil-*.dll`
(FFmpeg) are ordinary DLLs next to `自在投影.exe`; you may replace them with
your own ABI-compatible builds (e.g. from the deps-source zip) without
rebuilding the program.