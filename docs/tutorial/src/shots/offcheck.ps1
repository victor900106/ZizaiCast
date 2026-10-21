# Prints every visible top-level window of <ProcessId> and whether it overlaps any monitor.
# Exit 2 if one does (the caller then kills its own PID).
param([int]$ProcessId)
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public static class PmOff {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  public static List<string> List(uint pid) {
    var o = new List<string>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) { RECT r; GetWindowRect(h, out r); var sb = new StringBuilder(128); GetClassName(h, sb, 128);
        o.Add(sb.ToString() + "|" + r.L + "|" + r.T + "|" + r.R + "|" + r.B); } return true; }, IntPtr.Zero);
    return o;
  }
}
"@
Add-Type -AssemblyName System.Windows.Forms
$screens = [System.Windows.Forms.Screen]::AllScreens | ForEach-Object { $_.Bounds }
$bad = 0
foreach ($s in [PmOff]::List([uint32]$ProcessId)) {
  $f = $s -split '\|'; $L = [int]$f[1]; $T = [int]$f[2]; $R = [int]$f[3]; $B = [int]$f[4]
  $on = $false
  foreach ($b in $screens) { if ($L -lt $b.Right -and $R -gt $b.Left -and $T -lt $b.Bottom -and $B -gt $b.Top) { $on = $true } }
  if ($on) { $bad = 1 }
  "$($f[0]) [$L,$T,$R,$B] " + ($(if ($on) { 'ON-SCREEN' } else { 'off' }))
}
if ($bad) { exit 2 }
