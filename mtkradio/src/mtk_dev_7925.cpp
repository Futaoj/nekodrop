// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT7925 (Wi-Fi 7 Connac3) user-mode PCIe backend for MtkDevice.
 *
 * PCIe ownership, WFDMA register sequence, MCU replies and RX layouts
 * follow the LuoboDrop MT7925 bring-up and pinned OpenWrt mt76 reference.
 * Transmit DMA and BSS state are left disabled in passive monitor mode.
 */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>

#include "crypto.h"
#include "mt7925_regs.h"
#include "mt7925_active.h"
#include "mt7925_mcu_wait.h"
#include "mt7925_rx.h"
#include "mt7925_sar.h"
#include "mt7925_txpower_info.h"

extern "C" {
#include "clc_power.h"
#include "efuse_type.h"
#include "nic_caps.h"
#include "passive-scan.h"
#include "station-reg.h"
}
#include "mtk_dev.h"
#include "mtk_platform.h"

namespace mtk {

namespace {

uint64_t now_ms() { return plat::now_ms(); }

inline uint16_t rd16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t rd32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string hex32(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", v);
  return buf;
}

std::string mac_str(const uint8_t mac[6]) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                mac[4], mac[5]);
  return buf;
}

bool read_pinned_firmware(const std::string &path, const std::array<uint8_t, 32> &digest,
                          std::vector<uint8_t> &out, std::string &error) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
  if (!f) {
    error = "cannot open firmware file: " + path;
    return false;
  }
  const auto sz = f.tellg();
  if (sz <= 0 || sz > MT_FW_MAX_SIZE) {
    error = "invalid firmware file size: " + path;
    return false;
  }
  out.resize(static_cast<size_t>(sz));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char *>(out.data()), sz)) {
    error = "failed reading firmware file: " + path;
    return false;
  }
  std::array<uint8_t, 32> actual{};
  if (!crypto::sha256(out.data(), out.size(), actual.data()) || actual != digest) {
    error = "firmware SHA-256 mismatch: " + path;
    return false;
  }
  return true;
}

constexpr uint16_t kBasicRates[] = {0, 1, 2, 3, 75, 79, 74, 78, 73, 77, 72, 76};

} // namespace

/* --------------------------------------------------------------------------
 * Address translation & L1 remap (MT7925)
 * -------------------------------------------------------------------------- */
uint32_t MtkDevice::mt7925_remap_l1(uint32_t addr) {
  const uint32_t offset = addr & mt7925::HIF_REMAP_L1_OFFSET;
  const uint32_t base = (addr & mt7925::HIF_REMAP_L1_MASK) >> 16;

  if (_l1_base != base) {
    uint32_t v = _proxy.read32(mt7925::HIF_REMAP_L1);
    if (v == UINT32_MAX) return UINT32_MAX;
    v = (v & ~mt7925::HIF_REMAP_L1_MASK) | (base << 16);
    _proxy.write32(mt7925::HIF_REMAP_L1, v);
    if (_proxy.read32(mt7925::HIF_REMAP_L1) != v) return UINT32_MAX;
    _l1_base = base;
  }
  return mt7925::HIF_REMAP_BASE_L1 + offset;
}

uint32_t MtkDevice::mt7925_translate_addr(uint32_t addr) {
  if (addr < _proxy.bar_len()) return addr;

  for (const auto &e : mt7925::kFixedMap) {
    if (addr >= e.phys && addr < e.phys + e.size) {
      return e.mapped + (addr - e.phys);
    }
  }
  return mt7925_remap_l1(addr);
}

/* --------------------------------------------------------------------------
 * Ownership, EMI protection, WFSYS reset, Probe & Hard reset (MT7925)
 * -------------------------------------------------------------------------- */
bool MtkDevice::mt7925_fw_own() {
  _proxy.write32(mt7925::CONN_ON_LPCTL, 1u); /* PCIE_LPCR_HOST_SET_OWN */
  const uint64_t deadline = now_ms() + 500;
  while (now_ms() < deadline) {
    const uint32_t v = _proxy.read32(mt7925::CONN_ON_LPCTL);
    if (v != 0xffffffffu && (v & 4u) == 4u) return true;
    plat::sleep_ms(1);
  }
  return false;
}

bool MtkDevice::mt7925_driver_own(std::string &error) {
  _proxy.write32(mt7925::CONN_ON_LPCTL, 2u); /* PCIE_LPCR_HOST_CLR_OWN */
  plat::sleep_ms(3);
  const uint64_t deadline = now_ms() + 1000;
  uint32_t last = 0;
  while (now_ms() < deadline) {
    last = _proxy.read32(mt7925::CONN_ON_LPCTL);
    if (last != 0xffffffffu && (last & 4u) == 0u) return true;
    plat::sleep_ms(2);
  }
  error = "MT7925 driver-own failed (LPCTL=" + hex32(last) + ")";
  return false;
}

bool MtkDevice::mt7925_emi_protection(std::string &error) {
  const uint32_t saved = _proxy.read32(mt7925::HIF_REMAP_L1);
  if (saved == UINT32_MAX) {
    error = "MT7925 EMI remap register unavailable";
    return false;
  }
  const uint32_t off = mt7925_remap_l1(mt7925::EMI_PROT_ADDR);
  if (off == UINT32_MAX) {
    _proxy.write32(mt7925::HIF_REMAP_L1, saved);
    _l1_base = UINT32_MAX;
    error = "MT7925 EMI remap setup failed";
    return false;
  }
  const uint32_t v = _proxy.read32(off);
  if (v != UINT32_MAX) _proxy.write32(off, v | 2u);
  const uint32_t programmed = _proxy.read32(off);
  _proxy.write32(mt7925::HIF_REMAP_L1, saved);
  _l1_base = UINT32_MAX;
  if (v == UINT32_MAX || programmed == UINT32_MAX || !(programmed & 2u) ||
      _proxy.read32(mt7925::HIF_REMAP_L1) != saved || _proxy.failed()) {
    error = "MT7925 EMI protection setup failed";
    return false;
  }
  return true;
}

bool MtkDevice::mt7925_wfsys_reset(std::string &error) {
  const uint32_t v = _proxy.read32(mt7925::WFSYS_SW_RST_B);
  if (v == 0xffffffffu) {
    error = "MT7925 WFSYS_SW_RST_B read failed (0xffffffff)";
    return false;
  }
  _proxy.write32(mt7925::WFSYS_SW_RST_B, v & ~1u);
  plat::sleep_ms(50);
  _proxy.write32(mt7925::WFSYS_SW_RST_B, (v & ~1u) | 1u);

  const uint64_t deadline = now_ms() + 1000;
  uint32_t cur = 0;
  while (now_ms() < deadline) {
    cur = _proxy.read32(mt7925::WFSYS_SW_RST_B);
    if (cur != 0xffffffffu && (cur & 0x11u) == 0x11u) {
      log("MT7925 wfsys reset complete (WFSYS_SW_RST_B=" + hex32(cur) + ")");
      return true;
    }
    plat::sleep_ms(5);
  }
  error = "MT7925 wfsys reset timeout (WFSYS_SW_RST_B=" + hex32(cur) + ")";
  return false;
}

bool MtkDevice::is_e138_instance(const std::wstring &instance) {
  constexpr std::wstring_view prefix = L"PCI\\VEN_14C3&DEV_7925&SUBSYS_E138105B&REV_00\\";
  return instance.size() > prefix.size() &&
         _wcsnicmp(instance.c_str(), prefix.data(), prefix.size()) == 0 &&
         instance.find(L'\\', prefix.size()) == std::wstring::npos;
}

