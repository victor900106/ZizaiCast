# Posts a mouse stroke (WM_MOUSEMOVE / LBUTTON) to the --dev window of <ProcessId> only; the real pointer never moves.
param([int]$ProcessId, [double]$X0 = 0.30, [double]$Y0 = 0.30, [double]$X1 = 0.75, [double]$Y1 = 0.36)
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class PmSt {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr Find(uint pid) { IntPtr f = IntPtr.Zero; EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(128); GetClassName(h, sb, 128);
      if (p == pid && sb.ToString() == "PhoneMirrorVideoWindow") { f = h; return false; } return true; }, IntPtr.Zero); return f; }
  public static IntPtr XY(int x, int y) { return (IntPtr)((y << 16) | (x & 0xFFFF)); }
}
"@
$h = [PmSt]::Find([uint32]$ProcessId); if ($h -eq [IntPtr]::Zero) { "no window"; exit 1 }
$r = New-Object PmSt+RECT; [void][PmSt]::GetClientRect($h, [ref]$r)
$ax = [int]($r.R * $X0); $ay = [int]($r.B * $Y0); $bx = [int]($r.R * $X1); $by = [int]($r.B * $Y1)
[void][PmSt]::PostMessage($h, 0x200, [IntPtr]0, [PmSt]::XY($ax, $ay)); Start-Sleep -Milliseconds 60
[void][PmSt]::PostMessage($h, 0x201, [IntPtr]1, [PmSt]::XY($ax, $ay)); Start-Sleep -Milliseconds 60
for ($k = 1; $k -le 16; $k++) { $x = [int]($ax + ($bx - $ax) * $k / 16); $y = [int]($ay + ($by - $ay) * $k / 16 + [Math]::Sin($k / 16 * 3.14) * 12)
  [void][PmSt]::PostMessage($h, 0x200, [IntPtr]1, [PmSt]::XY($x, $y)); Start-Sleep -Milliseconds 25 }
[void][PmSt]::PostMessage($h, 0x202, [IntPtr]0, [PmSt]::XY($bx, $by)); "stroke $ax,$ay -> $bx,$by"
