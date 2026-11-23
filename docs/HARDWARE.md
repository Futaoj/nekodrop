# Wi-Fi cards beyond the supported Intel ones

nekodrop drives the Wi-Fi chip itself, from user mode, so every chip family
needs its own driver. Today that is the Intel 22000 / AX210 family, ported from
ForbiddenItlwm's `hal_iwx` (see the table in the [README](../README.md#supported-hardware)).
This note records what it would take to support other cards, and why the first
step is only better detection in the diagnostic report.

## Which card do I have?

Run the diagnostic report (Start menu → *nekodrop - diagnostic report*). On a
PC without nekodrop, download [`installer/report.ps1`](../installer/report.ps1)
and run `powershell -ExecutionPolicy Bypass -File report.ps1`; it needs no
nekodrop install. Without a supported card, its *Summary* names the card it
found and its *Wi-Fi card* section lists every wireless card with its PCI ID
(`VEN_xxxx&DEV_xxxx`), chip, driver, Device Manager problem code and why
nekodrop does not run on it. The cards come from three sources: adapters whose
medium is native 802.11, known Wi-Fi chip IDs (which also finds a card that is
disabled or has no driver), and adapter names. Ethernet chips from the same
vendors (Realtek `10EC:8168`, Intel `8086:15xx`) are never listed.

Please post that report in an [issue](../../../issues) if you want your card
supported: real device IDs from real machines decide what comes next.

`report.ps1 -SelfTest` checks the classification against made-up IDs without
a card or administrator rights.

## Card families

| Cards | PCI IDs | Feasibility | What is missing |
|---|---|---|---|
| Intel QnJ, SnJ, Meteor Lake (Ma) | `8086:2720` `2726` `2729` `7E40` | depends on upstream | `iwxradio/src/cards.h` knows them, but ForbiddenItlwm ships no firmware for them (QuQnj API 48, SoSnj and Ma API 63). `8086:7E40` is often sold as "AX211". |
| Intel Wi-Fi 7: BE200, BE201/BE211 | `8086:272B` `A840` `7740` `E440` … | depends on upstream | ForbiddenItlwm has no Wi-Fi 7 driver. |
| Another Intel device ID | `8086:*` | maybe easy | If upstream has firmware for it, adding the ID to `cards.h`, `CardCatalog.cs`, the register proxy's INF and `install.ps1` may be all. The report says *an Intel device ID nekodrop does not know*: please post it. |
| **Realtek RTL8852CE** | `10EC:C852` | **the most feasible** | See below. |
| Other Realtek (RTL8852AE/BE, RTL8851BE, RTL8922AE, RTL8821CE/8822CE…) | `10EC:*` | low | No user-mode Windows driver; winawdl's devourer is built only for the 8852C on Windows. |
| MediaTek MT7921/7922/7925/7927/7902, AMD RZ608/616/717 | `14C3:*` | low (long project) | No user-mode Windows driver exists. Would be a port of Linux `mt76` (registers, DMA, firmware, power) with nobody here owning the hardware. |
| Qualcomm (FastConnect 6800/6900/7800), Atheros | `17CB:*` `168C:*` | low | No user-mode Windows driver path. |
| Broadcom | `14E4:*` | very low | No open driver path on Windows. |
| USB adapters | `USB\VID_*` | not planned | The register proxy and the DMA code are PCIe only. |

### Realtek RTL8852CE

