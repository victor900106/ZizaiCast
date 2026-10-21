# Lists the windows of process(es) -ProcessId that Alt+Tab / the taskbar would show: visible top-level,
# not WS_EX_TOOLWINDOW, no owner (or WS_EX_APPWINDOW), not cloaked. Prints "alttab <pid> <count>" (+ one
# line per such window) and whether any visible window of the process is on a monitor.
#   powershell -File tools\alttab_check.ps1 -ProcessId 1234[,5678]
param([string]$ProcessId)
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic; using System.Text;
public static class PmAltTab {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
  [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int n);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public static List<string> Of(uint pid, out int alt, out int vis, List<int[]> rects) {
    var l = new List<string>(); int a = 0, v = 0;
    EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p != pid || !IsWindowVisible(h)) return true;
      v++; RECT r; GetWindowRect(h, out r); rects.Add(new int[] { r.L, r.T, r.R, r.B });
      long ex = GetWindowLongPtr(h, -20).ToInt64(); bool tool = (ex & 0x80) != 0, app = (ex & 0x40000) != 0;
      bool owned = GetWindow(h, 4) != IntPtr.Zero; int cloak = 0; DwmGetWindowAttribute(h, 14, out cloak, 4);
      if (!tool && (!owned || app) && cloak == 0) { a++; var sb = new StringBuilder(128); GetClassName(h, sb, 128); l.Add(sb.ToString() + " @" + r.L + "," + r.T); }
      return true; }, IntPtr.Zero);
    alt = a; vis = v; return l;
  }
}
"@
foreach ($p in ($ProcessId -split ",")) {
  $alt = 0; $vis = 0; $rects = New-Object 'System.Collections.Generic.List[int[]]'
  $list = [PmAltTab]::Of([uint32]$p, [ref]$alt, [ref]$vis, $rects)
  $on = 0
  foreach ($r in $rects) { foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { $b = $s.Bounds; if ($r[0] -lt $b.Right -and $r[2] -gt $b.Left -and $r[1] -lt $b.Bottom -and $r[3] -gt $b.Top) { $on++ } } }
  Write-Output "alttab $p $alt visible $vis onscreen $on"
  foreach ($w in $list) { Write-Output "  alttab-window $w" }
}
