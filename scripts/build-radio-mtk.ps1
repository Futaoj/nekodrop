# Builds the MediaTek MT7922/MT7921 user-mode radio daemon (nekodrop-radio-mtk.exe,
# mtkradio\) into build\mtkradio, and copies linux-firmware's MT7922/MT7921 blobs
# to build\mtkradio\firmware.
# Links statically against the MSVC runtime (/MT); no other dependencies.
param([switch]$E138Validation)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"

$repo = Split-Path $PSScriptRoot -Parent
$tools = Initialize-Toolchain
$src = Join-Path $repo 'mtkradio'
$fwSource = Join-Path $repo 'third_party\linux-firmware-mt7922'
foreach ($f in 'WIFI_MT7922_patch_mcu_1_1_hdr.bin', 'WIFI_RAM_CODE_MT7922_1.bin',
               'WIFI_MT7961_patch_mcu_1_2_hdr.bin', 'WIFI_RAM_CODE_MT7961_1.bin',
               'LICENCE.mediatek') {
  if (-not (Test-Path (Join-Path $fwSource $f))) { throw "$f missing in $fwSource`: run scripts\fetch-deps.ps1 first." }
}

$build = if ($E138Validation) { Join-Path $repo 'build\mtkradio-e138' } else { Join-Path $repo 'build\mtkradio' }
$e138Option = if ($E138Validation) { 'ON' } else { 'OFF' }
$e138Fw = Join-Path $repo 'third_party\linux-firmware-mt7925'
if ($E138Validation) {
  $expected = @{
    'WIFI_MT7925_PATCH_MCU_1_1_hdr.bin' = '67ff85f1a39d1a100a51abd8e2a26edd2f198d7052514b6b8981948b231e5016'
    'WIFI_RAM_CODE_MT7925_1_1.bin' = '329c0e27f780550789e4eba47452731b1723f69f9e67f525d0e50bf3cd2978ac'
  }
  foreach ($name in $expected.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $e138Fw $name) -Algorithm SHA256).Hash -ne $expected[$name]) {
      throw "Unexpected E138 firmware SHA-256 for $name"
    }
  }
}
& $tools.CMake -S $src -B $build -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$($tools.Ninja)" `
  "-DCMAKE_C_COMPILER=$($tools.Cl)" `
  "-DCMAKE_CXX_COMPILER=$($tools.Cl)" `
  -DCMAKE_BUILD_TYPE=Release "-DNEKODROP_E138_VALIDATION=$e138Option"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

& $tools.CMake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
$ctest = Join-Path (Split-Path $tools.CMake) 'ctest.exe'
& $ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "mtkradio tests failed ($LASTEXITCODE)" }
if ($E138Validation) {
  & (Join-Path $build 'mt7925_transport_test.exe') (Join-Path $e138Fw 'WIFI_MT7925_PATCH_MCU_1_1_hdr.bin') (Join-Path $e138Fw 'WIFI_RAM_CODE_MT7925_1_1.bin')
  if ($LASTEXITCODE -ne 0) { throw "E138 firmware validation failed ($LASTEXITCODE)" }
}

$fwDest = Join-Path $build 'firmware'
New-Item -ItemType Directory -Force $fwDest | Out-Null
Copy-Item "$fwSource\*" $fwDest -Force

$exe = Join-Path $build 'nekodrop-radio-mtk.exe'
if (-not (Test-Path $exe)) { throw "expected binary missing: $exe" }
Get-ChildItem $build -Filter *.exe | Select-Object Name, Length
