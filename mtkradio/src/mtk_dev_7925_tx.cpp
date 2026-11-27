// SPDX-License-Identifier: GPL-2.0-only
#include "mtk_dev.h"
#include "mtk_platform.h"
#include "awdl_policy.h"
#include "mt7925_active.h"
#include "mt7925_ba.h"
#include "mt7925_mcu_wait.h"
#include "mt7925_regs.h"
#include "regproxy_ioctl.h"

#include <chrono>

extern "C" {
#include "firmware.h"
}

namespace mtk {

bool MtkDevice::mt7925_set_idle_bss(uint8_t bss, const uint8_t mac[6]) {
  uint8_t device[16]{};
  device[0] = bss;
  device[1] = 0xff;
  device[6] = 12;  // DEV_INFO_ACTIVE
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
  initial[12] = 1;  // disconnected idle BSS
  initial[20] = initial[26] = static_cast<uint8_t>(19 - bss);
  return mt7925_mcu_send_uni(2, idle, sizeof(idle), true);
}

bool MtkDevice::mt7925_prepare_awdl_bss(uint8_t home, const uint8_t mac[6], bool block_ack) {
  if (!_mt7925_power_ready || (home != 36 && home != _mt7925_awdl_channel)) return false;
  constexpr uint8_t bss = mt7925::awdl_bss;
  const bool wide80 = _width_mhz == 80 && home == _mt7925_awdl_channel;
  if (!mt7925_set_idle_bss(bss, mac)) return false;
  uint8_t bss_info[116]{};
  bss_info[0] = bss;
  bss_info[2] = 7;
  uint8_t *basic = bss_info + 4;
  basic[2] = 32;
  basic[4] = 1;
  basic[5] = basic[6] = basic[13] = bss;
  basic[7] = 0xff;
  basic[8] = basic[10] = 1;  // CONNAC3 CONNECTION_INFRA_STA
  basic[20] = basic[26] = static_cast<uint8_t>(mt7925::awdl_broadcast_wcid);
  std::memcpy(basic + 14, mt7925::awdl_bssid.data(), mt7925::awdl_bssid.size());
  basic[25] = block_ack ? (wide80 ? 0x31 : 0x11) : 1;  // PHY_MODE_A | AN | AC
  basic[28] = 3;
  basic[31] = bss;
  uint8_t *security = basic + 32;
  security[0] = 16; security[2] = 8; security[5] = 1;
  uint8_t *qos = security + 8;
  qos[0] = 15; qos[2] = 8;
  qos[4] = block_ack;
  uint8_t *mld = qos + 8;
  mld[0] = 26; mld[2] = 20;  // UNI_BSS_INFO_MLD
  mld[4] = 0xff;
  mld[5] = static_cast<uint8_t>(bss + 32);
  std::memcpy(mld + 6, mac, 6);
  mld[12] = 0xff;
  mld[13] = 0xff;
  uint8_t *ifs = mld + 20;
  ifs[0] = 23; ifs[2] = 20; ifs[4] = 1; ifs[8] = 9;
  uint8_t *rlm = ifs + 20;
  rlm[0] = 2; rlm[2] = 16;
  rlm[4] = home;
  rlm[5] = mt7925::center_channel_5g(home, wide80 ? 80 : 20);
  rlm[7] = wide80 ? 2 : 0;   // CMD_CBW_80MHZ / CMD_CBW_20MHZ
  rlm[8] = 1;  // tx_streams: the host rate table supports one stream.
  rlm[9] = wide80 ? _mt7925_rx_streams : 1;
  rlm[10] = wide80 ? 4 : 0;  // HT 40M allowed
  rlm[11] = mt7925::rlm_sco_5g(home, rlm[5]);
  rlm[12] = 2;               // 5 GHz
  uint8_t *mbssid = rlm + 16;
  mbssid[0] = 6; mbssid[2] = 8;
  if (!mt7925_mcu_send_uni(2, bss_info, sizeof(bss_info), true)) return false;

  // Configure WMM EDCA parameters on BSS 1 (MCU_UNI_CMD_EDCA_UPDATE = 0x04).
  if (block_ack) {
    uint8_t edca[4 + 4 * 12]{};
    edca[0] = bss;
    // Standard WMM ACs (0=VO, 1=VI, 2=BE, 3=BK): cw_min/max = fls(cw).
    constexpr struct { uint8_t cw_min, cw_max, aifs; uint16_t txop; } ac_params[4] = {
        {2, 3, 2, 47},   // VO
        {3, 4, 2, 94},   // VI
        {4, 10, 3, 0},   // BE
        {4, 10, 7, 0},   // BK
    };
    for (uint8_t ac = 0; ac < 4; ++ac) {
      uint8_t *e = edca + 4 + ac * 12;
      e[2] = 12;
      e[4] = ac;
      e[5] = 0x0f;  // WMM_PARAM_SET
      e[6] = ac_params[ac].cw_min;
      e[7] = ac_params[ac].cw_max;
      e[8] = static_cast<uint8_t>(ac_params[ac].txop);
      e[9] = static_cast<uint8_t>(ac_params[ac].txop >> 8);
      e[10] = ac_params[ac].aifs;
    }
    if (!mt7925_mcu_send_uni(4, edca, sizeof(edca), true)) return false;
  }

  _channel = home;
  log("MT7925 AWDL BSS1 acknowledged, home=" + std::to_string(home) +
      " width_mhz=" + std::to_string(wide80 ? 80 : 20));
  return true;
}

bool MtkDevice::mt7925_roc_acquire(uint8_t channel, uint8_t bss, uint8_t type, uint16_t duration,
                                   std::string &error, unsigned width_mhz) {
  if (!_mt7925_concurrent &&
      !((bss == mt7925::awdl_bss && type == 1 && channel == _mt7925_awdl_channel && width_mhz == _width_mhz) ||
        (bss == 0 && type == 0 && channel == 36 && width_mhz == 20))) {
    error = "MT7925 channel grant request differs from the approved AWDL or home policy";
    return false;
  }
  if (!_mt7925_power_ready || _mt7925_roc_until ||
      channel < 36 || channel > 177 || duration <= 10 || duration > mt7925::roc_max_awdl_ms) {
    error = "MT7925 channel request state is invalid";
    return false;
  }

  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const uint8_t token = _mt7925_roc_token = static_cast<uint8_t>(_mt7925_roc_token % 255 + 1);
    const auto request = mt7925::roc_request(channel, token, type, bss, duration, width_mhz);
    constexpr uint32_t command = MT_MCU_UNI | 0x27;
    const uint64_t issued = plat::now_ms();
    bool ack = false;
    uint32_t granted = 0, firmware = 0;
    unsigned traced = 0;
    const unsigned wait_ms = std::min<unsigned>(duration - 10, mt7925::roc_visit_ms - 10);
    const int status = mt7925_exchange(command, request.data(), static_cast<uint32_t>(request.size()), wait_ms,
        [&](uint8_t seq, const uint8_t *packet, uint32_t bytes) {
          if (_verbose && traced++ < 12) {
            log("MT7925 ROC RX0 event: eid=" + std::to_string(packet[36]) +
                " seq=" + std::to_string(packet[37]) + " flag=" + std::to_string(packet[38]) +
                " bytes=" + std::to_string(bytes));
          }
          const auto lease = mt7925::roc_grant(packet, bytes, channel, token, type, granted, bss, duration, width_mhz);
          if (lease == mt7925::Grant::invalid) {
            if (bytes >= 68 && packet[36] == 0x27) {
              const uint8_t *g = packet + 48;
              log("MT7925 ROC grant mismatch: bss=" + std::to_string(g[4]) +
                  " tok=" + std::to_string(g[5]) + " st=" + std::to_string(g[6]) +
                  " ch=" + std::to_string(g[7]) + " sco=" + std::to_string(g[8]) +
                  " band=" + std::to_string(g[9]) + " cw=" + std::to_string(g[10]) +
                  " c1=" + std::to_string(g[11]) + " c2=" + std::to_string(g[12]) +
                  " type=" + std::to_string(g[13]) + " ms=" + std::to_string(mt7925::rx_word(g + 16)));
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
        log("MT7925 ROC acknowledged and granted: channel=" + std::to_string(channel) +
            " duration_ms=" + std::to_string(granted));
      }
      return true;
    }

    // A delayed valid grant may still be active in firmware. Abort its token
    // before requesting a fresh lease, with no transmit on the old grant.
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
    const auto request = mt7925::roc_abort(_mt7925_roc_token, _mt7925_roc_bss);
    return mt7925_mcu_send_uni(0x27, request.data(), request.size(), true, timeout, true);
  });
  if (ack && _verbose) log("MT7925 ROC abort acknowledged");
  return ack;
}

unsigned MtkDevice::mt7925_awdl_tx_available() const {
  const auto now = plat::now_ms();
  return now < _mt7925_roc_until ? _mt7925_tx.available_on_channel(_mt7925_roc_until - now) : 0;
}

bool MtkDevice::mt7925_queue_awdl_frame(const uint8_t *frame, size_t bytes, mt7925::AwDlRate rate,
                                      mt7925::AwDlTxFormat format, uint16_t *token_out) {
  if (!_mt7925_power_ready || _mt7925_roc_bss != mt7925::awdl_bss || !_mt7925_roc_until ||
      _mt7925_tx_fault || !mt7925_awdl_tx_available() ||
      (format == mt7925::AwDlTxFormat::ampdu && !_mt7925_tx_ba_active))
    return false;
  const unsigned index = _mt7925_tx.head();
  const uint16_t token = _mt7925_tx.next_token();
  auto &page = _tx_data.bufs[index];
  if (!mt7925::write_awdl_txwi(page.va, page.size, page.pa, token, frame, bytes, rate, format))
    return false;

  MtkDesc &desc = _tx_data.descs()[index];
  desc.buf0 = page.pa;
  desc.buf1 = 0;
  desc.info = 0;
  desc.ctrl = (64u << 16) | MT_DMA_CTL_LAST_SEC0;
  auto kind = mt7925::AwDlTxKind::basic;
  if (format == mt7925::AwDlTxFormat::amsdu) kind = mt7925::AwDlTxKind::aggregate;
  else if (format == mt7925::AwDlTxFormat::ampdu) kind = mt7925::AwDlTxKind::ampdu;
  else if (mt7925::awdl_fixed_rate_code(rate)) kind = mt7925::AwDlTxKind::fast;
  _mt7925_tx.prepare(token, kind);
  _mt7925_tracked_tx[index] = token_out != nullptr;
  if (token_out) *token_out = token;
  return true;
}

void MtkDevice::mt7925_take_awdl_completions(std::vector<Mt7925TxCompletion> &out) {
  out.clear();
  out.swap(_mt7925_tx_completions);
}

bool MtkDevice::mt7925_read_awdl_aggregation(Mt7925AwDlAggregation &out) {
  if (!is_mt7925() || !_mt7925_active_lab || !_mt7925_power_ready || !_mt7925_tx_ba_active ||
      _channel != _mt7925_awdl_channel || _mt7925_roc_bss != mt7925::awdl_bss || plat::now_ms() >= _mt7925_roc_until ||
      _mt7925_mcu_fault || _mt7925_tx_fault || !_proxy.is_open() || _proxy.failed()) return false;

  // The band-0 MIB histogram is read-clear. Connac3 packs two 16-bit bins
  // per register. Bin 0 counts 0 or 1 MPDU and named bins 1-14 count multiple MPDUs.
  constexpr uint32_t tx_aggregation = 0x820ed7dcu, rx_block_ack = 0x820ed55cu;
  constexpr size_t aggregate_bins = 15, histogram_words = (aggregate_bins + 1) / 2;
  std::vector<RegOp> reads;
  reads.reserve(histogram_words + 1);
  for (size_t i = 0; i < histogram_words; ++i)
    reads.push_back({mt7925_translate_addr(tx_aggregation + static_cast<uint32_t>(4 * i)), UINT32_MAX, REGPROXY_READ});
  reads.push_back({mt7925_translate_addr(rx_block_ack), UINT32_MAX, REGPROXY_READ});
  if (!_proxy.batch(reads) || plat::now_ms() >= _mt7925_roc_until || std::any_of(reads.begin(), reads.end(),
      [](const RegOp &op) { return op.value == UINT32_MAX; })) return false;

  Mt7925AwDlAggregation sample{};
  sample.zero_or_single = reads[0].value & 0xffffu;
  for (size_t bin = 1; bin < aggregate_bins; ++bin)
    sample.multi += (reads[bin / 2].value >> (16 * (bin % 2))) & 0xffffu;
  sample.rx_block_ack = reads.back().value;
  out = sample;
  return true;
}

void MtkDevice::mt7925_flush_awdl_tx() {
  if (!_mt7925_tx.unpublished()) return;
  const auto now = plat::now_ms();
  if (!_mt7925_concurrent && (!_mt7925_power_ready || _mt7925_mcu_fault || _mt7925_tx_fault ||
      _channel != _mt7925_awdl_channel || _mt7925_roc_bss != mt7925::awdl_bss ||
      !mt7925::AwDlTxSlots::can_submit(now, _mt7925_roc_until))) return;
  plat::mem_barrier();
  _mt7925_tx.publish(now);
  wr(_tx_data.reg_base + 8, _mt7925_tx.head());
}

void MtkDevice::mt7925_reclaim_awdl_tx(bool check_fetch) {
  if (_mt7925_tx_fault || (!check_fetch && !_mt7925_tx.front_completed())) return;
  if (_mt7925_tx.unfetched()) {
    const uint32_t fetched = rr(_tx_data.reg_base + 0xc);
    if (!_mt7925_tx.observe_fetch(fetched)) {
      _mt7925_tx_fault = true;
      log("MT7925 AWDL TX0 reported an invalid DMA consumer index=" + std::to_string(fetched));
      return;
    }
  }
  (void)_mt7925_tx.reclaim();
}

bool MtkDevice::mt7925_drain_awdl_tx(const RxHandler &on_rx) {
  mt7925_flush_awdl_tx();
  const auto deadline = plat::now_ms() + 500;
  while (_mt7925_tx.published_pending() && !_mt7925_tx_fault && !_mt7925_mcu_fault && !_proxy.failed() &&
         plat::now_ms() < deadline) {
    if (!mt7925_poll(on_rx) && _mt7925_tx.published_pending()) plat::sleep_ms(1);
  }
  if (!_mt7925_tx.pending() && !_mt7925_tx_fault && !_mt7925_mcu_fault && !_proxy.failed()) return true;
  log("MT7925 AWDL TX0 drain failed, pending=" + std::to_string(_mt7925_tx.pending()) +
      " published_pending=" + std::to_string(_mt7925_tx.published_pending()) +
      " unfetched=" + std::to_string(_mt7925_tx.unfetched()));
  return false;
}

bool MtkDevice::mt7925_configure_monitor(uint8_t channel, unsigned width_mhz) {
  const unsigned eff_w = mt7925::effective_width_5g(channel, width_mhz);
  const uint8_t center = mt7925::center_channel_5g(channel, eff_w);
  const uint8_t sco = mt7925::rlm_sco_5g(channel, center);
  const uint8_t enable[12]{0, 0, 0, 0, 0, 0, 8, 0, 1, 0, 0, 0};
  uint8_t config[20]{0, 0, 0, 0, 1, 0, 16, 0};
  config[10] = 2;  // 5 GHz
  config[11] = eff_w == 80 ? 1 : 0;  // sniffer bw: 1 = 80 MHz, 0 = 20/40 MHz
  config[12] = channel;
  config[13] = sco;
  config[14] = center;
  config[16] = 1;  // drop bad FCS

  // Keep the awake test radio in native monitor mode and clear the
  // separate firmware beacon filter even when no station is associated.
  uint8_t power[336]{0, 0, 0, 0, 2, 0, 0x4c, 1};
  constexpr char full_power[] = "KeepFullPwr 1";
  power[12] = sizeof(full_power);
  std::memcpy(power + 16, full_power, sizeof(full_power));
  uint8_t filter[72]{0, 0, 0, 0, 0x0c, 0, 68, 0, 1};
  mt7925::put32(filter + 16, 1u << 11);  // DROP_OTHER_BEACON
  filter[20] = 2;  // clear bits
  return mt7925_mcu_send_uni(0xe, power, sizeof(power), false) &&
         mt7925_mcu_send_uni(8, filter, sizeof(filter), true) &&
         mt7925_mcu_send_uni(0x24, enable, sizeof(enable), true) &&
         mt7925_mcu_send_uni(0x24, config, sizeof(config), true);
}

bool MtkDevice::mt7925_prepare_home(std::string &error) {
  const uint8_t radio[12]{0, 0, 0, 0, 0, 0, 8, 0, 1, 0, 0, 0};
  // The primary BSS has its own RLM channel even when unassociated.
  const uint8_t rlm[20]{0, 0, 1, 0, 2, 0, 16, 0, 36, 36, 0, 0, 1, 1, 4, 0, 2};
  uint8_t filter[72]{0, 0, 0, 0, 0x0c, 0, 68, 0};
  mt7925::put32(filter + 12, (1u << 31) | (1u << 6));  // Enable other-BSS RX.
  if (!_mt7925_power_ready ||
      !mt7925_mcu_send_uni(8, radio, sizeof(radio), true) ||
      !mt7925_set_idle_bss(0, _mac.data()) ||
      !mt7925_mcu_send_uni(2, rlm, sizeof(rlm), true) ||
      !mt7925_mcu_send_uni(8, filter, sizeof(filter), true)) {
    error = "MT7925 primary BSS setup failed";
    return false;
  }
  return true;
}

bool MtkDevice::mt7925_start_awdl(const uint8_t mac[6], bool block_ack, uint16_t lease_ms, std::string &error) {
  if (lease_ms < mt7925::roc_visit_ms || lease_ms > mt7925::roc_max_awdl_ms) {
    error = "MT7925 AWDL lease duration is outside the configured bounds";
    return false;
  }
  if (!mt7925_prepare_home(error)) return false;
  if (!mt7925_prepare_awdl_bss(_mt7925_awdl_channel, mac, block_ack) ||
      !mt7925_configure_monitor(_mt7925_awdl_channel, _width_mhz)) {
    error = "MT7925 channel " + std::to_string(_mt7925_awdl_channel) + " AWDL configuration failed";
    return false;
  }
  // Recheck after the firmware applied the BSS and monitor configuration.
  if (!mt7925_native_awdl_rx(error)) return false;
  _mt7925_awdl_lease_ms = lease_ms;
  return mt7925_roc_acquire(_mt7925_awdl_channel, mt7925::awdl_bss, 1, lease_ms, error, _width_mhz);
}

bool MtkDevice::mt7925_add_awdl_ba_peer(const uint8_t mac[6]) {
  if (!_mt7925_power_ready || _mt7925_roc_bss != mt7925::awdl_bss ||
      !mt7925_awdl_tx_available() || (mac[0] & 1u)) return false;
  if (_width_mhz == 80) {
    const auto message = mt7925::awdl_ba_station_vht80(mac, _mt7925_rx_streams);
    return mt7925_mcu_send_uni(3, message.data(), message.size(), true, 60);
  }
  const auto message = mt7925::awdl_ba_station(mac);
  return mt7925_mcu_send_uni(3, message.data(), message.size(), true, 60);
}

bool MtkDevice::mt7925_set_awdl_ba(uint8_t tid, uint16_t ssn, uint16_t window, bool enable,
                                  mt7925::AwDlBaDirection direction, bool amsdu) {
  if (!_mt7925_power_ready || _mt7925_roc_bss != mt7925::awdl_bss || !_mt7925_roc_until ||
      tid >= 8 || ssn >= 4096 || (enable && (!window || window > 64))) return false;
  const auto message = mt7925::awdl_ba_command(tid, ssn, window, enable, direction, amsdu);
  if (!mt7925_mcu_send_uni(3, message.data(), message.size(), true, 60)) return false;
  if (direction == mt7925::AwDlBaDirection::tx) _mt7925_tx_ba_active = enable;
  return true;
}

bool MtkDevice::mt7925_remove_awdl_ba_peer() {
  if (!_mt7925_power_ready || _mt7925_roc_bss != mt7925::awdl_bss || !_mt7925_roc_until) return false;
  bool ok = false;
  if (_width_mhz == 80) {
    const auto message = mt7925::awdl_remove_ba_station_mld();
    ok = mt7925_mcu_send_uni(3, message.data(), message.size(), true, 60);
  } else {
    const auto message = mt7925::awdl_remove_ba_station();
    ok = mt7925_mcu_send_uni(3, message.data(), message.size(), true, 60);
  }
  if (!ok) return false;
  _mt7925_tx_ba_active = false;
  return true;
}

bool MtkDevice::mt7925_maintain_awdl(std::string &error, const RxHandler &on_rx, bool peer_away) {
  if (!_mt7925_power_ready || _proxy.failed() || _mt7925_mcu_fault || _mt7925_tx_fault) {
    error = "MT7925 AWDL transport stopped";
    return false;
  }
  const auto now = plat::now_ms();
  constexpr unsigned reserve_ms = 35, early_renewal_ms = 250;
  const bool due = _mt7925_roc_until <= now + reserve_ms;
  // Never shorten the lease with unpublished work or a frame still owned by DMA.
  const bool early = !due && peer_away && _mt7925_roc_until - now < early_renewal_ms && !_mt7925_tx.pending();
  if (!due && !early) return true;
  mt7925_reclaim_awdl_tx(true);
  if (_mt7925_tx_fault || _proxy.failed()) {
    error = "MT7925 AWDL TX0 fetch failed during channel renewal";
    return false;
  }
  // A thread resuming after a long scheduling delay may already have every
  // TX_FREE waiting on RX0. Give both rings a turn before declaring a stall.
  if (_mt7925_tx.oldest_age(now) > 5000) mt7925_poll(on_rx);
  if (_mt7925_tx.oldest_age(now) > 5000) {
    error = "MT7925 AWDL TX0 stalled for over 5s, published_pending=" +
            std::to_string(_mt7925_tx.published_pending()) + " unfetched=" + std::to_string(_mt7925_tx.unfetched());
    return false;
  }
  // The normal loop keeps delivering RX2 and consuming TX_FREE on RX0 while
  // the old lease expires. The submission gate already closed at guard_ms.
  if (!_mt7925_tx.can_release_for_renewal(now, _mt7925_roc_until)) return true;
  const unsigned pending = _mt7925_tx.published_pending();
  const auto paused = std::chrono::steady_clock::now();
  if (!mt7925_roc_release()) {
    error = "MT7925 could not release the AWDL channel";
    return false;
  }
  if (!mt7925_roc_acquire(_mt7925_awdl_channel, mt7925::awdl_bss, 1, _mt7925_awdl_lease_ms, error, _width_mhz)) return false;
  ++_mt7925_awdl_renewals;
  if (pending && (++_mt7925_awdl_late_renewals % 32 == 1)) {
    log("MT7925 AWDL renewed with late TX_FREE: pending=" + std::to_string(pending) +
        " renewals_with_late_tx=" + std::to_string(_mt7925_awdl_late_renewals));
  }
  if (early) ++_mt7925_awdl_early_renewals;
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - paused).count();
  if (_verbose) log("MT7925 AWDL lease renewed: pause_us=" + std::to_string(elapsed));
  return true;
}

