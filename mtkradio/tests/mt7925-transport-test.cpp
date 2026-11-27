// SPDX-License-Identifier: GPL-2.0-only
#include "mt7925_rx.h"
#include "mt7925_rx_stats.h"
#include "mt7925_upload_ack.h"
#include "mt7925_regs.h"
#include "mt7925_active.h"
#include "mt7925_mcu_wait.h"
#include "mt7925_txpower_info.h"
#include "mt7925_ba.h"
#include "mt7925-ba-cases.h"
#include "mt7925-link-cases.h"
#include "mt7925-rate-cases.h"

extern "C" {
#include "clc_power.h"
#include "nic_caps.h"
#include "passive-scan.h"
#include "station-reg.h"
}

#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {
using namespace mtk::mt7925;

void put32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

void verify_rx() {
  uint32_t mdp_dcr0 = 0xfedcba98u | MDP_DCR0_DAMSDU_EN | MDP_DCR0_RX_HDR_TRANS_EN;
  const auto original = mdp_dcr0;
  const auto read = [&](uint32_t address) { assert(address == 0x820cc800); return mdp_dcr0; };
  assert(configure_native_awdl_rx([&](uint32_t address, uint32_t mask) {
    assert(address == 0x820cc800 && mask == 0x88000);
    mdp_dcr0 &= ~mask;
  }, read));
  assert(mdp_dcr0 == (original & ~0x88000u));
  mdp_dcr0 |= MDP_DCR0_DAMSDU_EN;
  assert(!configure_native_awdl_rx([](uint32_t, uint32_t) {}, read));
  RxDmaSnapshot snapshot;
  unsigned hardware_index = 5;
  const auto index = [&] { return hardware_index; };
  for (unsigned i = 0; i < 5; ++i) assert(snapshot.ready(i, 8, index));
  assert(snapshot.register_reads() == 1 && snapshot.consumed() == 5);
  assert(!snapshot.ready(5, 8, index)); // The descriptor at DIDX is still owned by the device.
  hardware_index = 2; // The DMA producer wrapped around the ring.
  for (unsigned i : {5, 6, 7, 0, 1}) assert(snapshot.ready(i, 8, index));
  assert(snapshot.register_reads() == 3 && snapshot.consumed() == 10);
  hardware_index = 0xffffffffu;
  assert(!snapshot.ready(2, 8, index));
  hardware_index = 3;
  assert(snapshot.ready(2, 8, index));

  std::array<uint8_t, 0x109> rx{};
  // Real channel-36 RX0 words. Frame bytes after RXD groups/pad are synthetic.
  put32(rx.data(), 0x38010109);
  put32(rx.data() + 4, 0x80870013);
  put32(rx.data() + 8, 0x60008c00);
  put32(rx.data() + 12, 0x004324d3);
  put32(rx.data() + 16, 0x81c60000);
  put32(rx.data() + 48, 0x12345678); // Group 2 hardware timestamp
  rx[76] = 100;  // Group 3 RCPI
  rx[88] = 0x80; // Beacon
  MonitorFrame frame{};
  assert(rx_kind(rx.data(), rx.size()) == RxKind::frame);
  assert(parse_monitor_frame(rx.data(), rx.size(), frame));
  assert(frame.offset == 88 && frame.length == 0x109 - 88 && frame.channel == 36 && frame.dbm == -60);
  assert(frame.has_timestamp && frame.timestamp_us == 0x12345678);
  // Replay the board RXD layout above with explicit little-endian PRXV words.
  // These Group 3 words exercise the mt76 bit layout, not a measured link speed.
  put32(rx.data() + 64, 0x00000089u);
  put32(rx.data() + 72, 0x0000200au);
  rx[77] = 112;
  assert(parse_monitor_frame(rx.data(), rx.size(), frame));
  assert(frame.phy_mode == 4 && frame.dbm == -54);
  assert(frame.phy.width_mhz == 80 && frame.phy.mcs == 9 && frame.phy.nss == 2 &&
         frame.phy.short_gi && frame.phy.measured_rssi);
  assert(parse_concurrent_frame(rx.data(), rx.size(), 36, 80, frame));
  assert(frame.phy.prxv0 == 0x89 && frame.phy.prxv2 == 0x200a && frame.phy.prxv3 == 0x7064);
  RxPhyWindow window;
  const uint8_t slots[] = {149, 44, 149, 0};
  assert(!window.note(10, 2000, 4, frame.phy, frame.dbm));
  assert(!window.note(500010, 2000, 4, frame.phy, frame.dbm));
  assert(window.note(1000010, 2000, 0xff, {}, -70));
  const std::string sample = window.finish(slots, 4);
  assert(sample.find("unicast_mpdu_bytes=6000 packets=3") != std::string::npos);
  assert(sample.find("no_group3=1 valid_rates=2 dominant=VHT80/MCS9/2SS/SGI dominant_packets=2 dominant_pct=66") != std::string::npos);
  assert(sample.find("rssi_avg_dbm=-54 rssi_samples=2") != std::string::npos);
  assert(sample.find("peer_slots_44=1 peer_slots_149=2 peer_slots_total=4") != std::string::npos);
  assert(!window.note(2000010, 120, 0xff, {}, -70));
  assert(!window.note(3000010, 120, 0xff, {}, -70));  // Idle keepalives.
  assert(!window.note(5000010, 5000, 0xff, {}, -70));
  assert(window.note(6000010, 5000, 0xff, {}, -70));
  const auto unknown = window.finish(nullptr, 0);
  assert(unknown.find("dominant=unknown") != std::string::npos &&
         unknown.find("rssi_avg_dbm=unknown") != std::string::npos &&
         unknown.find("peer_slots=unknown") != std::string::npos);
  put32(rx.data() + 64, 0x0000000c); // Reserved VHT MCS.
  put32(rx.data() + 76, 0xffffffff); // All RCPI chains unavailable.
  assert(parse_monitor_frame(rx.data(), rx.size(), frame));
  assert(frame.phy_mode == 4 && !frame.phy.width_mhz && !frame.phy.measured_rssi);
  put32(rx.data() + 64, 15);
  put32(rx.data() + 72, 0x1001); // HT MCS15 / 40 MHz, long GI.
  assert(parse_monitor_frame(rx.data(), rx.size(), frame) && frame.phy.width_mhz == 40 &&
         frame.phy.mcs == 15 && frame.phy.nss == 2 && !frame.phy.short_gi);
  // Actual E138 P-RXV words 0, 2, 3 from three iPhone upload intervals,
  // primary 149, 2026-11-26 23:39:12–15. No frame content was recorded.
  struct Sample { uint32_t v0, v2, v3; unsigned mcs; bool short_gi; int dbm; };
  constexpr Sample captured[] = {
      {0x87, 0x204a, 0xffff616d, 7, true, -56},
      {0x87, 0x2042, 0xffff6767, 7, false, -59},
      {0x84, 0x2042, 0xffff6a62, 4, false, -57},
  };
  for (const auto &s : captured) {
    put32(rx.data() + 64, s.v0);
    put32(rx.data() + 72, s.v2);
    put32(rx.data() + 76, s.v3);
    assert(parse_monitor_frame(rx.data(), rx.size(), frame));
    assert(frame.phy_mode == 4 && frame.phy.width_mhz == 80 && frame.phy.nss == 2 &&
           frame.phy.mcs == s.mcs && frame.phy.short_gi == s.short_gi &&
           frame.phy.measured_rssi && frame.dbm == s.dbm);
  }
  rx[88] = 0xd0;
  assert(parse_monitor_frame(rx.data(), rx.size(), frame));
  rx[89] = 0x40;
  assert(!parse_monitor_frame(rx.data(), rx.size(), frame));
  rx[89] = 0;
  rx[15] |= 1; // FCS failure
  assert(!parse_monitor_frame(rx.data(), rx.size(), frame));
  rx[15] &= ~1u;
  assert(!parse_monitor_frame(rx.data(), rx.size() - 1, frame));
  put32(rx.data() + 12, 0x00432cd3); // Channel 44
  assert(parse_monitor_frame(rx.data(), rx.size(), frame) && frame.channel == 44);

  put32(rx.data(), 0x10000044); // RX2 normal native 802.11
  put32(rx.data() + 4, 0);
  put32(rx.data() + 8, 0);
  rx[32] = 0x08;
  assert(parse_monitor_frame(rx.data(), rx.size(), frame) && frame.offset == 32 && frame.length == 36);
  assert(!frame.has_timestamp && frame.phy_mode == 0xff &&
         !frame.phy.width_mhz && !frame.phy.measured_rssi && !frame.phy.prxv0);
  assert(parse_concurrent_frame(rx.data(), rx.size(), 44, 80, frame) &&
         frame.phy_mode == 0xff && !frame.phy.width_mhz);
  put32(rx.data(), 0x10000034); // Short native Block Ack Request
  rx[32] = 0x84;
  assert(parse_monitor_frame(rx.data(), rx.size(), frame) && frame.length == 20);
  rx[32] = 0x08;
  assert(!parse_monitor_frame(rx.data(), rx.size(), frame));
  put32(rx.data(), 0x10000044);
  rx[8] |= 0x80; // Hardware header translation
  assert(!parse_monitor_frame(rx.data(), rx.size(), frame));
  put32(rx.data(), 0x30000008);
  assert(rx_kind(rx.data(), rx.size()) == RxKind::tx_free);
  put32(rx.data(), 0x38000034);
  assert(rx_kind(rx.data(), rx.size()) == RxKind::event);
  assert(!parse_monitor_frame(rx.data(), rx.size(), frame));
}

