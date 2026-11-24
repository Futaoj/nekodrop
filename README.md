<p align="center">
  <img src="app/assets/nekodrop.png" width="128" alt="nekodrop">
</p>

<h1 align="center">nekodrop</h1>

<p align="center">
  <b>Native AirDrop on Windows with your laptop's built-in Wi-Fi card. Wi-Fi keeps working.</b><br>
  No USB dongle, no Linux VM, no Mac.
</p>

<p align="center">
  English · <a href="README.zh-CN.md">简体中文</a> · <a href="docs/COMPATIBILITY.md">Card Compatibility</a> · <a href="CHANGELOG.md">Changelog</a>
</p>

<p align="center">
  <img alt="platform" src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-0078d4">
  <img alt="card" src="https://img.shields.io/badge/card-Intel%20AX201%20%7C%20AX200%20%7C%20AX210%20%7C%20AX211%20%7C%20BE200%20%C2%B7%20Realtek%20RTL8852CE%20%C2%B7%20MediaTek%20MT7902%20%7C%20MT7921%20%7C%20MT7922-6c5ce7">
  <img alt="license" src="https://img.shields.io/badge/license-GPL--2.0-blue">
  <img alt="status" src="https://img.shields.io/badge/status-preview-yellow">
</p>

<p align="center">
  <img src="docs/demo.gif" width="760" alt="Two-way AirDrop between iPhone and Windows 11 on built-in Intel AX201 while Wi-Fi stays connected">
  <br>
  <sub>
    <b>Real-hardware capture (unmodified iPhone ↔ Windows 11, built-in Intel AX201 on 5 GHz ch 44, Wi-Fi stays online):</b><br>
    ① iOS AirDrop discovers the PC and sends a photo → ② Accepted on Windows while WPA2 Wi-Fi stays connected → ③ Full-res (3556×2000) photo opens intact → ④ Explorer right-click → <i>Send with nekodrop</i> → ⑤ Pick nearby iPhone → ⑥ Native AirDrop banner opens in iOS Photos.
  </sub>
</p>

---

## What it does

- **Receive from iPhone, iPad and Mac.** nekodrop shows up in the AirDrop sheet like any Apple device. Files land in a folder you choose (default `Downloads\nekodrop`), and you can accept or decline each transfer from a notification.
- **Send to Apple devices.** Right-click a file in Explorer and choose **Send with nekodrop**. This is on the Windows 11 first-level menu, the classic menu and *Send to*. Then pick a nearby device.
- **Stay online.** In *Wi-Fi and AirDrop together* mode nekodrop drives the card itself. It joins your Wi-Fi on one virtual interface and runs AWDL (Apple Wireless Direct Link) on the other. Windows sees your internet connection as a wired adapter called **nekodrop**.
- **Manage Wi-Fi.** Scanning, connecting, hidden networks and saved networks with auto-connect all work. Saved Windows Wi-Fi profiles are imported, and captive portals (hotel, airport and campus sign-in pages) are detected and opened in your browser.
- **Recover by itself.** A service watches the radio, the AWDL daemon and the AirDrop engine and restarts whatever stops. It starts with Windows.
- **A small panel by the tray**, in Chinese and English, light and dark. It slides up from the tray icon or your shortcut (Win+Shift+D unless you record your own) and goes away when you click elsewhere. Nearby devices sit on a radar (or a list), with motion on Windows' own Fluent curves: a blue wave and a steady ring while sending, then green with a check mark.
- **Who can send to you, from the AirDrop mark.** Click the mark at the top left: **Receiving Off**, **Contacts Only** or **Everyone**, as on an iPhone.
- **Contacts Only (experimental).** Sign in to your Apple ID on the PC, or import a Mac's AirDrop certificate, and the PC shows up on your own iPhone like one of your Macs and works with iPhones set to Contacts Only. See [Contacts Only](#contacts-only-apple-id-experimental).
- **Windows Share sheet.** nekodrop is in Explorer's and other apps' Share panel.

## Screenshots

A small panel that slides up beside the tray (click the tray icon, or press the shortcut) and goes away when you click elsewhere: nearby iPhones and Macs on a radar (or as a list), the AirDrop mark that switches who can send to you, the status and quick controls in the title bar, settings in five tabs, one accent colour you can switch (or colourful), light and dark. While files come in, a progress ring turns around "This PC", which turns green with a check mark when they are saved. The global shortcut is Win+Shift+D unless you record your own in Settings.