bool MtkDevice::check_e138_passive(const std::wstring &instance, const std::string &firmware,
                                  StopCheck stop, uint32_t &beacons, unsigned &frames,
                                  std::string &error, const Logger &report) {
  beacons = 0;
  frames = 0;
  if (!is_e138_instance(instance) || !std::filesystem::u8path(firmware).is_absolute() || !stop || stop()) {
    error = "passive check stopped or invalid instance/firmware path";
    return false;
  }

  // The device never escapes this function. Its inactive default builds only
  // MCU/download and RX rings. No frame contents or addresses leave the scan.
  auto dev = std::make_unique<MtkDevice>(Logger{});
  dev->_stopping = std::move(stop);
  Options options;
  options.instance_id = instance;
  options.country = "00";
  options.firmware_dir = firmware;
  if (!dev->open(options, error)) return false;
  if (dev->_tx_data.count || dev->_mt7925_power_ready || dev->_mt7925_active_lab || dev->_stopping()) {
    error = "passive scan stopped or unexpected active radio state";
    return false;
  }
  const auto count = [&](const RxFrame &) { ++frames; };
  if (!dev->mt7925_passive_scan(44, count, nullptr, 250, &beacons) || dev->_stopping()) {
    error = "channel 44 passive scan stopped, refused or incomplete";
    return false;
  }
  // Scanning may have restored a different RF channel. Report the firmware's
  // channel-band field verbatim; this is telemetry, never an input to TX policy.
  mt7925::TxPowerReadback info{};
  const auto &request = mt7925::txpower_read_request;
  std::string observed;
  const int status = dev->mt7925_exchange(mt7925::txpower_command, request.data(), sizeof(request), 1000,
      [&](uint8_t seq, const uint8_t *packet, uint32_t size) {
        if (size >= 56 && packet[37] == seq && !(packet[38] & 4) && packet[36] != 1)
          observed = " event=" + hex32(packet[36]) + " declared=" + std::to_string(rd16(packet + 32)) +
                     " tlv=" + std::to_string(rd16(packet + 50)) + " category=" + std::to_string(packet[52]) +
                     " phy=" + std::to_string(packet[53]);
        return mt7925::decode_txpower_info(seq, packet, size, info);
      });
  if (dev->_stopping()) {
    error = "passive check stopped during firmware power readback";
    return false;
  }
  if (report) {
    if (status == MT_OK) {
      std::string result = "MT7925 current TMAC table: phy=" + std::to_string(info.phy) +
          " channel_band_raw=" + std::to_string(info.channel_band) + " format_raw=" + std::to_string(info.format) +
          " tag=" + std::to_string(info.tag) + " available_rates=" + std::to_string(info.available) +
          "/" + std::to_string(mt7925::txpower_rate_count);
      if (info.available)
        result += " raw_min=" + std::to_string(info.lowest) + " raw_max=" + std::to_string(info.highest);
      report(result);
    } else {
      report("MT7925 current TMAC table unavailable: transport=" + std::to_string(status) + observed);
    }
  }
  return true;
}

bool MtkDevice::mt7925_probe(std::string &error) {
  // Both modes start with the firmware's passive world domain. The active
  // paths read platform limits when calculating power, before enabling TX.
  _is_mt7925 = true;
  _l1_base = 0xffffffffu;

  if (_proxy.subsystem() != 0xe138u) {
    error = "MT7925 backend is enabled for the validated E138 subsystem";
    return false;
  }
  if (_proxy.bar_len() < mt7925::HIF_REMAP_L1 + sizeof(uint32_t)) {
    error = "MT7925 BAR0 too small: " + hex32(_proxy.bar_len());
    return false;
  }

  // The emergency RegProxy close path can leave the already-quiesced
  // radio in reset. The exclusive next client can release this local reset
  // before attempting LPCTL ownership.
  const uint32_t reset = _proxy.read32(mt7925::WFSYS_SW_RST_B);
  if (reset != UINT32_MAX && !(reset & 1u) && !mt7925_wfsys_reset(error)) return false;

  /* Take ownership before reading L1-mapped CBTOP registers. */
  (void)mt7925_fw_own();
  if (!mt7925_driver_own(error)) return false;
  _mt7925_owns_device = true;

  _chip_id = l1_rr(MT_HW_CHIPID) & 0xffffu;
  _hw_rev = l1_rr(MT_HW_REV) & 0xffu;

  if (_chip_id == 0xffffu || _chip_id == 0u) {
    error = "MT7925 chip ID read failed: " + hex32(_chip_id);
    return false;
  }
  if (_chip_id != 0x7925u) {
    error = "unexpected MT7925 chip ID: " + hex32(_chip_id);
    return false;
  }

  const uint32_t misc = _proxy.read32(mt7925::CONN_ON_MISC);
  const uint32_t glo = _proxy.read32(mt7925::WFDMA0_GLO_CFG);
  log("MT7925 probed: chip_id=" + hex32(_chip_id) + " hw_rev=" + hex32(_hw_rev) +
      " MISC=" + hex32(misc) + " WFDMA_GLO=" + hex32(glo));
  return true;
}

bool MtkDevice::mt7925_hard_reset(std::string &error) {
  if (!_proxy.is_open()) {
    if (!_proxy.open(error)) return false;
  }
  _is_mt7925 = true;
  _l1_base = 0xffffffffu;

  if (_chip_id != 0x7925u && !mt7925_probe(error)) return false;
  if (!mt7925_driver_own(error) || !mt7925_wfsys_reset(error) ||
      !mt7925_dma_disable(error)) return false;
  if (!mt7925_fw_own()) {
    error = "MT7925 failed to return firmware ownership";
    return false;
  }
  _mt7925_owns_device = false;
  log("MT7925 hard reset complete");
  return true;
}

/* --------------------------------------------------------------------------
 * WFDMA0 Disable / Init / Enable (MT7925 Connac3)
 * -------------------------------------------------------------------------- */
bool MtkDevice::mt7925_dma_disable(std::string &error) {
  _proxy.write32(mt7925::WFDMA0_HOST_INT_ENA, 0u);
  _proxy.write32(mt7925::PCIE_MAC_INT_ENABLE, 0u);
  _proxy.write32(mt7925::WFDMA0_MCU_INT_ENA, 0u);

  const uint32_t glo = _proxy.read32(mt7925::WFDMA0_GLO_CFG);
  if (glo != 0xffffffffu) {
    _proxy.write32(mt7925::WFDMA0_GLO_CFG, glo & ~0x18208005u);
  }

  if (!poll_reg(mt7925::WFDMA0_GLO_CFG, 0x0fu, 0u, 200)) {
    error = "MT7925 WFDMA0 did not go idle (GLO_CFG=" +
            hex32(_proxy.read32(mt7925::WFDMA0_GLO_CFG)) + ")";
    return false;
  }

  /* Disable TX scheduling and bypass DMASHDL during ring setup. */
  const uint32_t ext0 = _proxy.read32(mt7925::WFDMA0_GLO_CFG_EXT0);
  if (ext0 != 0xffffffffu) _proxy.write32(mt7925::WFDMA0_GLO_CFG_EXT0, ext0 & ~0x40u);
  const uint32_t shdl = _proxy.read32(mt7925::DMASHDL_CONTROL);
  if (shdl != 0xffffffffu) _proxy.write32(mt7925::DMASHDL_CONTROL, shdl | 0x10000000u);

  /* Reset WFDMA0 logic pointers (bits 4 & 5). */
  const uint32_t rst = _proxy.read32(mt7925::WFDMA0_RST);
  if (rst != 0xffffffffu) {
    _proxy.write32(mt7925::WFDMA0_RST, rst & ~0x30u);
    plat::sleep_ms(2);
    _proxy.write32(mt7925::WFDMA0_RST, rst | 0x30u);
    plat::sleep_ms(2);
  }

  _dma_running = false;
  return true;
}

