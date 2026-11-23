#Requires -RunAsAdministrator
# Installs nekodrop from the staged folder it sits in. Changes, in order:
#   1. copies the files to %ProgramFiles%\nekodrop
#   2. makes a code-signing certificate on this PC, trusts it, signs the
#      register-proxy driver with it and deletes its private key (nothing else
#      can ever be signed with what this PC now trusts); adds the driver to the
#      driver store, which takes over nothing by itself
#   3. installs TAP-Windows if no tap0901 adapter exists (concurrent mode)
#   4. two inbound firewall rules, link-local addresses only
#   5. the NekoDropService service, started
# The Wi-Fi card is only taken by the service, and given back on uninstall.
# -DryRun says what would happen and changes nothing.
param([switch]$DryRun, [string]$Target = "$env:ProgramFiles\nekodrop")
$ErrorActionPreference = 'Stop'
$Source = $PSScriptRoot
# The setup runs this hidden: keep a log, and say what failed through the exit
# code and install-error.txt, which the setup shows.
$LogDir = Join-Path $env:ProgramData 'nekodrop'
New-Item -ItemType Directory -Force $LogDir | Out-Null
if ((Get-Item -LiteralPath $LogDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
  throw "Refusing reparse point: $LogDir"
}
& icacls.exe $LogDir /inheritance:r /grant:r "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-544:(OI)(CI)F" | Out-Null
Remove-Item (Join-Path $LogDir 'install-error.txt') -ErrorAction SilentlyContinue
if (-not $DryRun) { Start-Transcript (Join-Path $LogDir 'install.log') -Force | Out-Null }
trap {
  $_ | Out-String | Set-Content (Join-Path $LogDir 'install-error.txt') -Encoding utf8
  if (-not $DryRun) { Stop-Transcript | Out-Null }
  exit 1
}
$CertSubject = 'CN=nekodrop driver (signed on this PC)'
$TrustStores = 'Root', 'TrustedPublisher'
$FirewallGroup = 'nekodrop AirDrop'
$Service = 'NekoDropService'

function Step($what, [scriptblock]$do) {
  "  $what"
  if (-not $DryRun) { & $do }
}

if (-not (Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'PCI\\VEN_8086&DEV_(43F0|A0F0|02F0|06F0|34F0|3DF0|4DF0|2723|2725|7A70|7AF0|51F0|51F1|54F0|7F70)|PCI\\VEN_14C3&DEV_(0616|7922|7961|0608)' })) {
  throw 'This PC has no supported Wi-Fi card (Intel AX200/AX201/AX210/AX211/AX411/9560 or MediaTek MT7922/MT7921); nekodrop cannot run here.'
}

'Files'
Step 'stop an earlier service' {
  if (Get-Service $Service -ErrorAction SilentlyContinue) { Stop-Service $Service -Force -ErrorAction SilentlyContinue }
  Get-Process nekodrop, nekodrop-radio, mtkradio, awdld -ErrorAction SilentlyContinue | Stop-Process -Force
}
Step "copy to $Target" {
  New-Item -ItemType Directory -Force $Target | Out-Null
  Copy-Item "$Source\*" $Target -Recurse -Force -Exclude 'install.ps1', 'tap-windows.exe'
}

'Driver'
$driver = Join-Path $Target 'driver'
Step 'sign the register-proxy driver with a certificate made and trusted on this PC' {
  foreach ($store in $TrustStores) {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $CertSubject } | Remove-Item
  }
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
            -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable `
            -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
  try {
    $public = Join-Path $driver 'nekodrop-signing.cer'
    Export-Certificate -Cert $cert -FilePath $public | Out-Null
    foreach ($store in $TrustStores) { Import-Certificate -FilePath $public -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null }
    foreach ($file in 'winawdl_regproxy.cat', 'winawdl_regproxy.dll') {
      $r = Set-AuthenticodeSignature -FilePath (Join-Path $driver $file) -Certificate $cert -HashAlgorithm SHA256
      if ($r.Status -ne 'Valid') { throw "$file could not be signed: $($r.StatusMessage)" }
    }
    # The Explorer menu's sparse package (Windows 11 first level), same certificate.
    # Not fatal: the classic menu under "Show more options" works without it.
    $msix = Join-Path $Target 'shell\nekodrop-shell.msix'
    $signtool = Join-Path $Target 'shell\signtool.exe'
    # An earlier registration was signed by the certificate just replaced: drop it,
    # each user's client registers the new one at its next start.
    Get-AppxPackage -AllUsers -Name nekodrop.shell -ErrorAction SilentlyContinue | Remove-AppxPackage -AllUsers -ErrorAction SilentlyContinue
    if ((Test-Path $msix) -and (Test-Path $signtool)) {
      & $signtool sign /q /fd SHA256 /s My /sha1 $cert.Thumbprint $msix
      if ($LASTEXITCODE -ne 0) { "    the Explorer menu package could not be signed ($LASTEXITCODE)" }
    }
  } finally {
    Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
  }
}
Step 'add it to the driver store (binds nothing)' {
  pnputil /add-driver (Join-Path $driver 'winawdl_regproxy.inf') | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "pnputil could not add the driver ($LASTEXITCODE)" }
}

'Network'
Step "nekodrop's own TAP adapter (DHCP) for Wi-Fi beside AirDrop" {
  try { & "$Source\setup-tap.ps1" -TapSetup "$Source\tap-windows.exe" | Out-Host }
  catch { "    TAP adapter not ready ($($_.Exception.Message)); concurrent mode will fall back to on-demand" }
}
Step 'firewall: AirDrop receiver and discovery, from link-local addresses only' {
  Get-NetFirewallRule -Group $FirewallGroup -ErrorAction SilentlyContinue | Remove-NetFirewallRule
  $common = @{ Group = $FirewallGroup; Direction = 'Inbound'; Action = 'Allow'; Profile = 'Any'; RemoteAddress = 'fe80::/64' }
  New-NetFirewallRule @common -DisplayName 'nekodrop AirDrop receiver' -Protocol TCP -LocalPort 8771-8779 | Out-Null
  New-NetFirewallRule @common -DisplayName 'nekodrop AirDrop discovery' -Protocol UDP -LocalPort 5353 | Out-Null
}

'Service'
Step 'install and start NekoDropService (LocalSystem, automatic)' {
  $exe = Join-Path $Target 'NekoDropService.exe'
  if (-not (Get-Service $Service -ErrorAction SilentlyContinue)) {
    New-Service -Name $Service -BinaryPathName "`"$exe`"" -DisplayName 'nekodrop AirDrop' `
      -Description 'AirDrop on the built-in Intel Wi-Fi card: owns the card while AirDrop is on.' -StartupType Automatic | Out-Null
    sc.exe failure $Service reset= 86400 actions= restart/5000/restart/30000/""/0 | Out-Null
  }
  Start-Service $Service
}
if ($DryRun) { 'Dry run: nothing was changed.' } else { 'nekodrop is installed.'; Stop-Transcript | Out-Null }
exit 0