bool MtkDevice::mt7925_stop_awdl(bool remove_ba_peer) {
  if (!_mt7925_roc_until) return true;
  const bool drained = mt7925_drain_awdl_tx({});
  if (remove_ba_peer && (!drained || !mt7925_remove_awdl_ba_peer()))
    log("MT7925 AWDL peer removal deferred to firmware reset");
  const bool released = mt7925_roc_release();
  if (!released || !drained || !_mt7925_power_ready) return released && drained;

  std::string error;
  const bool granted = mt7925_configure_monitor(36) && mt7925_roc_acquire(36, 0, 0, 1000, error);
  if (!granted) {
    log("MT7925 shutdown could not re-acquire channel 36: " + error);
    return false;
  }
  (void)mt7925_poll({});  // Discard frames queued before the return grant.
  unsigned frames = 0;
  const auto end = std::min(_mt7925_roc_until - 10, plat::now_ms() + 300);
  while (!frames && plat::now_ms() < end) {
    if (!mt7925_poll([&](const RxFrame &rx) {
          if (rx.channel == 36 && rx.has_timestamp) ++frames;
        })) plat::sleep_ms(1);
  }
  log("MT7925 shutdown return: home_grant=1 timed_rx=" + std::to_string(frames));
  return mt7925_roc_release() && drained;
}