void verify_mcu() {
  std::array<uint8_t, 4096> tx{}, payload{};
  uint8_t seq = 15;
  assert(mt_mcu_encode(0x10, &seq, payload.data(), 4, tx.data(), static_cast<uint32_t>(tx.size())) == 68);
  assert(seq == 1 && tx[36] == 0x10 && tx[39] == 1);
  payload[0] = 0x5a;
  assert(mt_mcu_encode(MT_FW_SCATTER, &seq, payload.data(), static_cast<uint32_t>(payload.size()), tx.data(), static_cast<uint32_t>(tx.size())) == 4096);
  assert(tx == payload);
  assert(mt_mcu_encode(MT_MCU_UNI | MT_MCU_QUERY | 0x2d, &seq, payload.data(), 32,
                       tx.data(), static_cast<uint32_t>(tx.size())) == 80);
  assert(tx[34] == 0x2d && tx[39] == seq && tx[43] == 3);

  std::array<uint8_t, 52> reply{};
  put32(reply.data(), 0x38000029);
  reply[32] = 9; reply[34] = 7; reply[37] = seq; reply[40] = 2;
  uint32_t status = 99;
  assert(mt_mcu_response(0x10, seq, reply.data(), 41, &status) == MT_OK && status == 2);
  status = 99;
  assert(mt_mcu_response(0x10, seq + 1, reply.data(), 41, &status) == MT_PENDING && status == 99);
  put32(reply.data(), 0x38000034);
  reply[32] = 20; reply[34] = 0; reply[35] = 0x38; reply[36] = 1; reply[44] = 0x24;
  put32(reply.data() + 48, 0xc00000bbu);
  assert(mt_mcu_response(MT_MCU_UNI | 0x24, seq, reply.data(), static_cast<uint32_t>(reply.size()), &status) == MT_OK &&
         status == 0xc00000bbu);
  assert(mt_mcu_response(MT_MCU_UNI | 0x15, seq, reply.data(), static_cast<uint32_t>(reply.size()), &status) == MT_PENDING);
  reply[38] = 4;
  assert(mt_mcu_response(MT_MCU_UNI | 0x24, seq, reply.data(), static_cast<uint32_t>(reply.size()), &status) == MT_PENDING);
}

