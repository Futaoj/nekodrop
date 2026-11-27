// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "mt7925_active.h"

#include <vector>

namespace mtk::mt7925 {

enum class AwDlBaDirection : uint8_t { tx = 1, rx = 2 };

inline size_t awdl_ba_data_header(const uint8_t *frame, size_t bytes) {
  const size_t header = awdl_header_size(frame, bytes);
  if (!header || (frame[0] != 0x08 && frame[0] != 0x88) || (frame[4] & 1u) || (frame[22] & 0x0fu))
    return 0;
  // Leave room for an inserted QoS Control field and accept only single MSDUs on TID 0.
  if (header == 24 ? bytes > 2302 : (frame[24] & 0x8fu) != 0) return 0;
  return header;
}

inline bool awdl_ba_bulk_data(const uint8_t *frame, size_t bytes) {
  return bytes >= 512 && awdl_ba_data_header(frame, bytes) != 0;
}

constexpr uint16_t next_awdl_ba_sequence(uint16_t seq) {
  return static_cast<uint16_t>((seq + 1u) & 0x0fffu);
}

// The vector is a separate scratch buffer. Only advance seq after submitting this frame.
inline size_t build_awdl_ba_qos(const uint8_t *frame, size_t bytes, uint16_t seq, std::vector<uint8_t> &out) {
  const size_t header = awdl_ba_data_header(frame, bytes);
  if (!header) return 0;

  out.resize(bytes + (26 - header));
  std::memcpy(out.data(), frame, 24);
  out[0] = 0x88;
  out[1] &= static_cast<uint8_t>(~0x08u);  // Assigning a new sequence clears Retry.
  const uint16_t sequence_control = static_cast<uint16_t>((seq & 0x0fffu) << 4);
  out[22] = static_cast<uint8_t>(sequence_control);
  out[23] = static_cast<uint8_t>(sequence_control >> 8);
  out[24] = 0;  // TID 0, Normal Ack, single MSDU.
  out[25] = header == 26 ? frame[25] : 0;
  std::memcpy(out.data() + 26, frame + header, bytes - header);
  return out.size();
}

struct AwDlTxBaFlow {
  enum class Phase { idle, awaiting, agreed, active, ending };
  static constexpr unsigned tid = 0, max_window = 64;
  Phase phase = Phase::idle;
  std::array<uint8_t, 6> peer{};
  uint64_t sample_start = 0, last_data = 0, next_request = 0, retry_after = 0;
  uint16_t next_seq = 0, window = 0;
  uint8_t token = 0;
  unsigned bulk_frames = 0, requests = 0;
  uint64_t close_backoff = 1000000u;
  bool send_delba = false;
  bool firmware_enabled = false;

  bool matches(const uint8_t *mac) const { return !std::memcmp(mac, peer.data(), peer.size()); }

  void note_bulk(const uint8_t *mac, uint16_t seq, uint64_t now) {
    if (phase != Phase::idle) {
      if (matches(mac)) last_data = now;
      return;
    }
    if (now < retry_after) return;
    if (!bulk_frames || !matches(mac) || now - sample_start > 250000u) {
      std::copy_n(mac, peer.size(), peer.begin());
      sample_start = now;
      bulk_frames = 0;
    }
    last_data = now;
    if (++bulk_frames < 2) return;
    token = static_cast<uint8_t>(token % 255u + 1u);
    next_seq = seq & 0x0fffu;
    requests = 0;
    next_request = now;
    phase = Phase::awaiting;
  }

  bool accept(const uint8_t *mac, uint8_t dialog, uint8_t answer_tid, uint16_t status,
              uint16_t offered, bool immediate, uint64_t now) {
    if (phase != Phase::awaiting || !requests || !matches(mac) || dialog != token || answer_tid != tid) return false;
    if (!immediate || status || offered < 2) {
      finish(now, 1500000u);
      return false;
    }
    window = std::min<uint16_t>(offered, max_window);
    phase = Phase::agreed;
    return true;
  }