bool MtkDevice::mt7925_dma_init(std::string &error) {
  if (!_dma.init(_mt7925_active_lab ? 1536 : 1024, _mt7925_active_lab ? 1024 : 896, error))
    return false;

  /* Prepare clean chip state: ownership cycle -> EMI protection -> WFSYS reset -> DMA disable. */
  (void)mt7925_fw_own();
  if (!mt7925_driver_own(error)) return false;
  if (!mt7925_emi_protection(error)) return false;
  if (!mt7925_wfsys_reset(error)) return false;
  if (!mt7925_dma_disable(error)) return false;

  // Only MCU command, firmware download and receive rings need DMA pages.
  if (!alloc_tx_ring(_tx_mcu, mt7925::TX_RING15_MCU_WM, 128, false, error)) return false;
  if (!alloc_tx_ring(_tx_fwdl, mt7925::TX_RING16_FWDL, 128, false, error)) return false;
  if (!alloc_rx_ring(_rx_mcu, mt7925::RX_RING0_MCU, 256, error)) return false;
  if (!alloc_rx_ring(_rx_data, mt7925::RX_RING2_DATA, 256, error)) return false;

  if (_mt7925_active_lab &&
      !alloc_tx_ring(_tx_data, mt7925::TX_RING0_DATA, mt7925::AwDlTxSlots::capacity, false, error)) return false;
  _mt7925_tx = {};
  _mt7925_tracked_tx.fill(false);
  _mt7925_tx_completions.clear();
  _mt7925_tx_ok = _mt7925_tx_failed = 0;
  _mt7925_fast_ok = _mt7925_fast_failed = 0;
  _mt7925_amsdu_ok = _mt7925_amsdu_failed = 0;
  _mt7925_ampdu_ok = _mt7925_ampdu_failed = 0;
  _mt7925_awdl_renewals = 0;
  _mt7925_awdl_early_renewals = 0;
  _mt7925_awdl_late_renewals = 0;
  _mt7925_tx_ba_active = false;
  _mt7925_tx_fault = false;

  for (uint32_t r = 0; r < 17; ++r) {
    if (!mt7925_program_ring(mt7925::TX_RING0_DATA + r * 16, 0, 0, 0)) {
      error = "MT7925 failed to clear inherited TX rings";
      return false;
    }
  }
  for (uint32_t r = 0; r < 4; ++r) {
    if (!mt7925_program_ring(mt7925::RX_RING0_MCU + r * 16, 0, 0, 0)) {
      error = "MT7925 failed to clear inherited RX rings";
      return false;
    }
  }

  /* Register emergency close-ops in regproxy so Windows never sees stray DMA if we crash. */
  std::vector<RegOp> close_ops = {
      {mt7925::WFDMA0_HOST_INT_ENA, 0u, 1},
      {mt7925::PCIE_MAC_INT_ENABLE, 0u, 1},
      {mt7925::WFDMA0_MCU_INT_ENA, 0u, 1},
      {mt7925::WFDMA0_GLO_CFG, 0x00000005u, 3}, /* clear TX_DMA_EN | RX_DMA_EN */
      {mt7925::WFDMA0_RST, 0x00000030u, 3},
      {mt7925::WFDMA0_RST, 0x00000030u, 2},
  };
  if (_mt7925_active_lab)
    close_ops.push_back({mt7925::WFSYS_SW_RST_B, 1u, 3});  // Hold the owned radio in reset on a crash.
  if (!_proxy.set_close_ops(close_ops)) {
    error = "MT7925 failed to register RegProxy DMA shutdown";
    return false;
  }

  /* MT7925 WFDMA0 prefetch configuration (chip_control.c verified). */
  _proxy.write32(mt7925::RX0_EXT_CTRL, 0x00000004u);
  _proxy.write32(mt7925::RX1_EXT_CTRL, 0x00400004u);
  _proxy.write32(mt7925::RX2_EXT_CTRL, 0x00800004u);
  _proxy.write32(mt7925::RX3_EXT_CTRL, 0x00c00004u);

  _proxy.write32(mt7925::TX0_EXT_CTRL, 0x01000010u);
  _proxy.write32(mt7925::TX1_EXT_CTRL, 0x02000010u);
  _proxy.write32(mt7925::TX2_EXT_CTRL, 0x03000010u);
  _proxy.write32(mt7925::TX3_EXT_CTRL, 0x04000010u);
  _proxy.write32(mt7925::TX15_EXT_CTRL, 0x05000004u);
  _proxy.write32(mt7925::TX16_EXT_CTRL, 0x05400004u);

  _proxy.write32(mt7925::WFDMA0_RST_DTX_PTR, 0xffffffffu);
  _proxy.write32(mt7925::WFDMA0_RST_DRX_PTR, 0xffffffffu);
  _proxy.write32(mt7925::WFDMA0_PRI_DLY_INT_CFG0, 0u);

  _mt7925_rings_published = true;
  for (const Ring *ring : {&_tx_mcu, &_tx_fwdl, &_rx_mcu, &_rx_data, &_tx_data}) {
    if (!ring->count) continue;
    if (!mt7925_program_ring(ring->reg_base, ring->desc_buf.pa, ring->count, ring->head) ||
        _proxy.read32(ring->reg_base + 0xc) != 0) {
      error = "MT7925 ring setup readback failed at " + hex32(ring->reg_base);
      return false;
    }
  }
  return mt7925_dma_enable(error);
}

bool MtkDevice::mt7925_program_ring(uint32_t base, uint32_t address, uint32_t count, uint32_t cpu) {
  plat::mem_barrier();
  _proxy.write32(base + 4, 0);
  _proxy.write32(base, address);
  _proxy.write32(base + 8, cpu);
  _proxy.write32(base + 4, count);
  return _proxy.read32(base) == address && _proxy.read32(base + 4) == count &&
         _proxy.read32(base + 8) == cpu && !_proxy.failed();
}

bool MtkDevice::mt7925_dma_enable(std::string &error) {
  uint32_t glo = _proxy.read32(mt7925::WFDMA0_GLO_CFG);
  if (glo == 0xffffffffu) {
    error = "MT7925 GLO_CFG read failed";
    return false;
  }
  glo &= ~0x0c000035u;
  glo |= 0x5020b870u;
  _proxy.write32(mt7925::WFDMA0_GLO_CFG, glo);

  _proxy.set_bits(mt7925::WFDMA0_GLO_CFG_EXT1, 0x10000000u);
  _proxy.set_bits(mt7925::WFDMA0_PAUSE_RX_Q_45_TH, 0x00000f00u);
  _proxy.set_bits(mt7925::WFDMA0_PAUSE_RX_Q_67_TH, 0x00007f00u);
  _proxy.set_bits(mt7925::WFDMA0_MCU_DMA0_CTL, 0x00000002u);

  _proxy.set_bits(mt7925::WFDMA0_GLO_CFG, 0x00000005u); /* TX_DMA_EN | RX_DMA_EN */

  /* Second ownership handoff cycle + disable PCIe L0s (chip_control.c verified). */
  (void)mt7925_fw_own();
  if (!mt7925_driver_own(error)) return false;
  _proxy.set_bits(mt7925::PCIE_MAC_PM, mt7925::PCIE_MAC_PM_L0S_DIS);

  const uint32_t enabled = _proxy.read32(mt7925::WFDMA0_GLO_CFG);
  if (_proxy.failed() || enabled == UINT32_MAX || (enabled & 5u) != 5u) {
    error = "MT7925 WFDMA0 enable failed (GLO_CFG=" + hex32(enabled) + ")";
    return false;
  }
  _dma_running = true;
  log("MT7925 WFDMA0 enabled (GLO_CFG=" + hex32(enabled) + ")");
  return true;
}

/* --------------------------------------------------------------------------
 * Connac3 MCU Transport (TX15 WM / TX16 FWDL, RX0 44-byte McuRxd)
 * -------------------------------------------------------------------------- */
void MtkDevice::mt7925_defer_frame(const uint8_t *data, size_t bytes) {
  if (_deferred_rx.size() >= 128) return;
  RxFrame rx{};
  if (mt7925_parse_rx_packet(data, bytes, 0, rx)) {
    DeferredRx frame;
    frame.frame.assign(rx.data, rx.data + rx.len);
    rx.data = nullptr;
    frame.info = rx;
    _deferred_rx.push_back(std::move(frame));
  }
}

