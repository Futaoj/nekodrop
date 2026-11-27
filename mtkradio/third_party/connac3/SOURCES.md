# MT7925 Connac3 sources

The firmware encoder, ROM download sequence, NIC capabilities, EFUSE query,
passive-scan command and event handling, and CLC and regulatory decoders come from the portable LuoboDrop MT7925
bring-up code in its source handoff. The reference code is derived from the
BSD-3-Clause-Clear MT7925 PCIe port at
[revision 74606871](https://github.com/Dy1an-Zou/Airdrop-for-Windows/tree/74606871b001bdba5e447f84a43bb96124fb495a/driver/mt7925)
and the upstream
[mt76 MT7925 driver at 4d7f2bed](https://github.com/openwrt/mt76/tree/4d7f2bed49cde7f1aa3b6e6e2d0bd8dc869f722e/mt7925).
The portable board SAR decoder and channel/SKU policy come from LuoboDrop's
MT7925 regulatory code and retain the licensing notices in the source files.

The CLC caller is adapted to the user-mode transport and preflights the
selected payloads against its actual descriptor buffer capacity. Windows
MMIO, DMA allocation, receive polling and the CLI integration live in
nekodrop's own code. [LICENSE](LICENSE) contains the Clear BSD license.

The Connac3 receive Block Ack station commands in `src/mt7925_ba.h` follow the
[upstream Linux MT7925 MCU layout](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/mediatek/mt76/mt7925/mcu.c).
