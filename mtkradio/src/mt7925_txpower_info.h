// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "firmware.h"
}

namespace mtk::mt7925 {

// mt76 4d7f2bed: mt7925_get_txpwr_info sends MCU_UNI_CMD(TXPOWER)
// with TX_POWER_SHOW_INFO and TXPOWER_ALL_RATE_POWER_INFO on PHY index 0.
inline constexpr uint32_t txpower_command = MT_MCU_UNI | 0x2bu;
inline constexpr std::array<uint8_t, 12> txpower_read_request{0, 0, 0, 0, 7, 0, 8, 0, 0, 2, 0, 0};

// struct mt7925_txpwr: CCK, OFDM, HT20/40, four VHT and seven HE
// tables (12 each), then sixteen EHT tables (16 each). Two PHY columns.
inline constexpr unsigned txpower_rate_count = 4 + 8 + 8 + 9 + 11 * 12 + 16 * 16;
struct TxPowerReadback {
  unsigned phy = 0, channel_band = 0, format = 0, tag = 0;
  unsigned available = 0;
  int lowest = 0, highest = 0;
};

inline int decode_txpower_info(uint8_t sequence, const uint8_t *p, std::size_t size,
                               TxPowerReadback &out) {
  if (!p || size < 40 || !sequence || sequence > 15) return MT_INVALID;
  if (p[37] != sequence || (p[38] & 4) || p[34] != 0 || p[35] != 0x38) return MT_PENDING;
  const auto word = [](const uint8_t *v) { return unsigned(v[0]) | (unsigned(v[1]) << 8); };
  const unsigned declared = word(p + 32);
  if (declared < 8 || declared > size - 32) return MT_INVALID;
  if (p[36] == 1) {  // Command ACK; only its data event supplies a table.
    if (declared < 20) return MT_INVALID;
    if (p[44] != (txpower_command & 0xff)) return MT_PENDING;
    return (p[48] | p[49] | p[50] | p[51]) ? MT_FW_PROTOCOL : MT_PENDING;
  }
  // MediaTek gen4m UNI_EVENT_ID_TXPOWER, TXPOWER_EVENT_SHOW_ALL_RATE_TXPOWER_INFO.
  if (p[36] != 0x2a) return MT_PENDING;
  if (declared < 24) return MT_INVALID;
  // The pinned E138 reply declares 861 bytes from RXD.len onward. Its
  // only TLV declares 841 bytes *after* its 4-byte tag/length header.
  // 12 bytes of MCU event header + 4 fixed + 4 TLV header + 841 body.
  const unsigned body_length = word(p + 50);
  if (body_length < 4 || body_length != declared - 20) return MT_INVALID;
  if (p[52] != 5 || p[53] != txpower_read_request[10]) return MT_PENDING;
  // Category/band/format, complete two-column rates, max/min/reserved.
  if (body_length < 4 + 2 * txpower_rate_count + 3) return MT_INVALID;
  TxPowerReadback info{p[53], p[54], p[55], word(p + 48)};
  for (unsigned i = 0; i < txpower_rate_count; ++i) {
    const int byte = p[56 + 2 * i + info.phy];
    if (byte == 127) continue; // mt7925/debugfs.c prints this as N.A.
    const int value = byte < 128 ? byte : byte - 256;
    if (info.available == 0) {
      info.lowest = info.highest = value;
    } else {
      info.lowest = std::min(info.lowest, value);
      info.highest = std::max(info.highest, value);
    }
    ++info.available;
  }
  out = info;
  return MT_OK;
}

} // namespace mtk::mt7925
