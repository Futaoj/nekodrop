// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT7925 (Wi-Fi 7 Connac3) dual-band BSS, ROC, STA/BA and TX backend
 * for MtkDevice. Supports both 2.4 GHz (ch 1..13, 20 MHz, band 1) and 5 GHz
 * (ch 36..177, 20/40/80 MHz, band 2) for unified AWDL + concurrent 5 GHz STA.
 */
#include "mt7925_active.h"
#include "mt7925_ba.h"
#include "mt7925_mcu_wait.h"
#include "mt7925_regs.h"
#include "mtk_dev.h"
#include "mtk_platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include "firmware.h"
}

namespace mtk {

namespace {

std::string mac_str(const uint8_t mac[6]) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                mac[4], mac[5]);
  return buf;
}

uint8_t mt7925_map_rate_val(uint16_t rate_val) {
  const uint8_t mode = static_cast<uint8_t>((rate_val >> 6) & 0x7u);
  const uint8_t code = static_cast<uint8_t>(rate_val & 0x3fu);
  if (mode == 0u) { /* CCK: BASIC_RATES_TBL slots 14..17 (1, 2, 5.5, 11 Mbps) */
    return static_cast<uint8_t>(14u + (code & 3u));
  }
  if (mode == 1u) { /* Legacy OFDM: BASIC_RATES_TBL slots 18..25 */
    static constexpr uint8_t ofdm_codes[8] = {0x0b, 0x0f, 0x0a, 0x0e, 0x09, 0x0d, 0x08, 0x0c};
    for (uint8_t i = 0; i < 8; ++i) {
      if (code == ofdm_codes[i]) return static_cast<uint8_t>(18u + i);
    }
    return 18u;
  }
  if (mode == 2u) { /* HT MCS -> pre-installed HT20 table slots */
    if (code >= 7u) return static_cast<uint8_t>(mt7925::AwDlRate::ht20);      /* 26 (MCS 7) */
    if (code >= 5u) return static_cast<uint8_t>(mt7925::AwDlRate::ht20_mcs5); /* 29 (MCS 5) */
    if (code >= 4u) return 32u;                                               /* 32 (MCS 4) */
    if (code >= 3u) return static_cast<uint8_t>(mt7925::AwDlRate::ht20_mcs3); /* 28 (MCS 3) */
    return 31u;                                                               /* 31 (MCS 2) */
  }
  return 18u; /* Default OFDM 6 Mbps */
}

uint8_t mt7925_map_tx_params(const TxParams &p) {
  if (p.is_vht) {
    return p.mcs >= 8 ? static_cast<uint8_t>(mt7925::AwDlRate::vht20_mcs8)
                      : static_cast<uint8_t>(mt7925::AwDlRate::vht20);
  }
  if (p.is_ht) {
    if (p.mcs >= 7) return static_cast<uint8_t>(mt7925::AwDlRate::ht20);
    if (p.mcs >= 5) return static_cast<uint8_t>(mt7925::AwDlRate::ht20_mcs5);
    if (p.mcs >= 4) return 32u;
    if (p.mcs >= 3) return static_cast<uint8_t>(mt7925::AwDlRate::ht20_mcs3);
    return 31u;
  }
  switch (p.rate_mbps) {
    case 1: return 14u;
    case 2: return 15u;
    case 5: return 16u;
    case 11: return 17u;
    case 9: return 19u;
    case 12: return 20u;
    case 18: return 21u;
    case 24: return 22u;
    case 36: return 23u;
    case 48: return 24u;
    case 54: return 25u;
    default: return 18u;
  }
}

uint16_t mt7925_hw_wcid(uint8_t wcid) {
  /* On Connac3, BSS 0 bmc = 19 and BSS 1 bmc = 18. Remap kApWcid (18) -> 17
   * so the Wi-Fi AP STA record never collides with BSS 1's broadcast WCID. */
  return (wcid == MtkDevice::kApWcid) ? uint16_t{17} : uint16_t{wcid};
}

} // namespace

/* --------------------------------------------------------------------------
 * BSS Setup, RLM & Remain-On-Channel (Dual-Band 2.4 GHz + 5 GHz)
 * -------------------------------------------------------------------------- */
bool MtkDevice::mt7925_set_idle_bss(uint8_t bss, const uint8_t mac[6]) {
  uint8_t device[16]{};
  device[0] = bss;
  device[1] = 0xff;
  device[6] = 12; /* DEV_INFO_ACTIVE */
  device[8] = 1;
  std::memcpy(device + 10, mac, 6);
  if (!mt7925_mcu_send_uni(1, device, sizeof(device), true)) return false;

  uint8_t idle[36]{};
  idle[0] = bss;
  uint8_t *initial = idle + 4;
  initial[2] = 32;
  initial[4] = 1;
  initial[5] = initial[6] = initial[13] = bss;
  initial[7] = 0xff;
  initial[8] = initial[10] = 1;
  initial[12] = 1; /* disconnected idle BSS */
  initial[20] = initial[26] = static_cast<uint8_t>(19 - bss);
  return mt7925_mcu_send_uni(2, idle, sizeof(idle), true);
}

