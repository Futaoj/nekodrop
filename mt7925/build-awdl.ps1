# Build the fixed-channel AWDL daemon paired with the E138 radio.
param([string]$MinGW)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. "$repo\scripts\toolchain.ps1"
$tools = Initialize-Toolchain
if (-not $MinGW) { $MinGW = "$repo\third_party\winlibs\mingw64\bin" }
$gcc = Join-Path $MinGW 'gcc.exe'
$make = Join-Path $MinGW 'mingw32-make.exe'
if (-not ((Test-Path $gcc) -and (Test-Path $make))) {
    throw 'Run scripts\fetch-deps.ps1 or pass -MinGW pointing to the WinLibs bin directory.'
}
$env:PATH = "$MinGW;$env:PATH"
$build = "$repo\build\mt7925\awdl"
& $tools.CMake -S "$PSScriptRoot\awdl" -B $build -G 'MinGW Makefiles' "-DCMAKE_C_COMPILER=$gcc" "-DCMAKE_MAKE_PROGRAM=$make" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE) { throw 'MT7925 AWDL configure failed.' }
& $tools.CMake --build $build --target awdld-mt7925 --parallel
if ($LASTEXITCODE) { throw 'MT7925 AWDL build failed.' }