void verify_abort_wait() {
  constexpr uint32_t ack_bytes = 52;
  using Packet = std::array<uint8_t, ack_bytes>;
  Packet ack{};
  put32(ack.data(), 0x38000034);
  ack[32] = 20; ack[35] = 0x38; ack[36] = 1; ack[37] = 3; ack[44] = 0x27;
  const auto replay = [&](bool oversleep, bool has_ack, uint32_t firmware) {
    uint64_t lease_end = 4000, now = 0;
    unsigned submissions = 0, step = 0, peeks = 0;
    uint32_t status = UINT32_MAX;
    Packet reply = ack;
    put32(reply.data() + 48, firmware);
    const auto decode = [&](const uint8_t *p, uint32_t size) {
      return mt_mcu_response(MT_MCU_UNI | 0x27, 3, p, size, &status);
    };
    const bool ok = abort_roc_once(lease_end, [&](unsigned budget) {
      ++submissions;
      assert(budget == 1000);
      const auto receive = [&](bool &received) -> int {
        assert(!AwDlTxSlots::can_submit(now, lease_end));
        received = true;
        Packet unrelated = ack;
        if (step == 0) unrelated[37] = 2;       // Stale sequence.
        else if (step == 1) unrelated[38] = 4;  // Asynchronous ROC event.
        else if (step == 2) unrelated[44] = 2;  // Another command's ACK.
        else if (now >= 180 && has_ack) return decode(reply.data(), ack_bytes);
        else { received = false; return MT_PENDING; }
        ++step;
        return decode(unrelated.data(), ack_bytes);
      };
      const auto expired = [&] {
        assert(!AwDlTxSlots::can_submit(now, lease_end));
        ++peeks;
        // The thread can resume after the deadline with the ACK behind a
        // wrapped, split frame. Even ACK-shaped fragment bytes are ignored.
        return peek_mcu_reply(3, has_ack ? 2 : 1, 4, false,
            [&](unsigned i) -> Rx0Peek {
              if (i == 3) return {true, false, ack.data(), ack_bytes};
              if (i == 0) return {true, true, ack.data(), ack_bytes};
              assert(i == 1);
              return {true, true, reply.data(), ack_bytes};
            }, decode);
      };
      const int result = wait_mcu_reply(budget, [&] { return now; }, receive,
          [&] { now += oversleep ? 1400 : 1; }, expired);
      assert(result == (has_ack ? MT_OK : MT_FW_TIMEOUT));
      return result == MT_OK && status == 0;
    });
    assert(step == 3 && submissions == 1 && !lease_end && ok == (has_ack && !firmware));
    assert(peeks == unsigned(oversleep || !has_ack));
    assert(status == (has_ack ? firmware : UINT32_MAX));
    assert(!abort_roc_once(lease_end, [&](unsigned) { assert(false); return true; }));
  };
  replay(false, true, 0); // 180 ms of delayed receipt.
  replay(false, true, 0xc00000bb);
  replay(false, false, 0);
  replay(true, true, 0); // Accept only a reply already queued on resumption.
  replay(true, true, 0xc00000bb);
  replay(true, false, 0);
  const auto never_decode = [](const uint8_t *, uint32_t) { assert(false); return int(MT_OK); };
  assert(peek_mcu_reply(0, 1, 4, true,
      [&](unsigned) { return Rx0Peek{true, true, ack.data(), ack_bytes}; }, never_decode) == MT_FW_TIMEOUT);
  assert(peek_mcu_reply(0, 2, 4, false,
      [](unsigned i) { assert(i == 0); return Rx0Peek{}; }, never_decode) == MT_FW_TIMEOUT);
}

void verify_power_readback() {
  using namespace mtk::mt7925;
  static_assert(txpower_rate_count == 417);
  // sizeof(mt7925_mcu_rxd) + sizeof(mt7925_txpwr_event), from the upstream ABI.
  std::array<uint8_t, 893> packet{};
  const auto set16 = [&](unsigned at, unsigned value) {
    packet[at] = static_cast<uint8_t>(value);
    packet[at + 1] = static_cast<uint8_t>(value >> 8);
  };
  set16(32, 861);
  set16(34, 0x3800);
  packet[36] = 0x2a;
  packet[37] = 3;
  set16(50, 841);  // E138 firmware reports TLV body length, excluding tag + length.
  packet[52] = 5;
  packet[54] = 1;
  packet[55] = 1;
  std::fill_n(packet.data() + 56, 834, 127);
  TxPowerReadback info{};
  const auto decode = [&] { return decode_txpower_info(3, packet.data(), packet.size(), info); };
  assert(decode() == MT_OK && info.available == 0);
  packet[56] = 0x80;
  packet[57] = 126; // other PHY column
  packet[56 + 2 * 416] = 12;
  assert(decode() == MT_OK && info.available == 2 && info.lowest == -128 && info.highest == 12);
  assert(info.phy == 0 && info.channel_band == 1 && info.format == 1);
  packet[37] = 5;
  assert(decode() == MT_PENDING);
  packet[37] = 3;
  packet[38] = 4;
  assert(decode() == MT_PENDING);
  packet[38] = 0;
  packet[53] = 1;
  assert(decode() == MT_PENDING);
  packet[53] = 0;
  packet[52] = 2; // request and response have distinct category IDs
  assert(decode() == MT_PENDING);
  packet[52] = 5;
  set16(50, 840);
  assert(decode() == MT_INVALID && info.available == 2);
  set16(50, 842);
  assert(decode() == MT_INVALID);
  set16(50, 845);  // Incorrect header-inclusive interpretation.
  assert(decode() == MT_INVALID);
  set16(50, 841);
  assert(decode_txpower_info(3, packet.data(), packet.size() - 1, info) == MT_INVALID);
  packet[36] = 0x43;
  assert(decode() == MT_PENDING);
  packet[36] = 1;
  set16(32, 20);
  packet[44] = 0x2b;
  set16(50, 0);
  assert(decode() == MT_PENDING);
  packet[48] = 4;
  assert(decode() == MT_FW_PROTOCOL);
  packet[44] = 0x2c;
  assert(decode() == MT_PENDING && info.available == 2);
}