bool MtkDevice::mt7925_setup_bss(uint8_t bss, uint8_t channel, unsigned width_mhz,
                                 const uint8_t mac[6], const uint8_t bssid[6], bool qos_edca) {
  if (!_mt7925_power_ready || bss > 1 || !mt7925::is_valid_channel(channel)) return false;
  const bool is_2g = channel <= 14;
  const unsigned eff_w = mt7925::effective_width(channel, width_mhz);
  const uint8_t center = mt7925::center_channel(channel, eff_w);
  const uint8_t sco = mt7925::rlm_sco(channel, center);
  const uint8_t band = mt7925::band_for_channel(channel);
  const uint8_t streams = (is_2g || eff_w <= 20) ? 1u : _mt7925_rx_streams;
  const uint8_t bmc_wcid = static_cast<uint8_t>(19u - bss);

  if (!mt7925_set_idle_bss(bss, mac)) return false;

  uint8_t bss_info[116]{};
  bss_info[0] = bss;
  bss_info[2] = 7;
  uint8_t *basic = bss_info + 4;
  basic[2] = 32;
  basic[4] = 1;
  basic[5] = basic[6] = basic[13] = bss;
  basic[7] = 0xff;
  basic[8] = basic[10] = 1; /* CONNAC3 CONNECTION_INFRA_STA */
  basic[20] = basic[26] = bmc_wcid;
  std::memcpy(basic + 14, bssid, 6);
  basic[25] = is_2g ? 0x0e : 0x31; /* PHY_MODE_B|G|GN vs PHY_MODE_A|AN|AC */
  basic[28] = is_2g ? 1 : 3;       /* PHY_TYPE_ERP_INDEX vs PHY_TYPE_OFDM_INDEX */
  basic[31] = bss;                 /* wmm_idx = bss */
  uint8_t *security = basic + 32;
  security[0] = 16;
  security[2] = 8;
  security[5] = 1;
  uint8_t *qos = security + 8;
  qos[0] = 15;
  qos[2] = 8;
  qos[4] = qos_edca ? 1 : 0;
  uint8_t *mld = qos + 8;
  mld[0] = 26;
  mld[2] = 20; /* UNI_BSS_INFO_MLD */
  mld[4] = 0xff;
  mld[5] = static_cast<uint8_t>(bss + 32);
  std::memcpy(mld + 6, mac, 6);
  mld[12] = 0xff;
  mld[13] = 0xff;
  uint8_t *ifs = mld + 20;
  ifs[0] = 23;
  ifs[2] = 20;
  ifs[4] = 1;
  ifs[8] = 9;
  uint8_t *rlm = ifs + 20;
  rlm[0] = 2;
  rlm[2] = 16;
  rlm[4] = channel;
  rlm[5] = center;
  rlm[7] = eff_w == 80 ? 2 : (eff_w == 40 ? 1 : 0);
  rlm[8] = streams;
  rlm[9] = streams;
  rlm[10] = eff_w >= 40 ? 4 : 0;
  rlm[11] = sco;
  rlm[12] = band; /* 1 = 2.4 GHz, 2 = 5 GHz */
  uint8_t *mbssid = rlm + 16;
  mbssid[0] = 6;
  mbssid[2] = 8;
  if (!mt7925_mcu_send_uni(2, bss_info, sizeof(bss_info), true)) return false;

  if (qos_edca) {
    uint8_t edca[4 + 4 * 12]{};
    edca[0] = bss;
    constexpr struct {
      uint8_t cw_min, cw_max, aifs;
      uint16_t txop;
    } ac_params[4] = {
        {2, 3, 2, 47},  /* VO */
        {3, 4, 2, 94},  /* VI */
        {4, 10, 3, 0},  /* BE */
        {4, 10, 7, 0},  /* BK */
    };
    for (uint8_t ac = 0; ac < 4; ++ac) {
      uint8_t *e = edca + 4 + ac * 12;
      e[2] = 12;
      e[4] = ac;
      e[5] = 0x0f;
      e[6] = ac_params[ac].cw_min;
      e[7] = ac_params[ac].cw_max;
      e[8] = static_cast<uint8_t>(ac_params[ac].txop);
      e[9] = static_cast<uint8_t>(ac_params[ac].txop >> 8);
      e[10] = ac_params[ac].aifs;
    }
    if (!mt7925_mcu_send_uni(4, edca, sizeof(edca), true)) return false;
  }
  return true;
}

bool MtkDevice::mt7925_update_bss_rlm(uint8_t bss, uint8_t channel, unsigned width_mhz) {
  if (!_mt7925_power_ready || bss > 1 || !mt7925::is_valid_channel(channel)) return false;
  const bool is_2g = channel <= 14;
  const unsigned eff_w = mt7925::effective_width(channel, width_mhz);
  const uint8_t center = mt7925::center_channel(channel, eff_w);
  const uint8_t sco = mt7925::rlm_sco(channel, center);
  const uint8_t streams = (is_2g || eff_w <= 20) ? 1u : _mt7925_rx_streams;

  uint8_t req[20]{};
  req[0] = bss;
  req[2] = 1;
  uint8_t *rlm = req + 4;
  rlm[0] = 2;
  rlm[2] = 16;
  rlm[4] = channel;
  rlm[5] = center;
  rlm[7] = eff_w == 80 ? 2 : (eff_w == 40 ? 1 : 0);
  rlm[8] = streams;
  rlm[9] = streams;
  rlm[10] = eff_w >= 40 ? 4 : 0;
  rlm[11] = sco;
  rlm[12] = mt7925::band_for_channel(channel);
  return mt7925_mcu_send_uni(2, req, sizeof(req), true, move_timeout_ms);
}

