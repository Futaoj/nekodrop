<p align="center">
  <img src="app/assets/nekodrop.png" width="128" alt="nekodrop">
</p>

<h1 align="center">nekodrop</h1>

<p align="center">
  <b>Native AirDrop on Windows with your laptop's built-in Wi-Fi card. Wi-Fi keeps working.</b><br>
  No USB dongle, no Linux VM, no Mac.
</p>

<p align="center">
  <a href="README.md">简体中文</a> · English · <a href="../../releases/latest">Download</a> · <a href="docs/COMPATIBILITY.md">Card compatibility</a> · <a href="CHANGELOG.md">Changelog</a>
</p>

<p align="center">
  <a href="#community"><img alt="QQ group 1137780084" src="https://img.shields.io/badge/QQ%20group-1137780084-12b7f5?logo=qq&logoColor=white"></a>
  <img alt="platform" src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-0078d4">
  <img alt="speed" src="docs/badges/speed.svg">
  <img alt="card" src="docs/badges/card.svg">
  <img alt="license" src="https://img.shields.io/badge/license-GPL--2.0-blue">
</p>

<p align="center">
  <img src="docs/demo.gif" width="760" alt="Two-way AirDrop between iPhone and Windows 11 on built-in Intel AX201 while Wi-Fi stays connected">
  <br>
  <sub>Real hardware: unmodified iPhone ↔ Windows 11 on a built-in Intel AX201, Wi-Fi online the whole time.</sub>
</p>

## Highlights

