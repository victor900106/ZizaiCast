# Downloads the two third-party binaries the Android source needs (not
# committed to git) and verifies them against pinned SHA-256 hashes:
#
#   platform-tools/  adb.exe, AdbWinApi.dll, AdbWinUsbApi.dll, NOTICE.txt,
#                    source.properties  -- Google Android SDK Platform-Tools
#                    r37.0.1 (official zip from dl.google.com; adb itself is
#                    AOSP code under Apache-2.0, see NOTICE.txt; the zip's
#                    SHA-1 is the one published in Google's repository2-3.xml)
#   scrcpy/scrcpy-server  -- scrcpy-server v5.0 (Genymobile, Apache-2.0),
#                    from the GitHub release; SHA-256 = release SHA256SUMS.txt
#
# Usage:  powershell -ExecutionPolicy Bypass -File fetch_tools.ps1 [-Dest <dir>]
# Idempotent: files already present with the right hash are kept.
param(
    [string]$Dest = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'

$PlatformToolsVersion = '37.0.1'
$PlatformToolsUrl = "https://dl.google.com/android/repository/platform-tools_r$PlatformToolsVersion-win.zip"
$PlatformToolsSha256 = '45f4d63113e895ebde0c90f194099a4676b6ac653bd28d54314a9e022bbc1a99'
$PlatformToolsFiles = @('adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'NOTICE.txt', 'source.properties')

$ScrcpyVersion = '5.0'
$ScrcpyUrl = "https://github.com/Genymobile/scrcpy/releases/download/v$ScrcpyVersion/scrcpy-server-v$ScrcpyVersion"
$ScrcpySha256 = '26cbc9ad0aced6c2282455bef4fb43462605c1f8758c74b4ab1dbf818c229daa'
$ScrcpyLicenseUrl = "https://raw.githubusercontent.com/Genymobile/scrcpy/v$ScrcpyVersion/LICENSE"

function Get-Sha256([string]$path) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
}

# ---- platform-tools ----
$ptDir = Join-Path $Dest 'platform-tools'
$stamp = Join-Path $ptDir 'zip.sha256'
$haveAll = (Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $PlatformToolsSha256)
foreach ($f in $PlatformToolsFiles) { if (-not (Test-Path (Join-Path $ptDir $f))) { $haveAll = $false } }
if ($haveAll) {
    Write-Host "platform-tools r$PlatformToolsVersion already present"
} else {
    New-Item -ItemType Directory -Force $ptDir | Out-Null
    $zip = Join-Path ([IO.Path]::GetTempPath()) "pm-platform-tools-$PlatformToolsVersion.zip"
    Write-Host "Downloading $PlatformToolsUrl"
    Invoke-WebRequest -UseBasicParsing -Uri $PlatformToolsUrl -OutFile $zip
    $h = Get-Sha256 $zip
    if ($h -ne $PlatformToolsSha256) { Remove-Item $zip; throw "platform-tools SHA-256 mismatch: $h" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $za = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        foreach ($f in $PlatformToolsFiles) {
            $e = $za.GetEntry("platform-tools/$f")
            if (-not $e) { throw "missing platform-tools/$f in zip" }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $ptDir $f), $true)
        }
    } finally { $za.Dispose() }
    Remove-Item $zip
    Set-Content -Encoding ascii -Path $stamp -Value $PlatformToolsSha256
    Write-Host "platform-tools r$PlatformToolsVersion OK ($PlatformToolsSha256)"
}

# ---- scrcpy-server ----
$scDir = Join-Path $Dest 'scrcpy'
$server = Join-Path $scDir 'scrcpy-server'
if ((Test-Path $server) -and ((Get-Sha256 $server) -eq $ScrcpySha256)) {
    Write-Host "scrcpy-server v$ScrcpyVersion already present"
} else {
    New-Item -ItemType Directory -Force $scDir | Out-Null
    Write-Host "Downloading $ScrcpyUrl"
    Invoke-WebRequest -UseBasicParsing -Uri $ScrcpyUrl -OutFile "$server.tmp"
    $h = Get-Sha256 "$server.tmp"
    if ($h -ne $ScrcpySha256) { Remove-Item "$server.tmp"; throw "scrcpy-server SHA-256 mismatch: $h" }
    Move-Item -Force "$server.tmp" $server
    Invoke-WebRequest -UseBasicParsing -Uri $ScrcpyLicenseUrl -OutFile (Join-Path $scDir 'LICENSE')
    Set-Content -Encoding ascii -Path (Join-Path $scDir 'VERSION') -Value $ScrcpyVersion
    Write-Host "scrcpy-server v$ScrcpyVersion OK ($ScrcpySha256)"
}
