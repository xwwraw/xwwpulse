#  Downloads Geist and Geist Mono (SIL Open Font License 1.1) as static
#  TrueType instances into res\fonts. xwwPulse Control embeds them as
#  resources and loads them with DirectWrite, so the interface looks the
#  same on every PC. Run it again to update the fonts.

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = 3072
$target = Join-Path (Split-Path $PSScriptRoot -Parent) 'res\fonts'
New-Item $target -ItemType Directory -Force | Out-Null

$fonts = [ordered]@{
    'Geist-Regular.ttf'      = @('Geist', 400)
    'Geist-Medium.ttf'       = @('Geist', 500)
    'GeistMono-Regular.ttf'  = @('Geist Mono', 400)
    'GeistMono-Medium.ttf'   = @('Geist Mono', 500)
}

foreach ($entry in $fonts.GetEnumerator()) {
    $family, $weight = $entry.Value
    # without a browser user agent Google Fonts answers with TrueType URLs
    $css = (Invoke-WebRequest "https://fonts.googleapis.com/css2?family=$([uri]::EscapeDataString($family)):wght@$weight" -UseBasicParsing -UserAgent 'xwwPulse-build').Content
    $url = [regex]::Match($css, 'url\((https://[^)]+\.ttf)\)').Groups[1].Value
    if (-not $url) { throw "No TrueType file for $family $weight" }
    Invoke-WebRequest $url -OutFile (Join-Path $target $entry.Key) -UseBasicParsing
    Write-Host "$($entry.Key) <- $url"
}

Invoke-WebRequest 'https://raw.githubusercontent.com/google/fonts/main/ofl/geist/OFL.txt' -OutFile (Join-Path $target 'OFL.txt') -UseBasicParsing
