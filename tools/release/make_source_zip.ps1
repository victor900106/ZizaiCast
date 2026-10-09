<#
.SYNOPSIS
  Builds the GPL-3.0 "Corresponding Source" archives for a ZizaiCast release.

.DESCRIPTION
  Output (default directory: <repo>\build-release\):

    ZizaiCast-<version>-source.zip
        `git archive <Ref>` of this repository (prefix ZizaiCast-<version>-source/)
        plus a generated BUILD.md at its root. Everything tracked is in it,
        including the original 投投 Toutou artwork (assets/public/toutou,
        video/res/toutou, app/res/app.ico) and the user guides
        (docs/tutorial), except:
          - generated test media: video/testdata/*.png|*.log|*.h264|*.h265|*.mp4|*.bin
          - _ref/, build*/, installer/Output/ (untracked anyway)
          - test material rebuilt from the owner's private screenshots
            (translate/testdata/eval/owner072.ref.json, make_owner_072.py)
        Guard: the archive is refused if any of the OLD mascot / icon art
        (removed in 64c29cc, copyright unknown, never to be distributed)
        reappears - by file name or by git blob id (every historical
        version of those files).

    ZizaiCast-<version>-deps-source.zip      (skip with -NoDeps)
        upstream/ + ports/: upstream source archives of the vcpkg libraries
        whose DLLs / static code ship in the installer (openssl, libplist,
        pthreads4w, alac, ffmpeg; pcre2 x64-windows-static, linked into
        bergamot.dll), taken from <vcpkg>\downloads, plus each vcpkg port
        directory (portfile.cmake + patches = the build scripts).
        bergamot/: the complete source of bergamot.dll (translation engine,
        MPL-2.0 + MIT / Apache-2.0 / BSD components): `git archive` of
        BergamotTranslatorSharp at the commit pinned in
        translate/tools/build_bergamot.sh and of every submodule (recursive)
        at the commit recorded in its parent, taken from -BergamotSrc (the
        checkout build_bergamot.sh made; local edits there are NOT archived),
        plus the build script / patch files of this project (bergamot/pm/).
        onnxruntime/: licence, third-party notices and provenance of the
        unmodified official onnxruntime.dll (MIT; no source needed, upstream URL).
        DEPS-README.txt: versions, commits, SHA-256, what is not included and why.

  Both files are meant to be attached to the GitHub release next to the
  installer (GPL-3 s.6(d) / LGPL-2.1 s.4: equivalent access from the same place).

.PARAMETER Ref         git revision to archive (default HEAD).
.PARAMETER Version     release version (default: PM_APP_VERSION in <Ref>:app/CMakeLists.txt).
.PARAMETER OutDir      output directory (default <repo>\build-release).
.PARAMETER VcpkgRoot   vcpkg root (default $env:VCPKG_ROOT, else %USERPROFILE%\vcpkg).
.PARAMETER NoDeps      do not build the deps-source zip.
.PARAMETER BergamotSrc BergamotTranslatorSharp checkout with initialised submodules
                       (default <repo>\build-translate\bts-src, made by build_bergamot.sh).