<table>
  <tr>
    <td align="center"><img src="docs/screenshots/main-radar.png" width="280" alt="Main window: radar"><br><sub>Radar: a request to answer, sending, sent, declined</sub></td>
    <td align="center"><img src="docs/screenshots/main-list-dark.png" width="280" alt="Main window: list, dark"><br><sub>List, dark</sub></td>
    <td align="center"><img src="docs/screenshots/send.png" width="230" alt="Send window"><br><sub>Right-click a file → Send with nekodrop</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/screenshots/settings.png" width="280" alt="Settings"><br><sub>Settings: accent colour, your own shortcut</sub></td>
    <td align="center"><img src="docs/screenshots/settings-appleid.png" width="280" alt="Settings: Apple ID"><br><sub>Apple ID and Contacts Only (experimental)</sub></td>
    <td align="center"><img src="docs/screenshots/wifi.png" width="250" alt="Wi-Fi window"><br><sub>nekodrop's own Wi-Fi</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/screenshots/tray.png" width="260" alt="Tray menu"><br><sub>Tray menu</sub></td>
    <td align="center" colspan="2"><img src="docs/screenshots/motion-send.png" width="560" alt="Sending, frame by frame"><br><sub>Sending, frame by frame: the ring fills, turns green, a check is drawn</sub></td>
  </tr>
</table>

## Tested on real hardware

Everything below was tested by the author on their own laptop, not just in theory:

| | |
|---|---|
| Wi-Fi card | **Intel Wi-Fi 6 AX201 160MHz** (CNVi, firmware `iwlwifi-QuZ-a0-hr-b0-68`) |
| OS | Windows 11 Pro x64 |
| iPhone → Windows | ✅ works: photos and files arrive intact |
| Windows → iPhone | ✅ works: right-click → Send with nekodrop, accepted and received on the iPhone |
| AirDrop speed | PC → iPhone **30.8 MB/s** (104.8 MB in 3.4 s; 63 frames per aggregate, 99.7% acknowledged); iPhone → PC **13.5 MB/s** average for the same file (0.1.30) |
| Wi-Fi at the same time | ✅ works: the PC stays online through nekodrop's own WPA2 connection while AirDrop runs, including with Wi-Fi and AWDL on the same 5 GHz channel |

Users have since confirmed two-way AirDrop on the AX200, AX210 and AX211 as well (see the table below). Reports from other machines are welcome in [Issues](../../issues).

## Supported hardware

