// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mt7925_rx.h"

namespace mtk::mt7925 {

constexpr uint8_t awdl_bss = 1;
constexpr uint16_t awdl_broadcast_wcid = 19 - awdl_bss;
constexpr uint16_t awdl_ba_wcid = 1;
constexpr uint16_t roc_visit_ms = 1000;
constexpr uint16_t roc_max_awdl_ms = 4000;

enum class AwDlTxKind : uint8_t { basic, fast, aggregate, ampdu };

class AwDlTxSlots {
public:
  static constexpr unsigned capacity = 128;
  static constexpr unsigned max_inflight = 64;
  static constexpr unsigned guard_ms = 120;

  unsigned head() const { return head_; }
  unsigned pending() const { return distance(tail_, head_); }
  unsigned available_on_channel(uint64_t milliseconds_left) const {
    // Leave time for retries and DMA completion before releasing the lease.
    constexpr unsigned per_frame_ms = 2;
    if (milliseconds_left <= guard_ms) return 0;
    const auto room = std::min<uint64_t>(max_inflight, (milliseconds_left - guard_ms) / per_frame_ms);
    return room > pending() ? static_cast<unsigned>(room - pending()) : 0;
  }
  bool unpublished() const { return published_ != head_; }
  unsigned published_pending() const { return distance(tail_, published_); }
  unsigned unfetched() const { return distance(fetched_, published_); }
  uint64_t oldest_age(uint64_t now) const {
    return published_pending() ? now - slots_[tail_].published_at : 0;
  }
  bool can_release_for_renewal(uint64_t now, uint64_t lease_end) const {
    // Allow the old grant to run to its end while the MAC still owns a frame.
    // TX_FREE may trail an expired grant; all DMA pages stay pinned until reclaim.
    return !published_pending() || (now >= lease_end && !unfetched());
  }
  static bool can_submit(uint64_t now, uint64_t lease_end) {
    return now < lease_end && lease_end - now > guard_ms;
  }
  bool front_completed() const { return tail_ != published_ && slots_[tail_].complete; }

  uint16_t next_token() const {
    const unsigned next = slots_[head_].token + capacity;
    return static_cast<uint16_t>(next < 8192 ? (next | head_) : (capacity | head_));
  }

  void prepare(uint16_t token, AwDlTxKind kind = AwDlTxKind::basic) {
    slots_[head_] = {token, false, kind, 0};
    head_ = (head_ + 1) % capacity;
  }

  void publish(uint64_t now) {
    while (published_ != head_) {
      slots_[published_].published_at = now;
      published_ = (published_ + 1) % capacity;
    }
  }

  bool complete(uint16_t token, AwDlTxKind *kind = nullptr) {
    const unsigned index = token % capacity;
    auto &slot = slots_[index];
    if (distance(tail_, index) >= distance(tail_, published_) ||
        slot.token != token || slot.complete) return false;
    slot.complete = true;
    if (kind) *kind = slot.kind;
    return true;
  }

  bool observe_fetch(unsigned index) {
    if (index >= capacity || distance(fetched_, index) > distance(fetched_, published_)) return false;
    fetched_ = index;
    return true;
  }

