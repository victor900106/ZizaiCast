# Read-only: samples a process's per-thread CPU every $Every seconds for $Minutes
# minutes; when the process uses more than $Threshold % of one core in a window,
# records the hottest threads with their start module (+offset). Touches nothing.
param([int]$ProcessId, [int]$Minutes = 6, [int]$Every = 15, [double]$Threshold = 3, [string]$Out)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class TI {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(int a, bool i, int id);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("ntdll.dll")] public static extern int NtQueryInformationThread(IntPtr h, int c, out IntPtr info, int len, IntPtr ret);
  public static long Start(int tid) { IntPtr h = OpenThread(0x0040, false, tid); if (h == IntPtr.Zero) return 0; IntPtr a; NtQueryInformationThread(h, 9, out a, IntPtr.Size, IntPtr.Zero); CloseHandle(h); return a.ToInt64(); }
}
"@
$p = Get-Process -Id $ProcessId
$mods = $p.Modules | Select-Object ModuleName, @{n='b';e={$_.BaseAddress.ToInt64()}}, @{n='s';e={$_.ModuleMemorySize}}
$lines = @()
$end = (Get-Date).AddMinutes($Minutes)
while ((Get-Date) -lt $end) {
  $p.Refresh()
  $a = @{}; foreach ($t in $p.Threads) { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds }
  $t0 = Get-Date; Start-Sleep $Every; $p.Refresh(); $wall = ((Get-Date) - $t0).TotalMilliseconds
  $rows = foreach ($t in $p.Threads) {
    $prev = if ($a.ContainsKey($t.Id)) { $a[$t.Id] } else { 0 }
    $d = $t.TotalProcessorTime.TotalMilliseconds - $prev
    if ($d -gt 0) { [pscustomobject]@{tid = $t.Id; pct = $d / $wall * 100} }
  }
  $total = ($rows | Measure-Object -Property pct -Sum).Sum
  $line = "{0:HH:mm:ss} total {1:N2}% of one core" -f (Get-Date), $total
  if ($total -gt $Threshold) {
    $hot = $rows | Sort-Object pct -Descending | Select-Object -First 5 | ForEach-Object {
      $s = [TI]::Start($_.tid); $m = $mods | Where-Object { $s -ge $_.b -and $s -lt ($_.b + $_.s) } | Select-Object -First 1
      "{0:N2}% {1}" -f $_.pct, $(if ($m) { "{0}+0x{1:X}" -f $m.ModuleName, ($s - $m.b) } else { "0x{0:X}" -f $s })
    }
    $line += " | " + ($hot -join "; ")
  }
  $lines += $line
  Set-Content -Encoding utf8 -Path $Out -Value $lines
}
