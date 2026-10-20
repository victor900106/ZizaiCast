# Downloads cloudflared (Cloudflare Tunnel client, Apache-2.0) for 免安裝外網
# (remote control's no-app link, 0.8.1) and checks it against a pinned SHA-256.
# Not committed to git (55 MB); the build copies it next to the app as
# <bin>\cloudflared\ and the installer ships it as 程式\cloudflared\.
#
#   cloudflared/cloudflared.exe  official release asset cloudflared-windows-amd64.exe
#                                (signed by Cloudflare, Inc.), unmodified
#   cloudflared/LICENSE          Apache License 2.0, from the same tag
#
# Usage:  powershell -ExecutionPolicy Bypass -File fetch_cloudflared.ps1 [-Dest <dir>]
# Idempotent: a file already present with the right hash is kept.
param(
    [string]$Dest = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'

$Version = '2026.7.2'
$ExeUrl = "https://github.com/cloudflare/cloudflared/releases/download/$Version/cloudflared-windows-amd64.exe"
$ExeSha256 = 'cdb5d4432f6ae1595654a692a51308b69d2bf7af961f5578d9391837cf072df9'
$LicenseUrl = "https://raw.githubusercontent.com/cloudflare/cloudflared/$Version/LICENSE"

$dir = Join-Path $Dest 'cloudflared'
New-Item -ItemType Directory -Force $dir | Out-Null
$exe = Join-Path $dir 'cloudflared.exe'
if ((Test-Path $exe) -and ((Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLowerInvariant() -eq $ExeSha256)) {
    Write-Host "cloudflared $Version already present"
} else {
    $tmp = "$exe.download"
    Write-Host "Downloading $ExeUrl"
    Invoke-WebRequest -UseBasicParsing -Uri $ExeUrl -OutFile $tmp
    $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $tmp).Hash.ToLowerInvariant()
    if ($h -ne $ExeSha256) { Remove-Item $tmp; throw "cloudflared SHA-256 mismatch: $h" }
    Move-Item -Force $tmp $exe
    Write-Host "cloudflared $Version OK ($ExeSha256)"
}
$lic = Join-Path $dir 'LICENSE'
if (-not (Test-Path $lic)) { Invoke-WebRequest -UseBasicParsing -Uri $LicenseUrl -OutFile $lic }
Set-Content -Encoding ascii -Path (Join-Path $dir 'VERSION.txt') -Value "cloudflared $Version (cloudflared-windows-amd64.exe, SHA-256 $ExeSha256)`r`nSource: https://github.com/cloudflare/cloudflared/tree/$Version"
