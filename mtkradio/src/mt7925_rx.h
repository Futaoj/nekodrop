// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>

namespace mtk::mt7925 {

constexpr std::array<uint8_t, 6> awdl_bssid{0x00, 0x25, 0x00, 0xff, 0x94, 0x73};

class RxDmaSnapshot {
public:
  template<typename ReadIndex>
  bool ready(uint32_t tail, uint32_t count, ReadIndex read_index) {
    if (tail >= count) return false;
    if (index_ >= count || tail == index_) {
      index_ = read_index();
      ++reads_;
      if (index_ < count) {
        const uint32_t queued = (count + index_ - tail) % count;
        if (queued > peak_) peak_ = queued;
        if (queued == count - 1) ++full_samples_;
      }
    }
    if (index_ >= count || tail == index_) return false;
    ++consumed_;
    return true;
  }

  uint64_t register_reads() const { return reads_; }
  uint64_t consumed() const { return consumed_; }
  uint32_t peak() const { return peak_; }
  uint64_t full_samples() const { return full_samples_; }

private:
  // The producer can only advance through descriptors the CPU returned.
  // The consumer still checks DDONE for each slot before using this snapshot.
  uint32_t index_ = UINT32_MAX;
  uint32_t peak_ = 0;
  uint64_t full_samples_ = 0;
  uint64_t reads_ = 0, consumed_ = 0;
};

enum class RxKind { other, frame, event, tx_free };

inline uint32_t rx_word(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline RxKind rx_kind(const uint8_t *p, size_t size) {
  if (size < 4) return RxKind::other;
  const uint32_t type = p[3] >> 3;
  // Real RX0 management records use type 7 with format 1 (e.g. 0x38010109).
  if (type == 2 || type == 8 || (type == 7 && (p[2] & 0xf) == 1)) return RxKind::frame;
  if (type == 7) return RxKind::event;
  if (type == 6) return RxKind::tx_free;
  return RxKind::other;
}

struct RxPhy {
  uint8_t width_mhz = 0;  // Zero when the Group 3 rate is absent or unsupported.
  uint8_t mcs = 0, nss = 0;
  bool short_gi = false, measured_rssi = false;
  bool ampdu_member = false;
  uint32_t prxv0 = 0, prxv2 = 0, prxv3 = 0;
};

// mt76 mt7925_mac_fill_rx_rate: P-RXV word 0 holds TX_RATE and NSTS,
// word 2 holds TX_MODE, FRAME_MODE and GI; word 3 holds per-chain RCPI.
inline void read_group3(const uint8_t *p, uint8_t &mode, int &signal, RxPhy &rate) {
  const uint32_t v0 = rx_word(p), v2 = rx_word(p + 8);
  rate.prxv0 = v0;
  rate.prxv2 = v2;
  rate.prxv3 = rx_word(p + 12);
  mode = static_cast<uint8_t>((v2 >> 11) & 15u);
  const unsigned bw = v2 & 7u, gi = (v2 >> 3) & 3u;
  const unsigned mcs = v0 & 127u;
  const bool ht = mode == 2 || mode == 3;
  const unsigned nss = ht ? 1 + mcs / 8 : 1 + ((v0 >> 7) & 15u);
  // HT MCS32 and vendor/reserved GI patterns are not represented by these bins.
  if (gi <= 1 && nss <= 2 && ((ht && bw <= 1 && mcs < 16) ||
                               (mode == 4 && bw <= 3 && mcs <= 11))) {
    rate.width_mhz = static_cast<uint8_t>(20u << bw);
    rate.mcs = static_cast<uint8_t>(mcs);
    rate.nss = static_cast<uint8_t>(nss);
    rate.short_gi = gi != 0;
  }
  const int a = p[12], b = p[13];
  const int rcpi = std::max(a <= 220 ? a : 0, b <= 220 ? b : 0);
  if (rcpi > 0) {
    signal = rcpi / 2 - 110;
    rate.measured_rssi = true;
  }
}

struct MonitorFrame {
  size_t offset, length;
  int8_t dbm;
  uint8_t channel;
  bool has_timestamp;
  uint32_t timestamp_us;
  uint8_t phy_mode;
  RxPhy phy{};
};

inline uint8_t center_channel_5g(uint8_t ch, unsigned width_mhz) {
  if (width_mhz == 80) {
    if (ch >= 36 && ch <= 48) return 42;
    if (ch >= 52 && ch <= 64) return 58;
    if (ch >= 100 && ch <= 112) return 106;
    if (ch >= 116 && ch <= 128) return 122;
    if (ch >= 132 && ch <= 144) return 138;
    if (ch >= 149 && ch <= 161) return 155;
    return ch;
  }
  if (width_mhz == 40) {
    if (ch == 36 || ch == 40) return 38;
    if (ch == 44 || ch == 48) return 46;
    if (ch == 52 || ch == 56) return 54;
    if (ch == 60 || ch == 64) return 62;
    if (ch == 100 || ch == 104) return 102;
    if (ch == 108 || ch == 112) return 110;
    if (ch == 116 || ch == 120) return 118;
    if (ch == 124 || ch == 128) return 126;
    if (ch == 132 || ch == 136) return 134;
    if (ch == 140 || ch == 144) return 142;
    if (ch == 149 || ch == 153) return 151;
    if (ch == 157 || ch == 161) return 159;
    return ch;
  }
  return ch;
}

inline unsigned effective_width_5g(uint8_t ch, unsigned width_mhz) {
  if (width_mhz >= 80 && center_channel_5g(ch, 80) != ch) return 80;
  if (width_mhz >= 40 && center_channel_5g(ch, 40) != ch) return 40;
  return 20;
}

inline uint8_t rlm_sco_5g(uint8_t ch, uint8_t center) {
  if (ch < center) return 1;  // SCA
  if (ch > center) return 3;  // SCB
  return 0;
}

inline uint8_t roc_sco_5g(uint8_t ch, unsigned width_mhz) {
  const unsigned eff = effective_width_5g(ch, width_mhz);
  if (eff < 40) return 0;
  const uint8_t pri40 = center_channel_5g(ch, 40);
  return ch < pri40 ? 1 : 3;
}

inline bool is_valid_channel(uint8_t ch) {
  return (ch >= 1 && ch <= 14) || (ch >= 36 && ch <= 177);
}

inline uint8_t band_for_channel(uint8_t ch) {
  return ch <= 14 ? 1u : 2u;
}

inline unsigned effective_width(uint8_t ch, unsigned width_mhz) {
  return ch <= 14 ? 20u : effective_width_5g(ch, width_mhz);
}

inline uint8_t center_channel(uint8_t ch, unsigned width_mhz) {
  return ch <= 14 ? ch : center_channel_5g(ch, width_mhz);
}

inline uint8_t rlm_sco(uint8_t ch, uint8_t center) {
  return ch <= 14 ? 0u : rlm_sco_5g(ch, center);
}

inline uint8_t roc_sco(uint8_t ch, unsigned width_mhz) {
  return ch <= 14 ? 0u : roc_sco_5g(ch, width_mhz);
}

inline bool parse_monitor_frame(const uint8_t *p, size_t size, MonitorFrame &out, uint8_t wide_primary = 0) {
  if (size < 32 || rx_kind(p, size) != RxKind::frame) return false;
  const size_t total = rx_word(p) & 0xffff;
  const uint32_t d1 = rx_word(p + 4);
  const uint32_t d2 = rx_word(p + 8);
  const uint32_t d3 = rx_word(p + 12);
  if (total < 32 || total > size ||
      (d1 & 0x1f400000u) ||                 // Non-primary PHY, ICV/MIC and cipher length errors
      (d2 & 0x199f0080u) ||                 // Invalid MPDU or hardware header translation
      (d3 & 0x01000000u) || (rx_word(p + 16) & 3u) ||
      ((d1 & (1u << 20)) && !(d1 & (1u << 18)))) return false;

  size_t offset = 32;
  int signal = -70;
  uint8_t phy_mode = 0xff;
  RxPhy phy{};
  phy.ampdu_member = !(d2 & (1u << 30)); // Connac3 RXD2 NORMAL_NON_AMPDU
  uint32_t timestamp_us = 0;
  constexpr unsigned groups[] = {19, 16, 17, 18, 20};
  for (unsigned group : groups) {
    if (!(d1 & (1u << group))) continue;
    const size_t length = group == 20 ? 96 : 16;
    if (length > total - offset) return false;
    if (group == 17) timestamp_us = rx_word(p + offset);
    if (group == 18) read_group3(p + offset, phy_mode, signal, phy);
    offset += length;
  }
  offset += 2u * ((d2 >> 13) & 7u);
  if (offset > total || total - offset < 20 || (p[offset] & 3u)) return false;
  const bool bar = p[offset] == 0x84;
  if (!bar && total - offset < 24) return false;
  // Without a matching WCID, CM can mark clear native AWDL data as well as management.
  const bool awdl_data = (p[offset] == 0x08 || p[offset] == 0x88) &&
      !(p[offset + 1] & 3u) && !std::memcmp(p + offset + 16, awdl_bssid.data(), awdl_bssid.size());
  if ((d1 & (1u << 23)) &&
      (((d1 >> 21) & 3u) || ((d2 >> 16) & 0x1fu) || (p[offset + 1] & 0x40u) ||
       (p[offset] != 0x80 && p[offset] != 0xd0 && !awdl_data && !bar))) return false;

  uint8_t channel = (d3 >> 8) & 0xff;
  if (wide_primary) {
    if (wide_primary != 44 && wide_primary != 149) return false;
    const int center = center_channel_5g(wide_primary, 80);
    const int relative = int(channel) - center;
    // RXD may report a 20 MHz constituent, our primary 40 MHz center,
    // or the 80 MHz center instead of the tuned primary.
    if (channel != center && channel != center_channel_5g(wide_primary, 40) &&
        relative != -6 && relative != -2 && relative != 2 && relative != 6) return false;
    channel = wide_primary;
  } else if (channel != 36 && channel != 44) return false;
  out = {offset, total - offset, static_cast<int8_t>(signal), channel,
         (d1 & (1u << 17)) != 0, timestamp_us, phy_mode, phy};
  return true;
}

inline bool parse_concurrent_frame(const uint8_t *p, size_t size, uint8_t tuned_channel,
                                   unsigned tuned_width_mhz, MonitorFrame &out) {
  if (size < 32 || rx_kind(p, size) != RxKind::frame) return false;
  const size_t total = rx_word(p) & 0xffff;
  const uint32_t d1 = rx_word(p + 4);
  const uint32_t d2 = rx_word(p + 8);
  const uint32_t d3 = rx_word(p + 12);
  if (total < 32 || total > size ||
      (d1 & 0x18000000u) ||                 // Non-primary band (BAND_IDX != 0)
      (d2 & 0x01800080u) ||                 // HDR_TRANS | AMSDU_ERR | MAX_LEN_ERROR
      (d3 & 0x01000000u) ||                 // FCS_ERR
      (rx_word(p + 16) & 3u) ||             // Multi-part A-MSDU fragment
      ((d1 & (1u << 20)) && !(d1 & (1u << 18)))) return false;

  size_t offset = 32;
  int signal = -70;
  uint8_t phy_mode = 0xff;
  RxPhy phy{};
  uint32_t timestamp_us = 0;
  constexpr unsigned groups[] = {19, 16, 17, 18, 20};
  for (unsigned group : groups) {
    if (!(d1 & (1u << group))) continue;
    const size_t length = group == 20 ? 96 : 16;
    if (length > total - offset) return false;
    if (group == 17) timestamp_us = rx_word(p + offset);
    if (group == 18) read_group3(p + offset, phy_mode, signal, phy);
    offset += length;
  }
  offset += 2u * ((d2 >> 13) & 7u);
  if (offset > total || total - offset < 16) return false;
  const uint8_t ftype = (p[offset] >> 2) & 3u;
  if (ftype != 1 && total - offset < 24) return false;

  uint8_t channel = (d3 >> 8) & 0xff;
  if (channel == 0) {
    channel = tuned_channel;
  } else if (tuned_channel >= 36 && tuned_width_mhz >= 40) {
    const uint8_t c40 = center_channel_5g(tuned_channel, 40);
    const uint8_t c80 = center_channel_5g(tuned_channel, 80);
    if (channel == c40 || channel == c80 ||
        (tuned_width_mhz >= 80 && c80 != tuned_channel &&
         channel >= c80 - 6 && channel <= c80 + 6) ||
        (tuned_width_mhz == 40 && c40 != tuned_channel &&
         channel >= c40 - 2 && channel <= c40 + 2)) {
      channel = tuned_channel;
    }
  }
  if (!is_valid_channel(channel)) return false;
  out = {offset, total - offset, static_cast<int8_t>(signal), channel,
         (d1 & (1u << 17)) != 0, timestamp_us, phy_mode, phy};
  return true;
}

} // namespace mtk::mt7925