[winawdl](https://github.com/raph559/winawdl), which nekodrop is built on (its
radio pipe, awdld and AirDrop engine, and the register proxy in
`driver/regproxy`), runs AirDrop on the RTL8852CE in user mode through
[devourer](https://github.com/raph559/devourer)'s `winawdl/pcie-windows`
branch. Its register proxy's INF matches only `PCI\VEN_10EC&DEV_C852`, and its
radio build turns on only devourer's `KESTREL_8852C`. RTL8852CE owners can use
winawdl today: AirDrop only, with no Wi-Fi connection while it has the card.

Bringing it into nekodrop is several high-risk changes at once:

1. **Register window.** nekodrop's register proxy maps the first memory BAR of
   at least 8 KiB (`MIN_BAR_LEN 0x2000`, Intel's CSR window). The 8852CE's
   registers are its 1 MiB BAR2, which winawdl found with a 64 KiB minimum.
   The proxy would need to pick the BAR by device, and its INF would gain a
   Realtek ID while the Windows 10 INF fix (#18, #12) still awaits
   confirmation.
2. **Build.** devourer is a git submodule built with CMake and a vcpkg
   manifest; `scripts/fetch-deps.ps1`, `scripts/build-installer.ps1` and CI
   would need it.
3. **Wi-Fi beside AirDrop.** devourer only monitors and injects; it has no
   station. nekodrop's default *together* mode (`NetMode.Concurrent`) cannot
   work, so an RTL8852CE would have to be locked to *on demand*. The service
   already does this from the card's profile (see below); the user's saved
   mode stays as chosen.
4. **Taking and giving back the card.** The service no longer assumes Intel
   (see below), but `installer/install.ps1` and `installer/restore.ps1` still
   do (`VEN_8086`, an Intel driver package to give the card back to).

### The service's card catalog

`app/service/CardCatalog.cs` is the service's one list of cards. Each
`AdapterProfile` holds the PCI vendor and device ID, the vendor's name, the
radio that drives it (`Iwx` today, `Devourer` for the RTL8852CE), its
`AdapterCapabilities` (Wi-Fi beside AirDrop, faster sending, A-MPDU sending,
nekodrop-wlan.sys, the smallest register BAR), the register proxy's INF,
service and provider, and the provider names of the vendor's own driver.
`Card.cs` takes only *active* profiles; `Supervisor.cs` runs the mode the
card can run (`CardCatalog.EffectiveMode`: *together* becomes *on demand*
without a station) and passes only the radio options the card has. The 15
Intel profiles have every capability, so nothing changes for them.

The RTL8852CE is listed but not active: it is never taken. It is there so
`NekoDropService.exe --selftest` (run by `scripts/build-installer.ps1`, so on
every CI build) checks the multi-vendor parsing, the forced *on demand* and
the provider matching without a card or administrator rights.
`NekoDropService.exe --status` prints the profile of the card it finds.

## MediaTek MT7922 / MT7921 (`mtkradio`)

Experimental user-mode support for MediaTek Connac2 cards (`14C3:0616` / `14C3:7922` MT7922 / AMD RZ616, and `14C3:7961` / `14C3:0608` MT7921 / AMD RZ608) is implemented in `mtkradio/` (and standalone at [YiHoooong/mtkradio](https://github.com/YiHoooong/mtkradio)). Like Realtek above, it runs in *on-demand* mode (`CardCatalog.cs`: `RadioBackendKind.MtkRadio`, `ConcurrentWifi = false`) via the same on-PC self-signed UMDF register proxy.

See [docs/MT7922.md](MT7922.md) for live Windows 11 benchmarks (4.5 MB/s peak RX on 394 MB transfer, 1.7 MB/s burst TX with VHT 80 MHz 2SS + HW TX BlockAck), Windows TCP buffer / timer findings, and the dual-BSS concurrent Wi-Fi roadmap.

## The steps

1. **Detection (done).** The diagnostic report names every wireless card, its
   chip and driver and why nekodrop does not run on it. Diagnostics and docs
   only: no driver, installer, service or radio change.
2. **Service card catalog (done).** `CardCatalog.cs` above: profiles and
   capabilities instead of Intel assumptions in `Card.cs` and
   `Supervisor.cs`, with an offline self-test. No change for Intel cards.
3. **Installer message.** Once the Windows 10 INF is confirmed (#18, #12),
   `install.ps1` refuses with the same classification: which card it found,
   its PCI ID, and an alternative (winawdl for an RTL8852CE).
4. **Experimental RTL8852CE.** After the register proxy picks its BAR by device
   and `restore.ps1` no longer assumes Intel: winawdl's RTL8852CE support
   behind an experimental switch, off by default (its profile becomes active
   only with the switch on), forced to *on demand* by its profile.

When adding IDs, keep `AllProfiles` in `app/service/CardCatalog.cs`,
`$SupportedIntelIds` in `installer/report.ps1`, `kDeviceIds` in
`iwxradio/src/cards.h`, the register proxy's INF and the check in
`installer/install.ps1` in step. The service self-test expects exactly the 15
Intel IDs: update its list too.
