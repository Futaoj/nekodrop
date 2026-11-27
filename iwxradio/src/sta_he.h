// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental Wi-Fi 6 (802.11ax HE) for the station beside AirDrop.
 *
 * What the station says about itself (the HE Capabilities element in the
 * association request) and what it learns from the access point's beacon
 * (HE Capabilities, HE Operation, MU EDCA Parameter Set), turned into the
 * values the Intel firmware wants: the HE station context (STA_HE_CTXT_CMD on
 * pre-Bz firmware, link + station config fields on Bz/MLD firmware), packet
 * extension thresholds and the HE rate masks for TLC.
 *
 * The profile is ForbiddenItlwm's conservative one (iwx_setup_he_rates,
 * iwx_build_he_sta, iwx_rs_fw_he_set_enabled_rates): HE SU with LDPC and up
 * to MCS 11 on at most two streams and 80 MHz; no TWT, OMI, multi-TID,
 * fragmentation or 160 MHz. Header-only so the self-test can check it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sta_he {

/* Element IDs (element 255 + extension ID). */
constexpr uint8_t kExtHeCap = 35;
constexpr uint8_t kExtHeOp = 36;
constexpr uint8_t kExtMuEdca = 38;

/* HE MAC / PHY capability bits used here (IEEE 802.11ax-2021, 9.4.2.248). */
constexpr uint8_t kMac0HtcHe = 0x01;
constexpr uint8_t kMac1TfPad16us = 0x08;
constexpr uint8_t kMac2Bsr = 0x08;
constexpr uint8_t kMac2Ba32 = 0x20;
constexpr uint8_t kMac2AckEn = 0x80;
constexpr uint8_t kMac3Omi = 0x02;
constexpr uint8_t kMac4Bqr = 0x04;
constexpr uint8_t kMac4AmsduInAmpdu = 0x40;
constexpr uint8_t kPhy0Width40In24 = 0x02;
constexpr uint8_t kPhy0Width4080In5 = 0x04;
constexpr uint8_t kPhy0Width160In5 = 0x08;
constexpr uint8_t kPhy0Width8080In5 = 0x10;
constexpr uint8_t kPhy1ClassA = 0x10;
constexpr uint8_t kPhy1Ldpc = 0x20;
constexpr uint8_t kPhy2Ndp4xLtf32us = 0x02;
constexpr uint8_t kPhy2StbcRxUnder80 = 0x08;
constexpr uint8_t kPhy6PpePresent = 0x80;
constexpr uint8_t kPhy7SuMu4xLtf08us = 0x04;
constexpr uint8_t kPhy9FullBwSuCompSigb = 0x10;
constexpr uint8_t kPhy9FullBwSuNonCompSigb = 0x20;
constexpr uint8_t kPhy9NominalPad16us = 0x80;

/* HE Operation parameters (first 4 bytes, little-endian, incl. BSS color). */
constexpr uint32_t kOpPeDurationMask = 0x00000007;
constexpr uint32_t kOpRtsThresholdMask = 0x00003ff0;
constexpr uint32_t kOpVhtOperInfo = 0x00004000;
constexpr uint32_t kOpCoHostedBss = 0x00008000;
constexpr uint32_t kOp6GhzOpInfo = 0x00020000;
constexpr uint32_t kOpPartialBssColor = 0x40000000;
constexpr uint32_t kOpBssColorDisabled = 0x80000000;

/* What the access point said about HE in its beacon. */
struct ApHe {
  bool cap = false, op = false, mu_edca = false;
  uint8_t mac[6] = {};
  uint8_t phy[11] = {};
  uint16_t rx_mcs80 = 0xffff; /* the AP's receive map at <= 80 MHz */
  uint8_t ppe[25] = {};       /* PPE thresholds, if phy[6] says present */
  uint8_t ppe_len = 0;
  uint32_t op_params = 0;     /* HE Operation parameters + BSS color info */
  uint8_t mu_edca_rec[12] = {}; /* BE, BK, VI, VO: ACI/AIFSN, ECWmin/max, timer */

  bool usable() const { return cap && op; }
  bool ldpc() const { return (phy[1] & kPhy1Ldpc) != 0; }
  uint8_t bss_color() const { return static_cast<uint8_t>((op_params >> 24) & 0x3f); }
};

inline unsigned popcount8(unsigned v) {
  unsigned n = 0;
  for (; v; v &= v - 1) n++;
  return n;
}

/* PPE threshold field size: header + 6 bits per (NSS, RU) pair, in bytes. */
inline size_t ppe_size(uint8_t header, const uint8_t phy[11]) {
  if (!(phy[6] & kPhy6PpePresent)) return 0;
  const unsigned bits = 7 + 6 * popcount8((header >> 3) & 0x0f) * ((header & 7) + 1);
  return (bits + 7) / 8;
}

/* HE Capabilities body after the extension ID; false if malformed. */
inline bool parse_cap(const uint8_t *v, size_t n, ApHe &ap) {
  ap.cap = false;
  ap.ppe_len = 0;
  if (n < 6 + 11 + 4) return false;
  std::memcpy(ap.mac, v, 6);
  std::memcpy(ap.phy, v + 6, 11);
  size_t mcs = 4;
  if (ap.phy[0] & kPhy0Width160In5) mcs += 4;
  if (ap.phy[0] & kPhy0Width8080In5) mcs += 4;
  if (n < 17 + mcs) return false;
  ap.rx_mcs80 = static_cast<uint16_t>(v[17] | v[18] << 8);
  if (ap.phy[6] & kPhy6PpePresent) {
    if (n <= 17 + mcs) return false;
    const size_t sz = ppe_size(v[17 + mcs], ap.phy);
    if (sz > sizeof(ap.ppe) || n < 17 + mcs + sz) return false;
    std::memcpy(ap.ppe, v + 17 + mcs, sz);
    ap.ppe_len = static_cast<uint8_t>(sz);
  }
  ap.cap = true;
  return true;
}

/* HE Operation body after the extension ID. */
inline bool parse_op(const uint8_t *v, size_t n, ApHe &ap) {
  ap.op = false;
  if (n < 6) return false;
  const uint32_t params = static_cast<uint32_t>(v[0]) | static_cast<uint32_t>(v[1]) << 8 |
                          static_cast<uint32_t>(v[2]) << 16 | static_cast<uint32_t>(v[3]) << 24;
  size_t need = 6;
  if (params & kOpVhtOperInfo) need += 3;
  if (params & kOpCoHostedBss) need += 1;
  if (params & kOp6GhzOpInfo) need += 5;
  if (n < need) return false;
  ap.op_params = params;
  ap.op = true;
  return true;
}

/* MU EDCA Parameter Set body after the extension ID: QoS info + 4 records. */
inline bool parse_mu_edca(const uint8_t *v, size_t n, ApHe &ap) {
  ap.mu_edca = false;
  if (n < 13) return false;
  std::memcpy(ap.mu_edca_rec, v + 1, 12);
  ap.mu_edca = true;
  return true;
}

/* Our HE Capabilities element (255, len, 35, ...). two_ss: two receive and
 * transmit chains. Receive A-MSDU size stays the HT/VHT one (3839/3895
 * bytes), so frames still fit the default 4 KiB receive buffers. */
constexpr size_t kOurCapElementSize = 3 + 6 + 11 + 4;
inline size_t our_cap_element(uint8_t out[kOurCapElementSize], bool two_ss) {
  const uint8_t mac[6] = {kMac0HtcHe, kMac1TfPad16us, kMac2Ba32, 0, kMac4AmsduInAmpdu, 0};
  uint8_t phy[11] = {};
  phy[0] = kPhy0Width40In24 | kPhy0Width4080In5;
  phy[1] = kPhy1ClassA | kPhy1Ldpc;
  phy[2] = kPhy2Ndp4xLtf32us;
  phy[7] = kPhy7SuMu4xLtf08us;
  phy[9] = kPhy9NominalPad16us | kPhy9FullBwSuCompSigb | kPhy9FullBwSuNonCompSigb;
  const uint16_t map = two_ss ? 0xfffa : 0xfffe; /* MCS 0-11 on 1 or 2 streams */
  out[0] = 255;
  out[1] = static_cast<uint8_t>(kOurCapElementSize - 2);
  out[2] = kExtHeCap;
  std::memcpy(out + 3, mac, 6);
  std::memcpy(out + 9, phy, 11);
  for (int k = 0; k < 2; k++) { /* RX map, TX map */
    out[20 + 2 * k] = static_cast<uint8_t>(map);
    out[21 + 2 * k] = static_cast<uint8_t>(map >> 8);
  }
  return kOurCapElementSize;
}

/* TLC HE rate mask for one stream: the AP's receive map against ours (MCS
 * 0-11), HE MCS 10/11 only with LDPC. */
inline uint16_t tlc_mask(uint16_t ap_rx_map, unsigned stream, bool ldpc) {
  const unsigned ap = (ap_rx_map >> (2 * stream)) & 3;
  if (ap == 3) return 0;
  uint16_t m = ap == 0 ? 0x00ff : ap == 1 ? 0x03ff : 0x0fff;
  if (!ldpc) m &= 0x03ff;
  return m;
}

/* Streams the AP can receive (1 or 2), from its HE MCS map. */
inline unsigned ap_streams(uint16_t ap_rx_map) {
  return ((ap_rx_map >> 2) & 3) != 3 ? 2u : 1u;
}

inline uint8_t ppe_value(const uint8_t *ppe, size_t len, unsigned bit) {
  if (bit / 8 >= len) return 7;
  unsigned value = ppe[bit / 8] >> (bit % 8);
  if (bit % 8 > 5 && bit / 8 + 1 < len) value |= static_cast<unsigned>(ppe[bit / 8 + 1]) << (8 - bit % 8);
  return static_cast<uint8_t>(value & 7);
}

/* Packet extension per stream and RU size: [ss][ru][0] PPET8, [1] PPET16
 * (7 = none). ru_count is 4 for STA_HE_CTXT_CMD, 5 for the MLD station
 * config. Returns whether the firmware should use them. */
inline bool packet_ext(const ApHe &ap, uint8_t *out, unsigned ru_count) {
  std::memset(out, 7, 2 * ru_count * 2);
  auto at = [&](unsigned ss, unsigned ru, unsigned k) -> uint8_t & { return out[(ss * ru_count + ru) * 2 + k]; };
  if ((ap.phy[6] & kPhy6PpePresent) && ap.ppe_len) {
    const unsigned nss = (ap.ppe[0] & 7) + 1;
    const unsigned mask = (ap.ppe[0] >> 3) & 15;
    unsigned bit = 7;
    for (unsigned ss = 0; ss < nss; ss++)
      for (unsigned ru = 0; ru < 4; ru++) {
        if (!(mask & (1u << ru))) continue;
        if (ss < 2 && ru < ru_count) {
          at(ss, ru, 1) = ppe_value(ap.ppe, ap.ppe_len, bit);
          at(ss, ru, 0) = ppe_value(ap.ppe, ap.ppe_len, bit + 3);
        }
        bit += 6;
      }
    return true;
  }
  const unsigned padding = (ap.phy[9] >> 6) & 3; /* nominal packet padding */
  if (padding == 3) return false;
  for (unsigned ss = 0; ss < 2; ss++)
    for (unsigned ru = 0; ru < ru_count; ru++) {
      at(ss, ru, 0) = padding == 1 ? 0 : 7;
      at(ss, ru, 1) = padding == 2 ? 0 : 7;
    }
  return true;
}

/* HT Control capabilities the firmware may use toward the AP (iwl HTC flags:
 * bit 0 HE HTC, bit 4 BSR, bit 5 OMI, bit 6 BQR). */
inline uint32_t htc_flags(const ApHe &ap) {
  uint32_t h = (ap.mac[0] & kMac0HtcHe) ? 1u : 0u;
  if (ap.mac[2] & kMac2Bsr) h |= 1u << 4;
  if (ap.mac[3] & kMac3Omi) h |= 1u << 5;
  if (ap.mac[4] & kMac4Bqr) h |= 1u << 6;
  return h;
}

/* Firmware AC index (BK 0, BE 1, VI 2, VO 3) of MU EDCA record r (BE, BK, VI, VO). */
inline unsigned mu_edca_fw_ac(unsigned r) {
  static const unsigned map[4] = {1, 0, 2, 3};
  return map[r & 3];
}

} // namespace sta_he