.PARAMETER AllowFdkAac allow archiving a revision whose pm_audio still links fdk-aac
                       (fdk-aac's licence is GPL-incompatible: such a revision must not be released).
.PARAMETER Verify      unzip the source zip to %TEMP%, fetch the Android tools, and do a
                       clean CMake configure + build of the whole tree (no placeholder
                       art needed: the original artwork is in the archive).
.PARAMETER KeepVerifyDir   keep the %TEMP% verification tree.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\release\make_source_zip.ps1 -Verify
#>
param(
    [string]$Ref = 'HEAD',
    [string]$Version,
    [string]$OutDir,
    [string]$VcpkgRoot,
    [switch]$NoDeps,
    [string]$BergamotSrc,
    [switch]$AllowFdkAac,
    [switch]$Verify,
    [switch]$KeepVerifyDir
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Invoke-Git {
    $out = & git -C $Repo -c core.quotepath=off @args
    if ($LASTEXITCODE -ne 0) { throw "git $args failed ($LASTEXITCODE)" }
    $out
}
function Get-Sha256([string]$p) { (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLowerInvariant() }
function Format-Size([long]$b) { '{0:N1} MB ({1:N0} bytes)' -f ($b / 1MB), $b }

$Repo = (& git -C $PSScriptRoot rev-parse --show-toplevel).Trim()
if (-not $Repo) { throw 'not inside the git repository' }
$Repo = $Repo -replace '/', '\'
$Commit = (Invoke-Git rev-parse $Ref).Trim()
$CommitDate = (Invoke-Git show -s --format=%cI $Commit).Trim()
if (-not $OutDir) { $OutDir = Join-Path $Repo 'build-release' }
if (-not $VcpkgRoot) { $VcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE 'vcpkg' } }

$appCmake = (Invoke-Git show "${Commit}:app/CMakeLists.txt") -join "`n"
if (-not $Version) {
    if ($appCmake -match 'set\(PM_APP_VERSION\s+([0-9][0-9A-Za-z.\-]*)\)') { $Version = $Matches[1] }
    else { throw 'cannot read PM_APP_VERSION from app/CMakeLists.txt; pass -Version' }
}
$dirty = Invoke-Git status --porcelain --untracked-files=no
if ($Ref -eq 'HEAD' -and $dirty) {
    Write-Warning "working tree has uncommitted changes; the archive contains $Ref ($($Commit.Substring(0,10))) only:"
    $dirty | ForEach-Object { Write-Warning "  $_" }
}

# ---- which audio codec does <Ref> link? (fdk-aac = GPL-incompatible) ----
$audioCmake = (Invoke-Git show "${Commit}:audio/CMakeLists.txt") -join "`n"
$usesFfmpeg = $audioCmake -match 'find_package\(FFMPEG'
$pmAudioFdk = $audioCmake -match 'target_link_libraries\(pm_audio\s+PRIVATE[^)]*FDK-AAC'
if ($pmAudioFdk) {
    $msg = "audio/CMakeLists.txt at $($Commit.Substring(0,10)) links fdk-aac into pm_audio (shipped). " +
           "The Fraunhofer FDK licence is incompatible with GPL-3.0: do not release this revision."
    if ($AllowFdkAac) { Write-Warning $msg } else { throw "$msg (override for testing: -AllowFdkAac)" }
}

# ---- vcpkg port versions (installed = what the release binaries were built from) ----
$ShippedPorts = [ordered]@{
    'openssl'  = @{ Archive = 'openssl-openssl-openssl-*';   Url = 'https://github.com/openssl/openssl' }
    'libplist' = @{ Archive = 'libimobiledevice-libplist-*'; Url = 'https://github.com/libimobiledevice/libplist' }
    'pthreads' = @{ Archive = 'pthreads4w-code-*';           Url = 'https://sourceforge.net/projects/pthreads4w/' }
    'alac'     = @{ Archive = 'macosforge-alac-*';           Url = 'https://github.com/macosforge/alac' }
}
if ($usesFfmpeg) { $ShippedPorts['ffmpeg'] = @{ Archive = 'ffmpeg-ffmpeg-n*'; Url = 'https://ffmpeg.org/ (git: https://git.ffmpeg.org/ffmpeg.git)' } }
foreach ($k in @($ShippedPorts.Keys)) { $ShippedPorts[$k].Triplet = 'x64-windows' }
# bergamot.dll (translate/tools/build_bergamot.sh) links PCRE2 statically (ssplit-cpp)
$hasTranslate = [bool](Invoke-Git ls-tree --name-only $Commit -- translate/tools/build_bergamot.sh)
if ($hasTranslate) { $ShippedPorts['pcre2'] = @{ Archive = 'PCRE2Project-pcre2-*'; Url = 'https://github.com/PCRE2Project/pcre2'; Triplet = 'x64-windows-static' } }

$installed = @{}
$statusFile = Join-Path $VcpkgRoot 'installed\vcpkg\status'
$statusText = @()
if (Test-Path $statusFile) { $statusText += Get-Content -Encoding UTF8 $statusFile }
$updDir = Join-Path $VcpkgRoot 'installed\vcpkg\updates'
if (Test-Path $updDir) { Get-ChildItem $updDir -File | Sort-Object Name | ForEach-Object { $statusText += ''; $statusText += Get-Content -Encoding UTF8 $_.FullName } }
$cur = @{}
foreach ($line in ($statusText + '')) {
    if ($line -eq '') {
        if ($cur.Package -and -not $cur.Feature -and $cur.Architecture) {
            $key = "$($cur.Package):$($cur.Architecture)"
            if ($cur.Status -match 'install ok installed') { $installed[$key] = $cur.Clone() }
            elseif ($installed.ContainsKey($key) -and $cur.Status -match 'deinstall|not-installed') { $installed.Remove($key) }
        }
        $cur = @{}; continue
    }
    if ($line -match '^([A-Za-z-]+):\s*(.*)$') { $cur[$Matches[1]] = $Matches[2] }
}
$vcpkgCommit = try { (& git -C $VcpkgRoot rev-parse HEAD).Trim() } catch { 'unknown' }
$PortInfo = [ordered]@{}
foreach ($p in $ShippedPorts.Keys) {
    $json = Get-Content -Raw (Join-Path $VcpkgRoot "ports\$p\vcpkg.json") | ConvertFrom-Json
    $portVer = @($json.version, $json.'version-string', $json.'version-semver', $json.'version-date') | Where-Object { $_ } | Select-Object -First 1
    $portRev = if ($json.'port-version') { [int]$json.'port-version' } else { 0 }
    $v = $portVer; $r = $portRev
    $trip = $ShippedPorts[$p].Triplet
    $ik = "${p}:$trip"
    if ($installed.ContainsKey($ik)) {
        $v = $installed[$ik].Version
        $r = if ($installed[$ik].'Port-Version') { [int]$installed[$ik].'Port-Version' } else { 0 }
        if ($v -ne $portVer -or $r -ne $portRev) { Write-Warning "$ik installed $v#$r but ports/ has $portVer#$portRev" }
    } else { Write-Warning "$ik is not installed in $VcpkgRoot (using ports/$p/vcpkg.json: $portVer#$portRev)" }
    $PortInfo[$p] = [pscustomobject]@{ Name = $p; Version = $v; PortVersion = $r; Url = $ShippedPorts[$p].Url; Archive = $ShippedPorts[$p].Archive; Triplet = $trip }
}
$portLines = ($PortInfo.Values | ForEach-Object {
    $spec = if ($_.Name -eq 'ffmpeg') { 'ffmpeg[core,avcodec]' } else { $_.Name }
    "| ``$spec`` | $($_.Triplet) | $($_.Version) | $($_.PortVersion) | $($_.Url) |"
}) -join "`n"
$installList = ($PortInfo.Values | Where-Object { $_.Triplet -eq 'x64-windows' } | ForEach-Object { if ($_.Name -eq 'ffmpeg') { '"ffmpeg[core,avcodec]"' } else { $_.Name } }) -join ' '

# ---- bergamot.dll / onnxruntime.dll provenance (translate/, 0.7.0+) ----
$BtsCommit = $null; $OrtUrl = $null; $OrtSha = $null; $OrtVer = $null
if ($hasTranslate) {
    $bsh = (Invoke-Git show "${Commit}:translate/tools/build_bergamot.sh") -join "`n"
    if ($bsh -match '(?m)^COMMIT=([0-9a-f]{40})') { $BtsCommit = $Matches[1] } else { throw 'cannot read COMMIT= from translate/tools/build_bergamot.sh' }
    $gsh = (Invoke-Git show "${Commit}:translate/tools/get_onnxruntime.sh") -join "`n"
    if ($gsh -match '(?m)^URL="([^"]+)"') { $OrtUrl = $Matches[1] }
    if ($gsh -match '(?m)^SHA="([0-9a-f]{64})"') { $OrtSha = $Matches[1] }
    if ($OrtUrl -match 'onnxruntime-([0-9.]+)-') { $OrtVer = $Matches[1] }
    if (-not ($OrtUrl -and $OrtSha -and $OrtVer)) { throw 'cannot read URL= / SHA= from translate/tools/get_onnxruntime.sh' }
    if (-not $BergamotSrc) { $BergamotSrc = Join-Path $Repo 'build-translate\bts-src' }
}

# ---- BUILD.md ----
$Prefix = "ZizaiCast-$Version-source"
$stage = Join-Path ([IO.Path]::GetTempPath()) ("zizai-src-stage-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $stage | Out-Null
$depsList = 'OpenSSL, libplist, pthreads4w, ALAC, FFmpeg + vcpkg port scripts/patches'
$translateMd = ''
$sec = 5
if ($hasTranslate) {
    $depsList = 'OpenSSL, libplist, pthreads4w, ALAC, FFmpeg, PCRE2 + vcpkg port scripts/patches; the complete source of `bergamot.dll`; licence and provenance of `onnxruntime.dll`'
    $sec = 6
    $translateMd = @"
## 5. Translation engine and OCR runtime (``bergamot.dll``, ``onnxruntime.dll``)

Both DLLs are loaded at run time from the program folder. CMake copies them
next to ``自在投影.exe`` when they exist at the cache paths ``PM_BERGAMOT_DLL``
(default ``build-translate/bergamot/bin/bergamot.dll``) and
``PM_ONNXRUNTIME_DLL`` (default ``build-translate/ort/onnxruntime.dll``).
Without them the program still builds and runs (翻譯 then reports that the
engine is missing / falls back to Windows OCR, with a CMake warning). Make
them before step $sec, or re-run the build afterwards.

### 5.1 ``bergamot.dll`` — built from source (MPL-2.0 + MIT / Apache-2.0 / BSD components)

Needs Git Bash (Git for Windows), the same Visual Studio / CMake (``cmake`` on
``PATH``, e.g. start ``bash`` from a Developer Command Prompt) and PCRE2 as a
static vcpkg library:

``````
C:\vcpkg\vcpkg install pcre2 --triplet x64-windows-static
``````

**Online** — ``translate/tools/build_bergamot.sh`` clones
BergamotTranslatorSharp at the pinned commit ``$BtsCommit`` with all its
submodules, applies this project's build patches (Eigen / ONNX sgemm instead of
Intel MKL or BLAS, ``translate/bergamot/lapack_stubs.cpp``) and builds:

``````
VCPKG_ROOT=C:/vcpkg bash translate/tools/build_bergamot.sh
``````

Output: ``build-translate/bergamot/bin/bergamot.dll``.

**Offline, from ``ZizaiCast-$Version-deps-source.zip``** — its
``bergamot/BergamotTranslatorSharp/`` is exactly that tree (every submodule at
the commit recorded by its parent, unmodified; list in ``DEPS-README.txt``).
BergamotTranslatorSharp's own CMake applies its patches from
``cmake/patches/`` at configure time. ``build_bergamot.sh`` expects a git
checkout, so run its patch + build steps on the extracted tree (Git Bash, at the
root of this archive, deps zip next to it; no download from GitHub needed):

``````
mkdir -p build-translate/x
unzip -q ../ZizaiCast-$Version-deps-source.zip -d build-translate/x
mv build-translate/x/bergamot/BergamotTranslatorSharp build-translate/bts-src
M=build-translate/bts-src/bergamot-translator/3rd_party/marian-dev   # Marian's CMake reads its revision from git:
git -C `$M init -q
git -C `$M -c user.name=build -c user.email=build@localhost commit -q --allow-empty -m "marian-dev from the deps-source zip"
sed -n '/^# 1)/,`$p' translate/tools/build_bergamot.sh > build-translate/steps.sh
HERE="`$PWD/translate" OUT="`$PWD/build-translate" SRC="`$PWD/build-translate/bts-src" \
  BLD="`$PWD/build-translate/bergamot-build" VCPKG=C:/vcpkg bash -euo pipefail build-translate/steps.sh
``````

### 5.2 ``onnxruntime.dll`` — official Microsoft binary, not rebuilt (MIT)

``````
bash translate/tools/get_onnxruntime.sh
``````

downloads ``$OrtUrl``,
checks SHA-256 ``$OrtSha`` and extracts ``onnxruntime.dll`` (ONNX Runtime
$OrtVer, CPU; the same DLL as ``onnxruntime-win-x64-$OrtVer.zip`` of the GitHub
release) with its ``LICENSE`` and ``ThirdPartyNotices.txt`` into
``build-translate/ort/``. It is shipped unmodified; upstream source:
https://github.com/microsoft/onnxruntime/tree/v$OrtVer . The C API headers it
is used through are in ``translate/third_party/onnxruntime/``, its notices in
``docs/licenses/onnxruntime/`` (installed to ``licenses\onnxruntime\``).

### 5.3 Models (not in the installer or in these archives)

The Firefox Translations models (MPL-2.0, from Mozilla's CDN) and the PaddleOCR
text detection / recognition models (Apache-2.0, ONNX conversions of the
RapidOCR project on ModelScope) are downloaded by the program itself, after the
user agrees, into the user's profile. They are data, not part of the program.
The same holds for the optional add-ons of 0.7.4+: the llama.cpp runtime b11514
(MIT, from its GitHub release) with the Qwen GGUF models (Apache-2.0, from
Hugging Face) for local AI translation, and the DirectML build of ONNX Runtime
with ``DirectML.dll`` (from Microsoft's NuGet feed) for text recognition on the
GPU. Pinned names, sizes and SHA-256: ``translate/src/llm_engine_models.inc``,
``translate/src/ocr_gpu.inc``; notices: ``docs/licenses/THIRD_PARTY_NOTICES.txt``
part 2d.


"@
}
$buildMd = @"
# Building ZizaiCast $Version from source

This archive is the Corresponding Source of the ZizaiCast $Version installer
(GPL-3.0, see ``LICENSE``, ``docs/licenses/SOURCE.md`` and
``docs/licenses/THIRD_PARTY_NOTICES.txt``).

* git commit: ``$Commit`` ($CommitDate)
* vcpkg commit used for the release build: ``$vcpkgCommit``
* Library sources ($depsList):
  ``ZizaiCast-$Version-deps-source.zip`` on the same release page.

## 1. Tools (Windows 10/11 x64)

| Tool | Version used for the release |
|---|---|
| Visual Studio 2026 Build Tools ("Visual Studio 18"), workload *Desktop development with C++* (MSVC v145, Windows SDK) | MSVC 14.51.36231 |
| CMake (the one bundled with VS Build Tools is fine) | 4.3.1 (>= 3.25 required) |
| vcpkg (classic mode) | commit ``$vcpkgCommit`` |
| Inno Setup (only for the installer) | 6.7.3 |
| Windows PowerShell 5.1 (for ``fetch_tools.ps1``) | |

## 2. Libraries (vcpkg; triplet x64-windows, pcre2 x64-windows-static)

| Port | Triplet | Version | Port-version | Upstream |
|---|---|---|---|---|
$portLines

``````
git clone https://github.com/microsoft/vcpkg C:\vcpkg
git -C C:\vcpkg checkout $vcpkgCommit
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install $installList --triplet x64-windows
``````
$(if ($hasTranslate) { "`n``pcre2`` (static) is only needed for ``bergamot.dll``, see section 5.`n" })

FFmpeg must stay an LGPL build: do not enable the ``gpl``, ``version3`` or
``nonfree`` features. ``fdk-aac`` is *not* needed by the shipped program
(at most by the optional ``pm_audio_test``).

## 3. Android helper binaries (not in the archive, pinned by SHA-256)

``````
powershell -ExecutionPolicy Bypass -File android\third_party\fetch_tools.ps1
``````

Downloads adb (Android SDK Platform-Tools r37.0.1, from dl.google.com) and
scrcpy-server v5.0 (github.com/Genymobile/scrcpy). CMake also runs it at
configure time if the files are missing (``-DPM_ANDROID_FETCH_TOOLS=OFF`` to
disable).

## 4. Artwork

The mascot 投投 (Toutou) and the app icon are original artwork of this
project and are included: sources in ``assets/public/toutou/`` (SVG + PNG
exports, generator ``docs/mascot/toutou/*.mjs``), the layers embedded by
``pm_video`` in ``video/res/toutou/``, the icon in ``app/res/app.ico``.
Nothing has to be added to build.

$translateMd## $sec. Configure and build

``````
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
``````

Result: ``build\bin\Release\自在投影.exe`` plus the vcpkg DLLs (copied by
vcpkg's applocal step), the VC++ runtime DLLs and ``android-tools\``.
Individual targets: ``pm_core pm_video pm_audio pm_recorder pm_miracast
pm_android PhoneMirror`` (the app), tests ``pm_probe pm_video_test
pm_audio_test pm_recorder_test pm_miracast_test pm_android_test``.

## $($sec + 1). Installer

``````
"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" /DBuildDir=..\build\bin\Release installer\zizai.iss
``````

(``installer\zizai.iss`` packages the user guides from ``docs\tutorial\`` and
the notices from ``docs\licenses\``, both in this archive. Output:
``installer\Output\自在投影-安裝程式-$Version.exe``, Traditional Chinese +
English.)

## $($sec + 2). Replacing the LGPL libraries

``plist-2.0.dll`` (libplist) and ``avcodec-*.dll`` / ``avutil-*.dll``
(FFmpeg) are ordinary DLLs next to ``自在投影.exe``; you may replace them with
your own ABI-compatible builds (e.g. from the deps-source zip) without
rebuilding the program.
"@
$buildMdPath = Join-Path $stage 'BUILD.md'
[IO.File]::WriteAllText($buildMdPath, ($buildMd -replace "`r?`n", "`r`n"), $Utf8NoBom)

# ---- source zip ----
New-Item -ItemType Directory -Force $OutDir | Out-Null
$srcZip = Join-Path $OutDir "$Prefix.zip"
if (Test-Path $srcZip) { Remove-Item -Force $srcZip }
$excludes = @(
    ':(exclude,glob)video/testdata/*.png',
    ':(exclude,glob)video/testdata/*.log',
    ':(exclude,glob)video/testdata/*.h264',
    ':(exclude,glob)video/testdata/*.h265',
    ':(exclude,glob)video/testdata/*.mp4',
    ':(exclude,glob)video/testdata/*.bin',
    ':(exclude,glob)_ref/**',
    ':(exclude,glob)build*/**',
    ':(exclude,glob)installer/Output/**',
    # test material rebuilt from the owner's private screenshots (not published)
    ':(exclude)translate/testdata/eval/owner072.ref.json',
    ':(exclude)translate/testdata/make_owner_072.py'
)
& git -C $Repo archive --format=zip -9 "--prefix=$Prefix/" "--add-file=$buildMdPath" -o $srcZip $Commit -- . @excludes
if ($LASTEXITCODE -ne 0) { throw "git archive failed ($LASTEXITCODE)" }

# Guard: the OLD mascot / icon art (copyright unknown; replaced by the original
# 投投 art and removed in 64c29cc) must never come back - neither under one of
# its file names nor as the same content under another name.
$OldArtNames = @('mascot.png', 'mascot_original.png', 'icon_source.png', 'make_icon.py', 'android-disconnected-toast.png')
$OldArtBlobs = @(   # every historical blob of app/res/app.ico, docs/tutorial/自在投影教學.html,
                    # assets/mascot*.png, assets/icon_source.png, video/res/mascot.png and the
                    # removed screenshot, up to 64c29cc^
    '12147dbc62fa1e7fd67903bd5eed2401652e16cc', '254a638d98ef6032d53840d077ccc3ae667408a8',
    '3881f116fcf5b54d556633eb584be327163685e8', '416d480ccfc371ee5488f21ad2bd0c47b3d837ac',
    '41b3baf190e26960b4da780a0348f78f97fd32b0', '54fa187337796a0bf94af3501f4be2a1859895d9',
    '7cdda20ebca3b6570c36384c3f63d90a8a9d8a0c', 'a6ae989f3ad4fca3e9aff619deaf3037c927edef',
    'b6406928996cd8bf0e3f539d8e3fdb74c9ece983'
)
$za = [IO.Compression.ZipFile]::OpenRead($srcZip)
try { $entries = @($za.Entries | ForEach-Object { $_.FullName.Substring($Prefix.Length + 1) } | Where-Object { $_ -and -not $_.EndsWith('/') }) }
finally { $za.Dispose() }
$bad = @($entries | Where-Object { $OldArtNames -contains ($_ -split '/')[-1] })
$treeLines = @(Invoke-Git ls-tree -r $Commit)   # "<mode> blob <sha>\t<path>"
$blobOf = @{}
foreach ($l in $treeLines) { if ($l -match '^\d+ blob ([0-9a-f]{40})\t(.+)$') { $blobOf[$Matches[2]] = $Matches[1] } }
$bad += @($entries | Where-Object { $blobOf.ContainsKey($_) -and $OldArtBlobs -contains $blobOf[$_] } | ForEach-Object { "$_ (old art content)" })
if ($bad) { throw "old mascot / icon art (not to be distributed) is in the zip: $($bad -join ', ')" }
# ... and the original art the build needs is in it.
$needArt = @('app/res/app.ico', 'video/res/toutou/cloud.png', 'video/res/toutou/toutou_layout.h', 'assets/public/toutou/toutou_340.png')
$missing = @($needArt | Where-Object { $entries -notcontains $_ })
if ($missing) { throw "original artwork missing from the zip: $($missing -join ', ')" }
$tracked = @(Invoke-Git ls-tree -r --name-only $Commit)
$entrySet = New-Object 'System.Collections.Generic.HashSet[string]' (, [string[]]$entries)
$left = @($tracked | Where-Object { -not $entrySet.Contains($_) })

# ---- deps-source zip ----
# Copies the entries of the zip $from into the open archive $to (prefix + optional
# path filter), skipping directory entries and names already present.
function Copy-ZipEntries($to, [string]$from, [string]$stripPrefix = '', [string]$addPrefix = '', $seen) {
    $src = [IO.Compression.ZipFile]::OpenRead($from)
    $n = 0
    try {
        foreach ($en in $src.Entries) {
            $name = $en.FullName
            if ($name.EndsWith('/')) { continue }
            if ($stripPrefix) { if (-not $name.StartsWith($stripPrefix)) { continue }; $name = $name.Substring($stripPrefix.Length) }
            $name = $addPrefix + $name
            if (-not $seen.Add($name)) { continue }
            $ne = $to.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
            $ne.LastWriteTime = $en.LastWriteTime
            $i = $en.Open(); $o = $ne.Open()
            try { $i.CopyTo($o) } finally { $o.Dispose(); $i.Dispose() }
            $n++
        }
    } finally { $src.Dispose() }
    return $n
}
# `git archive` of <commit> of the repository at $dir (prefix $prefix), then of every
# submodule (mode 160000 entry of that commit) at the commit recorded there, recursively.
# Returns "path commit files" lines. Local edits / other checked-out commits are ignored.
function Add-GitTreeRecursive($to, [string]$dir, [string]$commit, [string]$prefix, [string]$rel, $seen, $lines) {
    if (-not (Test-Path (Join-Path $dir '.git'))) { throw "bergamot source: $dir is not an initialised git checkout (run translate/tools/build_bergamot.sh once, or 'git submodule update --init --recursive')" }
    & git -C $dir cat-file -e "$commit^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0) { throw "bergamot source: commit $commit not present in $dir" }
    $tmp = Join-Path $stage ("ga-" + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.zip')
    & git -C $dir -c core.autocrlf=false archive --format=zip -0 "--prefix=$prefix" -o $tmp $commit
    if ($LASTEXITCODE -ne 0) { throw "git archive $commit in $dir failed" }
    $n = Copy-ZipEntries $to $tmp '' '' $seen
    Remove-Item -Force $tmp
    $label = if ($rel) { $rel } else { '.' }
    $lines.Add(('  {0,-62} {1}  {2,6} files' -f $label, $commit, $n))
    foreach ($l in @(& git -C $dir -c core.quotepath=off ls-tree -r $commit)) {
        if ($l -match '^160000 commit ([0-9a-f]{40})\t(.+)$') {
            $sub = $Matches[2]
            Add-GitTreeRecursive $to (Join-Path $dir ($sub -replace '/', '\')) $Matches[1] "$prefix$sub/" ($(if ($rel) { "$rel/$sub" } else { $sub })) $seen $lines
        }
    }
}
$depZip = $null
if (-not $NoDeps) {
    $depZip = Join-Path $OutDir "ZizaiCast-$Version-deps-source.zip"
    if (Test-Path $depZip) { Remove-Item -Force $depZip }
    $dl = Join-Path $VcpkgRoot 'downloads'
    $readme = New-Object System.Collections.Generic.List[string]
    $readme.Add("Source code of the third-party libraries shipped in ZizaiCast $Version")
    $readme.Add("(GPL-3.0 s.6 Corresponding Source; LGPL-2.1 s.4 for libplist and FFmpeg;")
    $readme.Add(" MPL-2.0 s.3.2 for bergamot.dll). Build instructions: BUILD.md in")
    $readme.Add("ZizaiCast-$Version-source.zip; licences: docs/licenses/ there and licenses\ in the program folder.")
    $readme.Add("")
    $readme.Add("upstream/     unmodified upstream source archives, as downloaded by vcpkg")
    $readme.Add("ports/        vcpkg port scripts (portfile.cmake, patches) = how they were configured/patched/built")
    if ($hasTranslate) {
        $readme.Add("bergamot/     complete source of bergamot.dll (translation engine), see below")
        $readme.Add("onnxruntime/  licence + notices of the unmodified official onnxruntime.dll, see below")
    }
    $readme.Add("vcpkg commit: $vcpkgCommit   (https://github.com/microsoft/vcpkg/tree/$vcpkgCommit/ports)")
    $readme.Add("triplet: x64-windows (DLLs, release); alac is a static library; pcre2 is x64-windows-static (linked into bergamot.dll)")
    $readme.Add("")
    $seen = New-Object 'System.Collections.Generic.HashSet[string]'
    $fs = [IO.File]::Open($depZip, 'CreateNew')
    $zip = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($pi in $PortInfo.Values) {
            $verKey = if ($pi.Name -eq 'alac') { ($pi.Version -split '-')[-1] } else { $pi.Version }
            $arc = Get-ChildItem $dl -File -Filter $pi.Archive -ErrorAction SilentlyContinue |
                   Where-Object { $_.Name -like "*$verKey*" -and $_.Name -notlike '*.part' } | Select-Object -First 1
            if (-not $arc) { throw "upstream archive for $($pi.Name) $($pi.Version) not found in $dl (pattern $($pi.Archive), run 'vcpkg install' first or use -NoDeps)" }
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $arc.FullName, "upstream/$($arc.Name)", [IO.Compression.CompressionLevel]::NoCompression)
            [void]$seen.Add("upstream/$($arc.Name)")
            $portDir = Join-Path $VcpkgRoot "ports\$($pi.Name)"
            Get-ChildItem $portDir -Recurse -File | ForEach-Object {
                $rel = $_.FullName.Substring($portDir.Length + 1) -replace '\\', '/'
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, "ports/$($pi.Name)/$rel", [IO.Compression.CompressionLevel]::Optimal)
                [void]$seen.Add("ports/$($pi.Name)/$rel")
            }
            $readme.Add(('{0,-9} {1}#{2}  {3}  ({4})' -f $pi.Name, $pi.Version, $pi.PortVersion, $pi.Url, $pi.Triplet))
            $readme.Add(('          upstream/{0}  sha256 {1}' -f $arc.Name, (Get-Sha256 $arc.FullName)))
        }
        if ($hasTranslate) {
            # bergamot.dll: BergamotTranslatorSharp + all submodules at the pinned commits
            $btsLines = New-Object System.Collections.Generic.List[string]
            Add-GitTreeRecursive $zip $BergamotSrc $BtsCommit 'bergamot/BergamotTranslatorSharp/' '' $seen $btsLines
            # this project's build script and patch file for it (also in the source zip)
            $pmTmp = Join-Path $stage 'pm-translate.zip'
            & git -C $Repo archive --format=zip -o $pmTmp $Commit -- translate/tools/build_bergamot.sh translate/bergamot docs/licenses/onnxruntime
            if ($LASTEXITCODE -ne 0) { throw 'git archive (translate build files) failed' }
            [void](Copy-ZipEntries $zip $pmTmp 'translate/' 'bergamot/pm/translate/' $seen)
            [void](Copy-ZipEntries $zip $pmTmp 'docs/licenses/onnxruntime/' 'onnxruntime/' $seen)
            Remove-Item -Force $pmTmp
            $readme.Add("")
            $readme.Add("bergamot.dll (translation engine; MPL-2.0, components MIT / Apache-2.0 / BSD, see THIRD_PARTY_NOTICES part 2b)")
            $readme.Add("  bergamot/BergamotTranslatorSharp/  git archive of https://github.com/Freeesia/BergamotTranslatorSharp")
            $readme.Add("  at $BtsCommit and of every submodule (recursive) at the commit recorded")
            $readme.Add("  by its parent - unmodified upstream source (no .git):")
            $btsLines | ForEach-Object { $readme.Add($_) }
            $readme.Add("  Patches: BergamotTranslatorSharp's CMake applies its own cmake/patches/ at configure time;")
            $readme.Add("  this project's changes are in bergamot/pm/translate/ (= translate/ of the source zip):")
            $readme.Add("  tools/build_bergamot.sh (USE_ONNX_SGEMM option in marian-dev/CMakeLists.txt, Eigen/ONNX sgemm")
            $readme.Add("  instead of MKL/BLAS, static CRT, x86-64-v2) and bergamot/lapack_stubs.cpp (added to the DLL).")
            $readme.Add("  PCRE2 (static, used by ssplit-cpp): upstream/ + ports/pcre2 above.")
            $readme.Add("  How to build from this zip (offline) or from GitHub: BUILD.md section 5.1.")
            $readme.Add("")
            $readme.Add("onnxruntime.dll (text recognition runtime; MIT) - shipped UNMODIFIED, not rebuilt:")
            $readme.Add("  ONNX Runtime $OrtVer, official Microsoft CPU build, taken from")
            $readme.Add("  $OrtUrl")
            $readme.Add("  (sha256 $OrtSha; translate/tools/get_onnxruntime.sh; the same DLL as")
            $readme.Add("  onnxruntime-win-x64-$OrtVer.zip on https://github.com/microsoft/onnxruntime/releases/tag/v$OrtVer).")
            $ortDll = Join-Path $Repo 'build-translate\ort\onnxruntime.dll'
            if (Test-Path $ortDll) { $readme.Add("  onnxruntime.dll sha256 $(Get-Sha256 $ortDll)") }
            $readme.Add("  Source: https://github.com/microsoft/onnxruntime/tree/v$OrtVer (MIT does not require it here).")
            $readme.Add("  onnxruntime/LICENSE.txt and onnxruntime/ThirdPartyNotices.txt = the licence and notices shipped")
            $readme.Add("  with the DLL (also installed in licenses\onnxruntime\).")
        }
        $readme.Add("")
        $readme.Add("Not included (separate programs shipped unmodified as binaries; source at upstream):")
        $readme.Add("  adb (Android SDK Platform-Tools r37.0.1): https://android.googlesource.com/platform/packages/modules/adb (tag platform-tools-37.0.1)")
        $readme.Add("  scrcpy-server v5.0: https://github.com/Genymobile/scrcpy/tree/v5.0")
        if ($hasTranslate) {
            $readme.Add("Not included (not distributed with the program; downloaded by it after the user agrees):")
            $readme.Add("  Firefox Translations models (MPL-2.0): https://github.com/mozilla/translations")
            $readme.Add("  PaddleOCR models, ONNX conversions by RapidOCR (Apache-2.0): https://github.com/PaddlePaddle/PaddleOCR,")
            $readme.Add("  https://www.modelscope.cn/models/RapidAI/RapidOCR")
            $readme.Add("  llama.cpp runtime b11514 for local AI translation (llama.dll, ggml*.dll, ggml-vulkan.dll; MIT;")
            $readme.Add("  libomp.dll Apache-2.0 WITH LLVM-exception): https://github.com/ggml-org/llama.cpp/releases/tag/b11514")
            $readme.Add("  (0.7.7+: unchanged mirror of the two zips at https://victor900106.github.io/ZizaiCast/addons/)")
            $readme.Add("  Qwen3.5 / Qwen3 GGUF models (Apache-2.0): https://huggingface.co/Qwen (files: translate/src/llm_engine_models.inc)")
            $readme.Add("  OCR on the GPU: onnxruntime.dll 1.24.4 DirectML build (MIT) and DirectML.dll 1.15.4 (Microsoft DirectML")
            $readme.Add("  licence), from Microsoft's NuGet packages Microsoft.ML.OnnxRuntime.DirectML / Microsoft.AI.DirectML")
            $readme.Add("  (translate/src/ocr_gpu.inc)")
        }
        $e = $zip.CreateEntry('DEPS-README.txt')
        $w = New-Object IO.StreamWriter($e.Open(), $Utf8NoBom)
        try { $w.Write(($readme -join "`r`n") + "`r`n") } finally { $w.Dispose() }
    } catch { $zip.Dispose(); $fs.Dispose(); Remove-Item -Force $depZip -ErrorAction SilentlyContinue; throw }
    $zip.Dispose(); $fs.Dispose()

    # Guard: everything the release ships from source is in the deps zip.
    $dz = [IO.Compression.ZipFile]::OpenRead($depZip)
    try { $dnames = New-Object 'System.Collections.Generic.HashSet[string]' (, [string[]]@($dz.Entries | ForEach-Object { $_.FullName })) }
    finally { $dz.Dispose() }
    $needDeps = @('DEPS-README.txt') + @($PortInfo.Values | ForEach-Object { "ports/$($_.Name)/portfile.cmake" })
    if ($hasTranslate) {
        $needDeps += @('bergamot/BergamotTranslatorSharp/CMakeLists.txt',
                       'bergamot/BergamotTranslatorSharp/bergamot-translator-dynamic/CMakeLists.txt',
                       'bergamot/BergamotTranslatorSharp/bergamot-translator/CMakeLists.txt',
                       'bergamot/BergamotTranslatorSharp/bergamot-translator/3rd_party/marian-dev/CMakeLists.txt',
                       'bergamot/BergamotTranslatorSharp/bergamot-translator/3rd_party/ssplit-cpp/CMakeLists.txt',
                       'bergamot/BergamotTranslatorSharp/bergamot-translator/3rd_party/marian-dev/src/3rd_party/intgemm/CMakeLists.txt',
                       'bergamot/BergamotTranslatorSharp/bergamot-translator/3rd_party/marian-dev/src/3rd_party/sentencepiece/CMakeLists.txt',
                       'bergamot/BergamotTranslatorSharp/bergamot-translator/3rd_party/marian-dev/src/3rd_party/onnxjs/deps/eigen/CMakeLists.txt',
                       'bergamot/pm/translate/tools/build_bergamot.sh', 'bergamot/pm/translate/bergamot/lapack_stubs.cpp',
                       'onnxruntime/LICENSE.txt', 'onnxruntime/ThirdPartyNotices.txt')
    }
    $missingDeps = @($needDeps | Where-Object { -not $dnames.Contains($_) })
    if ($missingDeps) { Remove-Item -Force $depZip; throw "deps zip incomplete, missing: $($missingDeps -join ', ')" }
    $upCount = @($dnames | Where-Object { $_ -like 'upstream/*' }).Count
    if ($upCount -ne $PortInfo.Count) { Remove-Item -Force $depZip; throw "deps zip: $upCount upstream archives for $($PortInfo.Count) ports" }
}

Remove-Item -Recurse -Force $stage
Write-Host ""
Write-Host "commit  $Commit  version $Version"
Write-Host ("source  {0}  {1}  sha256 {2}  ({3} files)" -f $srcZip, (Format-Size (Get-Item $srcZip).Length), (Get-Sha256 $srcZip), $entries.Count)
if ($depZip) { Write-Host ("deps    {0}  {1}  sha256 {2}" -f $depZip, (Format-Size (Get-Item $depZip).Length), (Get-Sha256 $depZip)) }
Write-Host "left out of the source zip ($($left.Count) tracked files):"
$left | ForEach-Object { Write-Host "  $_" }

# ---- verification: clean configure + build from the zip ----
if ($Verify) {
    $vroot = Join-Path ([IO.Path]::GetTempPath()) ("zizai-src-verify-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    [IO.Compression.ZipFile]::ExtractToDirectory($srcZip, $vroot)
    $src = Join-Path $vroot $Prefix
    Write-Host "`nverify: extracted to $src"
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $src 'android\third_party\fetch_tools.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'fetch_tools.ps1 failed' }
    $bdir = Join-Path $vroot 'build'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $cmake -S $src -B $bdir -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake"
    if ($LASTEXITCODE -ne 0) { throw "verify: configure failed ($LASTEXITCODE)" }
    & $cmake --build $bdir --config Release -- /m /v:minimal
    $rc = $LASTEXITCODE
    Write-Host ("verify: build exit code {0} after {1:N0} s" -f $rc, $sw.Elapsed.TotalSeconds)
    $bin = Join-Path $bdir 'bin\Release'
    Get-ChildItem $bin -File -ErrorAction SilentlyContinue | Sort-Object Name |
        ForEach-Object { Write-Host ('  {0,-28} {1,12:N0}' -f $_.Name, $_.Length) }
    if ($rc -eq 0) {
        $exe = Get-ChildItem $bin -Filter '*.exe' | Where-Object { $_.Name -notlike 'pm_*' } | Select-Object -First 1
        if (-not $exe) { throw 'verify: the app (自在投影.exe) was not built' }
        if (Test-Path (Join-Path $bin 'fdk-aac.dll')) { Write-Warning 'verify: fdk-aac.dll in the output (only pm_audio_test may use it; it must not be shipped)' }
    }
    if (-not $KeepVerifyDir) { Remove-Item -Recurse -Force $vroot -ErrorAction SilentlyContinue } else { Write-Host "verify tree kept: $vroot" }
    if ($rc -ne 0) { throw "verify: build failed ($rc)" }
    Write-Host 'verify: OK - the source zip configures and builds cleanly'
}
