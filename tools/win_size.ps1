# Sets the client area of the --dev app's (offscreen) video window of process <ProcessId> to
# <W> x <H> pixels without moving it (it stays where --test-offscreen put it, off every monitor),
# then prints the window rectangle and how many monitors it touches ("L T R B on").
#   powershell -File tools\win_size.ps1 -ProcessId 1234 -W 765 -H 1140
param([int]$ProcessId, [int]$W, [int]$H)
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class PmSize {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  public static IntPtr Find(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(128); GetClassName(h, sb, 128);
      if (p == pid && sb.ToString() == "PhoneMirrorVideoWindow") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
"@
$h = [PmSize]::Find([uint32]$ProcessId)
if ($h -eq [IntPtr]::Zero) { Write-Output "no window"; exit 1 }
$wr = New-Object PmSize+RECT; $cr = New-Object PmSize+RECT
[PmSize]::GetWindowRect($h, [ref]$wr) | Out-Null
[PmSize]::GetClientRect($h, [ref]$cr) | Out-Null
$fw = ($wr.R - $wr.L) - $cr.R; $fh = ($wr.B - $wr.T) - $cr.B
# SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE
[PmSize]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, $W + $fw, $H + $fh, 0x0002 -bor 0x0004 -bor 0x0010) | Out-Null
[PmSize]::GetWindowRect($h, [ref]$wr) | Out-Null
$on = 0
foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { $b = $s.Bounds; if ($wr.L -lt $b.Right -and $wr.R -gt $b.Left -and $wr.T -lt $b.Bottom -and $wr.B -gt $b.Top) { $on++ } }
Write-Output "$($wr.L) $($wr.T) $($wr.R) $($wr.B) $on"
