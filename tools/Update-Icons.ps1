#  Regenerates src\ui\icons.h from Lucide (ISC license, lucide.dev). Each SVG
#  becomes one path string on a 24x24 canvas (M L H V C S Q A Z, absolute or
#  relative), which ui\canvas.cpp strokes 1.5 wide with round caps and joins.
#  Run it after changing the icon list below.

param([string]$Version = '0.454.0')

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationCore, WindowsBase

# constant name in icons.h = Lucide icon name
$icons = [ordered]@{
    Home = 'house'; Games = 'gamepad-2'; Optimize = 'gauge'; Background = 'layers'; Logs = 'scroll-text'; Settings = 'settings'
    Close = 'x'; Minimize = 'minus'; Maximize = 'square'; Restore = 'copy'; Check = 'circle-check'; Info = 'info'; Alert = 'circle-alert'
    Folder = 'folder-open'; Link = 'external-link'; Refresh = 'refresh-cw'; Reset = 'rotate-ccw'; Plus = 'plus'
    Pause = 'pause'; Play = 'play'; Ban = 'ban'; Power = 'power'; Copy = 'copy'; Skip = 'circle-minus'; Off = 'circle'; Zap = 'zap'
}
function N([double]$v) { $v.ToString('0.###', [Globalization.CultureInfo]::InvariantCulture) }

# SVG path data -> WPF path markup with explicit separators (arc flags may be packed in SVG).
function Convert-PathData([string]$d) {
    $tokens = [regex]::Matches($d, '[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?') | ForEach-Object Value
    $counts = @{ M = 2; L = 2; H = 1; V = 1; C = 6; S = 4; Q = 4; T = 2; A = 7; Z = 0 }
    $out = New-Object System.Collections.Generic.List[string]
    $i = 0; $cmd = $null
    while ($i -lt $tokens.Count) {
        if ($tokens[$i] -match '^[A-Za-z]$') { $cmd = $tokens[$i]; $out.Add($cmd); $i++; if ($cmd -match '[Zz]') { continue } }
        if ($i -eq 1 -and $cmd -ceq 'm') {
            # a path's first "m" is measured from the origin; once paths are joined it would be relative
            # to the previous figure, so make it absolute (the pairs after it stay relative line-tos)
            $out[0] = 'M'; $out.Add(($tokens[1..2] -join ',')); $i = 3; $cmd = 'l'
            if ($i -lt $tokens.Count -and $tokens[$i] -notmatch '^[A-Za-z]$') { $out.Add('l') }
            continue
        }
        $n = $counts[$cmd.ToUpperInvariant()]
        if ($cmd -match '[Aa]') {
            # rx ry rotation large sweep x y; flags can be glued to the next number ("011")
            $args = @(); $j = 0
            while ($args.Count -lt 7) {
                $t = $tokens[$i]
                if (($args.Count -eq 3 -or $args.Count -eq 4) -and $t -match '^[01]\d') { $args += $t.Substring(0, 1); $tokens[$i] = $t.Substring(1); continue }
                $args += $t; $i++
            }
            $out.Add(('{0},{1} {2} {3} {4} {5},{6}' -f $args))
        } else {
            $vals = $tokens[$i..($i + $n - 1)]; $i += $n
            $out.Add(($vals -join ','))
        }
        if ($cmd -match '[Mm]') { $cmd = if ($cmd -ceq 'M') { 'L' } else { 'l' } }  # implicit lineto after moveto
    }
    ($out -join ' ') -replace '([A-Za-z]) ', '$1'
}

