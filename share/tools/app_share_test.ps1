# Off-screen end-to-end test of 傳到手機 in the real app (no phone, no LAN, no
# firewall prompt): --dev --test-offscreen --test-no-network, PM_SHARE_BIND=127.0.0.1,
# PM_VIDEO_OFFSCREEN=1. Drives it with PhoneMirror.DevCommand and writes PNGs /
# logs to %TEMP%\pmshots\<tag>_*. Usage:
#   powershell -File share\tools\app_share_test.ps1 -Exe build-app-share\bin\Release\自在投影.exe -Feed x.h264 [-Source android] [-Tag zh]
param([string]$Exe, [string]$Feed, [string]$Source = "airplay", [string]$Tag = "run", [string]$Before = "")
$ErrorActionPreference = "Stop"
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class PmWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr Find(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(128); GetClassName(h, sb, 128);
      if (p == pid && sb.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static string Classes(uint pid) {
    var all = new StringBuilder();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) { var sb = new StringBuilder(128); GetClassName(h, sb, 128); all.Append(sb.ToString()).Append(IsWindowVisible(h) ? "(v) " : " "); } return true; }, IntPtr.Zero);
    return all.ToString();
  }
}
"@
$shots = Join-Path $env:TEMP "pmshots"
New-Item -ItemType Directory -Force $shots | Out-Null
$env:PM_VIDEO_OFFSCREEN = "1"
$env:PM_SHARE_BIND = "127.0.0.1"
$args = @("--dev", "--test-offscreen", "--test-no-network", "--test-feed", "`"$Feed`"")
if ($Source -ne "airplay") { $args += @("--test-source", $Source) }
$p = Start-Process -FilePath $Exe -ArgumentList $args -PassThru
$msg = [PmWin]::RegisterWindowMessage("PhoneMirror.DevCommand")
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $h = [PmWin]::Find([uint32]$p.Id, "PhoneMirrorVideoWindow") }
if ($h -eq [IntPtr]::Zero) { Write-Output ("no window; classes: " + [PmWin]::Classes([uint32]$p.Id)); $p.Kill(); exit 1 }
function Dev([int]$cmd, [int]$lp = 0, [int]$wait = 700) { [PmWin]::PostMessage($h, $msg, [IntPtr]$cmd, [IntPtr]$lp) | Out-Null; Start-Sleep -Milliseconds $wait }
function Grab([string]$name) { foreach ($f in "share.png", "chip.png", "share_url.txt", "menu_context.png", "menu_tray.png") { $src = Join-Path $shots $f; if (Test-Path $src) { Move-Item -Force $src (Join-Path $shots "$($Tag)_$($name)_$f") } } }
Start-Sleep -Seconds 4               # fake phone connects at 1 s, picture shown
foreach ($c in ($Before -split "," | ? { $_ })) { Dev ([int]$c) 0 900 }
Dev 107 0 1200                       # 截圖 (Ctrl+S) → toast + 傳到手機 chip
Dev 903                              # chip.png (+ share.png if a panel is open)
Write-Output ("windows after snapshot: " + [PmWin]::Classes([uint32]$p.Id))
Grab "1-after-shot"
Dev 900 0 900                        # menus (share items)
Grab "2-menus"
Dev 160 0 1500                       # toolbar 傳到手機 (newest file)
Dev 903
Write-Output ("windows after share: " + [PmWin]::Classes([uint32]$p.Id))
Grab "3-shared"
Write-Output "PID $($p.Id)"