void verify_passive_scan() {
  std::array<uint8_t, LuoboPassiveScanBytes> request{};
  assert(!LuoboMt7925EncodePassiveScan(request.data(), 1, 4, 0));
  assert(LuoboMt7925EncodePassiveScan(request.data(), 1, LuoboWorldChannel44, 0));
  assert(request[8] == 0 && request[9] == 0);  // Passive, no probe transmissions
  constexpr size_t channel_tlv = 4 + 20 + 368 + 16;
  assert(request[channel_tlv + 5] == 1 && request[channel_tlv + 9] == 44);
  assert(!LuoboMt7925EncodePassiveScan(request.data(), 1, 3, 250));
  assert(LuoboMt7925EncodePassiveScan(request.data(), 1, LuoboWorldChannel44, 250));
  assert(request[8] == 0 && request[9] == 0 && request[10] == 0);
  assert(request[12] == 250 && request[13] == 0 && request[14] == 250 && request[15] == 0);
  assert(request[channel_tlv + 5] == 1 && request[channel_tlv + 9] == 44);

  std::array<uint8_t, 52> ack{};
  put32(ack.data(), 0x38000034);
  ack[32] = 20; ack[35] = 0x38; ack[36] = 1; ack[37] = 2; ack[44] = 0x16;
  std::array<uint8_t, 60> done{};
  put32(done.data(), 0x3800003c);
  done[32] = 28; done[35] = 0x38; done[36] = 0x0e; done[38] = 4;
  done[44] = 1; done[50] = 12; done[52] = 1; done[56] = 3;
  LUOBO_PASSIVE_SCAN scan{};
  assert(LuoboMt7925PassiveScanEvent(&scan, 2, 2, LuoboWorldChannel44,
                                   done.data(), static_cast<uint32_t>(done.size())) == MT_PENDING);
  assert(!scan.Done);
  assert(LuoboMt7925PassiveScanEvent(&scan, 1, 2, LuoboWorldChannel44,
                                   done.data(), static_cast<uint32_t>(done.size())) == MT_PENDING);
  assert(scan.Done && scan.CompleteChannels == 1 && scan.BeaconCount == 3);
  assert(LuoboMt7925PassiveScanEvent(&scan, 1, 2, LuoboWorldChannel44,
                                    ack.data(), static_cast<uint32_t>(ack.size())) == MT_OK);
}

void verify_active_wire() {
  const auto request = roc_request(44, 5);
  assert(request[8] == 1 && request[9] == 5 && request[10] == 44 && request[19] == 1);
  const auto abort = roc_abort(5);
  assert(abort[4] == 1 && abort[8] == 1 && abort[9] == 5 && abort[10] == 0xff);
  std::array<uint8_t, 68> event{};
  put32(event.data(), 0x38000044);
  event[32] = 36; event[35] = 0x38; event[36] = 0x27; event[38] = 4;
  uint8_t *grant = event.data() + 48;
  grant[2] = 20; grant[4] = 1; grant[5] = 5; grant[7] = 44;
  grant[9] = 2; grant[11] = 44; grant[13] = 1; grant[16] = 110;
  uint32_t duration = 0;
  assert(roc_grant(event.data(), static_cast<uint32_t>(event.size()), 44, 4, 1, duration) == Grant::pending && !duration);
  assert(roc_grant(event.data(), static_cast<uint32_t>(event.size()), 36, 5, 1, duration) == Grant::invalid && !duration);
  assert(roc_grant(event.data(), static_cast<uint32_t>(event.size()), 44, 5, 1, duration) == Grant::granted && duration == 110);
  put32(grant + 16, roc_visit_ms + 1);
  assert(roc_grant(event.data(), static_cast<uint32_t>(event.size()), 44, 5, 1, duration) == Grant::invalid);
  const auto long_request = roc_request(44, 5, 1, awdl_bss, roc_max_awdl_ms);
  assert(long_request[20] == 0xa0 && long_request[21] == 0x0f);
  put32(grant + 16, roc_max_awdl_ms);
  assert(roc_grant(event.data(), static_cast<uint32_t>(event.size()), 44, 5, 1, duration,
                   awdl_bss, roc_max_awdl_ms) == Grant::granted && duration == roc_max_awdl_ms);
  put32(grant + 16, roc_max_awdl_ms + 1);
  assert(roc_grant(event.data(), static_cast<uint32_t>(event.size()), 44, 5, 1, duration,
                   awdl_bss, roc_max_awdl_ms) == Grant::invalid);

  // 149 is the lowest 20 MHz constituent of the 80 MHz block centered at 155.
  const auto upper_request = roc_request(149, 5, 1, awdl_bss, roc_visit_ms, 80);
  assert(upper_request[10] == 149 && upper_request[11] == 1 && upper_request[12] == 2);
  assert(upper_request[13] == 1 && upper_request[14] == 155 && upper_request[16] == 1 &&
         upper_request[17] == 155 && rlm_sco_5g(149, 155) == 1);
  grant[7] = 149; grant[8] = 1; grant[10] = 1; grant[11] = 155;
  put32(grant + 16, roc_visit_ms);
  const auto upper_grant = [&] {
    return roc_grant(event.data(), static_cast<uint32_t>(event.size()), 149, 5, 1, duration,
                     awdl_bss, roc_visit_ms, 80);
  };
  assert(upper_grant() == Grant::granted && duration == roc_visit_ms);
  grant[7] = 44;
  assert(upper_grant() == Grant::invalid);
  grant[7] = 149; grant[11] = 151;
  assert(upper_grant() == Grant::invalid);
  grant[11] = 155; grant[10] = 0;
  assert(upper_grant() == Grant::invalid);
  grant[10] = 1; grant[8] = 3;
  assert(upper_grant() == Grant::invalid);

  std::array<uint8_t, 128> page{};
  std::array<uint8_t, 36> frame{};
  std::copy(awdl_bssid.begin(), awdl_bssid.end(), frame.begin() + 16);
  frame[0] = 0xd0; frame[4] = 0xff; frame[24] = 0x7f; frame[26] = 0x17; frame[27] = 0xf2; frame[28] = 8;
  assert(write_awdl_txwi(page.data(), page.size(), 0x4000, 3, frame.data(), frame.size()));
  assert(rx_word(page.data() + 32) == 0x8003 && rx_word(page.data() + 40) == 0x4040);
  assert(rx_word(page.data() + 44) == 0x8024 && (page[12] & 1) && page[64] == 0xd0);
  assert(!write_awdl_txwi(page.data(), page.size(), 0xffffffd0, 3, frame.data(), frame.size()));
  frame[1] = 0x40;
  assert(!write_awdl_txwi(page.data(), page.size(), 0x4000, 3, frame.data(), frame.size()));
  std::array<uint8_t, 160> data_page{};
  std::array<uint8_t, 82> data{};
  data[0] = 0x88;
  data[24] = 5;
  std::copy(awdl_bssid.begin(), awdl_bssid.end(), data.begin() + 16);
  constexpr uint8_t body[17]{0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0, 3, 4, 0, 0, 0, 0, 0x86, 0xdd, 0x60};
  std::copy(std::begin(body), std::end(body), data.begin() + 26);
  assert(write_awdl_txwi(data_page.data(), data_page.size(), 0x4000, 4, data.data(), data.size()));
  assert(((rx_word(data_page.data() + 4) >> 21) & 15) == 5 && data_page[8] == 0x28);
  assert((data_page[12] & 0x11) == 0);  // unicast requests an ACK
  assert(write_awdl_txwi(data_page.data(), data_page.size(), 0x4000, 4, data.data(), data.size(), AwDlRate::ht20));
  assert(((rx_word(data_page.data() + 24) >> 16) & 0x3fu) == 26);
  assert(rx_word(data_page.data() + 24) & (1u << 25)); // 20 MHz fixed bandwidth
  assert(!write_awdl_txwi(data_page.data(), data_page.size(), 0x4000, 4, data.data(), data.size() - 1));
  std::array<uint8_t, 96> aggregate{};
  std::copy_n(data.begin(), 26, aggregate.begin());
  aggregate[24] |= 0x80;
  aggregate[39] = 56;
  std::copy(data.begin() + 26, data.end(), aggregate.begin() + 40);
  assert(valid_awdl_rx(aggregate.data(), aggregate.size()));
  assert(!awdl_header_size(aggregate.data(), aggregate.size()));
  assert(!valid_awdl_rx(aggregate.data(), aggregate.size() - 1));
  std::vector<uint8_t> htc(aggregate.begin(), aggregate.end());
  htc[1] |= 0x80;
  htc.insert(htc.begin() + 26, 4, 0);
  assert(valid_awdl_rx(htc.data(), htc.size()));
  assert(!valid_awdl_rx(htc.data(), htc.size() - 1));
  aggregate[39] = 57;
  assert(!valid_awdl_rx(aggregate.data(), aggregate.size()));

  std::array<uint8_t, 90> mdns_frame{};
  std::copy(data.begin(), data.end(), mdns_frame.begin());
  mdns_frame[48] = 17;
  mdns_frame[84] = 0x14;
  mdns_frame[85] = 0xe9;
  assert(awdl_mdns(mdns_frame.data(), mdns_frame.size(), 26));
  assert(!awdl_mdns(mdns_frame.data(), mdns_frame.size() - 1, 26));
  mdns_frame[48] = 6;
  assert(!awdl_mdns(mdns_frame.data(), mdns_frame.size(), 26));
  data[16] ^= 1;
  assert(!write_awdl_txwi(data_page.data(), data_page.size(), 0x4000, 4, data.data(), data.size()));
}

