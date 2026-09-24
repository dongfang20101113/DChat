# 生成聊天程序的应用图标。
# 三种风格（-Style）：
#   circle  圆形蓝底 + 白色气泡（默认，最简洁、小尺寸也清晰）
#   bubble  只有一只蓝色气泡（透明背景）
#   double  两只重叠的气泡
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\make_dchat_icon.ps1
#   powershell -ExecutionPolicy Bypass -File tools\make_dchat_icon.ps1 -Style double -Preview
param(
    [ValidateSet('circle', 'bubble', 'double')]
    [string]$Style = 'circle',
    [string]$OutFile = (Join-Path (Split-Path -Parent $PSScriptRoot) 'resources\dchat.ico'),
    [switch]$Preview
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$blue = [System.Drawing.Color]::FromArgb(255, 24, 119, 216)
$blueDeep = [System.Drawing.Color]::FromArgb(255, 12, 92, 180)
$blueLight = [System.Drawing.Color]::FromArgb(255, 122, 190, 250)
$white = [System.Drawing.Color]::White

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    if ($d -gt $w) { $d = $w }
    if ($d -gt $h) { $d = $h }
    if ($d -le 0) {
        $path.AddRectangle((New-Object System.Drawing.RectangleF($x, $y, $w, $h)))
        return $path
    }
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

# 画一只气泡（圆角矩形 + 左下小尾巴；点可选）
function Draw-Bubble($g, [single]$x, [single]$y, [single]$w, [single]$h, [single]$r, $color, $dotColor) {
    $brush = New-Object System.Drawing.SolidBrush($color)
    $path = New-RoundedPath $x $y $w $h $r
    $g.FillPath($brush, $path)
    $path.Dispose()
    $tail = @(
        (New-Object System.Drawing.PointF([single]($x + $w * 0.22), [single]($y + $h - 1))),
        (New-Object System.Drawing.PointF([single]($x + $w * 0.46), [single]($y + $h - 1))),
        (New-Object System.Drawing.PointF([single]($x + $w * 0.20), [single]($y + $h + $h * 0.34)))
    )
    $g.FillPolygon($brush, $tail)
    if ($null -ne $dotColor) {
        $dot = New-Object System.Drawing.SolidBrush($dotColor)
        $radius = [single]($h * 0.105)
        foreach ($ratio in 0.3, 0.5, 0.7) {
            $cx = [single]($x + $w * $ratio)
            $cy = [single]($y + $h * 0.5)
            $g.FillEllipse($dot, [single]($cx - $radius), [single]($cy - $radius),
                           [single]($radius * 2), [single]($radius * 2))
        }
        $dot.Dispose()
    }
    $brush.Dispose()
}

function New-IconBitmap([int]$size, [string]$style) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    switch ($style) {
        'bubble' {
            Draw-Bubble $g ([single]($size * 0.06)) ([single]($size * 0.14)) ([single]($size * 0.88)) ([single]($size * 0.58)) ([single]($size * 0.19)) $blue $white
        }
        'double' {
            Draw-Bubble $g ([single]($size * 0.10)) ([single]($size * 0.11)) ([single]($size * 0.66)) ([single]($size * 0.45)) ([single]($size * 0.15)) $blueLight $null
            Draw-Bubble $g ([single]($size * 0.24)) ([single]($size * 0.40)) ([single]($size * 0.68)) ([single]($size * 0.47)) ([single]($size * 0.16)) $blue $white
        }
        'circle' {
            $disc = New-Object System.Drawing.SolidBrush($blue)
            $g.FillEllipse($disc, 0, 0, $size - 1, $size - 1)
            $disc.Dispose()
            Draw-Bubble $g ([single]($size * 0.17)) ([single]($size * 0.25)) ([single]($size * 0.66)) ([single]($size * 0.43)) ([single]($size * 0.14)) $white $null
            # 气泡里一条短横线代表文字；小尺寸时线更粗，避免糊掉
            $thickness = [single]($size * 0.055)
            if ($size -le 20) { $thickness = [single]($size * 0.09) }
            $line = New-Object System.Drawing.Pen($blueDeep, $thickness)
            $line.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
            $line.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
            $g.DrawLine($line, [single]($size * 0.33), [single]($size * 0.45), [single]($size * 0.67), [single]($size * 0.45))
            $line.Dispose()
        }
        default {
            $disc = New-Object System.Drawing.SolidBrush($blue)
            $g.FillEllipse($disc, 0, 0, $size - 1, $size - 1)
            $disc.Dispose()
            Draw-Bubble $g ([single]($size * 0.17)) ([single]($size * 0.25)) ([single]($size * 0.66)) ([single]($size * 0.43)) ([single]($size * 0.14)) $white $null
        }
    }
    $g.Dispose()
    return $bmp
}

function Get-IconBmpBytes([System.Drawing.Bitmap]$bmp) {
    $size = $bmp.Width
    $rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $rowBytes = $size * 4
    $pixels = New-Object byte[] ($rowBytes * $size)
    for ($y = 0; $y -lt $size; $y++) {
        $srcRow = $size - 1 - $y  # ICO 中的位图自下而上
        [System.Runtime.InteropServices.Marshal]::Copy(
            [IntPtr]::Add($data.Scan0, [int]($srcRow * $data.Stride)),
            $pixels, [int]($y * $rowBytes), [int]$rowBytes)
    }
    $bmp.UnlockBits($data)

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([uint32]40)
    $bw.Write([int32]$size)
    $bw.Write([int32]($size * 2))
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]0)
    $bw.Write([uint32]($rowBytes * $size))
    $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)
    $bw.Write([byte[]]$pixels)
    $maskRow = [int]([Math]::Ceiling($size / 32.0) * 4)
    $bw.Write([byte[]](New-Object byte[] ($maskRow * $size)))
    $bw.Flush()
    return $ms.ToArray()
}

function Get-IconPngBytes([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return $ms.ToArray()
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$images = @()
foreach ($size in $sizes) {
    $bmp = New-IconBitmap $size $Style
    if ($size -ge 256) { $bytes = Get-IconPngBytes $bmp } else { $bytes = Get-IconBmpBytes $bmp }
    $images += , @($size, $bytes)
    $bmp.Dispose()
}

$stream = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($stream)
$w.Write([uint16]0)
$w.Write([uint16]1)
$w.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $size = $img[0]
    $bytes = $img[1]
    $dim = if ($size -ge 256) { 0 } else { $size }
    $w.Write([byte]$dim); $w.Write([byte]$dim)
    $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$bytes.Length)
    $w.Write([uint32]$offset)
    $offset += $bytes.Length
}
foreach ($img in $images) { $w.Write([byte[]]$img[1]) }
$w.Flush()

$dir = Split-Path -Parent $OutFile
if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
[System.IO.File]::WriteAllBytes($OutFile, $stream.ToArray())
Write-Host ("已生成 {0}（风格 {1}，{2} 字节，尺寸：{3}）" -f $OutFile, $Style, (Get-Item -LiteralPath $OutFile).Length, ($sizes -join ', '))

if ($Preview) {
    foreach ($size in 16, 32, 128) {
        $bmp = New-IconBitmap $size $Style
        $png = Join-Path $dir ("preview-{0}.png" -f $size)
        $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        Write-Host "预览图: $png"
    }
}
