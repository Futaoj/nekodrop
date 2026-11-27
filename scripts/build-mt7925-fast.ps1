# Builds the dedicated E138 FastBa daemon and checks the matching firmware files.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'build\mt7925-fast'
$license = Join-Path $repo 'mtkradio\third_party\connac3\LICENSE'
$firmware = Join-Path $repo 'third_party\linux-firmware-mt7925'
$expected = @{
  'WIFI_MT7925_PATCH_MCU_1_1_hdr.bin' = '67ff85f1a39d1a100a51abd8e2a26edd2f198d7052514b6b8981948b231e5016'
  'WIFI_RAM_CODE_MT7925_1_1.bin' = '329c0e27f780550789e4eba47452731b1723f69f9e67f525d0e50bf3cd2978ac'
}
foreach ($name in $expected.Keys) {
  if ((Get-FileHash -LiteralPath (Join-Path $firmware $name) -Algorithm SHA256).Hash -ne $expected[$name]) {
    throw "Unexpected E138 firmware SHA-256 for $name"
  }
}
$tools = Initialize-Toolchain
& $tools.CMake -S (Join-Path $repo 'mtkradio') -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$($tools.Ninja)" "-DCMAKE_C_COMPILER=$($tools.Cl)" "-DCMAKE_CXX_COMPILER=$($tools.Cl)" -DCMAKE_BUILD_TYPE=Release -DNEKODROP_E138_FAST=ON
if ($LASTEXITCODE -ne 0) { throw "E138 CMake configure failed ($LASTEXITCODE)" }
# Localized MSVC /showIncludes output can defeat header dependency discovery.
& $tools.CMake --build $build --clean-first
if ($LASTEXITCODE -ne 0) { throw "E138 native build failed ($LASTEXITCODE)" }
$ctest = Join-Path (Split-Path $tools.CMake) 'ctest.exe'
& $ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "E138 offline tests failed ($LASTEXITCODE)" }
& (Join-Path $build 'mt7925_transport_test.exe') (Join-Path $firmware 'WIFI_MT7925_PATCH_MCU_1_1_hdr.bin') (Join-Path $firmware 'WIFI_RAM_CODE_MT7925_1_1.bin')
if ($LASTEXITCODE -ne 0) { throw "E138 firmware validation failed ($LASTEXITCODE)" }
Copy-Item -LiteralPath $license -Destination (Join-Path $build 'mt7925-connac3-BSD-3-Clause-Clear.txt') -Force
Get-Item -LiteralPath (Join-Path $build 'nekodrop-radio-mt7925-fast.exe') | Select-Object FullName, Length
