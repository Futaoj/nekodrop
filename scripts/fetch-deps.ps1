# Fetches what the build needs into third_party\ (not in this repository):
#   winawdl         radio pipe, awdld (OWL), Wintun, the AirDrop engine (MIT)
#   ForbiddenItlwm  iwlwifi firmware and its licence (GPL-2.0 / Intel redistribution)
#   winlibs MinGW   GCC for awdld
#   WDK (NuGet)     UMDF headers and tools, only when no WDK is installed
# Pinned to the versions nekodrop is tested with; the downloads are checked by SHA-256.
$ErrorActionPreference = 'Stop'
$tp = Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party'
New-Item -ItemType Directory -Force $tp | Out-Null

function Get-Repo($name, $url, $commit) {
  $dir = Join-Path $tp $name
  if (-not (Test-Path "$dir\.git")) { git clone --quiet $url $dir; if ($LASTEXITCODE) { throw "clone $url failed" } }
  git -C $dir fetch --quiet origin
  git -C $dir checkout --quiet $commit
  if ($LASTEXITCODE) { throw "$name`: commit $commit not found" }
  "$name @ $($commit.Substring(0, 7))"
}

function Get-File($url, $out, $sha256) {
  if (-not (Test-Path $out)) { Invoke-WebRequest $url -OutFile $out -UseBasicParsing }
  $got = (Get-FileHash $out -Algorithm SHA256).Hash
  if ($sha256 -and $got -ne $sha256) { Remove-Item $out; throw "$out`: SHA-256 $got, expected $sha256" }
}

Get-Repo winawdl https://github.com/raph559/winawdl.git 0db7031589dc2bca03c0893db341176376962047
Get-Repo ForbiddenItlwm https://github.com/retropineaple/ForbiddenItlwm.git 96ff71e0211789239f6fbf6b64ac8e7fd22b41aa

# Wintun for awdld: winawdl's script checks its SHA-256 and WireGuard's signature.
if (-not (Test-Path "$tp\winawdl\deps\wintun\wintun.dll")) {
  & "$tp\winawdl\scripts\fetch-wintun.ps1"
}

$mingw = Join-Path $tp 'winlibs'
if (-not (Test-Path "$mingw\mingw64\bin\gcc.exe")) {
  New-Item -ItemType Directory -Force $mingw | Out-Null
  Get-File 'https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-23.1.3-14.0.0-ucrt-r3/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.zip' `
           "$mingw\mingw.zip" '3C69305081E4A647144339A936244FCF7D3D9E558319E1AD7BE2CD2886D4B6A1'
  Expand-Archive "$mingw\mingw.zip" $mingw -Force
}
'winlibs MinGW (GCC 16.2.0)'

$kit = "${env:ProgramFiles(x86)}\Windows Kits\10"
if (-not (Test-Path "$kit\Include\wdf\umdf")) {
  $wdk = Join-Path $tp 'wdk-nuget'
  if (-not (Test-Path "$wdk\wdk\c")) {
    New-Item -ItemType Directory -Force $wdk | Out-Null
    Get-File 'https://www.nuget.org/api/v2/package/Microsoft.Windows.WDK.x64/10.0.26100.6584' "$wdk\wdk.nupkg" $null
    Copy-Item "$wdk\wdk.nupkg" "$wdk\wdk.zip"
    Expand-Archive "$wdk\wdk.zip" "$wdk\wdk" -Force
    Remove-Item "$wdk\wdk.zip"
  }
  'WDK 10.0.26100.6584 (NuGet)'
} else { 'WDK: installed' }

# MediaTek MT7922 firmware blobs (linux-firmware) for mtkradio.
$mtkFw = Join-Path $repo 'mtkradio\firmware'
New-Item -ItemType Directory -Force $mtkFw | Out-Null
$mtkBase = 'https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek'
foreach ($f in 'WIFI_MT7922_patch_mcu_1_1_hdr.bin', 'WIFI_RAM_CODE_MT7922_1.bin') {
  Get-File "$mtkBase/$f" (Join-Path $mtkFw $f) $null
}
'MediaTek MT7922 firmware (linux-firmware)'