int MtkDevice::mt7925_exchange(uint32_t command, const uint8_t *data, uint32_t bytes,
                              unsigned timeout_ms, const McuDecoder &decode, bool roc_abort) {
  if (_mt7925_mcu_fault) return _mt7925_mcu_fault;
  if (!_mt7925_active_lab && _stopping && _stopping()) return MT_FW_TIMEOUT;
  if (!_dma_running || (decode && command == MT_FW_SCATTER)) return MT_INVALID;
  std::array<uint8_t, DmaPool32::kPageBytes> request;
  const int encoded = mt_mcu_encode(command, &_mcu_seq, data, bytes,
                                   request.data(), static_cast<uint32_t>(request.size()));
  if (encoded < 0) return encoded;
  Ring &tx = command == MT_FW_SCATTER ? _tx_fwdl : _tx_mcu;
  if (!tx_ring_push(tx, request.data(), encoded, true) || _proxy.failed())
    return _mt7925_mcu_fault = MT_FW_PROTOCOL;
  if (!decode) return MT_OK;

  const uint8_t sequence = _mcu_seq;
  const uint64_t started = now_ms();
  std::vector<uint8_t> packet;
  unsigned frames = 0, tx_free = 0, events = 0, skipped = 0, other_seq = 0, peeked = 0;
  const auto receive = [&](bool &received) -> int {
    if (!_mt7925_active_lab && _stopping && _stopping()) return MT_FW_TIMEOUT;
    received = rx_ring_pop(_rx_mcu, packet);
    if (!received) return _proxy.failed() ? MT_FW_PROTOCOL : int(MT_PENDING);
    switch (mt7925::rx_kind(packet.data(), packet.size())) {
    case mt7925::RxKind::frame:
      ++frames;
      if (_fw_loaded) mt7925_defer_frame(packet.data(), packet.size());
      break;
    case mt7925::RxKind::tx_free:
      ++tx_free;
      mt7925_handle_tx_free(packet.data(), packet.size());
      break;
    case mt7925::RxKind::event:
      ++events;
      if (packet.size() >= 40) {
        const uint16_t id = rd16(packet.data() + 34);
        if (id == 0x3800 || (!_fw_loaded && (id == 7 || id == 0xe000))) {
          if (packet[37] != sequence) ++other_seq;
          return decode(sequence, packet.data(), static_cast<uint32_t>(packet.size()));
        }
      }
      ++skipped;
      break;
    default:
      ++skipped;
      break;
    }
    return MT_PENDING;
  };
  const auto expired = [&]() -> int {
    if (!roc_abort) return MT_FW_TIMEOUT;
    const auto producer = rr(_rx_mcu.reg_base + 0xc);
    // Do not consume, return, or otherwise publish any descriptors here. The
    // ordinary receive path will still deliver pending frames and TX_FREE.
    return mt7925::peek_mcu_reply(_rx_mcu.tail, producer, _rx_mcu.count,
        _rx_mcu.rx_reasm_drop || !_rx_mcu.rx_reasm.empty(),
        [&](unsigned i) -> mt7925::Rx0Peek {
          const auto ctrl = *static_cast<const volatile uint32_t *>(&_rx_mcu.descs()[i].ctrl);
          if (!(ctrl & MT_DMA_CTL_DMA_DONE)) return {};
          plat::mem_barrier();
          ++peeked;
          const auto length = field_get(MT_DMA_CTL_SD_LEN0, ctrl);
          return {true, (ctrl & MT_DMA_CTL_LAST_SEC0) != 0, _rx_mcu.bufs[i].va,
                  length <= _rx_mcu.bufs[i].size ? length : 0u};
        }, [&](const uint8_t *record, uint32_t size) { return decode(sequence, record, size); });
  };
  const int result = mt7925::wait_mcu_reply(started + timeout_ms, now_ms, receive,
                                            [] { plat::sleep_ms(1); }, expired);
  const auto elapsed = now_ms() - started;
  if (roc_abort && (elapsed >= 100 || result != MT_OK)) {
    log("MT7925 ROC abort ACK wait: elapsed_ms=" + std::to_string(elapsed) +
        " seq=" + std::to_string(sequence) + " transport=" + std::to_string(result) +
        " rx0_frames=" + std::to_string(frames) + " tx_free=" + std::to_string(tx_free) +
        " events=" + std::to_string(events) + " other_seq=" + std::to_string(other_seq) +
        " skipped=" + std::to_string(skipped) + " peeked=" + std::to_string(peeked));
  }
  if (result == MT_FW_TIMEOUT) log("MT7925 MCU timeout: cmd=" + hex32(command) + " seq=" + std::to_string(sequence));
  if (result != MT_OK) _mt7925_mcu_fault = result;
  return result;
}

int MtkDevice::mt7925_send(void *context, uint32_t command, const uint8_t *data,
                           uint32_t bytes, int wait, uint32_t *status) {
  auto &dev = *static_cast<MtkDevice *>(context);
  if (!wait) return dev.mt7925_exchange(command, data, bytes, 0);
  if (!status) return MT_INVALID;
  return dev.mt7925_exchange(command, data, bytes, 3000,
      [command, status](uint8_t seq, const uint8_t *packet, uint32_t size) {
        return mt_mcu_response(command, seq, packet, size, status);
      });
}

bool MtkDevice::mt7925_mcu_send_uni(uint16_t cid, const void *payload, size_t len,
                                   bool wait_rsp, unsigned timeout_ms, bool roc_abort) {
  uint32_t status = 0;
  const uint32_t command = MT_MCU_UNI | cid;
  McuDecoder decode;
  if (wait_rsp) {
    decode = [command, &status](uint8_t seq, const uint8_t *packet, uint32_t size) {
      return mt_mcu_response(command, seq, packet, size, &status);
    };
  }
  const int result = mt7925_exchange(command, static_cast<const uint8_t *>(payload),
                                    static_cast<uint32_t>(len), timeout_ms, decode, roc_abort);
  if (result != MT_OK || status != 0) {
    log("MT7925 UNI command " + hex32(cid) + " transport=" + std::to_string(result) +
        " firmware=" + hex32(status));
    return false;
  }
  return true;
}

bool MtkDevice::mt7925_load_firmware(const std::string &fw_dir, std::string &error) {
  static constexpr std::array<uint8_t, 32> patch_digest = {
      0x67, 0xff, 0x85, 0xf1, 0xa3, 0x9d, 0x1a, 0x10, 0x0a, 0x51, 0xab, 0xd8, 0xe2, 0xa2, 0x6e, 0xdd, 0x2f, 0x19, 0x8d, 0x70, 0x52, 0x51, 0x4b, 0x6b, 0x89, 0x81, 0x94, 0x8b, 0x23, 0x1e, 0x50, 0x16
  };
  static constexpr std::array<uint8_t, 32> ram_digest = {
      0x32, 0x9c, 0x0e, 0x27, 0xf7, 0x80, 0x55, 0x07, 0x89, 0xe4, 0xeb, 0xa4, 0x74, 0x52, 0x73, 0x1b, 0x17, 0x23, 0xf6, 0x9f, 0x9e, 0x67, 0xf5, 0x25, 0xd0, 0xe5, 0x0b, 0xf3, 0xcd, 0x29, 0x78, 0xac
  };
  std::vector<uint8_t> patch, ram;
  if (!read_pinned_firmware(fw_dir + "/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin", patch_digest, patch, error) ||
      !read_pinned_firmware(fw_dir + "/WIFI_RAM_CODE_MT7925_1_1.bin", ram_digest, ram, error))
    return false;
  if (!mt7925_dma_init(error)) return false;
  const mt_fw_transport transport{
    this, mt7925_send,
    [](void *context) -> int {
      auto &dev = *static_cast<MtkDevice *>(context);
      const Ring &ring = dev._tx_fwdl;
      return dev.poll_reg(ring.reg_base + 0xc, UINT32_MAX, ring.head, 1000)
          ? MT_OK : MT_FW_TIMEOUT;
    },
    [](void *context, uint32_t ready) -> int {
      auto &dev = *static_cast<MtkDevice *>(context);
      return dev.poll_reg(mt7925::CONN_ON_MISC, ready ? 3u : 7u, ready ? 3u : 1u,
                          ready ? 1500u : 1000u) ? MT_OK : MT_FW_TIMEOUT;
    },
  };
  mt_fw_result result{};
  const int boot = mt_fw_boot(patch.data(), static_cast<uint32_t>(patch.size()),
                            ram.data(), static_cast<uint32_t>(ram.size()), &transport, &result);
  if (boot != MT_OK || !result.ready || !result.start_acked) {
    error = "MT7925 firmware boot failed: transport=" + std::to_string(boot) +
        " release=" + std::to_string(result.release_error) +
        " MISC=" + hex32(_proxy.read32(mt7925::CONN_ON_MISC));
    return false;
  }
  _fw_loaded = true;
  log("MT7925 firmware ready: downloaded=" + std::to_string(result.bytes_sent));
  _mt7925_ram = std::move(ram);
  return mt7925_init_regulatory(error);
}