bool MtkDevice::mt7925_roc_acquire(uint8_t channel, uint8_t bss, uint8_t type, uint16_t duration,
                                   std::string &error, unsigned width_mhz) {
  if (!_mt7925_power_ready || _mt7925_roc_until || !mt7925::is_valid_channel(channel) ||
      duration <= 10 || duration > mt7925::roc_max_awdl_ms) {
    error = "MT7925 channel request state is invalid";
    return false;
  }

  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const uint8_t request_id =
        (_mt7925_roc_request_id == UINT8_MAX) ? 1 : static_cast<uint8_t>(_mt7925_roc_request_id + 1);
    _mt7925_roc_request_id = request_id;
    const auto request = mt7925::roc_request(channel, request_id, type, bss, duration, width_mhz);
    constexpr uint32_t command = MT_MCU_UNI | 0x27;
    const uint64_t issued = plat::now_ms();
    bool ack = false;
    uint32_t granted = 0, firmware = 0;
    const unsigned wait_ms = std::min<unsigned>(duration - 10, mt7925::roc_visit_ms - 10);
    const int status = mt7925_exchange(
        command, request.data(), static_cast<uint32_t>(request.size()), wait_ms,
        [&](uint8_t seq, const uint8_t *packet, uint32_t bytes) {
          const auto lease = mt7925::roc_grant(packet, bytes, channel, request_id, type, granted,
                                               bss, duration, width_mhz);
          if (lease == mt7925::Grant::invalid) {
            if (bytes >= 68 && packet[36] == 0x27) {
              const uint8_t *g = packet + 48;
              log("MT7925 ROC grant mismatch: bss=" + std::to_string(g[4]) +
                  " tok=" + std::to_string(g[5]) + " st=" + std::to_string(g[6]) +
                  " ch=" + std::to_string(g[7]) + " sco=" + std::to_string(g[8]) +
                  " band=" + std::to_string(g[9]) + " cw=" + std::to_string(g[10]) +
                  " c1=" + std::to_string(g[11]) + " c2=" + std::to_string(g[12]) +
                  " type=" + std::to_string(g[13]) +
                  " ms=" + std::to_string(mt7925::rx_word(g + 16)));
            }
            return int(MT_FW_PROTOCOL);
          }
          if (lease == mt7925::Grant::pending) {
            const int result = mt_mcu_response(command, seq, packet, bytes, &firmware);
            if (result != MT_OK && result != MT_PENDING) return result;
            if (result == MT_OK) {
              if (firmware) return int(MT_FW_PROTOCOL);
              ack = true;
            }
          }
          return ack && granted ? int(MT_OK) : int(MT_PENDING);
        });
    const auto elapsed = plat::now_ms() - issued;
    if (status) {
      error = "MT7925 ROC acquire failed on channel " + std::to_string(channel) +
              " BSS=" + std::to_string(bss) + " type=" + std::to_string(type) +
              " transport=" + std::to_string(status) + " firmware=" + std::to_string(firmware) +
              " ack=" + std::to_string(ack) + " grant_ms=" + std::to_string(granted);
      return false;
    }

    _mt7925_roc_bss = bss;
    _mt7925_roc_until = issued + granted;
    if (elapsed + 10 < granted) {
      _channel = channel;
      if (_verbose) {
        log("MT7925 ROC granted: channel=" + std::to_string(channel) +
            " duration_ms=" + std::to_string(granted));
      }
      return true;
    }

    const auto expired = "MT7925 ROC grant arrived too late: channel=" + std::to_string(channel) +
                         " elapsed_ms=" + std::to_string(elapsed) +
                         " grant_ms=" + std::to_string(granted);
    log(expired);
    if (!mt7925_roc_release()) {
      error = expired + " and abort was not acknowledged";
      return false;
    }
    if (attempt == 1) error = expired;
  }
  return false;
}

bool MtkDevice::mt7925_roc_release() {
  const bool ack = mt7925::abort_roc_once(_mt7925_roc_until, [&](unsigned timeout) {
    const auto request = mt7925::roc_abort(_mt7925_roc_request_id, _mt7925_roc_bss);
    return mt7925_mcu_send_uni(0x27, request.data(), request.size(), true, timeout, true);
  });
  if (ack && _verbose) log("MT7925 ROC abort acknowledged");
  return ack;
}

/* --------------------------------------------------------------------------
 * TX Ring Submission & Reclaim
 * -------------------------------------------------------------------------- */
void MtkDevice::mt7925_flush_awdl_tx() {
  if (!_mt7925_tx.unpublished()) return;
  plat::mem_barrier();
  _mt7925_tx.publish(plat::now_ms());
  wr(_tx_data.reg_base + 8, _mt7925_tx.head());
}

