# Idle wake-ups of the app (0.8.3): starts its own offscreen --dev instance (nothing shown, no
# phone, no sound, no input, remote control off), waits for it to settle, then reads the
# cumulative per-thread context-switch counter (Win32_PerfRawData_PerfProc_Thread) of THIS
# process only (by PID) twice, N seconds apart. A UI thread that sleeps in GetMessage
# wakes only for its timers, so context switches per second ~ wake-ups per second.
#   powershell -File tools\idle_wakeups.ps1 -Exe <path to .exe> [-Seconds 60] [-Tag before]
param([string]$Exe, [int]$Seconds = 60, [string]$Tag = 'run', [int]$Settle = 12)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class WinThreads {
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static int[] Of(int pid) {
    var set = new HashSet<int>();
    EnumWindows((h, l) => { uint p; uint t = GetWindowThreadProcessId(h, out p); if (p == pid) set.Add((int)t); return true; }, IntPtr.Zero);
    var a = new int[set.Count]; set.CopyTo(a); return a;
  }
}
"@
$dir = Join-Path $env:LOCALAPPDATA 'PhoneMirror-dev-shellfix'
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item (Join-Path $dir 'phonemirror*.log') -ErrorAction SilentlyContinue
Set-Content (Join-Path $dir 'settings.ini') "[settings]`nlanguage=en`nremote_port=$(18800 + (Get-Random -Max 100))`ntutorial_shown=1`ntray_hint_shown=1`nclose_hint_shown=1`ntoolbar_hint_shown=1`nwhats_new_seen=0.8.3`n" -Encoding ascii
$env:PM_DEV_INSTANCE = 'shellfix'; $env:PM_REMOTE_DRYRUN = '1'; $env:PM_VIDEO_OFFSCREEN = '1'
$p = Start-Process -FilePath $Exe -ArgumentList '--dev', '--test-offscreen' -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Seconds $Settle
    function Snap {
        $t = Get-CimInstance Win32_PerfRawData_PerfProc_Thread | Where-Object { $_.IDProcess -eq $p.Id }
        $sum = ($t | Measure-Object -Property ContextSwitchesPersec -Sum).Sum
        $top = $t | Sort-Object ContextSwitchesPersec -Descending | Select-Object -First 1
        $map = @{}; foreach ($x in $t) { $map[[int]$x.IDThread] = [double]$x.ContextSwitchesPersec }
        [pscustomobject]@{ Sum = [double]$sum; Threads = @($t).Count; Cpu = $p.TotalProcessorTime.TotalMilliseconds; Map = $map }
    }
    $p.Refresh(); $a = Snap; $t0 = Get-Date
    Start-Sleep -Seconds $Seconds
    $p.Refresh(); $b = Snap; $wall = ((Get-Date) - $t0).TotalSeconds
    $perSec = ($b.Sum - $a.Sum) / $wall
    '{0}: {1:N1} context switches/s over {2:N0} s ({3} threads), process CPU {4:N2} ms/s' -f $Tag, $perSec, $wall, $b.Threads, (($b.Cpu - $a.Cpu) / $wall)
    # The UI thread(s) = the threads owning this process's windows: they wake only for timers / messages.
    $ui = [WinThreads]::Of($p.Id) | Where-Object { $b.Map.ContainsKey($_) }
    $u = 0.0; foreach ($tid in $ui) { $u += ($b.Map[$tid] - $a.Map[$tid]) / $wall }
    '{0}: UI thread(s) {1}: {2:N2} wake-ups/s' -f $Tag, ($ui -join ','), $u
    $b.Map.Keys | ForEach-Object { [pscustomobject]@{ Tid = $_; PerSec = ($b.Map[$_] - $a.Map[$_]) / $wall } } | Sort-Object PerSec -Descending | Select-Object -First 5 | ForEach-Object { '{0}:   thread {1}: {2:N2}/s' -f $Tag, $_.Tid, $_.PerSec }
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(5000) | Out-Null }
    # the app's private adb server (our own child) is stopped too: only the adb.exe beside this .exe
    $adb = Join-Path (Split-Path $Exe) 'android-tools\adb.exe'
    Get-Process adb -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $adb } | Stop-Process -Force
}




