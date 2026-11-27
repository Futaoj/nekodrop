# Builds the dedicated MT7925 radio and unsigned KMDF package for the E138105B board.
# Outputs go to build\mt7925. Driver signing takes place on the target PC.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"
$repo = Split-Path $PSScriptRoot -Parent
$tools = Initialize-Toolchain
$out = Join-Path $repo 'build\mt7925'
$driver = Join-Path $repo 'mt7925\driver'
$pkg = Join-Path $out 'driver'
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $out, $pkg, $obj | Out-Null

& $tools.Cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT /utf-8 "/Fo$obj\radio.obj" `
  "/Fe$out\nekodrop-radio-mt7925.exe" "$repo\mt7925\radio\winawdl_mt7925.cpp" /link setupapi.lib bcrypt.lib
if ($LASTEXITCODE) { throw "MT7925 radio compile failed ($LASTEXITCODE)" }

$kit = $tools.WdkRoot
$version = $tools.WdkVersion
$includes = @("$($tools.MsvcRoot)\include", "$kit\Include\$version\km", "$kit\Include\$version\shared",
              "$($tools.SdkRoot)\Include\$($tools.SdkVersion)\shared", "$($tools.SdkRoot)\Include\$($tools.SdkVersion)\ucrt",
              "$kit\Include\wdf\kmdf\1.33") | ForEach-Object { "/external:I$_" }
$sources = @(Get-ChildItem -LiteralPath $driver -Filter '*.c' -File)
if (-not $sources.Count) { throw "No MT7925 driver sources under $driver" }
& $tools.Cl /nologo /c /W4 /WX /kernel /Zl /GS /O2 /guard:cf /external:W0 /D_AMD64_ /D_WIN64 `
  /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A00000C /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=33 `
  @includes "/Fo$obj\\" @($sources.FullName)
if ($LASTEXITCODE) { throw "MT7925 kernel compile failed ($LASTEXITCODE)" }
$objects = $sources | ForEach-Object { Join-Path $obj ($_.BaseName + '.obj') }
$link = Join-Path (Split-Path $tools.Cl) 'link.exe'
& $link /nologo /driver:wdm /release /subsystem:native /entry:FxDriverEntry /nodefaultlib /machine:x64 `
  /integritycheck /guard:cf /dynamicbase /nxcompat "/out:$pkg\mt7925_bringup.sys" `
  "/libpath:$kit\Lib\$version\km\x64" "/libpath:$kit\Lib\wdf\kmdf\x64\1.33" `
  @objects ntoskrnl.lib hal.lib wmilib.lib WdfDriverEntry.lib WdfLdr.lib BufferOverflowFastFailK.lib
if ($LASTEXITCODE) { throw "MT7925 kernel link failed ($LASTEXITCODE)" }
Copy-Item -LiteralPath "$driver\mt7925_bringup.inf" -Destination $pkg -Force
$now = Get-Date
& $tools.StampInf -f "$pkg\mt7925_bringup.inf" -d * -v "1.0.$(($now - [datetime]'2026-01-01').Days).$($now.Hour * 60 + $now.Minute)"
if ($LASTEXITCODE) { throw "MT7925 StampInf failed ($LASTEXITCODE)" }
& $tools.Inf2Cat "/driver:$pkg" /os:10_GE_X64 /uselocaltime
if ($LASTEXITCODE) { throw "MT7925 Inf2Cat failed ($LASTEXITCODE)" }
