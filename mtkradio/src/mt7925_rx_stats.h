// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "mt7925_rx.h"

#include <string>

namespace mtk::mt7925 {

// One second of admitted unicast MPDUs, measured before BA reordering.
class RxPhyWindow {
public:
  bool note(uint64_t now, size_t length, uint8_t mode, const RxPhy &rate, int dbm) {
    if (packets_ && now - last_ > interval_us) clear();
    if (!packets_) start_ = now;
    last_ = now;
    ++packets_;
    bytes_ += length;
    if (mode == 0xff) ++missing_;
    if (mode == 4) ++vht_;
    if (mode == 2 || mode == 3) ++ht_;
    if (rate.measured_rssi) {
      signal_sum_ += dbm;
      ++signals_;
    }
    if (rate.width_mhz) {
      ++valid_;
      const unsigned width = rate.width_mhz == 20 ? 0 : rate.width_mhz == 40 ? 1 :
                             rate.width_mhz == 80 ? 2 : 3;
      auto &count = bins_[mode == 4][width][rate.nss - 1][rate.mcs][rate.short_gi];
      if (++count > top_count_) {
        top_count_ = count;
        top_mode_ = mode;
        top_ = rate;
      }
    }
    if (now - start_ < interval_us) return false;
    // Exclude isolated keepalives from the transfer log.
    if (bytes_ >= 4096) return true;
    clear();
    return false;
  }

  std::string finish(const uint8_t *latest_peer_channels, size_t entries) {
    const auto top = top_count_ ?
        std::string(top_mode_ == 4 ? "VHT" : "HT") + std::to_string(top_.width_mhz) +
        "/MCS" + std::to_string(top_.mcs) + "/" + std::to_string(top_.nss) +
        (top_.short_gi ? "SS/SGI" : "SS/LGI") : "unknown";
    auto result = "MT7925 RX rates: elapsed_ms=" + std::to_string((last_ - start_) / 1000) +
        " unicast_mpdu_bytes=" + std::to_string(bytes_) + " packets=" + std::to_string(packets_) +
        " vht=" + std::to_string(vht_) + " ht=" + std::to_string(ht_) +
        " no_group3=" + std::to_string(missing_) + " valid_rates=" + std::to_string(valid_) +
        " dominant=" + top + " dominant_packets=" + std::to_string(top_count_) +
        " dominant_pct=" + std::to_string(100ull * top_count_ / packets_) +
        " rssi_avg_dbm=" + (signals_ ? std::to_string(signal_sum_ / int64_t(signals_)) : "unknown") +
        " rssi_samples=" + std::to_string(signals_);
    if (top_count_) {
      // Three PHY metadata words of one matching sample allow offline replay.
      result += " dominant_prxv0_2_3=" + std::to_string(top_.prxv0) + "," +
                std::to_string(top_.prxv2) + "," + std::to_string(top_.prxv3);
    }
    if (latest_peer_channels) {
      result += " peer_slots_44=" + std::to_string(std::count(latest_peer_channels, latest_peer_channels + entries, 44)) +
                " peer_slots_149=" + std::to_string(std::count(latest_peer_channels, latest_peer_channels + entries, 149)) +
                " peer_slots_total=" + std::to_string(entries);
    } else {
      result += " peer_slots=unknown";
    }
    clear();
    return result;
  }

private:
  void clear() {
    // The next packet starts a fresh interval. Clear bins once per interval.
    std::memset(bins_, 0, sizeof(bins_));
    packets_ = missing_ = valid_ = vht_ = ht_ = signals_ = top_count_ = 0;
    bytes_ = 0;
    signal_sum_ = 0;
  }
  static constexpr uint64_t interval_us = 1000000;
  uint64_t start_ = 0, last_ = 0, bytes_ = 0;
  int64_t signal_sum_ = 0;
  unsigned packets_ = 0, missing_ = 0, valid_ = 0, vht_ = 0, ht_ = 0, signals_ = 0;
  uint32_t bins_[2][4][2][16][2]{};
  unsigned top_count_ = 0;
  uint8_t top_mode_ = 0;
  RxPhy top_{};
};

} // namespace mtk::mt7925