int MtkDevice::mt7925_query_regdomain(const uint8_t country[2], mt_regdomain &domain) {
  // One UNI command must fit a single TX15 DMA page, including its 48-byte header.
  std::array<uint8_t, DmaPool32::kPageBytes - 48> request{};
  const auto ram_size = static_cast<uint32_t>(_mt7925_ram.size());
  const int length = mt_regd_request(_mt7925_ram.data(), ram_size, _mt7925_board_variant, country,
                                    request.data(), static_cast<uint32_t>(request.size()));
  return length < 0 ? length : mt7925_exchange(MT_MCU_UNI | 0x2cu, request.data(), length, 3000,
      [&domain](uint8_t seq, const uint8_t *packet, uint32_t size) {
        return mt_regd_response(seq, packet, size, &domain);
      });
}

bool MtkDevice::mt7925_init_regulatory(std::string &error) {
  static constexpr uint8_t nic_request[8] = {0, 0, 0, 0, 3, 0, 4, 0};
  static constexpr uint8_t efuse_request[32] = {0, 0, 0, 0, 1, 0, 28, 0, 0x70, 0x0a};
  static constexpr uint8_t world[2] = {'0', '0'};
  mt_nic_caps nic{};
  mt_hw_type board{};
  mt_regdomain domain{};
  int status = mt7925_exchange(MT_MCU_UNI | 0x0eu, nic_request, sizeof(nic_request), 3000,
      [&nic](uint8_t seq, const uint8_t *packet, uint32_t size) {
        return mt_nic_response(seq, packet, size, &nic);
      });
  if (status || !(nic.present & 8u) || !(nic.chip_low & 32u) || !(nic.bands & 2u)) {
    error = "MT7925 NIC capability query failed or 5 GHz regulatory capability missing";
    return false;
  }
  _mt7925_rx_streams = static_cast<uint8_t>(nic.nss);  // NIC_CAPAB accepts one or two chains.
  constexpr uint32_t rf_pin_reporting = 1u << 3;
  if (_verbose) {
    log("MT7925 firmware RF pin status supported=" +
        std::to_string((nic.chip_low & rf_pin_reporting) != 0));
  }
  std::copy_n(nic.mac, _mac.size(), _mac.begin());
  status = mt7925_exchange(MT_MCU_UNI | MT_MCU_QUERY | 0x2du,
      efuse_request, sizeof(efuse_request), 3000,
      [&board](uint8_t seq, const uint8_t *packet, uint32_t size) {
        return mt_efuse_type_response(seq, packet, size, &board);
      });
  if (status) {
    error = "MT7925 EFUSE board variant query failed: " + std::to_string(status);
    return false;
  }

  _mt7925_board_variant = static_cast<uint8_t>(board.encapsulation);
  status = mt7925_query_regdomain(world, domain);
  if (status) {
    error = "MT7925 firmware world-domain query failed: " + std::to_string(status);
    return false;
  }
  const mt_fw_transport transport{this, mt7925_send, nullptr, nullptr};
  mt_clc_result clc{};
  std::array<uint8_t, DmaPool32::kPageBytes - 48> request{};
  status = mt_clc_power_apply(&transport, _mt7925_ram.data(), static_cast<uint32_t>(_mt7925_ram.size()),
                              _mt7925_board_variant,
                              world, 0, request.data(), static_cast<uint32_t>(request.size()), &clc);
  if (status) {
    error = "MT7925 board CLC setup failed: " + std::to_string(status);
    return false;
  }
  if (!mt7925_mac_init(error)) return false;
  unsigned allowed = 0;
  status = LuoboMt7925SetPassiveWorldDomain(&transport, &domain, &allowed);
  if (status) {
    error = "MT7925 passive domain setup failed: " + std::to_string(status);
    return false;
  }
  _mt7925_passive_channels = static_cast<uint8_t>(allowed);
  log("MT7925 passive domain: MAC=" + mac_str(_mac.data()) +
      " EFUSE=" + std::to_string(board.encapsulation) + " rules=" + std::to_string(domain.count) +
      " CLC requests=" + std::to_string(clc.requests_sent) + " channels=" + hex32(allowed));
  return true;
}

void MtkDevice::mt7925_set_fixed_rate_table(uint8_t tbl_idx, uint16_t rate_idx) {
  wr(mt7925::WTBL_ITDR0, rate_idx);
  wr(mt7925::WTBL_ITDR1, mt7925::WTBL_SPE_IDX_SEL);
  wr(mt7925::WTBL_ITCR, mt7925::WTBL_ITCR_WR | mt7925::WTBL_ITCR_EXEC | tbl_idx);
  (void)rr(mt7925::WTBL_ITCR);
}

bool MtkDevice::mt7925_native_awdl_rx(std::string &error) {
  if (!mt7925::configure_native_awdl_rx(
          [&](uint32_t address, uint32_t mask) { clear(address, mask); },
          [&](uint32_t address) { return rr(address); }) || _proxy.failed()) {
    error = "MT7925 could not preserve native 802.11 MPDUs (MDP_DCR0 readback)";
    return false;
  }
  return true;
}

bool MtkDevice::mt7925_mac_init(std::string &error) {
  /* 1. UNI EFUSE_CTRL BUFFER_MODE (exact verified 12-byte payload from mac_init.c). */
  static constexpr uint8_t kEfuseBufMode[12] = {0, 0, 0, 0, 2, 0, 8, 0, 0, 1, 0, 0};
  if (!mt7925_mcu_send_uni(mt7925::UNI_CMD_EFUSE_CTRL, kEfuseBufMode, sizeof(kEfuseBufMode),
                           true)) {
    error = "MT7925 EFUSE_BUFFER_MODE failed";
    return false;
  }

  /* Hardware MAC/MDP/WTBL/Band initialization (verified mac_init.c). */
  rmw(mt7925::MDP_DCR1, mt7925::MDP_DCR1_MAX_RX_LEN, field_prep(mt7925::MDP_DCR1_MAX_RX_LEN, 1536u));
  if (!mt7925_native_awdl_rx(error)) return false;

  /* Clear WTBL admission counters 0..19. */
  for (uint32_t i = 0; i < MT792X_WTBL_SIZE; i++) {
    if (!poll_reg(mt7925::WTBL_UPDATE, mt7925::WTBL_UPDATE_BUSY, 0u, 10)) {
      error = "MT7925 WTBL admission counter operation timed out";
      return false;
    }
    rmw(mt7925::WTBL_UPDATE, 0xfffu, i | mt7925::WTBL_UPDATE_ADM_CLEAR);
    if (!poll_reg(mt7925::WTBL_UPDATE, mt7925::WTBL_UPDATE_BUSY, 0u, 10)) {
      error = "MT7925 WTBL admission counter operation timed out";
      return false;
    }
  }

  /* Initialize band 0 & band 1 MAC registers (exact mac_init.c offsets). */
  for (uint32_t band = 0; band < 2; band++) {
    const uint32_t boff = band ? 0x80000u : 0u;
    rmw(0x210f4u + boff, 0x3fu, 0x6003fu);
    set(0x217c4u + boff, 0x40000000u);
    set(0x21780u + boff, 0x40000000u);
    set(0x24804u + boff, 0x300u);
    rmw(0x21e00u + boff, 0x80fff8u, 0x3000u);
    rmw(0x23408u + boff, 0xc3000000u, 0x03000000u);
  }

  /* Program legacy basic rates into fixed-rate table slots 14..25. */
  for (uint8_t i = 0; i < 12; i++) {
    mt7925_set_fixed_rate_table(static_cast<uint8_t>(mt7925::BASIC_RATES_TBL + i), kBasicRates[i]);
  }
  if (_mt7925_active_lab) {
    for (const auto rate : mt7925::awdl_rate_steps) {
      const uint16_t code = mt7925::awdl_fixed_rate_code(rate);
      if (code) mt7925_set_fixed_rate_table(static_cast<uint8_t>(rate), code);
    }
    /* Additional HT20 slots for MtkStation fallback rate ladder (MCS2 / MCS4). */
    mt7925_set_fixed_rate_table(31, (2u << 6) | 2u);
    mt7925_set_fixed_rate_table(32, (2u << 6) | 4u);
  }

  if (_proxy.failed()) {
    error = "MT7925 MAC initialization failed on register I/O";
    return false;
  }

  log("MT7925 MAC init complete");
  return true;
}

