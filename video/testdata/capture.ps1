# Runs pm_video_test on a stream and grabs screenshots of the window's client
# area at given times (seconds after the window appears), scaled by -Scale.
# Example:
#   .\capture.ps1 -Exe ..\..\build-video\bin\Release\pm_video_test.exe -File portrait_rotate.h264 `
#                 -Times 1.4,3.4,8.4 -Names h264_portrait,h264_landscape,idle
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$File,
    [double[]]$Times = @(1.5),
    [string[]]$Names = @('shot'),
    [double]$Scale = 0.5,
    [string]$Log = '',
    [int[]]$ResizeTo = @(),        # client w,h applied right after the window appears
    [double]$DoubleClickAt = -1,    # seconds: post WM_LBUTTONDBLCLK (fullscreen toggle)
    [string]$ExtraArgs = '',        # appended to the test tool command line
    [string[]]$Mouse = @(),         # "t:x:y[:click]" client px: post WM_MOUSEMOVE (+ button down/up)
    [int[]]$At = @(100, 100)        # window position (screen px)
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
}
'@
[W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null  # per-monitor v2: real pixels everywhere
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = (Resolve-Path $Exe).Path
$psi.Arguments = '"' + (Resolve-Path $File).Path + '" ' + $ExtraArgs
$psi.UseShellExecute = $false
$psi.WorkingDirectory = $PSScriptRoot
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [System.Diagnostics.Process]::Start($psi)
$out = $p.StandardOutput.ReadToEndAsync()
$err = $p.StandardError.ReadToEndAsync()
while ($p.MainWindowHandle -eq 0 -and -not $p.HasExited) { Start-Sleep -Milliseconds 20; $p.Refresh() }
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$h = $p.MainWindowHandle
[W]::SetForegroundWindow($h) | Out-Null
# Keep the window above everything while capturing (HWND_TOPMOST, NOMOVE|NOSIZE).
[W]::SetWindowPos($h, [IntPtr](-1), $At[0], $At[1], 0, 0, 1) | Out-Null  # NOSIZE
if ($ResizeTo.Count -eq 2) {
    $wr = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$wr) | Out-Null
    $cr = New-Object W+RECT; [W]::GetClientRect($h, [ref]$cr) | Out-Null
    $fw = ($wr.R - $wr.L) - $cr.R; $fh = ($wr.B - $wr.T) - $cr.B
    [W]::SetWindowPos($h, [IntPtr](-1), $At[0], $At[1], $ResizeTo[0] + $fw, $ResizeTo[1] + $fh, 0) | Out-Null
}
$clicked = $false
$mi = 0
for ($i = 0; $i -lt $Times.Count; $i++) {
    while ($sw.Elapsed.TotalSeconds -lt $Times[$i]) {
        if (-not $clicked -and $DoubleClickAt -ge 0 -and $sw.Elapsed.TotalSeconds -ge $DoubleClickAt) {
            [W]::PostMessage($h, 0x0203, [IntPtr]1, [IntPtr]0) | Out-Null; $clicked = $true
        }
        while ($mi -lt $Mouse.Count) {
            $m = $Mouse[$mi].Split(':')
            if ($sw.Elapsed.TotalSeconds -lt [double]$m[0]) { break }
            $lp = [IntPtr](([int]$m[2] -shl 16) -bor ([int]$m[1] -band 0xffff))
            [W]::PostMessage($h, 0x0200, [IntPtr]0, $lp) | Out-Null           # WM_MOUSEMOVE
            if ($m.Count -gt 3 -and $m[3] -eq 'click') {
                [W]::PostMessage($h, 0x0201, [IntPtr]1, $lp) | Out-Null       # WM_LBUTTONDOWN
                [W]::PostMessage($h, 0x0202, [IntPtr]0, $lp) | Out-Null       # WM_LBUTTONUP
            }
            $mi++
        }
        Start-Sleep -Milliseconds 10
    }
    $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$pt) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap $r.R, $r.B
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
    $sw2 = [int]($r.R * $Scale); $sh2 = [int]($r.B * $Scale)
    $small = New-Object System.Drawing.Bitmap $bmp, $sw2, $sh2
    $path = Join-Path $PSScriptRoot ($Names[$i] + '.png')
    $small.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose(); $small.Dispose()
    Write-Host ("{0:N2}s -> {1} ({2}x{3} client)" -f $sw.Elapsed.TotalSeconds, $path, $r.R, $r.B)
}
$p.WaitForExit()
$text = $out.Result + $err.Result
Write-Host $text
if ($Log) { Set-Content -Path (Join-Path $PSScriptRoot $Log) -Value $text -Encoding utf8 }