void MtkDevice::mt7925_reclaim_awdl_tx(bool check_fetch) {
  if (_mt7925_tx_fault || (!check_fetch && !_mt7925_tx.front_completed())) return;
  if (_mt7925_tx.unfetched()) {
    const uint32_t fetched = rr(_tx_data.reg_base + 0xc);
    if (!_mt7925_tx.observe_fetch(fetched)) {
      _mt7925_tx_fault = true;
      log("MT7925 TX0 reported an invalid DMA consumer index=" + std::to_string(fetched));
      return;
    }
  }
  (void)_mt7925_tx.reclaim();
}

/* --------------------------------------------------------------------------
 * Unified MtkDevice Interface (Monitor, Channel Switch, STA, BA, TX)
 * -------------------------------------------------------------------------- */
bool MtkDevice::mt7925_start_monitor(uint8_t channel, unsigned width_mhz,
                                     const uint8_t address[6]) {
  if (!mt7925::is_valid_channel(channel)) {
    log("MT7925 start_monitor: invalid channel " + std::to_string(channel));
    return false;
  }

  std::string err;
  if (!_mt7925_power_ready && !mt7925_program_concurrent_power(_country, err)) {
    log("MT7925 start_monitor power setup failed: " + err);
    return false;
  }

  static constexpr uint8_t kRadioOn[12] = {0, 0, 0, 0, 0, 0, 8, 0, 1, 0, 0, 0};
  uint8_t filter[72]{0, 0, 0, 0, 0x0c, 0, 68, 0};
  mt7925::put32(filter + 12, (1u << 31) | (1u << 6)); /* Enable other-BSS RX */
  uint8_t clr_filter[72]{0, 0, 0, 0, 0x0c, 0, 68, 0, 1};
  mt7925::put32(clr_filter + 16,
                MT_WF_RFCR_DROP_OTHER_BEACON | MT_WF_RFCR_DROP_OTHER_BSS |
                    MT_WF_RFCR_DROP_OTHER_UC | MT_WF_RFCR_DROP_A2_BSSID |
                    MT_WF_RFCR_DROP_A3_BSSID | MT_WF_RFCR_DROP_A3_MAC |
                    MT_WF_RFCR_DROP_BCAST | MT_WF_RFCR_DROP_MCAST);
  clr_filter[20] = 2; /* clear bits */

  static constexpr char kFullPower[] = "KeepFullPwr 1";
  static constexpr char kThermalG[] = "ThermalProtGband 0 100 90 80 30 1 1 115 105 5";
  static constexpr char kThermalA[] = "ThermalProtAband 1 100 90 80 30 1 1 115 105 5";
  static constexpr uint8_t kSnifferEnable[12] = {0, 0, 0, 0, 0, 0, 8, 0, 1, 0, 0, 0};

  if (!mt7925_mcu_send_uni(8, kRadioOn, sizeof(kRadioOn), true) ||
      !mt7925_mcu_send_uni(8, filter, sizeof(filter), true) ||
      !mt7925_chip_cmd(kFullPower, sizeof(kFullPower)) ||
      !mt7925_chip_cmd(kThermalG, sizeof(kThermalG)) ||
      !mt7925_chip_cmd(kThermalA, sizeof(kThermalA)) ||
      !mt7925_mcu_send_uni(8, clr_filter, sizeof(clr_filter), true) ||
      !mt7925_mcu_send_uni(0x24, kSnifferEnable, sizeof(kSnifferEnable), true)) {
    log("MT7925 start_monitor radio/filter setup failed");
    return false;
  }

  /* Preserve native 802.11 MPDUs after sniffer enable (MDP_DCR0 readback). */
  if (!mt7925_native_awdl_rx(err)) {
    log("MT7925 start_monitor failed: " + err);
    return false;
  }

  _mt7925_concurrent = true;
  _mt7925_awdl_channel = channel;
  std::memcpy(_mt7925_awdl_mac.data(), address, 6);
  std::memcpy(_mac.data(), address, 6);

  if (!mt7925_setup_bss(0, channel, width_mhz, address, mt7925::awdl_bssid.data(), true)) {
    log("MT7925 start_monitor BSS 0 setup failed");
    return false;
  }
  log("listening on " + mac_str(address));

  if (!mt7925_set_channel(channel, width_mhz, CH_SWITCH_NORMAL)) {
    return false;
  }
  log("tuned to ch " + std::to_string(_channel) + " (" + std::to_string(_width_mhz) + " MHz)");
  return true;
}

bool MtkDevice::mt7925_set_address(const uint8_t address[6]) {
  const bool changed = std::memcmp(_mt7925_awdl_mac.data(), address, 6) != 0;
  std::memcpy(_mt7925_awdl_mac.data(), address, 6);
  std::memcpy(_mac.data(), address, 6);
  if (changed) {
    _peers.fill(PeerEntry{});
    _pending_addba.fill(PendingAddba{});
    _pending_tx_addba.fill(PendingAddba{});
  }
  if (!_mt7925_power_ready) return true;
  const uint8_t ch = mt7925::is_valid_channel(_channel) ? _channel : _mt7925_awdl_channel;
  const bool ok = mt7925_setup_bss(0, ch, _width_mhz, address, mt7925::awdl_bssid.data(), true);
  if (ok && changed) log("listening on " + mac_str(address));
  return ok;
}

