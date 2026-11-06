#  Builds xwwPulseService.exe (the service) and xwwPulse.exe (the app) with MSVC,
#  then xwwPulse-Setup-<version>.exe, which carries both and is what a release ships.
#  CMake is not needed: the compiler comes from Visual Studio or Build Tools,
#  and the Windows SDK from an installed kit or, when none is installed,
#  from Microsoft's NuGet packages unpacked into .build\sdk (nothing is
#  installed). CMakeLists.txt builds the same thing for IDE users.

param([switch]$Debug)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$sdkVersion = '10.0.28000.2957'

# ------------------------------------------------------------------ toolchain
$msvc = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*' -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'lib\x64\libcmt.lib') } | Sort-Object Name | Select-Object -Last 1
if (-not $msvc) { throw 'MSVC was not found. Install Visual Studio or Build Tools with the C++ workload.' }
$cl = @('Hostx64', 'Hostx86') | ForEach-Object { Join-Path $msvc.FullName "bin\$_\x64\cl.exe" } | Where-Object { Test-Path $_ } | Select-Object -First 1
$link = Join-Path (Split-Path $cl) 'link.exe'

$kit = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include\*\um\windows.h' -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
if ($kit) {
    $inc = Split-Path (Split-Path $kit.FullName)
    $ver = Split-Path $inc -Leaf
    $libRoot = "C:\Program Files (x86)\Windows Kits\10\Lib\$ver"
    $rc = "C:\Program Files (x86)\Windows Kits\10\bin\$ver\x64\rc.exe"
} else {
    $sdk = Join-Path $root '.build\sdk'
    if (-not (Test-Path "$sdk\microsoft.windows.sdk.cpp.x64")) {
        Write-Host "No Windows SDK installed; unpacking Microsoft.Windows.SDK.CPP $sdkVersion from nuget.org into .build\sdk"
        [Net.ServicePointManager]::SecurityProtocol = 3072
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        New-Item $sdk -ItemType Directory -Force | Out-Null
        foreach ($p in 'microsoft.windows.sdk.cpp', 'microsoft.windows.sdk.cpp.x64') {
            $file = Join-Path $sdk "$p.nupkg"
            Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/$p/$sdkVersion/$p.$sdkVersion.nupkg" -OutFile $file -UseBasicParsing
            [IO.Compression.ZipFile]::ExtractToDirectory($file, (Join-Path $sdk $p))
            Remove-Item $file
        }
    }
    $inc = (Get-ChildItem "$sdk\microsoft.windows.sdk.cpp\c\Include\*" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
    $libRoot = "$sdk\microsoft.windows.sdk.cpp.x64\c"
    $rc = (Get-ChildItem "$sdk\microsoft.windows.sdk.cpp\c\bin\*\x64\rc.exe" | Sort-Object FullName | Select-Object -Last 1).FullName
}

$env:INCLUDE = @("$($msvc.FullName)\include", "$inc\ucrt", "$inc\um", "$inc\shared", "$inc\winrt") -join ';'
$env:LIB = @("$($msvc.FullName)\lib\x64", "$libRoot\ucrt\x64", "$libRoot\um\x64") -join ';'
$env:PATH = (Split-Path $cl) + ';' + (Split-Path $rc) + ';' + $env:PATH

# ------------------------------------------------------------------ sources
$obj = Join-Path $root '.build\obj'
$dist = Join-Path $root 'dist'
New-Item $obj, $dist -ItemType Directory -Force | Out-Null
# a setup from an earlier version must not ship next to this one
Remove-Item (Join-Path $dist 'xwwPulse-Setup*') -Force -ErrorAction SilentlyContinue

$common = @('src\core\*.cpp', 'src\config\*.cpp') | ForEach-Object { Get-ChildItem (Join-Path $root $_) } | ForEach-Object FullName
$service = @('src\detection\*.cpp', 'src\optimizer\*.cpp', 'src\service\*.cpp') | ForEach-Object { Get-ChildItem (Join-Path $root $_) } | Where-Object Name -ne 'foreground_hook.cpp' | ForEach-Object FullName
$dashboard = @('src\agent\*.cpp', 'src\ui\*.cpp', 'src\detection\foreground_hook.cpp') | ForEach-Object { Get-ChildItem (Join-Path $root $_) } | ForEach-Object FullName

$cflags = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/permissive-', '/utf-8', '/MT', '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_WIN32_WINNT=0x0A00', "/I$root\src")
# /Brepro: no timestamps in the objects or the PE header, so the same sources give the same bytes and SHA-256
$cflags += if ($Debug) { @('/Od', '/Zi', '/DDEBUG') } else { @('/O2', '/GL', '/DNDEBUG', '/Brepro') }
$lflags = @('/nologo', '/MACHINE:X64', '/DYNAMICBASE', '/NXCOMPAT', '/CETCOMPAT')
$lflags += if ($Debug) { @('/DEBUG') } else { @('/LTCG', '/OPT:REF', '/OPT:ICF', '/Brepro') }
# minimal Build Tools installs ship without oldnames.lib (POSIX aliases xwwPulse does not use)
if (-not (Test-Path "$($msvc.FullName)\lib\x64\oldnames.lib")) { $lflags += '/NODEFAULTLIB:oldnames.lib' }
$version = (Get-Content (Join-Path $root 'version.txt') -Raw).Trim()
$binary = ((($version -split '[-+]')[0] -split '\.') + @('0', '0', '0', '0'))[0..3] -join ','

function Build-Exe([string]$name, [string[]]$sources, [string[]]$libs, [string]$subsystem, [string]$resource) {
    $dir = Join-Path $obj $name
    New-Item $dir -ItemType Directory -Force | Out-Null
    # objects of sources that no longer exist must not reach the linker
    Remove-Item "$dir\*.obj", "$dir\*.res" -Force -ErrorAction SilentlyContinue
    & $cl @cflags "/DXWW_VERSION=L\`"$version\`"" /c @sources "/Fo$dir\\" "/Fd$dir\\"
    if ($LASTEXITCODE -ne 0) { throw "$name failed to compile" }
    $objects = @(Get-ChildItem $dir -Filter *.obj | ForEach-Object FullName)
    if ($resource) {
        & $rc /nologo "/I$root\res" "/I$dist" "/DXWW_VERSION_TEXT=\`"$version\`"" "/DXWW_VERSION_BINARY=$binary" /fo "$dir\res.res" (Join-Path $root $resource)
        if ($LASTEXITCODE -ne 0) { throw "$name resources failed" }
        $objects += "$dir\res.res"
    }
    & $link @lflags "/SUBSYSTEM:$subsystem" "/OUT:$dist\$name.exe" @objects @libs
    if ($LASTEXITCODE -ne 0) { throw "$name failed to link" }
}

$sys = 'kernel32.lib', 'user32.lib', 'advapi32.lib', 'shell32.lib', 'ole32.lib', 'shlwapi.lib', 'ntdll.lib'
Write-Host "Building xwwPulse $version"
Build-Exe 'xwwPulseService' ($common + $service) ($sys + 'powrprof.lib', 'tdh.lib', 'gdi32.lib', 'version.lib', 'wtsapi32.lib', 'userenv.lib') 'CONSOLE' 'res\service.rc'
Build-Exe 'xwwPulse' ($common + $dashboard) ($sys + 'gdi32.lib', 'dwmapi.lib', 'd2d1.lib', 'dwrite.lib', 'windowscodecs.lib', 'uxtheme.lib', 'comctl32.lib', 'version.lib') 'WINDOWS' 'res\dashboard.rc'
# the setup carries the two programs built above, so it comes last
Build-Exe "xwwPulse-Setup-$version" @(Join-Path $root 'src\setup\main.cpp') ($sys + 'comctl32.lib', 'uuid.lib') 'WINDOWS' 'res\setup.rc'

Get-ChildItem $dist -Filter *.exe | ForEach-Object {
    (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $_.Name | Set-Content ($_.FullName + '.sha256') -Encoding ascii
}
Get-ChildItem $dist | Select-Object Name, Length | Format-Table -AutoSize

