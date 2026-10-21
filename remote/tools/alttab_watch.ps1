# 0.8.3.4 tests: while a test runs, count the windows of ITS OWN processes that Alt+Tab / the taskbar
# would show (visible, not cloaked, not WS_EX_TOOLWINDOW, unowned or WS_EX_APPWINDOW). Read-only: it never
# touches, moves, activates or focuses a window.
#   powershell -NoProfile -File alttab_watch.ps1 -Pids "123,456" -Out <file> [-IntervalMs 400] [-MaxSec 600]
# The test adds PIDs by appending lines to <Out>.pids; stops it by creating <Out>.stop. Every sample is
# one line in <Out>: "<ms> <count> [<pid>:<title> | ...]"; the last line is "max <n>".
param([string]$Pids = "", [string]$Out, [int]$IntervalMs = 400, [int]$MaxSec = 600)
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class PmAltTab {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int n);
  public static List<string> Of(HashSet<uint> pids) {
    var l = new List<string>();
    EnumWindows((h, x) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (!pids.Contains(p) || !IsWindowVisible(h)) return true;
      long ex = GetWindowLongPtr(h, -20).ToInt64();
      bool tool = (ex & 0x80) != 0, app = (ex & 0x40000) != 0;
      int cloaked = 0; DwmGetWindowAttribute(h, 14, out cloaked, 4);
      bool owned = GetWindow(h, 4) != IntPtr.Zero;
      if (cloaked != 0 || (tool && !app) || (owned && !app)) return true;
      var sb = new StringBuilder(128); GetWindowTextW(h, sb, 128);
      l.Add(p + ":" + sb.ToString());
      return true; }, IntPtr.Zero);
    return l;
  }
}
"@
$roots = New-Object System.Collections.Generic.List[uint32]
foreach ($x in ($Pids -split ',')) { if ($x.Trim()) { $roots.Add([uint32]$x.Trim()) } }
$t0 = Get-Date; $max = 0
Set-Content -Path $Out -Value "" -Encoding utf8
while (((Get-Date) - $t0).TotalSeconds -lt $MaxSec -and -not (Test-Path "$Out.stop")) {
  if (Test-Path "$Out.pids") { foreach ($x in (Get-Content "$Out.pids")) { if ($x.Trim() -and -not $roots.Contains([uint32]$x.Trim())) { $roots.Add([uint32]$x.Trim()) } } }
  # the roots and all their descendants
  $all = Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId
  $set = New-Object 'System.Collections.Generic.HashSet[uint32]'
  foreach ($r in $roots) { [void]$set.Add($r) }
  do { $n = $set.Count; foreach ($p in $all) { if ($set.Contains([uint32]$p.ParentProcessId)) { [void]$set.Add([uint32]$p.ProcessId) } } } while ($set.Count -ne $n)
  $w = [PmAltTab]::Of($set)
  if ($w.Count -gt $max) { $max = $w.Count }
  Add-Content -Path $Out -Value ("{0} {1} {2}" -f [int]((Get-Date) - $t0).TotalMilliseconds, $w.Count, ($w -join ' | ')) -Encoding utf8
  Start-Sleep -Milliseconds $IntervalMs
}
Add-Content -Path $Out -Value "max $max" -Encoding utf8
