# make_ico.ps1 - regenerate src/gui/retroarchiver.ico from the SVG drawings.
#
#   powershell.exe -File tests/xp/make_ico.ps1
#
# 16 pixels from icon16.svg (drawn separately), 32 and 48 from icon48.svg.
# Edge in headless mode renders SVGs with transparency; the script writes
# an .ico with 32-bit BMP images (BGRA + mask), the format XP reads: XP
# does not understand PNG-compressed icons.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$gui  = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'src\gui'
$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
if (-not (Test-Path $edge)) { $edge = "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe" }
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('ra_ico_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null

function Render([string]$svg, [int]$size) {
    $html = Join-Path $tmp "r$size.html"
    $png  = Join-Path $tmp "r$size.png"
    $src  = 'file:///' + (Join-Path $gui $svg).Replace('\', '/')
    Set-Content $html "<html><body style='margin:0;background:transparent'><img src='$src' width='$size' height='$size' style='display:block'></body></html>"
    # Edge writes "N bytes written" to stderr: with 'Stop' PowerShell 5 takes it for an error
    $ErrorActionPreference = 'Continue'
    & $edge --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 `
        --default-background-color=00000000 --window-size=200,200 "--screenshot=$png" `
        ('file:///' + $html.Replace('\', '/')) 2>&1 | Out-Null
    $ErrorActionPreference = 'Stop'
    for ($i = 0; $i -lt 50 -and -not (Test-Path $png); $i++) { Start-Sleep -Milliseconds 200 }
    $full = [System.Drawing.Bitmap]::FromFile($png)
    $bmp = $full.Clone((New-Object System.Drawing.Rectangle 0, 0, $size, $size), [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $full.Dispose()
    $bmp
}

# one .ico entry: BITMAPINFOHEADER, BGRA pixels bottom to top, AND mask
function IconImage([System.Drawing.Bitmap]$b) {
    $n = $b.Width
    $ms = New-Object IO.MemoryStream
    $w = New-Object IO.BinaryWriter $ms
    $maskRow = [int]([math]::Ceiling($n / 32.0) * 4)
    $w.Write([int]40); $w.Write([int]$n); $w.Write([int]($n * 2))
    $w.Write([int16]1); $w.Write([int16]32); $w.Write([int]0)
    $w.Write([int]($n * $n * 4 + $maskRow * $n)); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0)
    for ($y = $n - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $n; $x++) {
            $c = $b.GetPixel($x, $y)
            $w.Write([byte]$c.B); $w.Write([byte]$c.G); $w.Write([byte]$c.R); $w.Write([byte]$c.A)
        }
    }
    for ($y = $n - 1; $y -ge 0; $y--) {
        $row = New-Object byte[] $maskRow
        for ($x = 0; $x -lt $n; $x++) {
            if ($b.GetPixel($x, $y).A -lt 128) { $row[$x -shr 3] = $row[$x -shr 3] -bor (0x80 -shr ($x -band 7)) }
        }
        $w.Write($row)
    }
    $w.Flush()
    , $ms.ToArray()
}

try {
    $images = @(
        @{ Size = 16; Data = IconImage (Render 'icon16.svg' 16) },
        @{ Size = 32; Data = IconImage (Render 'icon48.svg' 32) },
        @{ Size = 48; Data = IconImage (Render 'icon48.svg' 48) }
    )
    $out = Join-Path $gui 'retroarchiver.ico'
    $fs = [IO.File]::Create($out)
    $w = New-Object IO.BinaryWriter $fs
    $w.Write([int16]0); $w.Write([int16]1); $w.Write([int16]$images.Count)
    $offset = 6 + 16 * $images.Count
    foreach ($im in $images) {
        $w.Write([byte]$im.Size); $w.Write([byte]$im.Size); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([int16]1); $w.Write([int16]32); $w.Write([int]$im.Data.Length); $w.Write([int]$offset)
        $offset += $im.Data.Length
    }
    foreach ($im in $images) { $w.Write($im.Data) }
    $w.Close()
    "written $out ($((Get-Item $out).Length) bytes)"
} finally {
    Remove-Item -Recurse -Force $tmp
}
