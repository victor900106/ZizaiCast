# Off-screen tests of 傳到手機 with several captures in the real
# app (no phone, no LAN, no firewall prompt: --dev --test-offscreen
# --test-no-network, PM_SHARE_BIND=127.0.0.1, PM_VIDEO_OFFSCREEN=1). Writes
# PNGs / logs to %TEMP%\pmshots\batch_*. The dev settings.ini is restored after.
#   -Mode picker    3 screenshots + 1 recording → 傳到手機 → the picker in zh / en / ja / ko,
#                   untick one, 傳送 → QR page with the 3 others (curl list)
param([string]$Exe, [string]$Feed, [ValidateSet("picker")][string]$Mode = "picker")
$ErrorActionPreference = "Stop"
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class PmWinB {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr Find(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(128); GetClassName(h, sb, 128);
      if (p == pid && sb.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
"@
$shots = Join-Path $env:TEMP "pmshots"
New-Item -ItemType Directory -Force $shots | Out-Null
$devDir = Join-Path $env:LOCALAPPDATA "PhoneMirror-dev"
$ini = Join-Path $devDir "settings.ini"
$iniBak = Join-Path $env:TEMP "pm_batch_settings.ini.bak"
if (Test-Path $ini) { Copy-Item -Force $ini $iniBak }
$logFile = Join-Path $devDir "phonemirror.log"
$logStart = if (Test-Path $logFile) { (Get-Item $logFile).Length } else { 0 }

$env:PM_VIDEO_OFFSCREEN = "1"
$env:PM_SHARE_BIND = "127.0.0.1"
$argv = @("--dev", "--test-offscreen", "--test-no-network", "--test-feed", "`"$Feed`"")
$p = Start-Process -FilePath $Exe -ArgumentList $argv -PassThru
$msg = [PmWinB]::RegisterWindowMessage("PhoneMirror.DevCommand")
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $h = [PmWinB]::Find([uint32]$p.Id, "PhoneMirrorVideoWindow") }
if ($h -eq [IntPtr]::Zero) { Write-Output "no window"; $p.Kill(); exit 1 }
function Dev([int]$cmd, [int]$lp = 0, [int]$wait = 700) { [PmWinB]::PostMessage($h, $msg, [IntPtr]$cmd, [IntPtr]$lp) | Out-Null; Start-Sleep -Milliseconds $wait }
function Grab([string]$name) {
  foreach ($f in "share.png", "chip.png", "picker.png", "window.png", "share_url.txt", "menu_context.png", "menu_tray.png") {
    $src = Join-Path $shots $f; if (Test-Path $src) { Move-Item -Force $src (Join-Path $shots "batch_$($Mode)_$($name)_$f") } }
}
function AppLog { $fs = [IO.File]::Open($logFile, 'Open', 'Read', 'ReadWrite'); $fs.Seek($logStart, 'Begin') | Out-Null
  $r = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8); $t = $r.ReadToEnd(); $r.Close(); return $t }
Start-Sleep -Seconds 4   # the fake phone connects at 1 s, picture shown
Dev 139 0 1200           # 語言: 繁體中文

try {
  if ($Mode -eq "picker") {
    Dev 107 0 1100; Dev 107 0 1100           # 截圖 ×2
    Dev 112 0 2500; Dev 112 0 1500           # 開始錄影 … 停止錄影
    Dev 107 0 1100                           # 截圖
    Dev 903; Grab "1-chip"                   # the chip after the 4th capture
    Dev 900 0 900; Grab "2-menus"            # 傳到手機 (4 個未傳) in the menus
    Dev 160 0 3000                           # toolbar 傳到手機 → picker (4 unsent)
    foreach ($l in @(@(139, "zh"), @(140, "en"), @(150, "ja"), @(151, "ko"))) { Dev $l[0] 0 1200; Dev 903 0 900; Grab "3-picker-$($l[1])" }
    Dev 139 0 1200
    Dev 909 1 400                            # untick the 2nd (the recording)
    Dev 903 0 900; Grab "4-picker-unticked"
    Dev 908 0 2000                           # 傳送 3 個 → QR page
    Dev 903 0 900; Grab "5-shared"
    $url = Get-Content (Join-Path $shots "batch_picker_5-shared_share_url.txt") -Raw
    if ($url) { Write-Output ("cards on the page: " + ([regex]::Matches((curl.exe -s $url.Trim()), 'data-i="')).Count) }
    Dev 160 0 1500                           # 傳到手機 again: nothing unsent → the newest file, as before
    Dev 903 0 900; Grab "6-again"
  }
} finally {
  Write-Output "---- app log (share lines)"
  (AppLog) -split "`n" | Where-Object { $_ -match "share|picker|test push|dev window" } | Select-Object -Last 60
  Dev 104 0 2500                             # 結束
  if (-not $p.HasExited) { $p.Kill() }
  if (Test-Path $iniBak) { Copy-Item -Force $iniBak $ini; Remove-Item $iniBak }
}