void verify_awdl_rates() {
  using namespace mtk::mt7925;
  std::array<uint8_t, 57> sync{};
  sync[0] = 0xd0;
  std::copy(awdl_bssid.begin(), awdl_bssid.end(), sync.begin() + 16);
  const uint8_t vendor[]{0x7f, 0, 0x17, 0xf2, 8, 0x10, 0, 0};
  std::copy(std::begin(vendor), std::end(vendor), sync.begin() + 24);
  sync[40] = 17; sync[41] = 14; sync[43] = 191; sync[44] = 12;
  sync[49] = 0xfa; sync[50] = 0xff;
  assert(awdl_peer_rate(sync.data(), sync.size()) == AwDlRate::vht20_mcs8);
  sync[49] = 0xfe;
  assert(awdl_peer_rate(sync.data(), sync.size()) == AwDlRate::vht20_mcs8);
  assert(awdl_peer_rate(sync.data(), sync.size() - 1) == AwDlRate::ofdm6);
  sync[40] = 7; sync[41] = 8; sync[48] = 0xff;
  assert(awdl_peer_rate(sync.data(), 51) == AwDlRate::ht20);

  std::array<uint8_t, 160> page{};
  assert(write_awdl_txwi(page.data(), page.size(), 0x4000, 1, sync.data(), 51, AwDlRate::vht20));
  assert((rx_word(page.data() + 24) >> 16) == 18); // Discovery always uses OFDM 6.
}