  unsigned reclaim() {
    const auto old_tail = tail_;
    while (tail_ != fetched_ && slots_[tail_].complete) tail_ = (tail_ + 1) % capacity;
    return distance(old_tail, tail_);
  }

private:
  static unsigned distance(unsigned from, unsigned to) { return (to + capacity - from) % capacity; }
  struct Slot { uint16_t token; bool complete; AwDlTxKind kind; uint64_t published_at; };
  std::array<Slot, capacity> slots_{};
  unsigned head_ = 0, tail_ = 0, published_ = 0, fetched_ = 0;
};

inline void put32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline std::array<uint8_t, 28> roc_request(uint8_t channel, uint8_t token, uint8_t type = 1,
                                              uint8_t bss = awdl_bss, uint16_t duration = roc_visit_ms,
                                              unsigned width_mhz = 20) {
  const unsigned eff_w = effective_width(channel, width_mhz);
  const uint8_t center = center_channel(channel, eff_w);
  const uint8_t cw = eff_w == 80 ? 1 : 0;  // MT7925_CW_80MHZ = 1, MT7925_CW_20_40MHZ = 0
  const uint8_t sco = roc_sco(channel, eff_w);
  const uint8_t band = band_for_channel(channel);
  std::array<uint8_t, 28> p{};
  p[6] = 24;
  p[8] = bss;
  p[9] = token;
  p[10] = channel;
  p[11] = sco;
  p[12] = band;            // 1 = 2.4 GHz, 2 = 5 GHz
  p[13] = cw;
  p[14] = center;
  p[16] = cw;              // bw_from_ap
  p[17] = center;
  p[19] = type;
  p[20] = duration & 0xff;
  p[21] = duration >> 8;
  p[24] = 0xff;
  return p;
}

inline std::array<uint8_t, 16> roc_abort(uint8_t token, uint8_t bss = awdl_bss) {
  std::array<uint8_t, 16> p{};
  p[4] = 1;
  p[6] = 12;
  p[8] = bss;
  p[9] = token;
  p[10] = 0xff;
  return p;
}

enum class Grant { pending, granted, invalid };

inline Grant roc_grant(const uint8_t *p, uint32_t bytes, uint8_t channel, uint8_t token,
                       uint8_t request_type, uint32_t &milliseconds, uint8_t bss = awdl_bss,
                       uint16_t requested = roc_visit_ms, unsigned width_mhz = 20) {
  if (bytes < 44 || p[36] != 0x27 || !(p[38] & 4)) return Grant::pending;
  if ((unsigned(p[32]) | (unsigned(p[33]) << 8)) != bytes - 32 || bytes < 48) return Grant::invalid;
  const unsigned eff_w = effective_width(channel, width_mhz);
  const uint8_t expected_sco = roc_sco(channel, eff_w);
  const uint8_t expected_cw = eff_w == 80 ? 1 : 0;
  const uint8_t expected_center = center_channel(channel, eff_w);
  const uint8_t expected_band = band_for_channel(channel);
  uint32_t candidate = 0;
  for (size_t pos = 48; pos < bytes;) {
    if (bytes - pos < 4) return Grant::invalid;
    const uint8_t *tlv = p + pos;
    const size_t length = unsigned(tlv[2]) | (unsigned(tlv[3]) << 8);
    if (length < 4 || length > bytes - pos) return Grant::invalid;
    if (tlv[0] == 0 && tlv[1] == 0) {
      if (length < 20) return Grant::invalid;
      if (tlv[4] == bss && tlv[5] == token) {
        const uint32_t duration = rx_word(tlv + 16);
        if (candidate || tlv[6] || tlv[7] != channel || tlv[8] != expected_sco ||
            tlv[9] != expected_band || tlv[10] != expected_cw || tlv[11] != expected_center ||
            tlv[12] || tlv[13] != request_type || duration <= 10 || duration > requested)
          return Grant::invalid;
        candidate = duration;
      }
    }
    pos += length;
  }
  if (!candidate) return Grant::pending;
  milliseconds = candidate;
  return Grant::granted;
}

// The radio link carries native, unencrypted AWDL action and IPv6 data frames.
inline bool awdl_ipv6_payload(const uint8_t *body, size_t bytes) {
  constexpr uint8_t prefix[]{0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0, 3, 4};
  return bytes >= 16 + 40 && !std::memcmp(body, prefix, sizeof(prefix)) &&
         body[14] == 0x86 && body[15] == 0xdd && (body[16] >> 4) == 6;
}

inline size_t awdl_header_size(const uint8_t *frame, size_t bytes) {
  if (!frame || bytes < 32 || bytes > 2304 || (frame[1] & 0xc7) ||
      std::memcmp(frame + 16, awdl_bssid.data(), awdl_bssid.size())) return 0;
  if (frame[0] == 0xd0 && frame[24] == 0x7f && frame[25] == 0 &&
      frame[26] == 0x17 && frame[27] == 0xf2 && frame[28] == 8) return 24;
  if (frame[0] != 0x08 && frame[0] != 0x88) return 0;
  const size_t header = frame[0] == 0x88 ? 26 : 24;
  return awdl_ipv6_payload(frame + header, bytes - header) ? header : 0;
}

// The winawdl receiver also accepts native QoS A-MSDUs. Their first
// LLC header follows a 14-byte subframe header. The TX path still uses
// awdl_header_size() to validate only the single MS-DU layout it submits.
inline bool valid_awdl_rx(const uint8_t *frame, size_t bytes) {
  if (awdl_header_size(frame, bytes)) return true;
  // RX QoS frames can have a four-byte HT Control field after QoS Control.
  if (!frame || bytes < 26 || bytes > 12288 || frame[0] != 0x88 ||
      (frame[1] & 0x47) || std::memcmp(frame + 16, awdl_bssid.data(), awdl_bssid.size()))
    return false;
  const size_t header = (frame[1] & 0x80) ? 30 : 26;
  if (bytes < header) return false;
  if (!(frame[24] & 0x80))
    return awdl_ipv6_payload(frame + header, bytes - header);
  const size_t llc = header + 14;
  if (bytes < llc + 56) return false;  // Subframe, SNAP, AWDL and IPv6 headers.
  const size_t payload = (size_t(frame[llc - 2]) << 8) | frame[llc - 1];
  return payload <= bytes - llc && awdl_ipv6_payload(frame + llc, payload);
}

constexpr size_t awdl_amsdu_max = 3839;

// Coalesce two queued MSDUs for the same receiver and TID. Use the first
// frame's 802.11 sequence number and preserve both AWDL payloads.
inline size_t build_awdl_amsdu(uint8_t *out, size_t capacity, const uint8_t *first, size_t first_len,
                               const uint8_t *second, size_t second_len) {
  const size_t a = awdl_header_size(first, first_len);
  const size_t b = awdl_header_size(second, second_len);
  if (!a || !b || first[0] == 0xd0 || second[0] == 0xd0 ||
      (first[4] & 1u) || std::memcmp(first + 4, second + 4, 12)) return 0;
  const uint8_t tid = a == 26 ? first[24] : 0;
  if ((tid & 0xf0) || tid != (b == 26 ? second[24] : 0)) return 0;
  const size_t first_payload = first_len - a, second_payload = second_len - b;
  const size_t padding = (4 - ((14 + first_payload) & 3)) & 3;
  const size_t total = 26 + 14 + first_payload + padding + 14 + second_payload;
  if (total > awdl_amsdu_max || total > capacity) return 0;

  std::memcpy(out, first, 24);
  out[0] = 0x88;
  out[24] = 0x80 | tid;
  out[25] = 0;
  const auto subframe = [](uint8_t *p, const uint8_t *frame, size_t hdr, size_t payload) {
    std::memcpy(p, frame + 4, 12);
    p[12] = static_cast<uint8_t>(payload >> 8);
    p[13] = static_cast<uint8_t>(payload);
    std::memcpy(p + 14, frame + hdr, payload);
  };
  subframe(out + 26, first, a, first_payload);
  std::memset(out + 26 + 14 + first_payload, 0, padding);
  subframe(out + 26 + 14 + first_payload + padding, second, b, second_payload);
  return total;
}

inline bool valid_awdl_tx_amsdu(const uint8_t *frame, size_t bytes) {
  if (bytes > awdl_amsdu_max || !valid_awdl_rx(frame, bytes) || frame[0] != 0x88 || (frame[1] & 0x80) ||
      (frame[4] & 1u) || (frame[24] & 0xf0) != 0x80) return false;
  size_t offset = 26;
  for (unsigned i = 0; i < 2; ++i) {
    if (bytes - offset < 14 || std::memcmp(frame + offset, frame + 4, 12)) return false;
    const size_t length = (size_t(frame[offset + 12]) << 8) | frame[offset + 13];
    if (length > bytes - offset - 14 || !awdl_ipv6_payload(frame + offset + 14, length)) return false;
    offset += 14 + length;
    if (!i) {
      const size_t padding = (4 - ((14 + length) & 3)) & 3;
      if (padding > bytes - offset) return false;
      offset += padding;
    }
  }
  return offset == bytes;
}

inline bool awdl_mdns(const uint8_t *frame, size_t bytes, size_t header) {
  if (!header || frame[0] == 0xd0 || bytes < header + 16 + 48) return false;
  const uint8_t *ip = frame + header + 16;
  if (ip[6] != 17) return false;
  constexpr uint16_t mdns = 5353;
  return ((uint16_t(ip[40]) << 8) | ip[41]) == mdns ||
         ((uint16_t(ip[42]) << 8) | ip[43]) == mdns;
}

// Indices in the fixed-rate table. The HT/VHT entries are programmed at MAC init.
enum class AwDlRate : uint8_t {
  ofdm6 = 18, ofdm24 = 22,
  ht20 = 26, vht20 = 27, // MCS 7
  ht20_mcs3 = 28, ht20_mcs5 = 29, vht20_mcs8 = 30
};

// Connac3 WTBL rate word: NSS - 1 in bits 13:10, PHY mode in 9:6 and MCS in 3:0.
constexpr uint16_t awdl_fixed_rate_code(AwDlRate rate) {
  switch (rate) {
    case AwDlRate::ht20_mcs3: return (2u << 6) | 3u;
    case AwDlRate::ht20_mcs5: return (2u << 6) | 5u;
    case AwDlRate::ht20: return (2u << 6) | 7u;
    case AwDlRate::vht20: return (4u << 6) | 7u;
    case AwDlRate::vht20_mcs8: return (4u << 6) | 8u;
    default: return 0; // Legacy rates are installed in the basic-rate table.
  }
}

constexpr std::array awdl_rate_steps{
    AwDlRate::ofdm24, AwDlRate::ht20_mcs3, AwDlRate::ht20_mcs5,
    AwDlRate::ht20, AwDlRate::vht20, AwDlRate::vht20_mcs8};

constexpr size_t awdl_rate_step(AwDlRate rate) {
  for (size_t i = 1; i < awdl_rate_steps.size(); ++i)
    if (awdl_rate_steps[i] == rate) return i;
  return 0;
}

constexpr AwDlRate awdl_higher_rate(AwDlRate a, AwDlRate b) {
  if (a == AwDlRate::ofdm6) return b;
  return awdl_rate_step(a) < awdl_rate_step(b) ? b : a;
}

// Feed only the first, on-channel result of a fixed-rate unicast submission.
// Apply the same thresholds as the Intel ladder to a 20 MHz, 1SS window.
class AwDlRateLadder {
public:
  AwDlRate rate(AwDlRate cap) const { return awdl_rate_steps[std::min(step_, awdl_rate_step(cap))]; }

