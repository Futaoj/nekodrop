# Locates MSVC, clang-cl, CMake/Ninja, the Windows SDK and the WDK; sets
# INCLUDE and LIB; returns tool paths. Adapted from winawdl's
# scripts\toolchain.ps1, with the Build Tools found through vswhere and the WDK
# taken from its NuGet package when it is not installed (no admin rights).
function Initialize-Toolchain {
  param([string]$UmdfVersion)

  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vsRoot = $null
  if (Test-Path $vswhere) {
    $vsRoot = & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  }
  if (-not $vsRoot) { $vsRoot = 'E:\BuildTools' }
  $msvc = Get-ChildItem "$vsRoot\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name | Select-Object -Last 1
  if (-not $msvc) { throw "MSVC toolset not found under $vsRoot" }

  $kit = 'C:\Program Files (x86)\Windows Kits\10'
  $kitVersion = Get-ChildItem "$kit\Include" -Directory |
                Where-Object { Test-Path "$($_.FullName)\um\windows.h" } |
                Sort-Object Name | Select-Object -Last 1 -ExpandProperty Name
  if (-not $kitVersion) { throw "Windows SDK headers not found under $kit" }

  $include = @("$($msvc.FullName)\include", "$kit\Include\$kitVersion\ucrt",
               "$kit\Include\$kitVersion\um", "$kit\Include\$kitVersion\shared")
  $lib = @("$($msvc.FullName)\lib\x64", "$kit\Lib\$kitVersion\ucrt\x64",
           "$kit\Lib\$kitVersion\um\x64")

  # The WDK: the installed one, else Microsoft.Windows.WDK.x64 from NuGet,
  # unpacked under third_party\wdk-nuget\wdk.
  $wdk = $kit
  if (-not (Test-Path "$kit\Include\wdf\umdf")) {
    $wdk = Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\wdk-nuget\wdk\c'
  }
  $wdkVersion = Get-ChildItem "$wdk\bin" -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -like '10.*' -and (Test-Path "$($_.FullName)\x64\stampinf.exe") } |
                Sort-Object Name | Select-Object -Last 1 -ExpandProperty Name
  if ($UmdfVersion) {
    $umdfInclude = "$wdk\Include\wdf\umdf\$UmdfVersion"
    if (-not (Test-Path $umdfInclude)) {
      throw "UMDF $UmdfVersion headers not found: install the WDK or unpack its NuGet package (README)."
    }
    $include += $umdfInclude
    $lib += "$wdk\Lib\wdf\umdf\x64\$UmdfVersion"
  }

  $env:INCLUDE = $include -join ';'
  $env:LIB = $lib -join ';'
  $msvcBin = "$($msvc.FullName)\bin\Hostx64\x64"
  $llvm = "$vsRoot\VC\Tools\Llvm\x64\bin"
  $cmakeDir = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake"
  $env:PATH = "$msvcBin;$llvm;$cmakeDir\CMake\bin;$cmakeDir\Ninja;$kit\bin\$kitVersion\x64;$env:PATH"

  $toolsDir = Get-ChildItem "$wdk\Tools" -Directory -ErrorAction SilentlyContinue |
              Where-Object { Test-Path "$($_.FullName)\x64\devcon.exe" } |
              Sort-Object Name | Select-Object -Last 1
  @{
    VsRoot   = $vsRoot
    WdkRoot  = $wdk
    WdkVersion = $wdkVersion
    SdkRoot = $kit
    SdkVersion = $kitVersion
    MsvcRoot = $msvc.FullName
    Cl       = "$msvcBin\cl.exe"
    ClangCl  = "$llvm\clang-cl.exe"
    CMake    = "$cmakeDir\CMake\bin\cmake.exe"
    Ninja    = "$cmakeDir\Ninja\ninja.exe"
    InfVerif = "$($toolsDir.FullName)\x64\infverif.exe"
    DevCon   = "$($toolsDir.FullName)\x64\devcon.exe"
    SignTool = "$kit\bin\$kitVersion\x64\signtool.exe"
    Inf2Cat  = "$wdk\bin\$wdkVersion\x86\Inf2Cat.exe"
    StampInf = "$wdk\bin\$wdkVersion\x64\stampinf.exe"
  }
}