function Convert-Element([xml.XmlElement]$e) {
    $a = { param($n) [double]::Parse($e.GetAttribute($n), [Globalization.CultureInfo]::InvariantCulture) }
    switch ($e.LocalName) {
        'path' { Convert-PathData $e.GetAttribute('d') }
        'circle' { $cx = & $a cx; $cy = & $a cy; $r = & $a r; "M$(N ($cx - $r)),$(N $cy) A$(N $r),$(N $r) 0 1 0 $(N ($cx + $r)),$(N $cy) A$(N $r),$(N $r) 0 1 0 $(N ($cx - $r)),$(N $cy)Z" }
        'line' { "M$(N (& $a x1)),$(N (& $a y1)) L$(N (& $a x2)),$(N (& $a y2))" }
        { $_ -in 'polyline', 'polygon' } {
            $p = ($e.GetAttribute('points').Trim() -split '[\s,]+')
            $pairs = for ($k = 0; $k -lt $p.Count; $k += 2) { "$($p[$k]),$($p[$k + 1])" }
            "M$($pairs[0]) L$(($pairs | Select-Object -Skip 1) -join ' ')" + $(if ($_ -eq 'polygon') { 'Z' })
        }
        'rect' {
            $x = & $a x; $y = & $a y; $w = & $a width; $h = & $a height
            $r = if ($e.HasAttribute('rx')) { & $a rx } else { 0 }
            if ($r -eq 0) { "M$(N $x),$(N $y) H$(N ($x + $w)) V$(N ($y + $h)) H$(N $x)Z" }
            else {
                "M$(N ($x + $r)),$(N $y) H$(N ($x + $w - $r)) A$(N $r),$(N $r) 0 0 1 $(N ($x + $w)),$(N ($y + $r)) V$(N ($y + $h - $r)) A$(N $r),$(N $r) 0 0 1 $(N ($x + $w - $r)),$(N ($y + $h)) " +
                "H$(N ($x + $r)) A$(N $r),$(N $r) 0 0 1 $(N $x),$(N ($y + $h - $r)) V$(N ($y + $r)) A$(N $r),$(N $r) 0 0 1 $(N ($x + $r)),$(N $y)Z"
            }
        }
        'ellipse' { $cx = & $a cx; $cy = & $a cy; $rx = & $a rx; $ry = & $a ry; "M$(N ($cx - $rx)),$(N $cy) A$(N $rx),$(N $ry) 0 1 0 $(N ($cx + $rx)),$(N $cy) A$(N $rx),$(N $ry) 0 1 0 $(N ($cx - $rx)),$(N $cy)Z" }
        default { throw "Unsupported SVG element <$($e.LocalName)>" }
    }
}

function Get-PathData([string]$url) {
    $xml = [xml]((Invoke-WebRequest $url -UseBasicParsing).Content -replace '<!--.*?-->', '')
    $data = (@($xml.svg.ChildNodes | Where-Object { $_ -is [xml.XmlElement] } | ForEach-Object { Convert-Element $_ }) -join ' ')
    [void][System.Windows.Media.Geometry]::Parse($data)   # same grammar canvas.cpp reads; throws on anything malformed
    $data
}

[Net.ServicePointManager]::SecurityProtocol = 3072
$base = "https://cdn.jsdelivr.net/npm/lucide-static@$Version/icons"
$lines = New-Object System.Collections.Generic.List[string]
foreach ($entry in $icons.GetEnumerator()) {
    $lines.Add("inline constexpr const char $($entry.Key)[] = `"$(Get-PathData "$base/$($entry.Value).svg")`"; // $($entry.Value)")
}

$target = Join-Path (Split-Path $PSScriptRoot -Parent) 'src\ui\icons.h'
$text = @"
//  Lucide icons $Version (ISC license, https://lucide.dev) as path data on a
//  24x24 canvas, stroked 1.5 wide with round caps and joins.
//  Generated by tools\Update-Icons.ps1; edit the list there, not this file.
#pragma once

namespace xww::icons {

$($lines -join "`n")

}  // namespace xww::icons
"@
[IO.File]::WriteAllText($target, ($text -replace "`r`n", "`n") + "`n")
Write-Host "Wrote $($lines.Count) icons to $target"