  void note(bool acked, AwDlRate sent, AwDlRate cap) {
    const size_t top = awdl_rate_step(cap);
    const size_t current = std::min(step_, top);
    if (sent != awdl_rate_steps[current]) return;
    if (step_ != current) {
      step_ = current;
      results_ = failures_ = 0;
    }
    failures_ += !acked;
    if (++results_ < 32) return;
    if (failures_ * 5 > results_ && step_ > 0) --step_;
    else if (failures_ * 20 < results_ && step_ < top) ++step_;
    results_ = failures_ = 0;
  }

private:
  size_t step_ = 0;
  unsigned results_ = 0, failures_ = 0;
};

enum class AwDlTxFormat { raw, amsdu, ampdu, block_ack_action, peer_data };

inline AwDlRate awdl_peer_rate(const uint8_t *frame, size_t bytes) {
  constexpr size_t tlv_start = 40;
  if (bytes < tlv_start || awdl_header_size(frame, bytes) != 24 || frame[0] != 0xd0)
    return AwDlRate::ofdm6;
  auto rate = AwDlRate::ofdm6;
  for (size_t pos = tlv_start; pos < bytes;) {
    if (bytes - pos < 3)
      return std::all_of(frame + pos, frame + bytes, [](uint8_t v) { return v == 0; }) ? rate : AwDlRate::ofdm6;
    const uint8_t kind = frame[pos];
    const size_t len = unsigned(frame[pos + 1]) | (unsigned(frame[pos + 2]) << 8);
    pos += 3;
    if (len > bytes - pos) return AwDlRate::ofdm6;
    const uint8_t *value = frame + pos;
    if (kind == 7 && len >= 6) {
      const uint8_t mcs = value[5];
      if (mcs & (1u << 7)) rate = awdl_higher_rate(rate, AwDlRate::ht20);
      else if (mcs & (1u << 5)) rate = awdl_higher_rate(rate, AwDlRate::ht20_mcs5);
      else if (mcs & (1u << 3)) rate = awdl_higher_rate(rate, AwDlRate::ht20_mcs3);
    } else if (kind == 17) { // AWDL's embedded 802.11 information elements.
      for (size_t ie = 0; ie < len;) {
        if (len - ie == 1) {
          if (value[ie]) return AwDlRate::ofdm6;
          break;
        }
        const size_t ie_len = value[ie + 1];
        if (ie_len > len - ie - 2) return AwDlRate::ofdm6;
        if (value[ie] == 191 && ie_len == 12) {
          const unsigned rx_mcs_1ss = value[ie + 6] & 3u;
          if (rx_mcs_1ss == 0) rate = awdl_higher_rate(rate, AwDlRate::vht20);
          if (rx_mcs_1ss == 1 || rx_mcs_1ss == 2) rate = AwDlRate::vht20_mcs8;
        }
        ie += 2 + ie_len;
      }
    }
    pos += len;
  }
  return rate;
}

inline bool write_awdl_txwi(uint8_t *page, size_t capacity, uint32_t address,
                           uint16_t token, const uint8_t *frame, size_t bytes,
                           AwDlRate rate = AwDlRate::ofdm6, AwDlTxFormat format = AwDlTxFormat::raw) {
  constexpr size_t txwi_bytes = 64;
  size_t header = 0;
  switch (format) {
    case AwDlTxFormat::raw:
      header = awdl_header_size(frame, bytes);
      break;
    case AwDlTxFormat::amsdu:
      if (rate >= AwDlRate::ht20 && valid_awdl_tx_amsdu(frame, bytes)) header = 26;
      break;
    case AwDlTxFormat::ampdu:
    case AwDlTxFormat::peer_data:
      if (awdl_header_size(frame, bytes) == 26 && !(frame[4] & 1u) && frame[24] == 0) header = 26;
      break;
    case AwDlTxFormat::block_ack_action:
      if (frame && (bytes == 33 || bytes == 30) && frame[0] == 0xd0 && !(frame[1] & 0xc7) && !(frame[4] & 1u) &&
          !std::memcmp(frame + 16, awdl_bssid.data(), awdl_bssid.size()) &&
          frame[24] == 3 && ((bytes == 33 && (frame[25] == 0 ||
              (frame[25] == 1 && !frame[27] && !frame[28]))) || (bytes == 30 && frame[25] == 2))) header = 24;
      break;
  }
  if (!header || !page || capacity < txwi_bytes + bytes ||
      (address & 3u) || UINT32_MAX - address < txwi_bytes + bytes - 1 || token >= 8192)
    return false;

  const bool group = (frame[4] & 1u) != 0;
  const bool action = frame[0] == 0xd0;
  const bool ampdu = format == AwDlTxFormat::ampdu;
  const bool station_data = ampdu || format == AwDlTxFormat::peer_data;
  const bool ba_action = format == AwDlTxFormat::block_ack_action;
  if (group || action) rate = AwDlRate::ofdm6;
  // Connac3 interprets this field as a management-frame subtype for ADDBA.
  const uint32_t qos = header == 26 ? frame[24] & 0x0fu : (ba_action && frame[25] == 0 ? 2u : 0u);
  const uint32_t seq = (frame[22] >> 4) | (uint32_t(frame[23]) << 4);
  std::memset(page, 0, txwi_bytes);
  const uint32_t queue = station_data ? 4u * awdl_bss + 1u : 0x10u; // WMM best effort or alternate management
  put32(page, (queue << 25) | static_cast<uint32_t>(32 + bytes));
  put32(page + 4, (ampdu ? 0 : 1u << 31) | (uint32_t(awdl_bss) << 25) |
                  (qos << 21) | (static_cast<uint32_t>(header / 2) << 16) |
                  (2u << 14) | (station_data || ba_action ? awdl_ba_wcid : awdl_broadcast_wcid));
  put32(page + 8, action ? 13u : (2u << 4) | (frame[0] >> 4));
  put32(page + 12, (1u << 31) | (ampdu ? 0 : 1u << 28) | (seq << 16) |
                  (group ? (1u << 11) | (1u << 4) | 1u : (ampdu ? 15u : 7u) << 11));
  put32(page + 20, 1);
  const uint32_t fixed_20mhz = rate >= AwDlRate::ht20 ? 1u << 25 : 0;
  put32(page + 24, (ampdu ? 0 : fixed_20mhz | (uint32_t(rate) << 16)) | 0x1cu);
  put32(page + 32, 0x8000u | token);
  put32(page + 40, address + txwi_bytes);
  put32(page + 44, 0x8000u | static_cast<uint32_t>(bytes));
  std::memcpy(page + txwi_bytes, frame, bytes);
  return true;
}

// General Connac3 802.11 TXD+TXP writer for concurrent Wi-Fi STA (BSS 1) and AWDL (BSS 0).
// Host-side CCMP encrypts STA data in software, so MT_TXD3_PROTECT_FRAME is always 0.
inline bool write_general_txwi(uint8_t *page, size_t capacity, uint32_t address,
                               uint16_t token, const uint8_t *frame, size_t bytes,
                               uint8_t bss, uint16_t wcid, bool fixed_rate, uint8_t rate_slot,
                               bool no_ack, bool force_altx, bool allow_ba) {
  constexpr size_t txwi_bytes = 64;
  if (!frame || bytes < 10 || !page || capacity < txwi_bytes + bytes ||
      (address & 3u) || UINT32_MAX - address < txwi_bytes + bytes - 1 || token >= 8192)
    return false;

  const uint16_t fc = static_cast<uint16_t>(frame[0] | (frame[1] << 8));
  const uint8_t fc_type = static_cast<uint8_t>((fc >> 2) & 3u);
  const uint8_t fc_stype = static_cast<uint8_t>((fc >> 4) & 0xfu);
  size_t header = 24;
  if (fc_type == 1) {
    header = (fc_stype == 10 || fc_stype == 11 || fc_stype == 13) ? 10 : 16;
  } else {
    if (fc_type == 2) {
      if ((fc & 0x0300u) == 0x0300u) header += 6;  // 4-address
      if (fc_stype & 8u) header += 2;              // QoS
    }
    if (fc & 0x8000u) header += 4;                 // HTC
  }
  if (bytes < header) return false;

  const bool multicast = (frame[4] & 1u) != 0;
  const bool eff_no_ack = no_ack || multicast;
  const bool is_qos = fc_type == 2 && (fc_stype & 8u);
  const bool is_addba_req = fc_type == 0 && fc_stype == 13 && bytes >= 26 &&
                            frame[24] == 3u && frame[25] == 0u;
  const uint32_t tid = is_qos ? (frame[header - 2] & 0x0fu) : (is_addba_req ? 2u : 0u);
  const uint32_t seq = (fc_type != 1 && bytes >= 24)
      ? ((frame[22] >> 4) | (static_cast<uint32_t>(frame[23]) << 4))
      : 0u;

  std::memset(page, 0, txwi_bytes);
  const uint32_t queue = (force_altx || fc_type == 0 || multicast || (fixed_rate && !is_qos))
      ? 0x10u
      : (4u * bss + 1u);
  put32(page, (queue << 25) | static_cast<uint32_t>(32 + bytes));
  put32(page + 4, (fixed_rate ? (1u << 31) : 0u) |
                  (static_cast<uint32_t>(bss) << 25) |
                  (tid << 21) |
                  (static_cast<uint32_t>(header / 2) << 16) |
                  (2u << 14) |
                  (wcid & 0x0fffu));
  put32(page + 8, (static_cast<uint32_t>(fc_type) << 4) | static_cast<uint32_t>(fc_stype));
  const uint32_t retries = eff_no_ack ? 1u : (allow_ba || bss == 1 ? 15u : 7u);
  put32(page + 12, (fc_type != 1 ? (1u << 31) : 0u) |
                   (allow_ba ? 0u : (1u << 28)) |
                   (seq << 16) |
                   (retries << 11) |
                   (multicast ? (1u << 4) : 0u) |
                   (eff_no_ack ? 1u : 0u));
  put32(page + 20, 1u);
  const uint32_t fixed_20mhz = (fixed_rate && rate_slot >= 26) ? (1u << 25) : 0u;
  put32(page + 24, (fixed_rate ? (fixed_20mhz | (static_cast<uint32_t>(rate_slot) << 16)) : 0u) | 0x1cu);
  put32(page + 32, 0x8000u | token);
  put32(page + 40, address + txwi_bytes);
  put32(page + 44, 0x8000u | static_cast<uint32_t>(bytes));
  std::memcpy(page + txwi_bytes, frame, bytes);
  return true;
}

} // namespace mtk::mt7925
