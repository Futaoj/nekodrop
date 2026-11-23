#Requires -RunAsAdministrator
# Removes nekodrop: gives the Wi-Fi card back to Intel's driver first, then
# removes the service, the driver package, the certificate trust, the firewall
# rules and the files. Received files stay where they were saved.
param([string]$Target = "$env:ProgramFiles\nekodrop")
$ErrorActionPreference = 'Continue'
$Service = 'NekoDropService'
$CertSubject = 'CN=nekodrop driver (signed on this PC)'

'Giving the Wi-Fi card back'
if (Get-Service $Service -ErrorAction SilentlyContinue) { Stop-Service $Service -Force }
Get-Process nekodrop, nekodrop-radio, mtkradio, awdld -ErrorAction SilentlyContinue | Stop-Process -Force
& (Join-Path $Target 'NekoDropService.exe') --give-back

'Service'
if (Get-Service $Service -ErrorAction SilentlyContinue) { sc.exe delete $Service | Out-Null }

'Driver package and certificate'
$store = [xml](pnputil /enum-drivers /format xml | Out-String)
$store.PnpUtil.Driver | Where-Object { $_.OriginalName -eq 'winawdl_regproxy.inf' -and $_.ProviderName -eq 'winairdrop project' } |
  ForEach-Object { pnputil /delete-driver $_.DriverName /uninstall /force | Out-Null }
foreach ($s in 'Root', 'TrustedPublisher') {
  Get-ChildItem "Cert:\LocalMachine\$s" | Where-Object { $_.Subject -eq $CertSubject } | Remove-Item
}

'Firewall'
Get-NetFirewallRule -Group 'nekodrop AirDrop' -ErrorAction SilentlyContinue | Remove-NetFirewallRule

'Files'
Get-AppxPackage -AllUsers -Name nekodrop.shell -ErrorAction SilentlyContinue | Remove-AppxPackage -AllUsers -ErrorAction SilentlyContinue
& (Join-Path $Target 'nekodrop.exe') --unregister
Remove-Item $Target -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$env:ProgramData\nekodrop" -Recurse -Force -ErrorAction SilentlyContinue
'nekodrop is removed.'
