# Generates Annex-B test streams for pm_video_test (outputs are gitignored).
#   portrait_rotate.h264 : 1170x2532 portrait (2 s) -> 2532x1170 landscape (2 s)
#                          -> portrait again (2 s), 60 fps, High profile, no B-frames.
#   portrait.h265        : 1170x2532 portrait HEVC Main, 60 fps, 3 s, no B-frames.
#   land1440.h264/.h265  : 2560x1440, 60 fps, 4 s (frame-tap cost at 1440p60).
#   portrait10.h265      : 1170x2532 HEVC Main10 (10-bit), 60 fps, 2 s.
# All segments carry in-band SPS/PPS(/VPS) before each IDR, BT.709 limited range,
# exactly what an iPhone mirroring stream looks like to the VideoSink.
# Usage: powershell -ExecutionPolicy Bypass -File video\testdata\make.ps1
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

function Seg([string]$size, [int]$secs, [string]$out, [string]$codec, [string]$pix = 'yuv420p') {
    $vf = "scale=out_color_matrix=bt709:out_range=tv,format=$pix," +
          "drawtext=fontfile='C\:/Windows/Fonts/arial.ttf':text='$size %{frame_num}':fontsize=96:fontcolor=white:box=1:boxcolor=black@0.6:x=40:y=40"
    $common = @('-hide_banner', '-loglevel', 'error', '-y',
                '-f', 'lavfi', '-i', "testsrc2=size=${size}:rate=60:duration=$secs",
                '-vf', $vf, '-colorspace', 'bt709', '-color_primaries', 'bt709',
                '-color_trc', 'bt709', '-color_range', 'tv')
    if ($codec -eq 'h264') {
        & ffmpeg @common -c:v libx264 -profile:v high -preset veryfast -tune zerolatency `
            -bf 0 -g 120 -x264-params 'repeat-headers=1' -bsf:v h264_mp4toannexb -f h264 $out
    } else {
        $prof = if ($pix -eq 'yuv420p') { 'main' } else { 'main10' }
        & ffmpeg @common -c:v libx265 -profile:v $prof -preset veryfast -tune zerolatency `
            -x265-params 'bframes=0:keyint=120:repeat-headers=1:log-level=error' -f hevc $out
    }
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed for $out" }
}

Seg '1170x2532' 2 'seg_p1.h264' 'h264'
Seg '2532x1170' 2 'seg_l.h264'  'h264'
Seg '1170x2532' 2 'seg_p2.h264' 'h264'
# Raw Annex-B concatenation == mid-stream resolution change (new SPS + IDR).
$bytes = [System.Collections.Generic.List[byte]]::new()
foreach ($f in 'seg_p1.h264', 'seg_l.h264', 'seg_p2.h264') {
    $bytes.AddRange([System.IO.File]::ReadAllBytes((Join-Path $PSScriptRoot $f)))
    Remove-Item $f
}
[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'portrait_rotate.h264'), $bytes.ToArray())

Seg '1170x2532' 3 'portrait.h265' 'h265'
Seg '2560x1440' 4 'land1440.h264' 'h264'
Seg '2560x1440' 4 'land1440.h265' 'h265'
Seg '1170x2532' 2 'portrait10.h265' 'h265' 'yuv420p10le'
Get-ChildItem *.h264, *.h265 | Format-Table Name, Length