bool MtkDevice::mt7925_passive_scan(uint8_t channel, const RxHandler &on_rx,
                                    std::array<uint8_t, 2> *firmware_country, uint16_t dwell_ms,
                                    uint32_t *firmware_beacons) {
  const unsigned mask = channel == 36 ? LuoboWorldChannel36 :
                        channel == 44 ? LuoboWorldChannel44 : 0;
  if (!is_mt7925() || !_fw_loaded || !_dma_running ||
      !(mask & _mt7925_passive_channels)) return false;

  _channel = channel;
  // Retire any previous visit's records. Only RX after submission counts.
  for (unsigned i = 0; i < 4 && mt7925_poll({}); ++i) {}
  _mt7925_scan_id = static_cast<uint8_t>(_mt7925_scan_id % 127 + 1);
  const auto id = _mt7925_scan_id;
  uint8_t request[LuoboPassiveScanBytes];
  if (!LuoboMt7925EncodePassiveScan(request, id, mask, dwell_ms)) return false;
  LUOBO_PASSIVE_SCAN scan{};
  const int result = mt7925_exchange(MT_MCU_UNI | 0x16u, request, sizeof(request), 2000,
      [&scan, id, mask](uint8_t seq, const uint8_t *record, uint32_t size) {
        return LuoboMt7925PassiveScanEvent(&scan, id, seq, mask, record, size);
      });
  if (result || scan.CompleteChannels != 1) {
    log("MT7925 passive scan incomplete: channel=" + std::to_string(channel) +
        " transport=" + std::to_string(result) + " firmware=" + hex32(scan.Refusal) +
        " completed=" + std::to_string(scan.CompleteChannels));
    _channel = 0;
    return false;
  }
  for (unsigned i = 0; i < 4 && mt7925_poll(on_rx); ++i) {}
  if (firmware_beacons) *firmware_beacons = scan.BeaconCount;
  if (firmware_country) *firmware_country = {scan.FirmwareCountry[0], scan.FirmwareCountry[1]};
  if (_verbose) {
    const char country[] = {static_cast<char>(scan.FirmwareCountry[0]),
                            static_cast<char>(scan.FirmwareCountry[1]), 0};
    log("MT7925 passive scan complete: channel=" + std::to_string(channel) +
        " requested_dwell_ms=" + std::to_string(dwell_ms) +
        " firmware_beacons=" + std::to_string(scan.BeaconCount) + " country=" + country);
  }
  return true;
}

bool MtkDevice::mt7925_confirm_country(std::array<uint8_t, 2> &country, std::string &error,
                                      bool require_candidate) {
  unsigned matches = 0;
  const uint64_t deadline = now_ms() + 5000;
  while (now_ms() < deadline && matches < 2) {
    std::array<uint8_t, 2> candidate{};
    if (!mt7925_passive_scan(44, {}, &candidate)) {
      error = "passive country scan failed";
      return false;
    }
    if (candidate[0] < 'A' || candidate[0] > 'Z' || candidate[1] < 'A' || candidate[1] > 'Z')
      continue;
    if (matches && candidate != country) {
      error = "conflicting country indications from firmware";
      return false;
    }
    country = candidate;
    ++matches;
  }
  if (matches < 2 && require_candidate) {
    error = "firmware did not confirm a country in two passive scan results";
    return false;
  }
  return true;
}

bool MtkDevice::mt7925_check_country(std::string &error) {
  std::array<uint8_t, 2> country{};
  if (!mt7925_confirm_country(country, error)) return false;
  mt_regdomain domain{};
  const int status = mt7925_query_regdomain(country.data(), domain);
  if (status) {
    error = "country regulatory query failed: " + std::to_string(status);
    return false;
  }
  unsigned passive = 0, active = 0;
  const int eligibility = LuoboMt7925QueryChannelEligibility(&domain, &passive, &active);
  const std::string code(country.begin(), country.end());
  log("MT7925 country policy: " + code + " DFS region=" + std::to_string(domain.dfs_region) +
      " passive mask=" + hex32(passive) + " active candidate mask=" + hex32(active));
  if (eligibility && eligibility != MT_PENDING) {
    error = "invalid firmware country channel policy: " + std::to_string(eligibility);
    return false;
  }
  return true;
}

bool MtkDevice::mt7925_program_awdl_power(uint8_t primary, const std::string &expected_country, bool indoor,
                                         bool activate, std::string &error) {
  _mt7925_power_ready = false;
  if (primary != 44 && primary != 149) {
    error = "Unsupported E138 AWDL primary channel";
    return false;
  }
  if (activate && (!indoor || !_mt7925_active_lab || !_tx_data.count)) {
    error = "MT7925 active lab requires an indoor test and a prepared TX ring";
    return false;
  }
  if (expected_country.size() != 2 || expected_country[0] < 'A' || expected_country[0] > 'Z' ||
      expected_country[1] < 'A' || expected_country[1] > 'Z') {
    error = "MT7925 AWDL power test requires the current test location country";
    return false;
  }
  std::array<uint8_t, 2> country{};
  if (!mt7925_confirm_country(country, error, !indoor)) return false;
  if (country[0] && std::string(country.begin(), country.end()) != expected_country) {
    error = "MT7925 firmware country " + std::string(country.begin(), country.end()) +
            " differs from the test location";
    return false;
  }
  if (!country[0]) {
    country = {static_cast<uint8_t>(expected_country[0]), static_cast<uint8_t>(expected_country[1])};
    log("MT7925 scan country unavailable; querying pinned regulatory data for the confirmed indoor location");
  }
  const std::string code(country.begin(), country.end());

  LUOBO_MT7925_BOARD_SAR sar{};
  std::string report;
  const bool valid = read_mt7925_sar(_proxy, sar, report, error);
  if (!report.empty()) log("MT7925 SAR " + report);
  if (!valid) return false;

  mt_regdomain domain{};
  int status = mt7925_query_regdomain(country.data(), domain);
  if (status) {
    error = "MT7925 hardware country query failed: " + std::to_string(status);
    return false;
  }

  const mt_fw_transport transport{this, mt7925_send, nullptr, nullptr};
  std::array<uint8_t, DmaPool32::kPageBytes - 48> scratch{};
  mt_clc_result clc{};
  status = mt_clc_power_apply(&transport, _mt7925_ram.data(),
      static_cast<uint32_t>(_mt7925_ram.size()), _mt7925_board_variant, country.data(),
      indoor ? 1u : 0u, scratch.data(), static_cast<uint32_t>(scratch.size()), &clc);
  if (status || clc.fallback_mask) {
    error = "MT7925 country CLC failed: status=" + std::to_string(status) +
            " fallback=" + hex32(clc.fallback_mask);
    return false;
  }

  // Board SAR and firmware CLC may tighten this 6 dBm conducted ceiling.
  constexpr int startup_ceiling_half_dbm = 12;
  LUOBO_MT7925_STA_POWER_RESULT power{};
  constexpr unsigned awdl_width = 80;
  status = LuoboMt7925ProgramAwdlChannels80ForPrimary(&transport, &domain, &sar, country.data(), primary,
      startup_ceiling_half_dbm, indoor, activate, &power);
  if (status == MT_PENDING && !power.Attempted) {
    error = "MT7925 E138 channel " + std::to_string(primary) + " at 80 MHz unavailable under the board or regulatory policy";
    return false;
  }
  log("MT7925 AWDL power " + code + " primary=" + std::to_string(primary) + " status=" + std::to_string(status) +
      " width_mhz=" + std::to_string(awdl_width) +
      " sku_batches=" + std::to_string(power.SkuBatchesAck) +
      " lowest_ceiling_half_dBm=" + std::to_string(power.LimitHalfDbm) +
      " active_domain=" + std::to_string(power.ChannelProgrammed != 0));
  if (status) {
    error = "MT7925 firmware rejected the AWDL power policy";
    return false;
  }
  if (activate && (power.ChannelProgrammed != 36 || power.AwdlPrimary != primary)) {
    error = "MT7925 firmware did not program both the requested AWDL block and the home channel";
    return false;
  }
  _width_mhz = awdl_width;
  _mt7925_awdl_channel = primary;
  _mt7925_power_ready = activate;
  return true;
}