bool MtkDevice::mt7925_set_station_identity(const uint8_t addr[6], const uint8_t bssid[6]) {
  if (!_mt7925_power_ready) return false;
  std::memcpy(_sta_addr.data(), addr, 6);
  std::memcpy(_sta_bssid.data(), bssid, 6);
  const uint8_t ch = mt7925::is_valid_channel(_channel) ? _channel : _mt7925_awdl_channel;
  _sta_iface_up = mt7925_setup_bss(kStationOmac, ch, _width_mhz, addr, bssid, true);
  if (_sta_iface_up) {
    log("MT7925 STA BSS 1 enabled: ch=" + std::to_string(ch));
  }
  return _sta_iface_up;
}

void MtkDevice::mt7925_clear_station_identity() {
  if (!_sta_iface_up) return;
  if (_mt7925_roc_until && _mt7925_roc_bss == kStationOmac) {
    (void)mt7925_roc_release();
  }
  (void)mt7925_set_idle_bss(kStationOmac, _sta_addr.data());
  _sta_iface_up = false;
}

bool MtkDevice::mt7925_set_channel(uint8_t channel, unsigned width_mhz, uint8_t /*switch_reason*/) {
  if (!_mt7925_power_ready || _mt7925_mcu_fault || _mt7925_tx_fault) return false;
  if (!mt7925::is_valid_channel(channel)) return false;

  const unsigned eff_w = mt7925::effective_width(channel, width_mhz);
  const uint8_t want_bss =
      (_sta_iface_up && channel >= 36 && channel != _mt7925_awdl_channel) ? kStationOmac
                                                                          : uint8_t{0};

  /* Bounded drain before leaving the current channel so switches never stall. */
  if (_mt7925_roc_until) {
    mt7925_flush_awdl_tx();
    const uint64_t drain_end =
        std::min(_mt7925_roc_until > 2 ? _mt7925_roc_until - 2 : 0, plat::now_ms() + 20);
    while (_mt7925_tx.pending() && !_mt7925_tx_fault && plat::now_ms() < drain_end) {
      if (!mt7925_poll({})) plat::sleep_ms(1);
    }
    (void)mt7925_roc_release();
  }

  /* When crossing between 2.4 GHz and 5 GHz, re-initialize BSS 0 so phymode
   * and nonht_basic_phy match the new band. Within the same band, only update RLM. */
  const bool band_changed = _channel != 0 && ((channel <= 14) != (_channel <= 14));
  if (band_changed) {
    _peers.fill(PeerEntry{});
    _pending_addba.fill(PendingAddba{});
    _pending_tx_addba.fill(PendingAddba{});
    if (!mt7925_setup_bss(0, channel, eff_w, _mt7925_awdl_mac.data(), mt7925::awdl_bssid.data(),
                          true)) {
      return false;
    }
  } else {
    if (!mt7925_update_bss_rlm(0, channel, eff_w)) return false;
  }
  if (_sta_iface_up && channel >= 36 && !mt7925_update_bss_rlm(kStationOmac, channel, eff_w)) {
    return false;
  }

  const uint8_t center = mt7925::center_channel(channel, eff_w);
  const uint8_t sco = mt7925::rlm_sco(channel, center);
  uint8_t sniffer_cfg[20]{0, 0, 0, 0, 1, 0, 16, 0};
  sniffer_cfg[10] = mt7925::band_for_channel(channel); /* 1 = 2.4 GHz, 2 = 5 GHz */
  sniffer_cfg[11] = eff_w == 80 ? 1 : 0;
  sniffer_cfg[12] = channel;
  sniffer_cfg[13] = sco;
  sniffer_cfg[14] = center;
  sniffer_cfg[16] = 1; /* drop bad FCS */
  if (!mt7925_mcu_send_uni(0x24, sniffer_cfg, sizeof(sniffer_cfg), true, move_timeout_ms)) {
    return false;
  }

  std::string err;
  if (!mt7925_roc_acquire(channel, want_bss, 1, mt7925::roc_max_awdl_ms, err, eff_w)) {
    if (_verbose) log("MT7925 set_channel(" + std::to_string(channel) + ") failed: " + err);
    return false;
  }
  _channel = channel;
  _width_mhz = eff_w;
  if (_verbose) {
    log("tuned to ch " + std::to_string(channel) + " (" + std::to_string(eff_w) + " MHz)");
  }
  return true;
}