nekodrop talks to the Wi-Fi chip directly through modular backends (`CardCatalog` + `radio_backend.h`). Today's built-in radios support Intel 22000/AX210-family cards (`nekodrop-radio.exe`, ported from [ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm)'s `hal_iwx`, concurrent Wi-Fi + AirDrop or on-demand) and **Realtek RTL8852CE** (`10EC:C852`, `nekodrop-radio-rtl.exe`, built from [winawdl](https://github.com/raph559/winawdl)/[devourer](https://github.com/raph559/devourer), on-demand mode; no user has tested it inside nekodrop yet).

**Full multi-vendor compatibility matrix (Intel / Realtek / MediaTek / Qualcomm): [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md)** · **Release history: [CHANGELOG.md](CHANGELOG.md)** · **Driver architecture: [docs/HARDWARE.md](docs/HARDWARE.md)**

Quick summary of currently active cards:

| Card | PCI IDs | Firmware / Backend | Status |
|---|---|---|---|
| **Intel Wi-Fi 6 AX201** 160 MHz (CNVi) | `8086:` `02F0` `06F0` `34F0` `3DF0` `4DF0` `43F0` `A0F0` | `Qu/QuZ-hr-b0-68` (`iwx`, Concurrent & On-Demand) | ✅ **tested** (QuZ on `43F0`) |
| Intel Wireless-AC 9560 / 9462 / 9461 on Qu/QuZ (CNVi) | same IDs as AX201 | `Qu/QuZ-jf-b0-68` (`iwx`, Concurrent & On-Demand) | ⚠️ untested |
| Intel Wi-Fi 6 AX200 (discrete) | `8086:2723` | `cc-a0-68` (`iwx`, Concurrent & On-Demand) | ✅ two-way AirDrop in user reports ([#1](../../issues/1), [#4](../../issues/4)); 9–25 MiB/s to an iPhone with A-MPDU ([#4](../../issues/4)) |
| Intel Wi-Fi 6E AX210 (discrete) | `8086:2725` | `ty-a0-gf-a0-68` + PNVM (`iwx`, Concurrent & On-Demand) | ✅ two-way AirDrop in user reports ([#14](../../issues/14), [#18](../../issues/18), [#11](../../issues/11)) |
| Intel Wi-Fi 6E AX211 (CNVi, 12th gen+) | `8086:` `7AF0` `7A70` `51F0` `51F1` `54F0` `7F70` | `so-a0-gf-a0-68` + PNVM (`iwx`, Concurrent & On-Demand) | ✅ two-way AirDrop in user reports ([#9](../../issues/9), [#29](../../issues/29)); iPhones list the PC since 0.1.31 ([#24](../../issues/24)) |
| Intel Wi-Fi 6E AX411 / Killer AX1690 (CNVi) | same IDs as AX211 | `so-a0-gf4-a0-68` + PNVM (`iwx`, Concurrent & On-Demand) | 🧪 recognized and firmware boots ([#25](../../issues/25)); has the AX211's 0.1.31 fix, awaiting confirmation |
| Intel Wi-Fi 6 AX203 / 9560 on So | same IDs as AX211 | `so-a0-hr/jf-b0-68` (`iwx`, Concurrent & On-Demand) | ⚠️ untested |
| **Realtek RTL8852CE** | `10EC:C852` | embedded (`devourer`, On-Demand only) | ⚠️ built in, no user report yet |
| Intel SnJ `2726`, QnJ `2720`, Meteor Lake `7E40`/`2729` | `8086:` | not shipped upstream | ❌ |
| **Intel Wi-Fi 7 BE200** (discrete) | `8086:272B` | linux-firmware `gl-c0-fm-c0-100` (`iwx` Bz/MLD path, Concurrent & On-Demand) | 🧪 experimental, built in; **not yet run on a real card**: please post a diagnostic report ([docs/BE200.md](docs/BE200.md)) |
| Intel BE201 / BE211 (Wi-Fi 7, CNVi) | `8086:` `A840` `7740` | needs CNVi hardware | ❌ |
| **MediaTek MT7902** | `14C3:7902` | linux-firmware `WIFI_RAM_CODE_MT7902_1` (`mt76` port, Concurrent (5 GHz Wi-Fi) & On-Demand) | ⚠️ built in; AirDrop both ways on one card (~15 MB/s receive, ~5 MB/s send); Concurrent tested on one card (Wi-Fi beside AirDrop ~3.5 MB/s down, ~1.3 MB/s up) |
| **MediaTek MT7922 / AMD RZ616** | `14C3:0616` `14C3:7922` | linux-firmware `WIFI_RAM_CODE_MT7922_1` (`mtkradio`, Concurrent (5 GHz Wi-Fi, experimental) & On-Demand) | 🧪 experimental, built in; two-way AirDrop tested on an MT7922 / AMD RZ616 ([#40](../../pull/40), [docs/MT7922.md](docs/MT7922.md)); Concurrent mode untested on hardware |
| **MediaTek MT7921 / AMD RZ608** | `14C3:7961` `14C3:0608` | linux-firmware `WIFI_RAM_CODE_MT7961_1` (`mtkradio`, Concurrent (5 GHz Wi-Fi, experimental) & On-Demand) | ⚠️ experimental, built in; untested on hardware (On-Demand and Concurrent) |
| Other Realtek, MediaTek, Qualcomm, Broadcom | | | ❌ |

To check your card: *Device Manager → Network adapters*. On a PC without a supported Wi-Fi card, the installer refuses and lists each detected wireless card, its PCI/USB ID, why nekodrop does not run on it, and how to run `installer/report.ps1`. On an untested card (including the built-in Realtek RTL8852CE backend), nekodrop may fail to start the radio; if it does, the card stays usable: run **restore** (below) and please report the log in an [issue](../../issues).

Cards are recognized by their PCI ID. When yours is not, *Settings > Wi-Fi card* lets you pick the card and its model by hand (for hardware IDs the driver package lists). To add a new card family to nekodrop, see [docs/ADDING-A-CARD.md](docs/ADDING-A-CARD.md).

Not sure which card you have, or want another card supported? The [diagnostic report](#reporting-a-problem-the-diagnostic-report) works without nekodrop installed: download [`installer/report.ps1`](installer/report.ps1) and run `powershell -ExecutionPolicy Bypass -File report.ps1`. It lists every wireless card with its PCI ID, chip, driver and why nekodrop does or does not run on it; please post it in an [issue](../../issues). On an installed nekodrop, `NekoDropService.exe --status` prints the matched card profile, capabilities and bound driver provider, and `NekoDropService.exe --selftest` runs offline catalog and radio-launch checks across all 16 active profiles (15 Intel + Realtek RTL8852CE) without a card or administrator rights. Intel's radio logs one `backend …` line at start and runs `iwx_selftest` on every build; the Realtek radio build runs its own offline unit tests (`block_ack_test`, `reorder_buffer_test`, `outgoing_session_test`, `dma_address_test`) on every build; `install.ps1 -SelfTest`, `restore.ps1 -SelfTest` and `report.ps1 -SelfTest` likewise run offline under Windows PowerShell 5.1 on every build. What other cards would take: [docs/HARDWARE.md](docs/HARDWARE.md).

### Something went wrong? One-click restore

Start menu → **nekodrop - restore Windows Wi-Fi** (or double-click `restore.cmd` in `C:\Program Files\nekodrop`). It is standalone (works even if nekodrop's install folder is damaged), stops nekodrop, gives the card back to its vendor driver without hardcoding Intel (keeping Intel rollback behaviour unchanged), removes nekodrop's driver packages and brings Windows Wi-Fi back, whatever state nekodrop left the card in. nekodrop stays installed; run the installer again to use it. Log: `C:\ProgramData\nekodrop\restore.log`.

### Reporting a problem: the diagnostic report

Logs are what let us fix things, especially on cards we don't have. Please attach them to every [issue](../../issues).

1. Start menu → **nekodrop - diagnostic report** (or tray icon → **Diagnostic report…**, or **Diagnostic report** at the bottom of the main window). Allow the administrator prompt; the logs and driver state are readable by administrators only.
2. Two files appear on your Desktop and the report opens in Notepad:
   - `nekodrop-report-<time>.md`: a summary, a step-by-step check of the last radio start (driver bind → firmware → PNVM → init → ready), system, card, driver, network and Bluetooth details, a discovery timeline (with how many seconds each step of discovery took) and the key log lines. It is also copied to the clipboard: **paste it into the issue**.
   - `nekodrop-logs-<time>.txt`: the full logs. **Drag it into the issue** as an attachment.
3. SSIDs, MAC and private IP addresses, your user name and the computer name are replaced by placeholders such as `<SSID-1>` and `<MAC-2>`. Nothing is uploaded. Please look the report over before posting.

**On an older nekodrop without this entry**, download [`installer/report.ps1`](installer/report.ps1) and run it from a command prompt: `powershell -ExecutionPolicy Bypass -File report.ps1`. Or collect the logs by hand:

| File | What's in it |
|---|---|
| `C:\ProgramData\nekodrop\service.log` (and `service.log.1`) | the service, the radio (firmware, AWDL, Wi-Fi) and awdld; readable by administrators |
| `%LocalAppData%\nekodrop\engine.log` (and `engine.log.old`) | the AirDrop engine: discovery, Bluetooth beacon, transfers |
| `C:\ProgramData\nekodrop\install.log` | the installer: driver, TAP adapter, firewall, service |
| `C:\ProgramData\nekodrop\restore.log` | the one-click restore |

## Requirements

- Windows 10 2004 (build 19041) or later, x64 (Windows 10 22H2 confirmed working in 0.1.16+, [#18](../../issues/18)).
- A supported Wi-Fi card: Intel Wi-Fi 6/6E (Concurrent & On-Demand) or Realtek RTL8852CE `10EC:C852` (built-in On-Demand, untested inside nekodrop; see above).
- Administrator rights to install.
- Bluetooth, for sending: AirDrop wakes nearby Apple devices with a Bluetooth LE beacon.
- On the Apple side, AirDrop set to **Everyone for 10 Minutes**. *Contacts Only* needs an Apple ID certificate nekodrop doesn't have.

## Install

1. Download `nekodrop-<version>-setup.exe` from [Releases](../../releases) and run it.
2. The installer:
   - copies the files to `C:\Program Files\nekodrop`;
   - creates a code-signing certificate **on your PC**, signs the small register-proxy driver with it, trusts it, and **deletes the private key**, so nothing else can ever be signed with it;
   - creates the TAP adapter **nekodrop** (TAP-Windows, signed by OpenVPN);
   - adds two inbound firewall rules, link-local addresses only (TCP 8771-8779, UDP 5353);
   - installs and starts the **NekoDropService** service.
3. The tray icon (the purple cat) appears. Open it and turn on **Allow receiving**.

On first start nekodrop takes the Wi-Fi card away from the Intel driver and reconnects you to the network Windows was using. The Windows Wi-Fi icon disappears; use nekodrop's **Wi-Fi…** window instead.

### Updates

nekodrop checks GitHub for a newer release a minute after it starts and then every 6 hours. When it finds one, a notification offers **Update**. nekodrop then downloads the installer, checks it against the SHA-256 GitHub publishes for it, and runs it (Windows asks for administrator rights). It never installs anything on its own. *Settings → Updates* turns the check off, chooses whether preview releases count, and has **Check now**. There are two channels: **Stable** releases, and preview releases built as fixes land. Previews are included by default; untick *Include preview releases* to get only stable ones.

### Uninstall

Use *Settings → Apps → nekodrop → Uninstall*, or run `uninstall.ps1` in the install folder. Uninstalling gives the card back to the Intel driver, then removes the service, the driver package, the certificate trust and the firewall rules.

## Usage

| I want to… | Do this |
|---|---|
| Receive | Keep receiving on (the AirDrop mark at the top left is coloured, not grey). On the iPhone, tap Share → AirDrop and pick your PC's name |
| Choose who can send to me | Click the AirDrop mark: **Receiving Off**, **Contacts Only** (experimental, Settings → Apple ID) or **Everyone** |
| Send | Right-click files → **Send with nekodrop** → pick a device. Or select files in Explorer and press the shortcut (Win+Shift+D by default) |
| Change Wi-Fi | Tray → **Wi-Fi…**: pick a network, enter the password, Connect |
| Hidden network | **Wi-Fi…** → *Hidden network*: type the SSID and password |
| Sign in to hotel Wi-Fi | Connect; the sign-in page opens in your browser by itself |
| Give the card back to Windows | Tray → *Wi-Fi and AirDrop* → **Take the card on demand** or **AirDrop off** |
| Change name, folder or language | Tray → **Settings…** |
| Change the shortcut | Settings → General: click the shortcut, press the new keys |

### Modes

| Mode | Internet | AirDrop |
|---|---|---|
| **Wi-Fi and AirDrop together** (default) | Through nekodrop's own Wi-Fi connection (adapter "nekodrop") | Always available |
| **Take the card on demand** | The normal Intel driver | nekodrop takes the card when an iPhone nearby opens AirDrop or you send. Internet drops during the transfer |
| **AirDrop off** | The normal Intel driver | Off |

### Wi-Fi security

| Network | Together mode |
|---|---|
| Open | ✅ |
| WPA/WPA2-Personal | ✅ |
| WPA2/WPA3 transition | ✅ (joins with WPA2) |
| WPA2 with protected management frames required, WPA2-PSK-SHA256 | 🧪 0.1.12, untested on hardware |
| Captive portal | ✅ detected, sign-in page opened |
| Hidden SSID | ✅ |
| WPA3-Personal only (SAE) | 🧪 0.1.12, untested on hardware (hash-to-element-only networks, mostly 6 GHz, not yet) |
| Enhanced Open (OWE) | 🧪 0.1.12, untested on hardware |
| WPA2/WPA3-Enterprise (802.1X, PEAP…) | ❌ not yet. nekodrop switches to *on demand* |
| WEP, WPA/TKIP | ❌ (insecure; not planned) |

### Contacts Only (Apple ID, experimental)

Settings → **Apple ID**: turn on the experimental switch at the top, then choose who can send to this PC, **Everyone** or **Contacts Only**. Contacts Only needs an AirDrop identity like a Mac's, which you get one of two ways:

- **Sign in with Apple ID** on this PC. nekodrop asks Apple for an AirDrop certificate the way a Mac does (WirDrop's `appleid`, MIT), keeps it renewed and reads your iCloud contacts. The first sign-in downloads a component of iCloud for Windows from Apple (about 160 MB; iCloud isn't installed). The password is used once and never stored.
- **Import from a Mac** signed in to your Apple ID: export `certificate.pem`, `key.pem` and `validation_record.cms` with [airdrop-keychain-extractor](https://github.com/seemoo-lab/airdrop-keychain-extractor) (it needs SIP partly off), pick their folder, then import your contacts as a `.vcf`.

With an identity, your own iPhone lists this PC like one of your Macs, and iPhones set to Contacts Only see it when your Apple ID's e-mail or phone number is in their contacts. The receiver's Contacts Only filter reads the sender's record without checking Apple's signature, so it is a filter, not proof. Confirmed working by the maintainer on 2.1.0.

### Experimental features

In *Settings → Experimental*, all off by default; turning one off restores the old behaviour exactly. (Graduated from here and always on: aggregated sending, A-MPDU, in 0.1.35: 30.8 MB/s to an iPhone on AX201, 9–25 MiB/s on AX200; balanced Bluetooth coexistence on Intel cards in 0.1.37, which lets the Bluetooth beacon through so the PC finds iPhones and Macs in seconds, measured on AX200 and AX211: thanks to [@XXD051030](https://github.com/XXD051030), [#26](../../pull/26).)

- **Time sharing when Wi-Fi and AirDrop use different channels**: tells the iPhone when the PC is away with the router. With nekodrop's Wi-Fi on another 5 GHz channel, one AX211 heard no AirDrop frame without it ([#35](../../issues/35)); on one AX200 speeds went both up and down ([#4](../../issues/4)).
- **Faster unlock of a passive 5 GHz channel**: scans sooner so cards that refuse the country code can transmit earlier ([#11](../../issues/11)).

## Status

| Part | State |
|---|---|
| User-mode Intel driver: firmware boot, monitor, inject, scan | ✅ works on AX201, AX200, AX210, AX211 (user reports for the last three) |
| iPhone → Windows | ✅ works (photos received intact, hash-checked) |
| Windows → iPhone | ✅ works (AX201; AX200, AX210, AX211 in user reports); up to 30.8 MB/s with A-MPDU |
| Wi-Fi and AirDrop at the same time | ✅ works (WPA2, DHCP, DNS through the "nekodrop" adapter) |
| Wi-Fi manager, saved networks, captive portal | ✅ |
| Service, tray app, notifications, context menu, installer | ✅ |
| Tray panel, radar, Share sheet, custom shortcut | ✅ 2.0.2 |
| Contacts Only (Apple ID sign-in or a Mac's certificate) | ✅ 2.1.0, experimental (confirmed by the maintainer) |
| WPA3-only, OWE | 🧪 0.1.12, untested on hardware |
| 802.1X (Enterprise) | ⏳ planned |
| AX200, AX210, AX211 | ✅ two-way AirDrop in user reports |
| AX411 / Killer AX1690 | 🧪 recognized and firmware boots ([#25](../../issues/25)); AirDrop awaiting confirmation |
| 9560 on Qu/So, AX203 | 🧪 ported from upstream, no user report yet |
| Windows 10 | ✅ two-way AirDrop confirmed in user reports on 0.1.16+ ([#18](../../issues/18)) |
| Kernel WiFiCx driver (`kdriver/`) | 🧊 shelved: experimental and **not installed** by the setup. See [docs/KERNEL.md](docs/KERNEL.md) |

## How it works

```
 nekodrop.exe (tray app, your session) ──JSON pipe──► NekoDropService (SYSTEM)
   │  AirDrop engine (Python)                         │ takes/gives back the card
   │  notifications · context menu · send window      │ Wi-Fi manager, watchdogs
   │                                                  ▼
   └── awdl0 (Wintun, IPv6) ◄── awdld (AWDL, OWL) ◄─pipe─ nekodrop-radio (user-mode Intel driver)
                                                          ├ MAC 0: AWDL on channel 6/44/149
     "nekodrop" TAP adapter ◄── Ethernet bridge ◄─────────┤ MAC 1: station, joined to your router
     (Windows' internet)                                  └ UMDF register proxy + user-mode DMA → AX201
```

1. **Register proxy.** A tiny UMDF driver (`driver/regproxy`) binds to the AX201 in place of Intel's driver. It does nothing but map the card's registers and interrupts into one user-mode process. No kernel code of ours runs.
2. **User-mode driver.** `nekodrop-radio` (`iwxradio/`) is a port of the OpenBSD/itlwm `iwx` driver to user mode. It loads Intel's firmware through user-mode DMA, then runs two MACs at once: one listens and injects AWDL frames, the other is a normal Wi-Fi station with WPA2 (4-way handshake, keys in firmware, rate control).
3. **AWDL.** `awdld` (OWL, the Open Wireless Link) speaks Apple's AWDL protocol over the radio pipe and exposes the link as the IPv6 interface `awdl0`.
4. **AirDrop.** A Python engine speaks AirDrop over `awdl0`: mDNS discovery, HTTPS on port 8771 (spare ports 8772-8779), the `/Discover`, `/Ask` and `/Upload` flow, and the BLE beacon used for sending.
5. **Internet.** Frames from the station MAC are bridged to the TAP adapter "nekodrop". Windows treats it as Ethernet and gets an address by DHCP.

Design notes: [docs/DESIGN.md](docs/DESIGN.md) (driver, DMA, risks) · [docs/CLIENT.md](docs/CLIENT.md) (service, client, modes) · [docs/TESTING.md](docs/TESTING.md) (throughput test method).

## Troubleshooting

| Symptom | Try |
|---|---|
| iPhone doesn't see the PC | Set the iPhone's AirDrop to *Everyone for 10 Minutes*, turn it off and on (this clears a cached record), and keep the phone unlocked nearby |
| The PC takes long to find the iPhone | Check that Bluetooth is on: the PC wakes iPhones with a Bluetooth beacon. The diagnostic report's engine lines say `beacon: false` and why when it cannot send one |
| Sending to the iPhone is slow | If nekodrop's Wi-Fi is on a different 5 GHz channel than AirDrop (44 or 149), speed drops; disconnecting Wi-Fi in nekodrop or moving the router to that channel helps |
| AirDrop stops working once nekodrop joins Wi-Fi | The router's 5 GHz channel differs from AirDrop's (44 or 149): turn on *Settings → Experimental → Time sharing when Wi-Fi and AirDrop use different channels*, or disconnect Wi-Fi in nekodrop if the PC is wired ([#35](../../issues/35)) |
| The PC shows up twice on the iPhone | A cached record from an earlier run. Toggle AirDrop on the iPhone |
| No internet | Tray → **Wi-Fi…**: check you're connected. The network may need a sign-in page or be WPA3-only/802.1X (see above) |
| The Windows Wi-Fi icon disappeared | Expected in *together* mode: nekodrop owns the card. Use **Wi-Fi…**, or switch to *on demand* |
| An app or game refuses to log in / shows no QR code in *together* mode | Some apps want to see a Windows Wi-Fi adapter, and in *together* mode there is none (nekodrop's adapter is a wired-type one). Switch to *on demand* for those apps ([#33](../../issues/33)) |
| Something else | Open an [issue](../../issues) with the [diagnostic report](#reporting-a-problem-the-diagnostic-report) |

## Building from source

Needs Visual Studio 2022 Build Tools (C++, Windows SDK, CMake, vcpkg), the .NET 8 SDK and Git. Inno Setup 6 is optional (without it you get a zip). No WDK install is needed: the NuGet package is used.

```powershell
git clone https://github.com/zheznanohana/nekodrop.git
cd nekodrop
.\scripts\fetch-deps.ps1        # winawdl, devourer, ForbiddenItlwm (firmware), MinGW, WDK NuGet → third_party\
.\scripts\build-installer.ps1   # everything → build\nekodrop-<version>-setup.exe
.\scripts\build-radio.ps1       # just the Intel user-mode driver, plus its offline self-tests
.\scripts\build-radio-rtl.ps1   # just the Realtek RTL8852CE radio (devourer), plus its offline unit tests
```

| Folder | What |
|---|---|
| `iwxradio/` | User-mode Intel driver (C++): firmware, AWDL monitor/inject, station, WPA2, UMAC scan |
| `driver/regproxy/` | UMDF register-proxy driver |
| `app/service/` | NekoDropService (C#): card ownership, radio supervision, Wi-Fi manager |
| `app/client/` | Tray app (C# WPF): UI, notifications, AirDrop engine host |
| `app/shellext/` | Windows 11 context-menu handler |
| `installer/` | Inno Setup script, install/uninstall scripts, TAP setup |
| `kdriver/` | Shelved kernel WiFiCx driver (experimental, not shipped) |

## Where the code comes from

nekodrop is built on other open-source projects. This section says exactly what was **used as source code** (copied, ported or bundled) and what was only **studied as a reference**.

### Source code used

| Project | License | What nekodrop takes from it | Where it ends up |
|---|---|---|---|
| [retropineaple/ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) 0.6.0 (itlwm fork, macOS) | GPL-2.0 | The Intel 22000-series driver `hal_iwx`: firmware loading, TLV parsing, context info, TX/RX rings, firmware commands, the register header `if_iwxreg.h`. Its AWDL strategies too: channel following (the 44/149 min rule, country-code learning), receive Block Ack, promiscuous off during BA, rate stepping, host-side unicast resend, beacon hints | `iwxradio/`, ported from kernel C to a user-mode C++ driver |
| ForbiddenItlwm's firmware folder | Intel redistribution license | iwlwifi firmware `iwlwifi-Qu*-hr-b0-68.ucode` | shipped in `radio\firmware\` |
| [raph559/winawdl](https://github.com/raph559/winawdl) | MIT / GPL-2.0 (`radio/`) | The way to take over a PCIe Wi-Fi card from user mode on Windows: the UMDF register-proxy driver, user-mode DMA, the radio pipe protocol, and the RTL8852CE radio daemon (`radio/`) | `driver/regproxy/` (Intel + RTL8852CE PCI IDs), `iwxradio/` (pipe and DMA), `radio\nekodrop-radio-rtl.exe` |
| [raph559/devourer](https://github.com/raph559/devourer) (`winawdl/pcie-windows`) | GPL-2.0 | Realtek RTL8852CE user-mode PCIe driver (`KESTREL_8852C`, embedded firmware and A-MPDU stack) | built into `radio\nekodrop-radio-rtl.exe` (`licenses\devourer-GPL-2.0.txt`) |
| winawdl `awdl/` (`awdld`) | GPL-3.0-or-later (builds on OWL) | The AWDL daemon: OWL's protocol core with a Windows I/O layer and Wintun | built from source, shipped as `awdl\awdld.exe` |
| winawdl `airdrop/` | MIT | The AirDrop engine in Python: mDNS, the HTTPS receiver, the sender, the BLE beacon | shipped as `airdrop\` with two small patches (`scripts/patch-engine.py`: stable instance name, spare ports) |
| [JungyoKim/WirDrop](https://github.com/JungyoKim/WirDrop) | MIT | Its Apple ID sign-in, `appleid` and `appleid-anisette`: Apple's sign-in, the AirDrop certificate from aidc.apple.com, the iCloud contacts. Its identity layout and the record / hash handling of owly-airdrop (MIT) guided `installer/nekodrop_identity.py` | `appleid\appleid.exe`, `appleid\appleid-anisette.exe`, built from source with one patch (`scripts/patch-appleid.py`: phone numbers default to +86) |
| [seemoo-lab/owl](https://github.com/seemoo-lab/owl) | GPL-3.0 | The AWDL implementation inside `awdld` | through winawdl |
| [Wintun](https://www.wintun.net/) | Wintun license | The `awdl0` virtual interface | `awdl\wintun.dll` (prebuilt) |
| [TAP-Windows6](https://github.com/OpenVPN/tap-windows6) | GPL-2.0 / OpenVPN | The "nekodrop" virtual Ethernet adapter for Windows' internet | installed by the setup (signed OpenVPN installer) |

### Studied as references (no code copied)

| Project | What it was used for |
|---|---|
| OpenBSD `iwx(4)`, Linux `iwlwifi` | How the firmware, UMAC scan, station mode and keys work |
| [seemoo-lab/opendrop](https://github.com/seemoo-lab/opendrop) | The AirDrop protocol (`/Discover`, `/Ask`, `/Upload`, mDNS records) |
| [microsoft/Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples) | WiFiCx structure for the shelved kernel driver (`kdriver/`) |
| [t4t5/owly](https://github.com/t4t5/owly), [JungyoKim/WirDrop](https://github.com/JungyoKim/WirDrop) | Other AirDrop-on-other-platforms attempts, compared during design |

### Written for nekodrop

- `iwxradio/`: the user-mode port itself, plus everything Windows-specific: running two MACs at once (AWDL listener plus Wi-Fi station on its own PHY), station mode with WPA2 (4-way handshake, keys in firmware, TLC rate control), bridging to TAP, firmware UMAC scan, beacon parsing and security detection.
- `app/service/`: the Windows service: card ownership, radio and awdld supervision, the Wi-Fi manager (DPAPI-encrypted saved networks, auto-connect, hidden networks, captive-portal detection).
- `app/client/`: the tray app (WPF), notifications, the send window, the Wi-Fi window, Chinese/English UI.
- `app/shellext/`: the Windows 11 context-menu handler.
- `installer/`: the Inno Setup installer, the on-PC driver signing, TAP setup and uninstall.

## Contributors

Thanks to everyone who sent code and careful test reports:

- [@BaiBaiAi](https://github.com/BaiBaiAi): Windows → iPhone sending without a client certificate ([#13](../../pull/13)), pipelined AWDL transmit so large uploads finish ([#15](../../pull/15)), and the first AX210 test report ([#14](../../issues/14)).
- [@XXD051030](https://github.com/XXD051030): the optional balanced Bluetooth coexistence mode for Intel cards, so the PC finds iPhones and Macs in seconds instead of minutes, with a careful before/after study on an AX200 ([#26](../../pull/26)).
- [@GGBond-cer](https://github.com/GGBond-cer): many detailed AX200 test reports that pinned down the A-MPDU and speed problems ([#4](../../issues/4)), the root cause of the uninstaller leftovers ([#22](../../issues/22)), the throughput testing guide ([#28](../../pull/28)), and the uninstaller fix that keeps Setup from being killed by its own cleanup step ([#38](../../pull/38)), deleting the downloaded installer after an update ([#44](../../pull/44)), and keeping the panel and file dialogs on one screen with mixed-DPI monitors ([#49](../../pull/49)).
- [@DaiJiequn](https://github.com/DaiJiequn): MediaTek MT7902 support, a user-mode port of Linux mt76 that runs AirDrop both ways on demand ([#34](../../pull/34)), and its Concurrent mode, Wi-Fi beside AirDrop with the router's A-MPDUs, whose shared station code MT7921/MT7922 now use too ([#47](../../pull/47)).
- [@sh17632](https://github.com/sh17632): the AirDrop-style radar and Share sheet design that became the 2.0 interface ([#36](../../issues/36)), and AX211 test reports ([#35](../../issues/35)).
- [@YiHoooong](https://github.com/YiHoooong): MediaTek MT7922/MT7921 user-mode radio (mtkradio), tested on an MT7922 / AMD RZ616 ([#40](../../pull/40)).

## License

nekodrop as a whole is **GPL-2.0** (see [LICENSE](LICENSE)), because `iwxradio/` derives from itlwm/ForbiddenItlwm.

- `if_iwxreg.h`: dual BSD/GPL-2.0.
- `driver/regproxy` and the AirDrop engine: keep winawdl's MIT license.
- `nekodrop-radio-rtl.exe` (`raph559/winawdl` `radio/` + `raph559/devourer`): GPL-2.0 (`licenses\devourer-GPL-2.0.txt`).
- `awdld`: GPL-3.0-or-later, from winawdl/OWL. It is a separate program talking over a pipe.
- iwlwifi firmware: Intel's redistribution license (`licenses\iwlwifi-firmware.txt`).
- Wintun and TAP-Windows: their own licenses.

The installer copies every license into `licenses\`.

## Community

- [LINUX DO](https://linux.do/) — friendly community link

## Disclaimer

nekodrop is an independent project, not affiliated with or endorsed by Apple or Intel. AirDrop, iPhone, iPad and Mac are trademarks of Apple Inc.; Intel is a trademark of Intel Corporation. nekodrop replaces your Wi-Fi driver while it runs. It's experimental software, provided as is, without warranty.