bool MtkDevice::mt7925_program_concurrent_power(const std::string &country_hint, std::string &error) {
  if (!_mt7925_active_lab || !_tx_data.count) {
    error = "MT7925 concurrent mode requires an initialized TX ring";
    return false;
  }
  std::array<uint8_t, 2> country{};
  const bool valid_hint = country_hint.size() == 2 &&
                          country_hint[0] >= 'A' && country_hint[0] <= 'Z' &&
                          country_hint[1] >= 'A' && country_hint[1] <= 'Z' &&
                          country_hint != "ZZ";
  if (valid_hint) {
    country = {static_cast<uint8_t>(country_hint[0]), static_cast<uint8_t>(country_hint[1])};
  }
  // If country was not explicitly specified (or defaults to US), check one quick
  // passive scan on ch 44 to pick up local AP country code (e.g. CN) automatically.
  if (!valid_hint || country_hint == "US") {
    std::array<uint8_t, 2> detected{};
    if (mt7925_passive_scan(44, {}, &detected, 120) &&
        detected[0] >= 'A' && detected[0] <= 'Z' &&
        detected[1] >= 'A' && detected[1] <= 'Z') {
      country = detected;
    }
  }
  if (!country[0]) country = {'C', 'N'};
  const std::string code(country.begin(), country.end());

  LUOBO_MT7925_BOARD_SAR sar{};
  std::string report;
  const bool valid = read_mt7925_sar(_proxy, sar, report, error);
  if (!report.empty()) log("MT7925 SAR " + report);
  if (!valid) return false;

  mt_regdomain domain{};
  int status = mt7925_query_regdomain(country.data(), domain);
  if (status) {
    error = "MT7925 hardware country query failed for " + code + ": " + std::to_string(status);
    return false;
  }

  const mt_fw_transport transport{this, mt7925_send, nullptr, nullptr};
  std::array<uint8_t, DmaPool32::kPageBytes - 48> scratch{};
  mt_clc_result clc{};
  status = mt_clc_power_apply(&transport, _mt7925_ram.data(),
      static_cast<uint32_t>(_mt7925_ram.size()), _mt7925_board_variant, country.data(),
      1u /* indoor */, scratch.data(), static_cast<uint32_t>(scratch.size()), &clc);
  if (status) {
    error = "MT7925 country CLC failed for " + code + ": status=" + std::to_string(status);
    return false;
  }

  constexpr int startup_ceiling_half_dbm = 12; /* 6 dBm conducted ceiling */
  LUOBO_MT7925_STA_POWER_RESULT power{};
  status = LuoboMt7925ProgramConcurrent5G(&transport, &domain, &sar, country.data(),
      startup_ceiling_half_dbm, true, true, &power);
  log("MT7925 concurrent 5G power " + code + " status=" + std::to_string(status) +
      " sku_batches=" + std::to_string(power.SkuBatchesAck) +
      " lowest_ceiling_half_dBm=" + std::to_string(power.LimitHalfDbm) +
      " active_domain=" + std::to_string(power.ChannelProgrammed != 0));
  if (status || !power.ChannelProgrammed) {
    error = "MT7925 firmware rejected the concurrent 5 GHz power domain";
    return false;
  }
  _mt7925_power_ready = true;
  return true;
}

bool MtkDevice::mt7925_parse_rx_packet(const uint8_t *buf, size_t len, uint8_t ring, RxFrame &out) {
  mt7925::MonitorFrame frame{};
  bool parsed = false;
  if (_mt7925_concurrent) {
    parsed = mt7925::parse_concurrent_frame(buf, len, _channel, _width_mhz, frame);
  } else {
    const uint8_t wide_primary = _width_mhz == 80 && _channel == _mt7925_awdl_channel ? _channel : 0;
    parsed = mt7925::parse_monitor_frame(buf, len, frame, wide_primary);
  }
  if (!parsed) {
    if (len >= 32 && mt7925::rx_kind(buf, len) == mt7925::RxKind::frame) {
      ++_mt7925_rxd_rejected;
      if (mt7925::rx_word(buf + 16) & 3u) ++_mt7925_split_rx_dropped;
    }
    static unsigned drop_logs = 0;
    if (_verbose && drop_logs < 12 && len >= 64 && mt7925::rx_kind(buf, len) == mt7925::RxKind::frame) {
      for (size_t i = 32; i + 6 <= std::min<size_t>(len, 220); ++i) {
        const bool is_awdl = !std::memcmp(buf + i, mt7925::awdl_bssid.data(), mt7925::awdl_bssid.size());
        const bool is_sta = _sta_iface_up &&
                            (!std::memcmp(buf + i, _sta_bssid.data(), 6) ||
                             !std::memcmp(buf + i, _sta_addr.data(), 6));
        if (is_awdl || is_sta) {
          ++drop_logs;
          const uint32_t d0 = mt7925::rx_word(buf);
          const uint32_t d1 = mt7925::rx_word(buf + 4);
          const uint32_t d2 = mt7925::rx_word(buf + 8);
          const uint32_t d3 = mt7925::rx_word(buf + 12);
          const uint32_t d4 = mt7925::rx_word(buf + 16);
          const uint8_t ch = (d3 >> 8) & 0xff;
          const uint8_t fc0 = i >= 16 ? buf[i - 16] : 0;
          const uint8_t fc1 = i >= 15 ? buf[i - 15] : 0;
          log(std::string("MT7925 RX drop ") + (is_sta ? "STA" : "AWDL") +
              " ring=" + std::to_string(ring) +
              " ch=" + std::to_string(ch) + " d0=" + hex32(d0) + " d1=" + hex32(d1) +
              " d2=" + hex32(d2) + " d3=" + hex32(d3) + " d4=" + hex32(d4) +
              " fc=" + std::to_string(fc0) + "/" + std::to_string(fc1));
          break;
        }
      }
    }
    return false;
  }
  out = {buf + frame.offset, frame.length, frame.dbm, frame.channel, ring,
         frame.has_timestamp, frame.timestamp_us, frame.phy_mode, frame.phy};
  return true;
}