void verify_awdl_amsdu() {
  using namespace mtk::mt7925;
  std::array<uint8_t, 1511> first{}, second{};
  first[0] = 0x08;
  first[4] = first[10] = 2;
  first[5] = 4; first[11] = 8;
  first[22] = 0x10;
  std::copy(awdl_bssid.begin(), awdl_bssid.end(), first.begin() + 16);
  constexpr uint8_t payload[]{0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0, 3, 4, 0, 0, 0, 0, 0x86, 0xdd, 0x60};
  std::copy(std::begin(payload), std::end(payload), first.begin() + 24);
  first.back() = 0xa5;
  second = first;
  second[22] = 0x20;
  second.back() = 0x5a;
  std::array<uint8_t, 4096> out{}, page{};
  const size_t joined = build_awdl_amsdu(out.data(), out.size(), first.data(), first.size(),
                                         second.data(), second.size());
  const size_t second_header = 26 + 1504;
  assert(joined == second_header + 14 + 1487);
  assert(out[0] == 0x88 && out[22] == 0x10 && out[24] == 0x80);
  assert(out[26 + 12] == 5 && out[26 + 13] == 207);
  assert(out[second_header - 4] == 0xa5 && out[second_header - 3] == 0 && out[joined - 1] == 0x5a);
  assert(valid_awdl_rx(out.data(), joined));
  // A complete over-MTU MPDU crosses RXD unchanged, so OWL sees both original
  // MSDUs. Isolated fragments from an unexpectedly re-enabled deaggregator
  // cannot be delivered as if they were complete, independently numbered MPDUs.
  std::vector<uint8_t> rxd(32 + joined);
  put32(rxd.data(), 0x10000000u | static_cast<uint32_t>(rxd.size()));
  put32(rxd.data() + 12, 42u << 8);
  std::copy_n(out.begin(), joined, rxd.begin() + 32);
  MonitorFrame received{};
  assert(parse_monitor_frame(rxd.data(), rxd.size(), received, 44));
  assert(received.channel == 44 && received.length == joined &&
         std::equal(out.begin(), out.begin() + joined, rxd.begin() + received.offset));
  assert(valid_awdl_rx(rxd.data() + received.offset, received.length));
  for (uint8_t reported : {149, 151, 153, 155, 157, 161}) {
    put32(rxd.data() + 12, uint32_t(reported) << 8);
    assert(parse_monitor_frame(rxd.data(), rxd.size(), received, 149) && received.channel == 149);
    assert(!parse_monitor_frame(rxd.data(), rxd.size(), received, 44));
  }
  for (uint8_t outside : {44, 144, 148, 150, 152, 154, 156, 160, 165}) {
    put32(rxd.data() + 12, uint32_t(outside) << 8);
    assert(!parse_monitor_frame(rxd.data(), rxd.size(), received, 149));
  }
  put32(rxd.data() + 12, 42u << 8);
  for (uint8_t format : {1, 2, 3}) {
    rxd[16] = format;
    assert(!parse_monitor_frame(rxd.data(), rxd.size(), received, 44));
  }

  assert(!write_awdl_txwi(page.data(), page.size(), 0x4000, 1, out.data(), joined));
  assert(write_awdl_txwi(page.data(), page.size(), 0x4000, 1, out.data(), joined, AwDlRate::ht20, AwDlTxFormat::amsdu));
  assert(rx_word(page.data() + 44) == (0x8000u | joined));
  assert(!write_awdl_txwi(page.data(), page.size(), 0x4000, 1, out.data(), joined - 1, AwDlRate::ht20, AwDlTxFormat::amsdu));
  second[5] ^= 1;
  assert(!build_awdl_amsdu(out.data(), out.size(), first.data(), first.size(), second.data(), second.size()));
}

void verify_awdl_tx_slots() {
  using mtk::mt7925::AwDlTxKind;
  using mtk::mt7925::AwDlTxSlots;
  AwDlTxSlots slots;
  const auto first = slots.next_token();
  slots.prepare(first);
  const auto second = slots.next_token();
  slots.prepare(second, AwDlTxKind::aggregate);
  assert(first != second && slots.pending() == 2);
  assert(slots.available_on_channel(124) == 0);
  assert(slots.available_on_channel(126) == 1);
  assert(slots.available_on_channel(1000) == AwDlTxSlots::max_inflight - 2);
  assert(!slots.complete(first)); // Not visible to hardware until a doorbell.
  slots.publish(100);
  assert(!slots.observe_fetch(3)); // DIDX cannot pass the published head.
  AwDlTxKind kind{};
  assert(slots.complete(second, &kind) && kind == AwDlTxKind::aggregate);
  assert(!slots.complete(second));
  assert(slots.observe_fetch(2));
  assert(slots.reclaim() == 0); // Later TX_FREE does not release an earlier page.
  assert(slots.complete(first));
  assert(slots.reclaim() == 2 && !slots.pending());

  // Reproduce a fully fetched burst with one late notification at expiry.
  // A newly prepared frame must stay private until the replacement grant.
  for (const uint64_t lease_end : {1000u, 4000u}) {
    AwDlTxSlots late;
    const auto wait_for = late.next_token();
    late.prepare(wait_for);
    const auto next = late.next_token();
    late.prepare(next);
    late.publish(7);
    const auto held = late.next_token();
    late.prepare(held);
    assert(!late.can_release_for_renewal(lease_end, lease_end)); // Not fetched.
    assert(late.observe_fetch(2) && late.complete(next));
    assert(late.reclaim() == 0 && late.oldest_age(lease_end) == lease_end - 7);
    assert(!late.can_release_for_renewal(lease_end - 1, lease_end));
    assert(late.can_release_for_renewal(lease_end, lease_end));
    assert(late.published_pending() == 2 && !late.complete(held));
    assert(!late.can_submit(lease_end - AwDlTxSlots::guard_ms, lease_end));
    assert(!late.can_submit(lease_end, lease_end) && !late.can_submit(lease_end, 0));
    assert(late.can_submit(lease_end + 1, lease_end + 1000));
    late.publish(lease_end + 1);
    assert(late.complete(held) && late.observe_fetch(3));
    assert(late.reclaim() == 0 && late.complete(wait_for) && late.reclaim() == 3);
    assert(!late.complete(wait_for) && !late.pending() && late.oldest_age(lease_end + 2) == 0);
  }

  for (unsigned n = 0; n < AwDlTxSlots::capacity * 2; ++n) {
    const auto token = slots.next_token();
    assert(token != first && token != second);
    slots.prepare(token);
    slots.publish(100);
    assert(slots.complete(token));
    assert(slots.reclaim() == 0); // A TX_FREE alone cannot release the descriptor.
    assert(slots.observe_fetch(slots.head()));
    assert(slots.reclaim() == 1);
  }
  for (unsigned n = 0; n < AwDlTxSlots::max_inflight; ++n) slots.prepare(slots.next_token());
  assert(slots.available_on_channel(1000) == 0);
}

