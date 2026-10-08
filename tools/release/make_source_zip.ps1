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
        Guard: the archive is refused if any of the OLD mascot / icon art
        (removed in 64c29cc, copyright unknown, never to be distributed)
        reappears - by file name or by git blob id (every historical
        version of those files).

    ZizaiCast-<version>-deps-source.zip      (skip with -NoDeps)
        Upstream source archives of the vcpkg libraries whose DLLs / static
        code ship in the installer (openssl, libplist, pthreads4w, alac,
        ffmpeg), taken from <vcpkg>\downloads, plus each vcpkg port directory
        (portfile.cmake + patches = the build scripts) and DEPS-README.txt.

  Both files are meant to be attached to the GitHub release next to the
  installer (GPL-3 s.6(d) / LGPL-2.1 s.4: equivalent access from the same place).

.PARAMETER Ref         git revision to archive (default HEAD).
.PARAMETER Version     release version (default: PM_APP_VERSION in <Ref>:app/CMakeLists.txt).
.PARAMETER OutDir      output directory (default <repo>\build-release).
.PARAMETER VcpkgRoot   vcpkg root (default $env:VCPKG_ROOT, else %USERPROFILE%\vcpkg).
.PARAMETER NoDeps      do not build the deps-source zip.
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

$installed = @{}
$statusFile = Join-Path $VcpkgRoot 'installed\vcpkg\status'
$statusText = @()
if (Test-Path $statusFile) { $statusText += Get-Content -Encoding UTF8 $statusFile }
$updDir = Join-Path $VcpkgRoot 'installed\vcpkg\updates'
if (Test-Path $updDir) { Get-ChildItem $updDir -File | Sort-Object Name | ForEach-Object { $statusText += ''; $statusText += Get-Content -Encoding UTF8 $_.FullName } }
$cur = @{}
foreach ($line in ($statusText + '')) {
    if ($line -eq '') {
        if ($cur.Package -and -not $cur.Feature -and $cur.Architecture -eq 'x64-windows') {
            if ($cur.Status -match 'install ok installed') { $installed[$cur.Package] = $cur.Clone() }
            elseif ($installed.ContainsKey($cur.Package) -and $cur.Status -match 'deinstall|not-installed') { $installed.Remove($cur.Package) }
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
    if ($installed.ContainsKey($p)) {
        $v = $installed[$p].Version
        $r = if ($installed[$p].'Port-Version') { [int]$installed[$p].'Port-Version' } else { 0 }
        if ($v -ne $portVer -or $r -ne $portRev) { Write-Warning "$p installed $v#$r but ports/ has $portVer#$portRev" }
    } else { Write-Warning "$p is not installed in $VcpkgRoot (using ports/$p/vcpkg.json: $portVer#$portRev)" }
    $PortInfo[$p] = [pscustomobject]@{ Name = $p; Version = $v; PortVersion = $r; Url = $ShippedPorts[$p].Url; Archive = $ShippedPorts[$p].Archive }
}
$portLines = ($PortInfo.Values | ForEach-Object {
    $spec = if ($_.Name -eq 'ffmpeg') { 'ffmpeg[core,avcodec]' } else { $_.Name }
    "| ``$spec`` | $($_.Version) | $($_.PortVersion) | $($_.Url) |"
}) -join "`n"
$installList = ($PortInfo.Values | ForEach-Object { if ($_.Name -eq 'ffmpeg') { '"ffmpeg[core,avcodec]"' } else { $_.Name } }) -join ' '

# ---- BUILD.md ----
$Prefix = "ZizaiCast-$Version-source"
$stage = Join-Path ([IO.Path]::GetTempPath()) ("zizai-src-stage-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $stage | Out-Null
$buildMd = @"
# Building ZizaiCast $Version from source

This archive is the Corresponding Source of the ZizaiCast $Version installer
(GPL-3.0, see ``LICENSE``, ``docs/licenses/SOURCE.md`` and
``docs/licenses/THIRD_PARTY_NOTICES.txt``).

* git commit: ``$Commit`` ($CommitDate)
* vcpkg commit used for the release build: ``$vcpkgCommit``
* Library sources (OpenSSL, libplist, pthreads4w, ALAC, FFmpeg + vcpkg port
  scripts/patches): ``ZizaiCast-$Version-deps-source.zip`` on the same release page.

## 1. Tools (Windows 10/11 x64)

| Tool | Version used for the release |
|---|---|
| Visual Studio 2026 Build Tools ("Visual Studio 18"), workload *Desktop development with C++* (MSVC v145, Windows SDK) | MSVC 14.51.36231 |
| CMake (the one bundled with VS Build Tools is fine) | 4.3.1 (>= 3.25 required) |
| vcpkg (classic mode) | commit ``$vcpkgCommit`` |
| Inno Setup (only for the installer) | 6.7.3 |
| Windows PowerShell 5.1 (for ``fetch_tools.ps1``) | |

## 2. Libraries (vcpkg, triplet x64-windows)

| Port | Version | Port-version | Upstream |
|---|---|---|---|
$portLines

``````
git clone https://github.com/microsoft/vcpkg C:\vcpkg
git -C C:\vcpkg checkout $vcpkgCommit
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install $installList --triplet x64-windows
``````

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

## 5. Configure and build

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

## 6. Installer

``````
"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" /DBuildDir=..\build\bin\Release installer\zizai.iss
``````

(``installer\zizai.iss`` packages the user guides from ``docs\tutorial\`` and
the notices from ``docs\licenses\``, both in this archive. Output:
``installer\Output\自在投影-安裝程式-$Version.exe``, Traditional Chinese +
English.)

## 7. Replacing the LGPL libraries

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
    ':(exclude,glob)installer/Output/**'
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
$depZip = $null
if (-not $NoDeps) {
    $depZip = Join-Path $OutDir "ZizaiCast-$Version-deps-source.zip"
    if (Test-Path $depZip) { Remove-Item -Force $depZip }
    $dl = Join-Path $VcpkgRoot 'downloads'
    $readme = New-Object System.Collections.Generic.List[string]
    $readme.Add("Source code of the third-party libraries shipped in ZizaiCast $Version")
    $readme.Add("(GPL-3.0 s.6 Corresponding Source / LGPL-2.1 s.4 for libplist and FFmpeg).")
    $readme.Add("")
    $readme.Add("upstream/  unmodified upstream source archives, as downloaded by vcpkg")
    $readme.Add("ports/     vcpkg port scripts (portfile.cmake, patches) = how they were configured/patched/built")
    $readme.Add("vcpkg commit: $vcpkgCommit   (https://github.com/microsoft/vcpkg/tree/$vcpkgCommit/ports)")
    $readme.Add("triplet: x64-windows (DLLs, release); alac is a static library")
    $readme.Add("")
    $fs = [IO.File]::Open($depZip, 'CreateNew')
    $zip = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($pi in $PortInfo.Values) {
            $verKey = if ($pi.Name -eq 'alac') { ($pi.Version -split '-')[-1] } else { $pi.Version }
            $arc = Get-ChildItem $dl -File -Filter $pi.Archive -ErrorAction SilentlyContinue |
                   Where-Object { $_.Name -like "*$verKey*" -and $_.Name -notlike '*.part' } | Select-Object -First 1
            if (-not $arc) { throw "upstream archive for $($pi.Name) $($pi.Version) not found in $dl (pattern $($pi.Archive), run 'vcpkg install' first or use -NoDeps)" }
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $arc.FullName, "upstream/$($arc.Name)", [IO.Compression.CompressionLevel]::NoCompression)
            $portDir = Join-Path $VcpkgRoot "ports\$($pi.Name)"
            Get-ChildItem $portDir -Recurse -File | ForEach-Object {
                $rel = $_.FullName.Substring($portDir.Length + 1) -replace '\\', '/'
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, "ports/$($pi.Name)/$rel", [IO.Compression.CompressionLevel]::Optimal)
            }
            $readme.Add(('{0,-9} {1}#{2}  {3}' -f $pi.Name, $pi.Version, $pi.PortVersion, $pi.Url))
            $readme.Add(('          upstream/{0}  sha256 {1}' -f $arc.Name, (Get-Sha256 $arc.FullName)))
        }
        $readme.Add("")
        $readme.Add("Not included (separate programs shipped unmodified as binaries; source at upstream):")
        $readme.Add("  adb (Android SDK Platform-Tools r37.0.1): https://android.googlesource.com/platform/packages/modules/adb (tag platform-tools-37.0.1)")
        $readme.Add("  scrcpy-server v5.0: https://github.com/Genymobile/scrcpy/tree/v5.0")
        $e = $zip.CreateEntry('DEPS-README.txt')
        $w = New-Object IO.StreamWriter($e.Open(), $Utf8NoBom)
        try { $w.Write(($readme -join "`r`n") + "`r`n") } finally { $w.Dispose() }
    } catch { $zip.Dispose(); $fs.Dispose(); Remove-Item -Force $depZip -ErrorAction SilentlyContinue; throw }
    $zip.Dispose(); $fs.Dispose()
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