bool MtkDevice::mt7925_awdl_tx_test(std::string &error) {
  if (!mt7925_prepare_home(error)) return false;
  auto mac = _mac;
  mac[0] = (mac[0] | 2u) & ~1u;
  if (!mt7925_configure_monitor(36)) {
    error = "MT7925 monitor configuration for home channel failed";
    return false;
  }
  if (!mt7925_roc_acquire(36, 0, 0, 1000, error)) return false;
  const auto observe = [&](uint8_t channel, uint64_t deadline) {
    (void)mt7925_poll({});
    unsigned seen = 0, native = 0;
    const auto rx0 = rr(_rx_mcu.reg_base + 0xc);
    const auto rx2 = rr(_rx_data.reg_base + 0xc);
    constexpr uint32_t band0_rx_airtime = 0x820ed058u;
    const auto airtime = rr(band0_rx_airtime) & 0x00ffffffu;
    while (!seen && plat::now_ms() < deadline) {
      if (!mt7925_poll([&](const RxFrame &rx) {
            if (rx.channel == channel) {
              ++native;
              if (rx.has_timestamp) ++seen;
            }
          })) plat::sleep_ms(1);
    }
    log("MT7925 observed channel=" + std::to_string(channel) + " native_rx=" +
        std::to_string(native) + " timed_rx=" + std::to_string(seen) +
        " RX0_DIDX=" + std::to_string(rx0) + "->" + std::to_string(rr(_rx_mcu.reg_base + 0xc)) +
        " RX2_DIDX=" + std::to_string(rx2) + "->" + std::to_string(rr(_rx_data.reg_base + 0xc)) +
        " MAC_RX_AIRTIME=" + std::to_string(airtime) + "->" +
        std::to_string(rr(band0_rx_airtime) & 0x00ffffffu));
    return seen;
  };

  const unsigned home_before = observe(36, std::min(plat::now_ms() + 400, _mt7925_roc_until - 20));
  if (!mt7925_roc_release() || !mt7925_prepare_awdl_bss(44, mac.data(), true)) {
    error = "MT7925 could not release the home lease and set up AWDL BSS";
    return false;
  }
  log("MT7925 active home frames before visit=" + std::to_string(home_before));
  if (!mt7925_configure_monitor(44, _width_mhz)) {
    error = "MT7925 monitor configuration for AWDL channel failed";
    return false;
  }
  if (!mt7925_roc_acquire(44, mt7925::awdl_bss, 1, mt7925::roc_visit_ms, error, _width_mhz)) return false;
  std::array<uint8_t, 36> action{};
  action[0] = 0xd0;
  std::fill_n(action.begin() + 4, 6, 0xff);
  std::copy(mac.begin(), mac.end(), action.begin() + 10);
  std::copy(mt7925::awdl_bssid.begin(), mt7925::awdl_bssid.end(), action.begin() + 16);
  const uint8_t vendor[8]{0x7f, 0, 0x17, 0xf2, 8, 0, 0, 0};
  std::copy_n(vendor, sizeof(vendor), action.begin() + 24);
  constexpr unsigned smoke_batch = 8;
  unsigned queued = 0;
  for (; queued < smoke_batch; ++queued) {
    action[22] = static_cast<uint8_t>(queued << 4);
    if (!mt7925_queue_awdl_frame(action.data(), action.size())) break;
  }
  const bool drained = mt7925_drain_awdl_tx({});
  const bool sent = queued == smoke_batch && drained && _mt7925_tx_ok == smoke_batch;
  log("MT7925 AWDL TX0 batch=" + std::to_string(queued) + " completed=" + std::to_string(_mt7925_tx_ok) +
      " failed=" + std::to_string(_mt7925_tx_failed));

  // Target our own primary MAC. This checks DMA and firmware completion for
  // a large raw MPDU without requiring a second receiver to ACK it.
  std::array<uint8_t, 1460> first{};
  first[0] = 0x08;
  std::copy(_mac.begin(), _mac.end(), first.begin() + 4);
  std::copy(mac.begin(), mac.end(), first.begin() + 10);
  std::copy(mt7925::awdl_bssid.begin(), mt7925::awdl_bssid.end(), first.begin() + 16);
  constexpr uint8_t ip6[]{0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0, 3, 4, 0, 0, 0, 0, 0x86, 0xdd, 0x60};
  std::copy(std::begin(ip6), std::end(ip6), first.begin() + 24);
  first[46] = 59; // IPv6 No Next Header
  auto second = first;
  second[22] = 0x10;
  std::array<uint8_t, mt7925::awdl_amsdu_max> aggregate{};
  const size_t length = mt7925::build_awdl_amsdu(aggregate.data(), aggregate.size(), first.data(), first.size(),
                                               second.data(), second.size());
  const bool aggregate_queued = sent && length &&
      mt7925_queue_awdl_frame(aggregate.data(), length, mt7925::AwDlRate::ht20, mt7925::AwDlTxFormat::amsdu);
  const bool aggregate_completed = aggregate_queued && mt7925_drain_awdl_tx({}) &&
      _mt7925_amsdu_ok + _mt7925_amsdu_failed == 1;
  log("MT7925 AWDL HT20 aggregate bytes=" + std::to_string(length) +
      " complete=" + std::to_string(aggregate_completed) + " ack=" + std::to_string(_mt7925_amsdu_ok));
  auto test_peer = mac;
  test_peer.back() ^= 0x80;
  const bool station_added = mt7925_add_awdl_ba_peer(test_peer.data());
  std::array<uint8_t, awdl::kAddbaResponseLength> request{};
  awdl::build_addba_request(request.data(), test_peer.data(), mac.data(), mt7925::awdl_bssid.data(),
                            1, 0, 32, 0x100, 1);
  const auto completed_before = _mt7925_tx_ok + _mt7925_tx_failed;
  const bool addba_completed = station_added &&
      mt7925_queue_awdl_frame(request.data(), request.size(), mt7925::AwDlRate::ofdm6,
                              mt7925::AwDlTxFormat::block_ack_action) && mt7925_drain_awdl_tx({}) &&
      _mt7925_tx_ok + _mt7925_tx_failed == completed_before + 1;
  log("MT7925 AWDL ADDBA request descriptor complete=" + std::to_string(addba_completed));
  const bool ba_added = station_added && mt7925_set_awdl_ba(0, 0x100, 64, true, mt7925::AwDlBaDirection::rx);
  const bool ba_removed = ba_added && mt7925_set_awdl_ba(0, 0, 0, false, mt7925::AwDlBaDirection::rx);
  const bool tx_ba_added = station_added && mt7925_set_awdl_ba(0, 0x100, 32, true, mt7925::AwDlBaDirection::tx);
  std::array<uint8_t, 1462> qos_frame{};
  std::copy_n(first.begin(), 24, qos_frame.begin());
  std::copy(first.begin() + 24, first.end(), qos_frame.begin() + 26);
  std::copy(test_peer.begin(), test_peer.end(), qos_frame.begin() + 4);
  qos_frame[0] = 0x88;
  qos_frame[23] = 0x10;
  unsigned tx_ba_queued = 0;
  if (tx_ba_added) {
    for (; tx_ba_queued < 2; ++tx_ba_queued) {
      qos_frame[22] = static_cast<uint8_t>(tx_ba_queued << 4);
      if (!mt7925_queue_awdl_frame(qos_frame.data(), qos_frame.size(), mt7925::AwDlRate::ofdm6,
                                  mt7925::AwDlTxFormat::ampdu)) break;
    }
  }
  const bool tx_ba_drained = mt7925_drain_awdl_tx({});
  const bool tx_ba_removed = tx_ba_added && mt7925_set_awdl_ba(0, 0, 0, false, mt7925::AwDlBaDirection::tx);
  const bool station_removed = station_added && mt7925_remove_awdl_ba_peer();
  log("MT7925 AWDL RX Block Ack station_added=" + std::to_string(station_added) +
      " ba_added=" + std::to_string(ba_added) + " ba_removed=" + std::to_string(ba_removed) +
      " station_removed=" + std::to_string(station_removed));
  log("MT7925 AWDL TX Block Ack ba_added=" + std::to_string(tx_ba_added) +
      " queued=" + std::to_string(tx_ba_queued) + " drained=" + std::to_string(tx_ba_drained) +
      " completed=" + std::to_string(_mt7925_ampdu_ok + _mt7925_ampdu_failed) +
      " ack=" + std::to_string(_mt7925_ampdu_ok) + " ba_removed=" + std::to_string(tx_ba_removed));
  const unsigned away = observe(44, _mt7925_roc_until > 10 ? _mt7925_roc_until - 10 : 0);
  const bool aborted = mt7925_roc_release();
  const bool home_granted = aborted && mt7925_configure_monitor(36) &&
                            mt7925_roc_acquire(36, 0, 0, 1000, error);
  const unsigned returned = home_granted ? observe(36, _mt7925_roc_until - 10) : 0;
  const bool home_released = home_granted && mt7925_roc_release();
  unsigned passive_home_frames = 0;
  if (home_released && !returned) {
    const uint8_t monitor_off[12]{0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0};
    if (mt7925_mcu_send_uni(0x24, monitor_off, sizeof(monitor_off), true)) {
      const uint64_t probe_end = plat::now_ms() + 3000;
      while (!passive_home_frames && plat::now_ms() < probe_end) {
        if (!mt7925_passive_scan(36, [&](const RxFrame &rx) {
              if (rx.channel == 36 && rx.has_timestamp) ++passive_home_frames;
            })) break;
      }
    }
  }
  log("MT7925 active return: pre_home_rx=" + std::to_string(home_before) +
      " away_rx=" + std::to_string(away) + " abort_ack=" + std::to_string(aborted) +
      " home_grant=" + std::to_string(home_granted) + " post_home_rx=" + std::to_string(returned) +
      " passive_rx=" + std::to_string(passive_home_frames));
  if (!sent || !aggregate_completed || !addba_completed || !ba_removed || !tx_ba_removed || tx_ba_queued != 2 ||
      !tx_ba_drained || _mt7925_ampdu_ok + _mt7925_ampdu_failed != 2 ||
      !station_removed || !home_released || !returned) {
    if (error.empty()) error = "MT7925 active test failed to confirm TX completion and home-channel reception";
    return false;
  }
  return true;
}

