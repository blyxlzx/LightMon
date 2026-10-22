# Original LightMon icon: a teal disk and three rising monitoring bars.
# Rebuild using Windows PowerShell 5.1; no third-party art or font required.
param([string]$OutputPath = (Join-Path $PSScriptRoot '..\source\src\app.ico'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$sizes = @(16,32,48,256)
$images = @()
foreach($size in $sizes) {
    $bitmap = New-Object Drawing.Bitmap($size,$size)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $background = New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(255,38,99,107))
    $bar = New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(255,227,245,231))
    $stream = New-Object IO.MemoryStream
    try {
        $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.Clear([Drawing.Color]::Transparent)
        $graphics.FillEllipse($background,0,0,($size-1),($size-1))
        for($i=0;$i -lt 3;$i++) {
            $x=[single]($size*(0.23+0.20*$i)); $h=[single]($size*(0.22+0.12*$i))
            $graphics.FillRectangle($bar,$x,[single]($size*0.74-$h),[single]($size*0.13),$h)
        }
        $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
        $images += ,$stream.ToArray()
    } finally { $stream.Dispose(); $bar.Dispose(); $background.Dispose(); $graphics.Dispose(); $bitmap.Dispose() }
}
$path=[IO.Path]::GetFullPath($OutputPath)
$file=[IO.File]::Create($path); $writer=New-Object IO.BinaryWriter($file)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset=6+16*$sizes.Count
    for($i=0;$i -lt $sizes.Count;$i++) {
        $dimension=if($sizes[$i] -eq 256){0}else{$sizes[$i]}
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
        $offset += $images[$i].Length
    }
    foreach($bytes in $images){$writer.Write([byte[]]$bytes)}
} finally { $writer.Dispose(); $file.Dispose() }
Write-Output $path