  void finish(uint64_t now, uint64_t backoff) {
    phase = Phase::idle;
    bulk_frames = 0;
    retry_after = now + backoff;
    close_backoff = 1000000u;
    send_delba = false;
    firmware_enabled = false;
  }
};

// Connac3 UNI_CMD_STA_REC_UPDATE uses an eight-byte request header.
template<size_t Size>
inline std::array<uint8_t, Size> awdl_ba_sta_request(uint8_t tlvs) {
  std::array<uint8_t, Size> request{};
  request[0] = awdl_bss;
  request[1] = awdl_ba_wcid;
  request[2] = tlvs;
  request[4] = 1;  // append TLVs
  request[5] = awdl_bss;  // own MUAR index
  return request;
}

inline std::array<uint8_t, 88> awdl_ba_station(const uint8_t peer[6]) {
  auto request = awdl_ba_sta_request<88>(6);
  auto *basic = request.data() + 8;
  basic[2] = 20;
  put32(basic + 4, (1u << 16) | (1u << 1));  // CONNECTION_INFRA_AP, peer of our STA BSS
  basic[8] = 2;  // port secure (no key is installed for AWDL)
  basic[9] = 1;  // QoS
  std::memcpy(basic + 12, peer, 6);
  basic[18] = 3; // version + new record

  auto *phy = basic + 20;
  phy[0] = 0x15; phy[2] = 12;
  phy[4] = 0x15;  // 5 GHz basic rates 6, 12 and 24 Mbps
  phy[6] = 0x18;  // OFDM and HT
  phy[7] = phy[10] = 3;  // 64 KiB A-MPDU

  auto *ht = phy + 12;
  ht[0] = 9; ht[2] = 8; ht[4] = 0x0c;  // 20 MHz, SM power save disabled

  auto *rate = ht + 8;
  rate[0] = 1; rate[2] = 16;
  rate[4] = 0xc0; rate[5] = 0x3f; rate[6] = 0xff;

  auto *state = rate + 16;
  state[0] = 7; state[2] = 16; state[4] = 2;  // Associated-state rate record

  auto *native = state + 16;
  native[0] = 0x2b; native[2] = 8; native[6] = 1;  // No 802.3 translation
  return request;
}

inline std::array<uint8_t, 132> awdl_ba_station_vht80(const uint8_t peer[6], uint8_t local_rx_streams) {
  auto request = awdl_ba_sta_request<132>(8);
  auto *basic = request.data() + 8;
  basic[2] = 20;
  put32(basic + 4, (1u << 16) | (1u << 1));  // CONNECTION_INFRA_AP, peer of our STA BSS
  basic[8] = 2;  // CONN_STATE_PORT_SECURE
  basic[9] = 1;  // QoS
  std::memcpy(basic + 12, peer, 6);
  basic[18] = 3;  // EXTRA_INFO_VER | EXTRA_INFO_NEW

  auto *phy = basic + 20;
  phy[0] = 0x15; phy[2] = 12;
  phy[4] = 0x15;  // 5 GHz basic rates 6, 12 and 24 Mbps
  phy[6] = 0x38;  // PHY_TYPE_BIT_OFDM | PHY_TYPE_BIT_HT | PHY_TYPE_BIT_VHT
  phy[7] = 3;     // HT 64 KiB A-MPDU, 0 spacing
  phy[10] = 7;    // VHT max_ampdu_len exponent 7

  auto *ht = phy + 12;
  ht[0] = 9; ht[2] = 8;
  ht[4] = 0xef; ht[5] = 0x01;  // LDPC, 40 MHz, SMPS disabled, SGI 20/40, TX/RX STBC

  auto *vht = ht + 8;
  vht[0] = 10; vht[2] = 16;
  put32(vht + 4, 0x038001b2u);  // MPDU 11454, RX LDPC, SGI80, TX/RX STBC, A-MPDU exp 7
  vht[8] = 0xfe; vht[9] = 0xff;   // Peer RX restricted to the local TX stream.
  vht[10] = local_rx_streams == 2 ? 0xfa : 0xfe; vht[11] = 0xff; // Peer TX limited by local RX.

  auto *rate = vht + 16;
  rate[0] = 1; rate[2] = 16;
  rate[4] = 0xc0; rate[5] = 0x3f;  // All 8 legacy 5 GHz OFDM rates
  rate[6] = 0xff;                  // Local TX uses HT MCS 0-7 on one stream.

  auto *state = rate + 16;
  state[0] = 7; state[2] = 16;
  state[4] = 2;     // MT76_STA_INFO_STATE_ASSOC
  state[12] = 0x12; // Peer operating mode: BW80, one RX stream for our TX.

  auto *mld = state + 16;
  mld[0] = 0x20; mld[2] = 28;
  std::memcpy(mld + 4, peer, 6);
  mld[10] = static_cast<uint8_t>(awdl_ba_wcid);  // primary_id
  mld[14] = static_cast<uint8_t>(awdl_ba_wcid);  // wlan_id
  mld[16] = 1;                                    // link_num
  mld[20] = static_cast<uint8_t>(awdl_ba_wcid);  // link[0].wlan_id
  mld[22] = awdl_bss;                             // link[0].bss_idx

  auto *native = mld + 28;
  native[0] = 0x2b; native[2] = 8; native[6] = 1;  // No 802.3 header translation
  return request;
}

inline std::array<uint8_t, 24> awdl_ba_command(uint8_t tid, uint16_t ssn, uint16_t window, bool enable,
                                               AwDlBaDirection direction, bool amsdu = false) {
  auto request = awdl_ba_sta_request<24>(1);
  auto *ba = request.data() + 8;
  ba[0] = 6; ba[2] = 16;
  ba[4] = tid; ba[5] = static_cast<uint8_t>(direction);
  ba[6] = enable && direction == AwDlBaDirection::rx && amsdu;
  if (enable && tid < 8) ba[7] = static_cast<uint8_t>(1u << tid);
  ba[8] = static_cast<uint8_t>(ssn); ba[9] = static_cast<uint8_t>(ssn >> 8);
  ba[10] = static_cast<uint8_t>(window); ba[11] = static_cast<uint8_t>(window >> 8);
  return request;
}

inline std::array<uint8_t, 16> awdl_remove_ba_station() {
  auto request = awdl_ba_sta_request<16>(1);
  request[8] = 0x25; request[10] = 8;
  return request;
}

inline std::array<uint8_t, 20> awdl_remove_ba_station_mld() {
  auto request = awdl_ba_sta_request<20>(2);
  request[8] = 0x25; request[10] = 8;   // STA_REC_REMOVE
  request[16] = 0x23; request[18] = 4;  // STA_REC_MLD_OFF
  return request;
}

// Generalized Connac3 STA_REC builders for arbitrary (bss, wcid) — used by
// concurrent Wi-Fi STA (AP on BSS 1) and AWDL peers (BSS 0).
template<size_t Size>
inline std::array<uint8_t, Size> sta_rec_header(uint8_t bss, uint16_t wcid, uint8_t tlvs) {
  std::array<uint8_t, Size> request{};
  request[0] = bss;
  request[1] = static_cast<uint8_t>(wcid & 0xffu);
  request[2] = tlvs;
  request[4] = 1;    // append TLVs
  request[5] = bss;  // own MUAR index
  request[6] = static_cast<uint8_t>(wcid >> 8);
  return request;
}

inline std::array<uint8_t, 132> sta_rec_add_vht(uint8_t bss, uint16_t wcid, const uint8_t peer[6],
                                                uint16_t ht_cap, uint8_t ampdu_params,
                                                const uint8_t rx_mcs[2], bool vht, uint32_t vht_cap,
                                                uint16_t vht_mcs, uint8_t bw, uint8_t nss) {
  auto request = sta_rec_header<132>(bss, wcid, vht ? 8 : 7);
  auto *basic = request.data() + 8;
  basic[2] = 20;
  put32(basic + 4, (1u << 16) | (1u << 1));  // CONNECTION_INFRA_AP
  basic[8] = 2;  // CONN_STATE_PORT_SECURE (host-side CCMP or cleartext AWDL)
  basic[9] = 1;  // QoS
  std::memcpy(basic + 12, peer, 6);
  basic[18] = 3;  // EXTRA_INFO_VER | EXTRA_INFO_NEW

  auto *phy = basic + 20;
  phy[0] = 0x15; phy[2] = 12;
  phy[4] = 0x15;  // 5 GHz basic rates 6, 12, 24 Mbps
  phy[6] = vht ? 0x38 : 0x18;  // OFDM | HT (| VHT)
  phy[7] = ampdu_params;
  phy[10] = vht ? static_cast<uint8_t>((vht_cap >> 23) & 7u) : static_cast<uint8_t>(ampdu_params & 3u);

  auto *ht = phy + 12;
  ht[0] = 9; ht[2] = 8;
  ht[4] = static_cast<uint8_t>(ht_cap & 0xffu);
  ht[5] = static_cast<uint8_t>(ht_cap >> 8);

  uint8_t *next = ht + 8;
  if (vht) {
    auto *v = next;
    v[0] = 10; v[2] = 16;
    put32(v + 4, vht_cap);
    v[8] = static_cast<uint8_t>(vht_mcs & 0xffu);
    v[9] = static_cast<uint8_t>(vht_mcs >> 8);
    v[10] = v[8];
    v[11] = v[9];
    next = v + 16;
  }

  auto *rate = next;
  rate[0] = 1; rate[2] = 16;
  rate[4] = 0xc0; rate[5] = 0x3f;  // 5 GHz OFDM rates
  rate[6] = rx_mcs[0];
  rate[7] = rx_mcs[1];

  auto *state = rate + 16;
  state[0] = 7; state[2] = 16;
  state[4] = 2;  // MT76_STA_INFO_STATE_ASSOC
  if (vht) {
    const uint8_t eff_nss = nss >= 2 ? 2 : 1;
    const uint8_t eff_bw = bw >= 2 ? 2 : (bw == 1 ? 1 : 0);
    state[12] = static_cast<uint8_t>((eff_nss << 4) | eff_bw);
  }

  auto *mld = state + 16;
  mld[0] = 0x20; mld[2] = 28;
  std::memcpy(mld + 4, peer, 6);
  mld[10] = static_cast<uint8_t>(wcid);
  mld[14] = static_cast<uint8_t>(wcid);
  mld[16] = 1;
  mld[20] = static_cast<uint8_t>(wcid);
  mld[22] = bss;

  auto *native = mld + 28;
  native[0] = 0x2b; native[2] = 8; native[6] = 1;  // No 802.3 header translation
  return request;
}

inline size_t sta_rec_add_size(bool vht) { return vht ? 132 : 116; }

inline std::array<uint8_t, 24> sta_rec_ba(uint8_t bss, uint16_t wcid, uint8_t tid,
                                          uint16_t ssn, uint16_t window, bool enable,
                                          AwDlBaDirection direction) {
  auto request = sta_rec_header<24>(bss, wcid, 1);
  auto *ba = request.data() + 8;
  ba[0] = 6; ba[2] = 16;
  ba[4] = tid;
  ba[5] = static_cast<uint8_t>(direction);
  if (enable && tid < 8) ba[7] = static_cast<uint8_t>(1u << tid);
  ba[8] = static_cast<uint8_t>(ssn);
  ba[9] = static_cast<uint8_t>(ssn >> 8);
  ba[10] = static_cast<uint8_t>(window);
  ba[11] = static_cast<uint8_t>(window >> 8);
  return request;
}

inline std::array<uint8_t, 20> sta_rec_remove_mld(uint8_t bss, uint16_t wcid) {
  auto request = sta_rec_header<20>(bss, wcid, 2);
  request[8] = 0x25; request[10] = 8;   // STA_REC_REMOVE
  request[16] = 0x23; request[18] = 4;  // STA_REC_MLD_OFF
  return request;
}

} // namespace mtk::mt7925
