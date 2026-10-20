# PNG -> JPEG at most MaxW pixels wide (for the tutorial's embedded screenshots).
param([string]$In, [string]$Out, [int]$MaxW = 640, [int]$Quality = 82)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile($In)
$w = [Math]::Min($MaxW, $src.Width)
$h = [int][Math]::Round($src.Height * $w / $src.Width)
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$g.Clear([System.Drawing.Color]::FromArgb(255, 42, 34, 36))
$g.DrawImage($src, 0, 0, $w, $h)
$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$ep = New-Object System.Drawing.Imaging.EncoderParameters 1
$ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), ([long]$Quality)
$bmp.Save($Out, $codec, $ep)
$g.Dispose(); $bmp.Dispose(); $src.Dispose()