/* --------------------------------------------------------------------------
 * MT7925 Concurrent Mode (Simultaneous 5 GHz Wi-Fi STA on BSS 1 + AWDL on BSS 0)
 * -------------------------------------------------------------------------- */
namespace {

uint8_t mt7925_map_rate_val(uint16_t rate_val) {
  const uint8_t mode = static_cast<uint8_t>((rate_val >> 6) & 0x7u);
  const uint8_t code = static_cast<uint8_t>(rate_val & 0x3fu);
  if (mode == 1u) {  // Legacy OFDM: match Connac2 rate codes to BASIC_RATES_TBL slots 18..25
    static constexpr uint8_t ofdm_codes[8] = {0x0b, 0x0f, 0x0a, 0x0e, 0x09, 0x0d, 0x08, 0x0c};
    for (uint8_t i = 0; i < 8; ++i) {
      if (code == ofdm_codes[i]) return static_cast<uint8_t>(18u + i);
    }
    return 18u;
  }
  if (mode == 2u) {  // HT MCS -> pre-installed HT20 table slots
    if (code >= 7u) return static_cast<uint8_t>(mt7925::AwDlRate::ht20);       // 26 (MCS 7)
    if (code >= 5u) return static_cast<uint8_t>(mt7925::AwDlRate::ht20_mcs5);  // 29 (MCS 5)
    if (code >= 4u) return 32u;                                                 // 32 (MCS 4)
    if (code >= 3u) return static_cast<uint8_t>(mt7925::AwDlRate::ht20_mcs3);  // 28 (MCS 3)
    return 31u;                                                                 // 31 (MCS 2)
  }
  return 18u;  // Default OFDM 6 Mbps
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
  // On Connac3, BSS 0 bmc = 19 and BSS 1 bmc = 18. Remap kApWcid (18) -> 17
  // so the Wi-Fi AP STA record never collides with BSS 1's broadcast WCID.
  return (wcid == MtkDevice::kApWcid) ? uint16_t{17} : uint16_t{wcid};
}

} // namespace