- **53 MB/s both ways.** An 846 MB file in about 16 s on an Intel AX201, 0 drops ([details](#speed)).
- **Your Wi-Fi stays online.** One card runs your Wi-Fi connection and AirDrop at the same time.
- **The card you already have.** Intel AX200 / AX201 / AX210 / AX211 / 9560, Realtek RTL8852CE, MediaTek MT7902 / MT7921 / MT7922. No dongle ([all cards](#supported-cards)).
- **Works like a Mac.** iPhone, iPad and Mac see your PC in the AirDrop sheet. Send with Explorer's right-click **Send with nekodrop** or the Windows Share sheet.
- **Contacts Only** (experimental) with your Apple ID or a Mac's certificate ([how](docs/USAGE.md#contacts-only-apple-id-experimental)).
- **Android Share** (experimental, `v2.3.3`+): send and receive with OnePlus / OPPO / Xiaomi / vivo / Honor / realme phones that support the Mutual Transmission Alliance on the same Wi-Fi, and relay files between iPhone and Android ([how](docs/USAGE.md#experimental-features)).
- **Install and forget.** One installer, auto-updates, a self-healing service and a one-click restore back to Windows Wi-Fi.

## Speed

Measured on Intel AX201, Windows 11, `v2.2.9+` with High-throughput mode (HE80 + MTU 1500):

| Direction | Speed | Test |
|---|---|---|
| PC → iPhone | **53.00 MB/s** (50.55 MiB/s) | 846 MB in 15.97 s, 99.85% Block Ack, 0 drops |
| iPhone → PC | **53.28 MB/s** net (50.81 MiB/s) · 49.66 MB/s end-to-end | 846 MB in 15.84 s net / 17.00 s total, 0 drops |

How we measure: [docs/TESTING.md](docs/TESTING.md).

## Supported cards

| Vendor | Cards | Status |
|---|---|---|
| **Intel** | AX201 · AX200 · AX210 · AX211 · AX411 / Killer AX1690 · 9560 | Tested (AX201 by the author, others by users) |
| **Intel** | BE200 (Wi-Fi 7) | Experimental |
| **Realtek** | RTL8852CE | User-tested, ~24.5 MB/s (on demand) |
| **MediaTek** | MT7902 | Tested both ways, Wi-Fi at the same time |
| **MediaTek** | MT7922 / RZ616 · MT7921 / RZ608 | Experimental (MT7922 tested both ways) |

Not listed means not supported (Qualcomm, Broadcom, Intel BE201/BE211…). PCI IDs, firmware and per-card notes: **[Card compatibility](docs/COMPATIBILITY.md)** · **[Status and how to add a card](docs/CARDS.md)**.

## Install

1. Windows 10 2004+ / 11 x64, a supported card, Bluetooth on.
2. Download `nekodrop-<version>-setup.exe` from **[Releases](../../releases/latest)** and run it.
3. Click the purple cat in the tray and turn on **Allow receiving**. On the iPhone, set AirDrop to **Everyone for 10 Minutes**.

Something broke? Start menu → **nekodrop - restore Windows Wi-Fi** gives the card back to Windows. More: [Troubleshooting](docs/SUPPORT.md).

## Screenshots

<table>
  <tr>
    <td align="center"><img src="docs/screenshots/main-radar.png" width="260" alt="Main window: radar"><br><sub>Nearby devices on a radar</sub></td>
    <td align="center"><img src="docs/screenshots/send.png" width="230" alt="Send window"><br><sub>Right-click → Send with nekodrop</sub></td>
    <td align="center"><img src="docs/screenshots/wifi.png" width="240" alt="Wi-Fi window"><br><sub>nekodrop's own Wi-Fi</sub></td>
  </tr>
</table>

More screenshots and every feature: [Using nekodrop](docs/USAGE.md).

## Documentation

| | |
|---|---|
| [Using nekodrop](docs/USAGE.md) | Features, install details, modes, Wi-Fi security, Contacts Only, experimental switches, updates, uninstall |
| [Troubleshooting](docs/SUPPORT.md) | Common problems, one-click restore, the diagnostic report |
| [Card compatibility](docs/COMPATIBILITY.md) · [Card status](docs/CARDS.md) | Every card, PCI ID and firmware; how to port a new card |
| [How it works](docs/HOW-IT-WORKS.md) | Architecture, project status, building from source |
| [Credits](docs/CREDITS.md) | Contributors, where the code comes from, licenses |
| [Privacy](docs/PRIVACY.md) | Where nekodrop connects and what it sends |
| [Changelog](CHANGELOG.md) | Release history |

## Community

Join the nekodrop QQ group to chat with other users: **1137780084**

<p>
  <img src="docs/community/qq-group.jpg" width="240" alt="nekodrop QQ group QR code, group number 1137780084">
</p>

Community link: [LINUX DO](https://linux.do/).

## Contributors

Thanks to [@BaiBaiAi](https://github.com/BaiBaiAi), [@XXD051030](https://github.com/XXD051030), [@GGBond-cer](https://github.com/GGBond-cer), [@DaiJiequn](https://github.com/DaiJiequn), [@sh17632](https://github.com/sh17632), [@YiHoooong](https://github.com/YiHoooong), [@Yanlyn](https://github.com/Yanlyn) and everyone who sent logs. What each of them did: [Credits](docs/CREDITS.md#contributors).

Bug reports with a [diagnostic report](docs/SUPPORT.md#reporting-a-problem-the-diagnostic-report) are welcome in [Issues](../../issues).

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/). (Applied for: installers released before signing starts are still unsigned.)

- Committers and reviewers: [@zheznanohana](https://github.com/zheznanohana). Every pull request from an outside contributor is reviewed by the maintainer before it is merged.
- Approvers: [@zheznanohana](https://github.com/zheznanohana). Every release is approved by hand before it is signed.
- Installers are built from this repository's source by GitHub Actions ([build.yml](.github/workflows/build.yml)).
- Privacy: this program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it. See the [privacy notice](docs/PRIVACY.md).

## License

**GPL-2.0** ([LICENSE](LICENSE)); bundled components keep their own licenses ([details](docs/CREDITS.md#license)).

nekodrop is an independent project, not affiliated with or endorsed by Apple or Intel. AirDrop, iPhone, iPad and Mac are trademarks of Apple Inc.; Intel is a trademark of Intel Corporation. nekodrop replaces your Wi-Fi driver while it runs. It's experimental software, provided as is, without warranty.
