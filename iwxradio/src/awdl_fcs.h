// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>

namespace awdl {

/* IEEE 802.3 CRC-32, as the 802.11 FCS uses. */
inline uint32_t crc32_ieee(const uint8_t *p, size_t n) {
  struct Table {
    uint32_t t[8][256]{};
    constexpr Table() {
      for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[0][i] = c;
      }
      for (unsigned slice = 1; slice < 8; ++slice) {
        for (unsigned i = 0; i < 256; ++i) {
          const uint32_t c = t[slice - 1][i];
          t[slice][i] = t[0][c & 0xffu] ^ (c >> 8);
        }
      }
    }
  };
  static constexpr Table table;
  uint32_t c = 0xFFFFFFFFu;
  // Slicing-by-8 folds eight input bytes into the reflected IEEE CRC at once.
  // Explicit little-endian assembly also works on unaligned monitor frames.
  while (n >= 8) {
    c ^= uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    c = table.t[7][c & 0xffu] ^ table.t[6][(c >> 8) & 0xffu] ^
        table.t[5][(c >> 16) & 0xffu] ^ table.t[4][c >> 24] ^
        table.t[3][p[4]] ^ table.t[2][p[5]] ^ table.t[1][p[6]] ^ table.t[0][p[7]];
    p += 8;
    n -= 8;
  }
  for (size_t i = 0; i < n; i++) c = table.t[0][(c ^ p[i]) & 0xffu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

/* The length without the FCS when the last four bytes are this frame's valid
 * FCS, else `len`. The monitor context asks for IWX_MAC_FILTER_IN_CRC32
 * ("append the FCS"), and whether the firmware does so differs between
 * cards: OWL rejects an AWDL action frame with any bytes after its last
 * TLV, before it marks a MIF, so a peer whose frames keep their FCS never
 * turns valid ("not valid yet", AX211/AX411, #24/#25). A frame without an
 * FCS matches by chance with odds of 2^-32. */
inline size_t strip_valid_fcs(const uint8_t *f, size_t len) {
  if (len < 14) return len;
  const uint8_t *t = f + len - 4;
  const uint32_t fcs = uint32_t(t[0]) | uint32_t(t[1]) << 8 | uint32_t(t[2]) << 16 | uint32_t(t[3]) << 24;
  return crc32_ieee(f, len - 4) == fcs ? len - 4 : len;
}

} // namespace awdl
