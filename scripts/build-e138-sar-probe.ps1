# Build the read-only E138 board SAR probe. Running it is a separate step.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"
$repo = Split-Path $PSScriptRoot -Parent
$out = Join-Path $repo 'build\e138-sar-probe'
New-Item -ItemType Directory -Force $out | Out-Null
$tools = Initialize-Toolchain
Push-Location $out
try {
    & $tools.Cl /nologo /std:c++17 /W4 /WX /EHsc /O2 /MT /DUNICODE /D_UNICODE `
        "$repo\mtkradio\tools\e138-sar-probe.cpp" "$repo\iwxradio\src\proxy.cpp" `
        /Fe:e138-sar-probe.exe /link SetupAPI.lib
    if ($LASTEXITCODE) { throw "E138 SAR probe compilation failed ($LASTEXITCODE)" }
    Get-Item .\e138-sar-probe.exe
} finally { Pop-Location }