bool MtkDevice::mt7925_setup_bss(uint8_t bss, uint8_t channel, unsigned width_mhz,
                                 const uint8_t mac[6], const uint8_t bssid[6], bool qos_edca) {
  if (!_mt7925_power_ready || bss > 1 || channel < 36 || channel > 177) return false;
  const unsigned eff_w = mt7925::effective_width_5g(channel, width_mhz);
  const uint8_t center = mt7925::center_channel_5g(channel, eff_w);
  const uint8_t sco = mt7925::rlm_sco_5g(channel, center);
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
  basic[8] = basic[10] = 1;  // CONNAC3 CONNECTION_INFRA_STA
  basic[20] = basic[26] = bmc_wcid;
  std::memcpy(basic + 14, bssid, 6);
  basic[25] = 0x31;  // PHY_MODE_A | AN | AC
  basic[28] = 3;
  basic[31] = bss;   // wmm_idx = bss
  uint8_t *security = basic + 32;
  security[0] = 16; security[2] = 8; security[5] = 1;
  uint8_t *qos = security + 8;
  qos[0] = 15; qos[2] = 8;
  qos[4] = qos_edca ? 1 : 0;
  uint8_t *mld = qos + 8;
  mld[0] = 26; mld[2] = 20;  // UNI_BSS_INFO_MLD
  mld[4] = 0xff;
  mld[5] = static_cast<uint8_t>(bss + 32);
  std::memcpy(mld + 6, mac, 6);
  mld[12] = 0xff;
  mld[13] = 0xff;
  uint8_t *ifs = mld + 20;
  ifs[0] = 23; ifs[2] = 20; ifs[4] = 1; ifs[8] = 9;
  uint8_t *rlm = ifs + 20;
  rlm[0] = 2; rlm[2] = 16;
  rlm[4] = channel;
  rlm[5] = center;
  rlm[7] = eff_w == 80 ? 2 : (eff_w == 40 ? 1 : 0);
  rlm[8] = 1;
  rlm[9] = eff_w == 80 ? _mt7925_rx_streams : 1;
  rlm[10] = eff_w >= 40 ? 4 : 0;
  rlm[11] = sco;
  rlm[12] = 2;  // 5 GHz
  uint8_t *mbssid = rlm + 16;
  mbssid[0] = 6; mbssid[2] = 8;
  if (!mt7925_mcu_send_uni(2, bss_info, sizeof(bss_info), true)) return false;

  if (qos_edca) {
    uint8_t edca[4 + 4 * 12]{};
    edca[0] = bss;
    constexpr struct { uint8_t cw_min, cw_max, aifs; uint16_t txop; } ac_params[4] = {
        {2, 3, 2, 47},   // VO
        {3, 4, 2, 94},   // VI
        {4, 10, 3, 0},   // BE
        {4, 10, 7, 0},   // BK
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
  if (!_mt7925_power_ready || bss > 1 || channel < 36 || channel > 177) return false;
  const unsigned eff_w = mt7925::effective_width_5g(channel, width_mhz);
  const uint8_t center = mt7925::center_channel_5g(channel, eff_w);
  const uint8_t sco = mt7925::rlm_sco_5g(channel, center);
  uint8_t req[20]{};
  req[0] = bss;
  req[2] = 1;
  uint8_t *rlm = req + 4;
  rlm[0] = 2; rlm[2] = 16;
  rlm[4] = channel;
  rlm[5] = center;
  rlm[7] = eff_w == 80 ? 2 : (eff_w == 40 ? 1 : 0);
  rlm[8] = rlm[9] = 2;
  rlm[10] = eff_w >= 40 ? 4 : 0;
  rlm[11] = sco;
  rlm[12] = 2;  // 5 GHz
  return mt7925_mcu_send_uni(2, req, sizeof(req), true, move_timeout_ms);
}

bool MtkDevice::mt7925_start_monitor(uint8_t channel, unsigned width_mhz, const uint8_t address[6]) {
  std::string err;
  if (!_mt7925_power_ready && !mt7925_program_concurrent_power(_country, err)) {
    log("MT7925 start_monitor power setup failed: " + err);
    return false;
  }
  const uint8_t radio[12]{0, 0, 0, 0, 0, 0, 8, 0, 1, 0, 0, 0};
  uint8_t filter[72]{0, 0, 0, 0, 0x0c, 0, 68, 0};
  mt7925::put32(filter + 12, (1u << 31) | (1u << 6));  // Enable other-BSS RX
  uint8_t clr_filter[72]{0, 0, 0, 0, 0x0c, 0, 68, 0, 1};
  mt7925::put32(clr_filter + 16,
                MT_WF_RFCR_DROP_OTHER_BEACON | MT_WF_RFCR_DROP_OTHER_BSS |
                MT_WF_RFCR_DROP_OTHER_UC | MT_WF_RFCR_DROP_A2_BSSID |
                MT_WF_RFCR_DROP_A3_BSSID | MT_WF_RFCR_DROP_A3_MAC |
                MT_WF_RFCR_DROP_BCAST | MT_WF_RFCR_DROP_MCAST);
  clr_filter[20] = 2;
  uint8_t power[336]{0, 0, 0, 0, 2, 0, 0x4c, 1};
  constexpr char full_power[] = "KeepFullPwr 1";
  power[12] = sizeof(full_power);
  std::memcpy(power + 16, full_power, sizeof(full_power));
  const uint8_t sniffer_enable[12]{0, 0, 0, 0, 0, 0, 8, 0, 1, 0, 0, 0};
  if (!mt7925_mcu_send_uni(8, radio, sizeof(radio), true) ||
      !mt7925_mcu_send_uni(8, filter, sizeof(filter), true) ||
      !mt7925_mcu_send_uni(0xe, power, sizeof(power), false) ||
      !mt7925_mcu_send_uni(8, clr_filter, sizeof(clr_filter), true) ||
      !mt7925_mcu_send_uni(0x24, sniffer_enable, sizeof(sniffer_enable), true)) {
    log("MT7925 start_monitor radio/filter setup failed");
    return false;
  }

  _mt7925_concurrent = true;
  _mt7925_awdl_channel = channel;
  std::memcpy(_mt7925_awdl_mac.data(), address, 6);
  if (!mt7925_setup_bss(0, channel, width_mhz, address, mt7925::awdl_bssid.data(), true)) {
    log("MT7925 start_monitor BSS 0 setup failed");
    return false;
  }
  return mt7925_set_channel(channel, width_mhz, CH_SWITCH_NORMAL);
}

bool MtkDevice::mt7925_set_address(const uint8_t address[6]) {
  std::memcpy(_mt7925_awdl_mac.data(), address, 6);
  if (!_mt7925_power_ready) return true;
  const uint8_t ch = _channel >= 36 ? _channel : _mt7925_awdl_channel;
  return mt7925_setup_bss(0, ch, _width_mhz, address, mt7925::awdl_bssid.data(), true);
}

bool MtkDevice::mt7925_set_station_identity(const uint8_t addr[6], const uint8_t bssid[6]) {
  if (!_mt7925_power_ready) return false;
  std::memcpy(_sta_addr.data(), addr, 6);
  std::memcpy(_sta_bssid.data(), bssid, 6);
  const uint8_t ch = _channel >= 36 ? _channel : _mt7925_awdl_channel;
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
  if (channel < 36 || channel > 177) return false;

  const unsigned eff_w = mt7925::effective_width_5g(channel, width_mhz);
  const uint8_t want_bss = (_sta_iface_up && channel != _mt7925_awdl_channel)
      ? kStationOmac : uint8_t{0};

  // Bounded drain before leaving the current channel so switches never stall.
  if (_mt7925_roc_until) {
    mt7925_flush_awdl_tx();
    const uint64_t drain_end = std::min(_mt7925_roc_until > 2 ? _mt7925_roc_until - 2 : 0,
                                        plat::now_ms() + 20);
    while (_mt7925_tx.pending() && !_mt7925_tx_fault && plat::now_ms() < drain_end) {
      if (!mt7925_poll({})) plat::sleep_ms(1);
    }
    (void)mt7925_roc_release();
  }

  if (!mt7925_update_bss_rlm(0, channel, eff_w)) return false;
  if (_sta_iface_up && !mt7925_update_bss_rlm(kStationOmac, channel, eff_w)) return false;

  const uint8_t center = mt7925::center_channel_5g(channel, eff_w);
  const uint8_t sco = mt7925::rlm_sco_5g(channel, center);
  uint8_t sniffer_cfg[20]{0, 0, 0, 0, 1, 0, 16, 0};
  sniffer_cfg[10] = 2;  // 5 GHz
  sniffer_cfg[11] = eff_w == 80 ? 1 : 0;
  sniffer_cfg[12] = channel;
  sniffer_cfg[13] = sco;
  sniffer_cfg[14] = center;
  sniffer_cfg[16] = 1;  // drop bad FCS
  if (!mt7925_mcu_send_uni(0x24, sniffer_cfg, sizeof(sniffer_cfg), true, move_timeout_ms))
    return false;

  std::string err;
  if (!mt7925_roc_acquire(channel, want_bss, 1, mt7925::roc_max_awdl_ms, err, eff_w)) {
    if (_verbose) log("MT7925 set_channel(" + std::to_string(channel) + ") failed: " + err);
    return false;
  }
  _channel = channel;
  _width_mhz = eff_w;
  return true;
}

bool MtkDevice::mt7925_renew_concurrent_roc(const RxHandler &on_rx) {
  if (!_mt7925_power_ready || !_mt7925_roc_until || _mt7925_mcu_fault || _mt7925_tx_fault)
    return false;
  const uint8_t ch = _channel;
  const unsigned w = _width_mhz;
  const uint8_t bss = _mt7925_roc_bss;
  mt7925_flush_awdl_tx();
  const uint64_t drain_end = std::min(_mt7925_roc_until > 2 ? _mt7925_roc_until - 2 : 0,
                                      plat::now_ms() + 25);
  while (_mt7925_tx.pending() && !_mt7925_tx_fault && plat::now_ms() < drain_end) {
    // Reclaim TX_FREE without re-entering renew.
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
  const auto msg = mt7925::sta_rec_add_vht(omac, hw_wcid, peer, caps.ht_cap, caps.ampdu_params,
                                           caps.rx_mcs, caps.vht, caps.vht_cap, caps.vht_mcs,
                                           caps.bw, caps.nss);
  const bool ok = mt7925_mcu_send_uni(3, msg.data(), mt7925::sta_rec_add_size(caps.vht),
                                      true, sta_cmd_timeout_ms);
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
  if (!_mt7925_power_ready || wcid == 0 || wcid >= MT792X_WTBL_RESERVED || tid >= 8 || omac > 1)
    return false;
  const uint16_t hw_wcid = mt7925_hw_wcid(wcid);
  const uint16_t clamped_win = enable ? (win == 0 || win > 64 ? 64 : win) : 0;
  const auto dir = tx ? mt7925::AwDlBaDirection::tx : mt7925::AwDlBaDirection::rx;
  const auto msg = mt7925::sta_rec_ba(omac, hw_wcid, tid, ssn & 0x0fffu, clamped_win, enable, dir);
  return mt7925_mcu_send_uni(3, msg.data(), msg.size(), true, sta_cmd_timeout_ms);
}

int MtkDevice::mt7925_queue_general_frame(const uint8_t *frame, size_t len, uint8_t bss,
                                          uint16_t wcid, bool fixed_rate, uint8_t rate_slot,
                                          bool no_ack, bool force_altx, bool allow_ba, bool kick) {
  if (!_mt7925_power_ready || !_mt7925_roc_until || _mt7925_tx_fault || _mt7925_mcu_fault)
    return -1;
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
  _mt7925_tracked_tx[index] = false;
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
  // Slots 1..16 belong to AWDL peers on BSS 0 (17 = AP, 18 = BSS1 BMC, 19 = BSS0 BMC).
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
    if (!_peers[i].in_use) { slot = i; break; }
    if (_peers[i].last_used_ms < oldest) { oldest = _peers[i].last_used_ms; slot = i; }
  }
  if (!slot) return -1;
  if (_peers[slot].in_use) {
    (void)mt7925_sta_remove(slot, _peers[slot].mac.data(), 0);
  }
  PeerCaps caps{};
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
  if (action == 0u && len >= 33) {  // ADDBA Request from peer -> record for RX BA
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
  } else if (action == 1u && len >= 33) {  // ADDBA Response from peer -> enable TX BA
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
        if (wcid > 0 && mt7925_sta_ba(static_cast<uint8_t>(wcid), peer, tid, p.ssn, win, true, true, 0)) {
          _peers[wcid].tx_ba_active[tid] = true;
        }
        break;
      }
    }
  } else if (action == 2u) {  // DELBA from peer
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
  if (action == 1u && len >= 33) {  // ADDBA Response sent by us -> enable RX BA
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
  } else if (action == 0u && len >= 33) {  // ADDBA Request sent by us -> record for TX BA
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

  uint16_t wcid = 19u;  // BSS 0 broadcast WCID
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
  return mt7925_queue_general_frame(frame, len, 0, wcid, fixed_rate, rate_slot, no_ack,
                                    force_altx, allow_ba, kick) >= 0;
}

} // namespace mtk