bool MtkDevice::mt7925_renew_concurrent_roc(const RxHandler &on_rx) {
  if (!_mt7925_power_ready || !_mt7925_roc_until || _mt7925_mcu_fault || _mt7925_tx_fault) {
    return false;
  }
  const uint8_t ch = _channel;
  const unsigned w = _width_mhz;
  const uint8_t bss = _mt7925_roc_bss;
  mt7925_flush_awdl_tx();
  const uint64_t drain_end =
      std::min(_mt7925_roc_until > 2 ? _mt7925_roc_until - 2 : 0, plat::now_ms() + 25);
  while (_mt7925_tx.pending() && !_mt7925_tx_fault && plat::now_ms() < drain_end) {
    const auto saved_until = _mt7925_roc_until;
    _mt7925_roc_until = 0;
    const size_t got = mt7925_poll(on_rx);
    _mt7925_roc_until = saved_until;
    if (!got && _mt7925_tx.pending()) plat::sleep_ms(1);
  }
  if (!mt7925_roc_release()) return false;
  std::string err;
  if (!mt7925_roc_acquire(ch, bss, 1, mt7925::roc_max_awdl_ms, err, w)) {
    log("MT7925 concurrent ROC renew failed on ch " + std::to_string(ch) + ": " + err);
    return false;
  }
  ++_mt7925_awdl_renewals;
  return true;
}

bool MtkDevice::mt7925_sta_add(uint8_t wcid, const uint8_t peer[6], const PeerCaps &caps,
                               uint8_t omac) {
  if (!_mt7925_power_ready || wcid == 0 || wcid >= MT792X_WTBL_RESERVED || omac > 1) return false;
  const uint16_t hw_wcid = mt7925_hw_wcid(wcid);
  const bool is_2g = (_channel > 0 && _channel <= 14 && omac == 0);
  const bool use_vht = caps.vht && !is_2g;
  const auto msg = mt7925::sta_rec_add_vht(omac, hw_wcid, peer, caps.ht_cap, caps.ampdu_params,
                                           caps.rx_mcs, use_vht, caps.vht_cap, caps.vht_mcs,
                                           caps.bw, caps.nss, is_2g);
  const bool ok = mt7925_mcu_send_uni(3, msg.data(), mt7925::sta_rec_add_size(use_vht), true,
                                      sta_cmd_timeout_ms);
  log("MT7925 station wcid=" + std::to_string(hw_wcid) + " bss=" + std::to_string(omac) +
      (ok ? " added" : " failed"));
  return ok;
}

bool MtkDevice::mt7925_sta_remove(uint8_t wcid, const uint8_t /*peer*/[6], uint8_t omac) {
  if (!_mt7925_power_ready || wcid == 0 || wcid >= MT792X_WTBL_RESERVED || omac > 1) return false;
  const uint16_t hw_wcid = mt7925_hw_wcid(wcid);
  const auto msg = mt7925::sta_rec_remove_mld(omac, hw_wcid);
  return mt7925_mcu_send_uni(3, msg.data(), msg.size(), true, sta_cmd_timeout_ms);
}

bool MtkDevice::mt7925_sta_ba(uint8_t wcid, const uint8_t /*peer*/[6], uint8_t tid, uint16_t ssn,
                              uint16_t win, bool tx, bool enable, uint8_t omac) {
  if (!_mt7925_power_ready || wcid == 0 || wcid >= MT792X_WTBL_RESERVED || tid >= 8 || omac > 1) {
    return false;
  }
  const uint16_t hw_wcid = mt7925_hw_wcid(wcid);
  const uint16_t clamped_win = enable ? (win == 0 || win > 64 ? 64 : win) : 0;
  const auto dir = tx ? mt7925::AwDlBaDirection::tx : mt7925::AwDlBaDirection::rx;
  const auto msg = mt7925::sta_rec_ba(omac, hw_wcid, tid, ssn & 0x0fffu, clamped_win, enable, dir);
  return mt7925_mcu_send_uni(3, msg.data(), msg.size(), true, sta_cmd_timeout_ms);
}

int MtkDevice::mt7925_queue_general_frame(const uint8_t *frame, size_t len, uint8_t bss,
                                          uint16_t wcid, bool fixed_rate, uint8_t rate_slot,
                                          bool no_ack, bool force_altx, bool allow_ba, bool kick) {
  if (!_mt7925_power_ready || !_mt7925_roc_until || _mt7925_tx_fault || _mt7925_mcu_fault) {
    return -1;
  }
  mt7925_reclaim_awdl_tx();
  if (_mt7925_tx.pending() >= mt7925::AwDlTxSlots::max_inflight) return -1;

  const unsigned index = _mt7925_tx.head();
  const uint16_t token = _mt7925_tx.next_token();
  auto &page = _tx_data.bufs[index];
  if (!mt7925::write_general_txwi(page.va, page.size, page.pa, token, frame, len, bss, wcid,
                                  fixed_rate, rate_slot, no_ack, force_altx, allow_ba)) {
    return -1;
  }

  MtkDesc &desc = _tx_data.descs()[index];
  desc.buf0 = page.pa;
  desc.buf1 = 0;
  desc.info = 0;
  desc.ctrl = (64u << 16) | MT_DMA_CTL_LAST_SEC0;
  _mt7925_tx.prepare(token, allow_ba ? mt7925::AwDlTxKind::ampdu : mt7925::AwDlTxKind::basic);
  if (kick) {
    mt7925_flush_awdl_tx();
    _tx_pending_kick = false;
  } else {
    _tx_pending_kick = true;
  }
  return static_cast<int>(token);
}

