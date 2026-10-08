# Off-screen test of 翻譯 (PaddleOCR + Bergamot) in the real app: a fake
# iPhone mirrors a photographed label (--test-feed, an H.264 loop of
# translate/testdata/make_photos.py output), DevCommand 192 (翻譯整個畫面),
# window shots.  No phone, no network listener, no firewall prompt, nothing on
# screen (--dev --test-offscreen --test-no-network, PM_VIDEO_OFFSCREEN=1).
#   -Models DIR   PM_MODELS_DIR with the OCR + translation models (the run
#                 never downloads: the consent dialog is answered by -Consent)
#   -Consent 0|1  answer to a download question (0 = 不要: Windows OCR fallback)
#   -Lang  zh|en|ja|ko   UI language (DevCommand 139 / 140 / 150 / 151)
# Shots: %TEMP%\pmshots\tr_<feed>_<lang>_*.png; log lines: translate ….
#   powershell -File translate/tools/app_translate_test.ps1 -Exe build-app\bin\Release\自在投影.exe -Feed x.h264 -Models build-translate\models
param([string]$ExeDir, [string]$Feed, [string]$Models, [int]$Consent = 1, [string]$Lang = "zh")
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\..\launch\tools\cap.ps1")
# The app exe (自在投影.exe) found by exclusion: no CJK text on the command line.
$script:Exe = (Get-ChildItem $ExeDir -Filter *.exe | Where-Object { $_.Name -notlike "pm_*" } | Select-Object -First 1).FullName
Write-Host "app: $script:Exe"
$shots = Join-Path $env:TEMP "pmshots"
New-Item -ItemType Directory -Force $shots | Out-Null
$devDir = Join-Path $env:LOCALAPPDATA "PhoneMirror-dev"
$ini = Join-Path $devDir "settings.ini"
$iniBak = Join-Path $env:TEMP "pm_tr_settings.ini.bak"
if (Test-Path $ini) { Copy-Item -Force $ini $iniBak }
$logFile = Join-Path $devDir "phonemirror.log"
$logStart = if (Test-Path $logFile) { @(Get-Content $logFile -Encoding UTF8).Count } else { 0 }
function New-LogLines { if (Test-Path $logFile) { @(Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $script:logStart) } else { @() } }
$env:PM_VIDEO_OFFSCREEN = "1"
$env:PM_MODELS_DIR = (Resolve-Path $Models).Path
$name = [IO.Path]::GetFileNameWithoutExtension($Feed) + "_" + $Lang
$asked = $false
try {
    $app = Start-App "--test-no-network --test-feed `"$((Resolve-Path $Feed).Path)`""
    Start-Sleep -Seconds 3
    $langCmd = @{ zh = 139; en = 140; ja = 150; ko = 151 }[$Lang]
    Send-Cmd $app $langCmd
    Start-Sleep -Milliseconds 800
    Save-Shot $app (Join-Path $shots "tr_${name}_1-live.png")
    Send-Cmd $app 192   # 翻譯整個畫面
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        $new = (New-LogLines) -join "`n"
        if ($new -match "consent asked" -and -not $asked) {
            $asked = $true
            [Cap]::PostMessage($app.H, $script:DevMsg, [IntPtr]905, [IntPtr]0) | Out-Null   # ask.png
            Start-Sleep -Milliseconds 600
            Copy-Item -Force (Join-Path $shots "ask.png") (Join-Path $shots "tr_${name}_2-consent.png") -ErrorAction SilentlyContinue
            [Cap]::PostMessage($app.H, $script:DevMsg, [IntPtr]906, [IntPtr]$Consent) | Out-Null
        }
        if ($new -match "translate (done|ended)") { break }
    }
    Start-Sleep -Milliseconds 600
    Save-Shot $app (Join-Path $shots "tr_${name}_3-translated.png")
    Send-Cmd $app 194   # 顯示原文
    Start-Sleep -Milliseconds 500
    Save-Shot $app (Join-Path $shots "tr_${name}_4-original.png")
    Send-Cmd $app 196   # 關閉翻譯
    Start-Sleep -Milliseconds 500
    Stop-App $app
} finally {
    if (Test-Path $iniBak) { Copy-Item -Force $iniBak $ini; Remove-Item $iniBak }
}
New-LogLines | Where-Object { $_ -match "translate|consent|OCR|ocr|title" } | Select-Object -Last 15
Write-Host "shots: $shots\tr_${name}_*.png"