void verify_awdl_rx_ba_wire() {
  using namespace mtk::mt7925;
  const uint8_t peer[]{2, 1, 2, 3, 4, 5};
  const auto station = awdl_ba_station(peer);
  assert(station[0] == awdl_bss && station[1] == awdl_ba_wcid && station[2] == 6);
  assert(station[4] == 1 && station[5] == awdl_bss);
  assert(station[16] == 2 && station[17] == 1 && !std::memcmp(station.data() + 20, peer, 6));
  const auto add = awdl_ba_command(3, 0xabc, 64, true, AwDlBaDirection::rx);
  assert(add[8] == 6 && add[10] == 16 && add[12] == 3 && add[13] == 2 && add[15] == 8);
  assert(add[16] == 0xbc && add[17] == 0x0a && add[18] == 64);
  assert(add[14] == 0);
  assert(awdl_ba_command(3, 0, 64, true, AwDlBaDirection::rx, true)[14] == 1);
  assert(awdl_ba_command(3, 0, 0, false, AwDlBaDirection::rx, true)[14] == 0);
  assert(awdl_ba_command(3, 0, 0, false, AwDlBaDirection::rx)[15] == 0);
  assert(awdl_remove_ba_station()[8] == 0x25);

  std::array<uint8_t, 33> response{};
  response[0] = 0xd0;
  std::copy_n(peer, 6, response.data() + 4);
  std::copy(awdl_bssid.begin(), awdl_bssid.end(), response.begin() + 16);
  response[24] = 3; response[25] = 1;
  std::array<uint8_t, 128> page{};
  assert(!write_awdl_txwi(page.data(), page.size(), 0x4000, 1, response.data(), response.size()));
  assert(write_awdl_txwi(page.data(), page.size(), 0x4000, 1, response.data(), response.size(),
                        AwDlRate::ofdm6, AwDlTxFormat::block_ack_action));
  assert((rx_word(page.data() + 4) & 0xfffu) == awdl_ba_wcid);
  response[25] = 0;
  assert(write_awdl_txwi(page.data(), page.size(), 0x4000, 1, response.data(), response.size(),
                        AwDlRate::ofdm6, AwDlTxFormat::block_ack_action));
  assert(((rx_word(page.data() + 4) >> 21) & 0xfu) == 2);
  response[25] = 1;
  response[27] = 1;
  assert(!write_awdl_txwi(page.data(), page.size(), 0x4000, 1, response.data(), response.size(),
                         AwDlRate::ofdm6, AwDlTxFormat::block_ack_action));
}

void verify_awdl_tx_ba_wire() {
  const auto add = awdl_ba_command(0, 0x321, 32, true, AwDlBaDirection::tx);
  assert(add[13] == 1 && add[15] == 1 && add[16] == 0x21 && add[17] == 3 && add[18] == 32);
  std::array<uint8_t, 82> frame{};
  frame[0] = 0x88;
  frame[4] = 2;
  std::copy(awdl_bssid.begin(), awdl_bssid.end(), frame.begin() + 16);
  const uint8_t payload[]{0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0, 3, 4, 0, 0, 0, 0, 0x86, 0xdd, 0x60};
  std::copy(std::begin(payload), std::end(payload), frame.begin() + 26);
  std::array<uint8_t, 160> page{};
  assert(write_awdl_txwi(page.data(), page.size(), 0x4000, 9, frame.data(), frame.size(),
                        AwDlRate::ofdm6, AwDlTxFormat::ampdu));
  assert((rx_word(page.data()) >> 25) == 5); // WMM set 1, best effort
  assert((rx_word(page.data() + 4) & 0x80000fffu) == awdl_ba_wcid); // Auto rate, station WCID
  assert(!(rx_word(page.data() + 12) & (1u << 28)));
  assert(rx_word(page.data() + 24) == 0x1c);
  assert(write_awdl_txwi(page.data(), page.size(), 0x4000, 9, frame.data(), frame.size(),
                        AwDlRate::ofdm24, AwDlTxFormat::peer_data));
  assert((rx_word(page.data()) >> 25) == 5);
  assert((rx_word(page.data() + 4) & 0x80000fffu) == (0x80000000u | awdl_ba_wcid));
  assert((rx_word(page.data() + 12) & ((1u << 28) | 1u)) == (1u << 28)); // BA disabled, ACK required
  assert(rx_word(page.data() + 24) == ((uint32_t(AwDlRate::ofdm24) << 16) | 0x1c));
  frame[24] = 1;
  assert(!write_awdl_txwi(page.data(), page.size(), 0x4000, 9, frame.data(), frame.size(),
                         AwDlRate::ofdm6, AwDlTxFormat::ampdu));

  const uint8_t peer[]{2, 8, 7, 6, 5, 4}, other[]{2, 8, 7, 6, 5, 1};
  AwDlTxBaFlow flow;
  for (unsigned i = 0; i < 8; ++i) flow.note_bulk(peer, 100, 1000 + i);
  assert(flow.phase == AwDlTxBaFlow::Phase::awaiting && flow.next_seq == 100);
  assert(!flow.accept(peer, flow.token, 0, 0, 64, true, 1010)); // No request sent yet.
  ++flow.requests;
  assert(!flow.accept(other, flow.token, 0, 0, 64, true, 1011));
  assert(!flow.accept(peer, flow.token, 1, 0, 64, true, 1012));
  assert(flow.accept(peer, flow.token, 0, 0, 64, true, 1013));
  assert(flow.phase == AwDlTxBaFlow::Phase::agreed && flow.window == 64);

  const auto wide_req = roc_request(44, 7, 1, awdl_bss, roc_max_awdl_ms, 80);
  assert(wide_req[10] == 44 && wide_req[11] == 1 && wide_req[13] == 1 && wide_req[14] == 42 &&
         wide_req[16] == 1 && wide_req[17] == 42);
  const auto sta80 = awdl_ba_station_vht80(peer, 2);
  assert(sta80[0] == awdl_bss && sta80[1] == awdl_ba_wcid && sta80[2] == 8);
  const auto sta80_1ss = awdl_ba_station_vht80(peer, 1);
  // VHT TLV starts at byte 48: the peer can receive one stream from this host.
  assert(sta80[56] == 0xfe && sta80[58] == 0xfa);
  assert(sta80_1ss[56] == 0xfe && sta80_1ss[58] == 0xfe);
  assert(awdl_remove_ba_station_mld()[2] == 2 && awdl_remove_ba_station_mld()[16] == 0x23);
  flow.finish(1020, 1000);
  flow.note_bulk(peer, 200, 1021);
  assert(flow.phase == AwDlTxBaFlow::Phase::idle && !flow.bulk_frames);
  for (unsigned i = 0; i < 8; ++i) flow.note_bulk(peer, 0x1fff, 3000 + i);
  assert(flow.phase == AwDlTxBaFlow::Phase::awaiting && flow.next_seq == 0x0fff);
  ++flow.requests;
  assert(!flow.accept(peer, flow.token, 0, 37, 32, true, 3010));
  assert(flow.phase == AwDlTxBaFlow::Phase::idle && flow.retry_after > 3010);
}