int MtkDevice::mt7925_transmit_raw(const uint8_t *frame, size_t len, uint16_t rate_val,
                                   uint8_t omac, bool no_ack, bool force_altx) {
  const uint8_t bss = omac > 1 ? 0 : omac;
  const uint16_t bmc_wcid = static_cast<uint16_t>(19u - bss);
  const uint8_t slot = mt7925_map_rate_val(rate_val);
  return mt7925_queue_general_frame(frame, len, bss, bmc_wcid, true, slot, no_ack, force_altx,
                                    false, true);
}

int MtkDevice::mt7925_transmit_sta(const uint8_t *frame, size_t len, uint8_t wcid, uint8_t omac) {
  const uint8_t bss = omac > 1 ? kStationOmac : omac;
  const uint16_t hw_wcid = mt7925_hw_wcid(wcid);
  return mt7925_queue_general_frame(frame, len, bss, hw_wcid, false, 0, false, false, false, true);
}

int MtkDevice::mt7925_get_or_add_awdl_peer(const uint8_t mac[6]) {
  /* Slots 1..16 belong to AWDL peers on BSS 0 (17 = AP, 18 = BSS1 BMC, 19 = BSS0 BMC). */
  constexpr uint8_t kMaxAwdlPeer = 16;
  const uint64_t now = plat::now_ms();
  for (uint8_t i = 1; i <= kMaxAwdlPeer; ++i) {
    if (_peers[i].in_use && std::memcmp(_peers[i].mac.data(), mac, 6) == 0) {
      _peers[i].last_used_ms = now;
      return i;
    }
  }
  uint8_t slot = 0;
  uint64_t oldest = UINT64_MAX;
  for (uint8_t i = 1; i <= kMaxAwdlPeer; ++i) {
    if (!_peers[i].in_use) {
      slot = i;
      break;
    }
    if (_peers[i].last_used_ms < oldest) {
      oldest = _peers[i].last_used_ms;
      slot = i;
    }
  }
  if (!slot) return -1;
  if (_peers[slot].in_use) {
    (void)mt7925_sta_remove(slot, _peers[slot].mac.data(), 0);
  }
  PeerCaps caps{};
  if (_channel <= 14) {
    caps.vht = false;
    caps.bw = 0;
    caps.nss = 1;
    caps.rx_mcs[1] = 0;
  }
  if (!mt7925_sta_add(slot, mac, caps, 0)) return -1;
  _peers[slot].in_use = true;
  std::memcpy(_peers[slot].mac.data(), mac, 6);
  _peers[slot].tx_ba_active.fill(false);
  _peers[slot].last_used_ms = now;
  return slot;
}

void MtkDevice::mt7925_snoop_rx_ba(const uint8_t *frame, size_t len) {
  if (!frame || len < 28 || (frame[0] & 0xfcu) != 0xd0u || frame[24] != 3u) return;
  if (std::memcmp(frame + 4, _mt7925_awdl_mac.data(), 6) != 0) return;
  const uint8_t action = frame[25];
  const uint8_t *peer = frame + 10;
  if (action == 0u && len >= 33) { /* ADDBA Request from peer -> record for RX BA */
    const uint8_t token = frame[26];
    const uint16_t params = static_cast<uint16_t>(frame[27] | (frame[28] << 8));
    const uint16_t seqctl = static_cast<uint16_t>(frame[31] | (frame[32] << 8));
    const uint8_t tid = static_cast<uint8_t>((params >> 2) & 0x0fu);
    const uint16_t ssn = static_cast<uint16_t>((seqctl >> 4) & 0x0fffu);
    if (tid >= 8) return;
    size_t idx = 0;
    for (size_t i = 0; i < _pending_addba.size(); ++i) {
      if (!_pending_addba[i].valid ||
          (std::memcmp(_pending_addba[i].peer_mac.data(), peer, 6) == 0 &&
           _pending_addba[i].tid == tid)) {
        idx = i;
        break;
      }
    }
    _pending_addba[idx].valid = true;
    std::memcpy(_pending_addba[idx].peer_mac.data(), peer, 6);
    _pending_addba[idx].tid = tid;
    _pending_addba[idx].token = token;
    _pending_addba[idx].ssn = ssn;
    _pending_addba[idx].ts_ms = plat::now_ms();
  } else if (action == 1u && len >= 33) { /* ADDBA Response from peer -> enable TX BA */
    const uint8_t token = frame[26];
    const uint16_t status = static_cast<uint16_t>(frame[27] | (frame[28] << 8));
    const uint16_t params = static_cast<uint16_t>(frame[29] | (frame[30] << 8));
    const uint8_t tid = static_cast<uint8_t>((params >> 2) & 0x0fu);
    const uint16_t win = static_cast<uint16_t>((params >> 6) & 0x03ffu);
    if (status != 0 || tid >= 8) return;
    for (auto &p : _pending_tx_addba) {
      if (p.valid && p.token == token && p.tid == tid &&
          std::memcmp(p.peer_mac.data(), peer, 6) == 0) {
        p.valid = false;
        const int wcid = mt7925_get_or_add_awdl_peer(peer);
        if (wcid > 0 &&
            mt7925_sta_ba(static_cast<uint8_t>(wcid), peer, tid, p.ssn, win, true, true, 0)) {
          _peers[wcid].tx_ba_active[tid] = true;
        }
        break;
      }
    }
  } else if (action == 2u) { /* DELBA from peer */
    const uint16_t params = static_cast<uint16_t>(frame[26] | (frame[27] << 8));
    const bool initiator = ((params >> 11) & 1u) != 0;
    const uint8_t tid = static_cast<uint8_t>((params >> 12) & 0x0fu);
    if (tid >= 8) return;
    for (uint8_t i = 1; i <= 16; ++i) {
      if (_peers[i].in_use && std::memcmp(_peers[i].mac.data(), peer, 6) == 0) {
        if (initiator) {
          (void)mt7925_sta_ba(i, peer, tid, 0, 0, false, false, 0);
        } else {
          (void)mt7925_sta_ba(i, peer, tid, 0, 0, true, false, 0);
          _peers[i].tx_ba_active[tid] = false;
        }
        break;
      }
    }
  }
}

