# Posts PhoneMirror.DevCommand <Cmd> [<Lp>] to the --dev app window of process <ProcessId>.
#   powershell -File share\tools\dev_cmd.ps1 -ProcessId 1234 -Cmd 903,164
param([int]$ProcessId, [string]$Cmd, [int]$Lp = 0, [int]$Wait = 800)
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class PmDev {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr Find(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(128); GetClassName(h, sb, 128);
      if (p == pid && sb.ToString() == "PhoneMirrorVideoWindow") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
"@
$h = [PmDev]::Find([uint32]$ProcessId)
if ($h -eq [IntPtr]::Zero) { Write-Output "no window"; exit 1 }
$msg = [PmDev]::RegisterWindowMessage("PhoneMirror.DevCommand")
foreach ($c in ($Cmd -split ",")) { [PmDev]::PostMessage($h, $msg, [IntPtr][int]$c, [IntPtr]$Lp) | Out-Null; Start-Sleep -Milliseconds $Wait }
