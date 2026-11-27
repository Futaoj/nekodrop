# Builds the E138 RegProxy and board SAR package. install.ps1 signs it on the target PC.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"
$repo = Split-Path $PSScriptRoot -Parent
& "$repo\driver\regproxy\build.ps1" | Out-Null
$tools = Initialize-Toolchain
$kit = $tools.WdkRoot
$version = $tools.WdkVersion
$out = Join-Path $repo 'build\mt7925-fast'
$package = Join-Path $out 'driver'
$obj = Join-Path $out 'sar-obj'
New-Item -ItemType Directory -Force $package, $obj | Out-Null

$includes = @("$kit\Include\$version\km", "$kit\Include\$version\km\crt", "$kit\Include\$version\shared",
              "$($tools.SdkRoot)\Include\$($tools.SdkVersion)\shared", "$kit\Include\wdf\kmdf\1.31") |
  ForEach-Object { "/external:I$_" }
$object = Join-Path $obj 'mt7925_sar.obj'
& $tools.Cl /nologo /c /kernel /Zl /W4 /WX /external:W0 /O2 /GS /guard:cf /utf-8 /D_AMD64_ /D_WIN64 `
    /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00 /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=31 `
    @includes "/Fo$object" "$repo\driver\mt7925-sar\mt7925_sar.c"
if ($LASTEXITCODE) { throw "MT7925 SAR compile failed ($LASTEXITCODE)" }
$link = Join-Path (Split-Path $tools.Cl) 'link.exe'
& $link /nologo /driver:wdm /release /subsystem:native /entry:FxDriverEntry /nodefaultlib /machine:x64 `
    /integritycheck /guard:cf /dynamicbase /nxcompat "/out:$package\nekodrop_mt7925_sar.sys" `
    "/libpath:$kit\Lib\$version\km\x64" "/libpath:$kit\Lib\wdf\kmdf\x64\1.31" `
    $object ntoskrnl.lib hal.lib wmilib.lib BufferOverflowFastFailK.lib WdfLdr.lib WdfDriverEntry.lib
if ($LASTEXITCODE) { throw "MT7925 SAR link failed ($LASTEXITCODE)" }

Copy-Item -LiteralPath "$repo\driver\regproxy\build\winawdl_regproxy.dll",
    "$repo\driver\mt7925-fast\nekodrop_mt7925_sar.inf" -Destination $package -Force
$inf = Join-Path $package 'nekodrop_mt7925_sar.inf'
$now = Get-Date
& $tools.StampInf -f $inf -d * -v "1.1.$(($now - [datetime]'2026-01-01').Days).$($now.Hour * 60 + $now.Minute)"
if ($LASTEXITCODE) { throw "MT7925 SAR StampInf failed ($LASTEXITCODE)" }
& $tools.InfVerif /v $inf
if ($LASTEXITCODE) { throw "MT7925 SAR INF validation failed ($LASTEXITCODE)" }
& $tools.Inf2Cat "/driver:$package" /os:10_GE_X64 /uselocaltime
if ($LASTEXITCODE) { throw "MT7925 SAR catalog creation failed ($LASTEXITCODE)" }