void MtkDevice::mt7925_snoop_tx_ba(const uint8_t *frame, size_t len) {
  if (!frame || len < 28 || (frame[0] & 0xfcu) != 0xd0u || frame[24] != 3u) return;
  const uint8_t action = frame[25];
  const uint8_t *peer = frame + 4;
  if (action == 1u && len >= 33) { /* ADDBA Response sent by us -> enable RX BA */
    const uint8_t token = frame[26];
    const uint16_t status = static_cast<uint16_t>(frame[27] | (frame[28] << 8));
    const uint16_t params = static_cast<uint16_t>(frame[29] | (frame[30] << 8));
    const uint8_t tid = static_cast<uint8_t>((params >> 2) & 0x0fu);
    const uint16_t win = static_cast<uint16_t>((params >> 6) & 0x03ffu);
    if (status != 0 || tid >= 8) return;
    for (auto &p : _pending_addba) {
      if (p.valid && p.token == token && p.tid == tid &&
          std::memcmp(p.peer_mac.data(), peer, 6) == 0) {
        p.valid = false;
        const int wcid = mt7925_get_or_add_awdl_peer(peer);
        if (wcid > 0) {
          (void)mt7925_sta_ba(static_cast<uint8_t>(wcid), peer, tid, p.ssn, win, false, true, 0);
        }
        break;
      }
    }
  } else if (action == 0u && len >= 33) { /* ADDBA Request sent by us -> record for TX BA */
    const uint8_t token = frame[26];
    const uint16_t params = static_cast<uint16_t>(frame[27] | (frame[28] << 8));
    const uint16_t seqctl = static_cast<uint16_t>(frame[31] | (frame[32] << 8));
    const uint8_t tid = static_cast<uint8_t>((params >> 2) & 0x0fu);
    const uint16_t ssn = static_cast<uint16_t>((seqctl >> 4) & 0x0fffu);
    if (tid >= 8) return;
    (void)mt7925_get_or_add_awdl_peer(peer);
    size_t idx = 0;
    for (size_t i = 0; i < _pending_tx_addba.size(); ++i) {
      if (!_pending_tx_addba[i].valid ||
          (std::memcmp(_pending_tx_addba[i].peer_mac.data(), peer, 6) == 0 &&
           _pending_tx_addba[i].tid == tid)) {
        idx = i;
        break;
      }
    }
    _pending_tx_addba[idx].valid = true;
    std::memcpy(_pending_tx_addba[idx].peer_mac.data(), peer, 6);
    _pending_tx_addba[idx].tid = tid;
    _pending_tx_addba[idx].token = token;
    _pending_tx_addba[idx].ssn = ssn;
    _pending_tx_addba[idx].ts_ms = plat::now_ms();
  }
}

bool MtkDevice::mt7925_transmit_awdl(const uint8_t *frame, size_t len, const TxParams &params,
                                     bool kick) {
  if (!frame || len < 16) return false;
  mt7925_snoop_tx_ba(frame, len);
  const bool multicast = (frame[4] & 1u) != 0;
  const bool no_ack = multicast || params.no_ack;
  const bool is_qos_data = (frame[0] & 0xfcu) == 0x88u;
  const uint8_t tid = (is_qos_data && len >= 26) ? static_cast<uint8_t>(frame[24] & 0x07u) : 0u;

  uint16_t wcid = 19u; /* BSS 0 broadcast WCID */
  bool fixed_rate = true;
  bool allow_ba = false;
  if (is_qos_data && !multicast && !no_ack) {
    const int peer_wcid = mt7925_get_or_add_awdl_peer(frame + 4);
    if (peer_wcid > 0) {
      wcid = static_cast<uint16_t>(peer_wcid);
      fixed_rate = false;
      allow_ba = _peers[peer_wcid].tx_ba_active[tid];
    }
  }
  const uint8_t rate_slot = fixed_rate ? mt7925_map_tx_params(params) : 0u;
  const bool force_altx = (frame[0] & 0x0cu) == 0u;
  return mt7925_queue_general_frame(frame, len, 0, wcid, fixed_rate, rate_slot, no_ack, force_altx,
                                    allow_ba, kick) >= 0;
}

} // namespace mtk