struct FirmwareMock {
  unsigned sem_calls = 0;
  unsigned scatter_calls = 0;
  unsigned ready_state = 0;
  unsigned clc_calls = 0;
  static int send(void *context, uint32_t command, const uint8_t *data,
                  uint32_t bytes, int wait, uint32_t *status) {
    auto &mock = *static_cast<FirmwareMock *>(context);
    *status = 0;
    if (command == 0x10) {
      assert(wait && bytes == 4);
      *status = data[0] ? 2 : 3;
      ++mock.sem_calls;
    } else if (command == MT_FW_SCATTER) {
      assert(!wait && bytes > 0 && bytes <= MT_FW_CHUNK);
      ++mock.scatter_calls;
    } else if (command == (MT_MCU_UNI | 0x2cu)) {
      assert(wait && bytes < 4096 - 48 && data[4] == 3);
      ++mock.clc_calls;
    }
    return MT_OK;
  }
};

std::vector<uint8_t> read(const char *path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void verify_firmware_files(const char *patch_file, const char *ram_file) {
  auto patch = read(patch_file);
  auto ram = read(ram_file);
  assert(!patch.empty() && !ram.empty());
  FirmwareMock mock;
  mt_fw_transport transport{&mock, FirmwareMock::send,
    [](void *) { return static_cast<int>(MT_OK); },
    [](void *context, uint32_t state) {
      auto &m = *static_cast<FirmwareMock *>(context);
      assert(state == m.ready_state++);
      return static_cast<int>(MT_OK);
    }
  };
  mt_fw_result boot{};
  assert(mt_fw_boot(patch.data(), static_cast<uint32_t>(patch.size()), ram.data(),
                    static_cast<uint32_t>(ram.size()), &transport, &boot) == MT_OK);
  assert(boot.start_acked && boot.ready && !boot.release_error && mock.ready_state == 2);
  assert(mock.sem_calls == 2 && mock.scatter_calls > 300);
  std::array<uint8_t, 4096 - 48> scratch{};
  const uint8_t world[2] = {'0', '0'};
  for (uint32_t variant = 0; variant < 2; ++variant) {
    const int bytes = mt_regd_request(ram.data(), static_cast<uint32_t>(ram.size()),
                                     variant, world, scratch.data(), static_cast<uint32_t>(scratch.size()));
    assert(bytes > 88 && scratch[4] == 6 && scratch[16] == '0' && scratch[17] == '0');
    mt_clc_result result{};
    assert(mt_clc_power_apply(&transport, ram.data(), static_cast<uint32_t>(ram.size()),
                             variant, world, 0, scratch.data(), static_cast<uint32_t>(scratch.size()), &result) == MT_OK);
    assert(result.selected_mask == 3 && result.fallback_mask == 0 && result.requests_sent == 12);
  }
  assert(mock.clc_calls == 24);
  // The pinned E138 image must contain its own CN indoor power tables. The
  // frequencies themselves are only decoded by the running firmware.
  const uint8_t cn[2] = {'C', 'N'};
  const auto size = static_cast<uint32_t>(ram.size());
  const auto room = static_cast<uint32_t>(scratch.size());
  assert(mt_regd_request(ram.data(), size, 0, cn, scratch.data(), room) > 88);
  assert(scratch[16] == 'C' && scratch[17] == 'N');
  mt_clc_result cn_clc{};
  assert(mt_clc_power_apply(&transport, ram.data(), size, 0, cn, 1, scratch.data(), room, &cn_clc) == MT_OK);
  assert(cn_clc.selected_mask == 3 && cn_clc.fallback_mask == 0 && cn_clc.requests_sent > 0);
}

} // namespace

int main(int argc, char **argv) {
  {
    std::array<uint8_t, 72> old{}, fresh{};
    uint8_t *a = old.data() + 32, *b = fresh.data() + 32;
    a[12] = b[12] = 0x50;
    a[13] = b[13] = 0x10;
    a[14] = b[14] = 1;
    b[10] = 4;
    assert(mtk::mt7925::advances_upload_ack(a, b));
    assert(!mtk::mt7925::advances_upload_ack(a, a));
    assert(!mtk::mt7925::advances_upload_ack(b, a));
    b[9] = 1;
    assert(mtk::mt7925::advances_upload_ack(a, b));
    b[9] = 4;
    assert(!mtk::mt7925::advances_upload_ack(a, b));
    b[9] = 0;
    b[13] |= 0x40;
    assert(!mtk::mt7925::advances_upload_ack(a, b));
    b[13] = a[13];
    b[14] = 0;
    assert(!mtk::mt7925::advances_upload_ack(a, b));
    b[14] = a[14];
    b[-1] = 1;
    assert(!mtk::mt7925::advances_upload_ack(a, b));
  }
  verify_rx();
  verify_mcu();
  verify_abort_wait();
  verify_passive_scan();
  verify_power_readback();
  verify_active_wire();
  verify_awdl_rates();
  verify_mt7925_ba_cases();
  verify_mt7925_link_cases();
  verify_mt7925_rate_cases();
  verify_awdl_amsdu();
  verify_awdl_tx_slots();
  verify_awdl_rx_ba_wire();
  verify_awdl_tx_ba_wire();
  if (argc == 3) verify_firmware_files(argv[1], argv[2]);
  std::cout << "MT7925 transport and RX tests passed" << std::endl;
}