void MtkDevice::mt7925_handle_tx_free(const uint8_t *data, size_t len) {
  if (!data || len < 8) return;
  const uint32_t d0 = rd32(data);
  const size_t declared = d0 & 0xffffu;
  if (declared < 8 || declared > len) return;
  const uint32_t d1 = rd32(data + 4);
  const uint32_t ver = field_get(mt7925::TXFREE1_VER, d1);
  if (ver < 4) return;

  const uint32_t total = field_get(mt7925::TXFREE0_MSDU_CNT, d0);
  uint32_t count = 0;
  bool cur_acked = true;
  unsigned cur_tries = 1;

  for (size_t off = 8; off + 4 <= declared && count < total; off += 4) {
    const uint32_t info = rd32(data + off);
    if (info & mt7925::TXFREE_INFO_PAIR) {
      cur_acked = true;
      cur_tries = 1;
      continue;
    }
    if (info & mt7925::TXFREE_INFO_HEADER) {
      const uint32_t stat = field_get(mt7925::TXFREE_INFO_STAT, info);
      const uint32_t cnt = field_get(mt7925::TXFREE_INFO_COUNT, info);
      cur_acked = (stat == 0u);
      cur_tries = cnt ? static_cast<unsigned>(cnt) : 1u;
      continue;
    }
    for (unsigned i = 0; i < 2 && count < total; i++) {
      const uint32_t token = (info >> (15u * i)) & mt7925::TXFREE_INFO_MSDU_ID;
      if (token == mt7925::TXFREE_INFO_MSDU_ID) continue;
      count++;
      mt7925::AwDlTxKind kind{};
      if (_mt7925_tx.complete(static_cast<uint16_t>(token), &kind)) {
        const auto index = token % mt7925::AwDlTxSlots::capacity;
        if (_mt7925_tracked_tx[index]) {
          _mt7925_tracked_tx[index] = false;
          _mt7925_tx_completions.push_back({static_cast<uint16_t>(token), cur_acked});
        }
        ++(cur_acked ? _mt7925_tx_ok : _mt7925_tx_failed);
        if (kind == mt7925::AwDlTxKind::aggregate)
          ++(cur_acked ? _mt7925_amsdu_ok : _mt7925_amsdu_failed);
        if (kind == mt7925::AwDlTxKind::fast)
          ++(cur_acked ? _mt7925_fast_ok : _mt7925_fast_failed);
        if (kind == mt7925::AwDlTxKind::ampdu)
          ++(cur_acked ? _mt7925_ampdu_ok : _mt7925_ampdu_failed);
      }
      if (_on_tx_status) {
        _on_tx_status(token, cur_acked, cur_tries);
      }
    }
  }
}

size_t MtkDevice::mt7925_poll(const RxHandler &on_rx) {
  if (!_dma_running || !_channel || _mt7925_mcu_fault) return 0;
  if (_mt7925_concurrent && _mt7925_roc_until && plat::now_ms() + 80 >= _mt7925_roc_until) {
    (void)mt7925_renew_concurrent_roc(on_rx);
  }
  size_t delivered = 0;

  /* Deliver any frames captured during synchronous MCU waits first. */
  while (!_deferred_rx.empty()) {
    auto d = std::move(_deferred_rx.front());
    _deferred_rx.pop_front();
    if (d.info.channel != _channel) continue;
    if (_mt7925_concurrent) mt7925_snoop_rx_ba(d.frame.data(), d.frame.size());
    if (on_rx) {
      d.info.data = d.frame.data();
      on_rx(d.info);
    }
    delivered++;
  }

  // Service TX_FREE before spending up to 128 callbacks on bulk data.
  // The independent RX0 budget guarantees an RX2 turn even in an event storm.
  const auto mcu_before = _rx_mcu.rx_didx.consumed();
  for (unsigned n = 0; n < 64 && rx_ring_pop(_rx_mcu, _rx_pkt_buf, false); n++) {
    if (_rx_pkt_buf.size() < 4) continue;
    const uint32_t pt = field_get(MT_RXD0_PKT_TYPE, rd32(_rx_pkt_buf.data()));
    if (pt == PKT_TYPE_TXRX_NOTIFY) {
      mt7925_handle_tx_free(_rx_pkt_buf.data(), _rx_pkt_buf.size());
    } else if (mt7925::rx_kind(_rx_pkt_buf.data(), _rx_pkt_buf.size()) == mt7925::RxKind::frame) {
      RxFrame rx{};
      if (mt7925_parse_rx_packet(_rx_pkt_buf.data(), _rx_pkt_buf.size(), 0, rx) && rx.channel == _channel) {
        if (_mt7925_concurrent) mt7925_snoop_rx_ba(rx.data, rx.len);
        if (on_rx) on_rx(rx);
        delivered++;
      }
    }
  }
  if (_rx_mcu.rx_didx.consumed() != mcu_before) rx_ring_commit(_rx_mcu);

  /* Drain RX2 (data/monitor) — batch ring commit at end. */
  const auto data_before = _rx_data.rx_didx.consumed();
  for (unsigned n = 0; n < 128 && rx_ring_pop(_rx_data, _rx_pkt_buf, false); n++) {
    if (_rx_pkt_buf.size() < 4) continue;
    const uint32_t pt = field_get(MT_RXD0_PKT_TYPE, rd32(_rx_pkt_buf.data()));
    if (pt == PKT_TYPE_TXRX_NOTIFY) {
      mt7925_handle_tx_free(_rx_pkt_buf.data(), _rx_pkt_buf.size());
      continue;
    }
    RxFrame rx{};
    if (mt7925_parse_rx_packet(_rx_pkt_buf.data(), _rx_pkt_buf.size(), 2, rx) && rx.channel == _channel) {
      if (_mt7925_concurrent) mt7925_snoop_rx_ba(rx.data, rx.len);
      if (on_rx) on_rx(rx);
      delivered++;
    }
  }
  if (_rx_data.rx_didx.consumed() != data_before) rx_ring_commit(_rx_data);

  mt7925_reclaim_awdl_tx();

  return delivered;
}

void MtkDevice::mt7925_close() {
  if (_mt7925_active_lab && _rx_data.count) {
    log("MT7925 RX DMA: rx0_desc=" + std::to_string(_rx_mcu.rx_didx.consumed()) +
        " rx0_didx_reads=" + std::to_string(_rx_mcu.rx_didx.register_reads()) +
        " rx2_desc=" + std::to_string(_rx_data.rx_didx.consumed()) +
        " rx2_didx_reads=" + std::to_string(_rx_data.rx_didx.register_reads()));
  }
  _mt7925_power_ready = false;
  _mt7925_concurrent = false;
  _mt7925_roc_until = 0;
  _sta_iface_up = false;
  _peers = {};
  _pending_addba = {};
  _pending_tx_addba = {};
  _mt7925_tracked_tx.fill(false);
  _mt7925_tx_completions.clear();
  if (_mt7925_owns_device) {
    if (_mt7925_rings_published) {
      std::string error;
      bool stopped = mt7925_driver_own(error) && mt7925_dma_disable(error);
      if (!stopped)
        stopped = mt7925_wfsys_reset(error) && mt7925_driver_own(error) && mt7925_dma_disable(error);
      _mt7925_rings_published = !stopped;
      if (!stopped) log("MT7925 DMA stop unconfirmed: " + error);
      if (!_mt7925_rings_published) {
        for (const Ring *ring : {&_tx_mcu, &_tx_fwdl, &_rx_mcu, &_rx_data, &_tx_data}) {
          if (ring->count) (void)mt7925_program_ring(ring->reg_base, 0, 0, 0);
        }
        if (!_mt7925_active_lab || mt7925_wfsys_reset(error)) {
          (void)_proxy.set_close_ops({});
        } else {
          log("MT7925 active shutdown reset unconfirmed: " + error);
        }
      }
    }
    if (!_mt7925_rings_published) (void)mt7925_fw_own();
    _mt7925_owns_device = false;
  }
  _dma_running = false;
  _fw_loaded = false;
  _mt7925_passive_channels = 0;
  _mt7925_ram.clear();
  _deferred_rx.clear();
}

} // namespace mtk
