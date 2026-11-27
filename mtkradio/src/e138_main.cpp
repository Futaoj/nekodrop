// SPDX-License-Identifier: GPL-2.0-only
// Dedicated E138 FastBa radio.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "mt7925_upload_ack.h"
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "awdl_policy.h"
#include "awdl_schedule.h"
#include "mtk_dev.h"
#include "mt7925_active.h"
#include "mt7925_link.h"
#include "mt7925_rx_stats.h"
#include "pipe_server.h"
#include "plat.h"
#include "radio_link.h"
#include "rx_ba_reorder.h"

namespace {

std::atomic<bool> g_stop{false};
bool stopping() { return g_stop.load(); }

BOOL WINAPI on_console(DWORD) {
  g_stop = true;
  return TRUE;
}

void log_line(const std::string &line) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(stderr, "%02u:%02u:%02u.%03u mtkradio: %s\n", t.wHour, t.wMinute, t.wSecond,
               t.wMilliseconds, line.c_str());
  std::fflush(stderr);
}

uint16_t channel_to_mhz(uint8_t ch) {
  if (ch >= 1 && ch <= 13) return static_cast<uint16_t>(2407 + ch * 5);
  if (ch == 14) return 2484;
  return static_cast<uint16_t>(5000 + ch * 5);
}

/* Radiotap flags (no trailing FCS), channel and signal for awdld. */
#pragma pack(push, 1)
struct MinimalRadiotap {
  uint8_t version;
  uint8_t pad;
  uint16_t length;
  uint32_t present;
  uint8_t rx_flags;
  uint8_t channel_pad;
  uint16_t freq_mhz;
  uint16_t flags;
  int8_t dbm;
  uint8_t pad2;
};
#pragma pack(pop)
static_assert(sizeof(MinimalRadiotap) == 16 && offsetof(MinimalRadiotap, freq_mhz) == 10);

void append_rx_wire(const mtk::RxFrame &f, std::vector<uint8_t> &batch) {
  MinimalRadiotap rt{};
  rt.version = 0;
  rt.length = sizeof(rt);
  rt.present = (1u << 1) | (1u << 3) | (1u << 5);
  rt.freq_mhz = channel_to_mhz(f.channel);
  rt.flags = (f.channel <= 14) ? 0x00a0u : 0x0140u; /* 2 GHz CCK/OFDM vs 5 GHz OFDM */
  rt.dbm = f.dbm;

  const radio_link_header hdr{static_cast<uint16_t>(RADIO_LINK_RX), 0,
                              static_cast<uint32_t>(sizeof(rt) + f.len)};
  const size_t old_sz = batch.size();
  batch.resize(old_sz + sizeof(hdr) + sizeof(rt) + f.len);
  uint8_t *dst = batch.data() + old_sz;
  std::memcpy(dst, &hdr, sizeof(hdr));
  std::memcpy(dst + sizeof(hdr), &rt, sizeof(rt));
  std::memcpy(dst + sizeof(hdr) + sizeof(rt), f.data, f.len);
}

bool is_awdl_bssid(const uint8_t *frame, size_t len) {
  if (len < 22) return false;
  static constexpr uint8_t kAwdl[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
  return std::memcmp(frame + 16, kAwdl, 6) == 0;
}

struct PendingCmd {
  uint16_t type = 0;
  std::vector<uint8_t> payload;
  uint64_t at_us = 0;
};

bool run_mt7925_awdl_link(mtk::MtkDevice &dev, std::string &error) {
  using mtk::mt7925::AwDlBaDirection;
  using mtk::mt7925::AwDlRate;
  using mtk::mt7925::upload_ack;
  using mtk::mt7925::advances_upload_ack;
  using mtk::mt7925::AwDlTxBaFlow;
  using mtk::mt7925::AwDlTxFormat;
  using BaPhase = AwDlTxBaFlow::Phase;
  const uint8_t channel = dev.mt7925_awdl_channel();
  constexpr bool fast = true, rx_ba = true, tx_ba = true;
  constexpr uint16_t lease_ms = mtk::mt7925::roc_max_awdl_ms;
  std::atomic<bool> link_done{false};
  const auto done = [&] { return stopping() || link_done; };
  PipeServer pipe(RADIO_LINK_PIPE_NAME, done, 1u << 19);
  if (!pipe.valid()) {
    error = "could not create the AWDL radio pipe";
    return false;
  }

  constexpr unsigned control_reserve = 8, data_limit = 2048;
  constexpr uint64_t spin_window_us = 5000;
  constexpr unsigned tx_batch_limit = 32;

  struct QueuedTx {
    PendingCmd command;
    awdl::UnicastRetry retry;
  };
  const auto frame_in = [](const PendingCmd &cmd) -> std::span<const uint8_t> {
    if (cmd.type != RADIO_LINK_TX || cmd.payload.size() < 8 || cmd.payload[0]) return {};
    const size_t rt = unsigned(cmd.payload[2]) | (unsigned(cmd.payload[3]) << 8);
    if (rt < 8 || rt >= cmd.payload.size()) return {};
    const auto frame = std::span<const uint8_t>(cmd.payload).subspan(rt);
    return mtk::mt7925::awdl_header_size(frame.data(), frame.size()) ? frame : std::span<const uint8_t>{};
  };
  std::mutex mutex;
  std::deque<PendingCmd> commands;
  std::deque<QueuedTx> controls, payloads;
  unsigned pipe_drops = 0;
  std::atomic_uint64_t ack_offered{}, ack_replaced{};
  uint64_t ack_sent = 0, ack_ok = 0, ack_failed = 0, last_offered = 0;
  uint32_t first_ack = 0, latest_ack = 0;
  unsigned last_window = 0, last_peer_port = 0;
  const auto enqueue = [&](PendingCmd cmd) {
    std::lock_guard<std::mutex> lock(mutex);
    if (cmd.type != RADIO_LINK_TX) {
      if (commands.size() < 32) commands.push_back(std::move(cmd));
      else ++pipe_drops;
      return;
    }
    const auto frame = frame_in(cmd);
    if (upload_ack(frame)) ++ack_offered;
    const bool data = mtk::mt7925::awdl_tx_lane(frame.data(), frame.size()) == mtk::mt7925::AwDlTxLane::data &&
                      !mtk::mt7925::awdl_mdns(frame.data(), frame.size(), frame[0] == 0x88 ? 26 : 24);
    auto &lane = data ? payloads : controls;
    if (data && lane.size() >= 3 && !lane.back().retry.attempts) {
      const auto *fresh = upload_ack(frame);
      if (fresh) {
        const auto *tail = upload_ack(frame_in(lane.back().command));
        const auto *previous = upload_ack(frame_in(lane[lane.size()-2].command));
        if (advances_upload_ack(tail, fresh) && advances_upload_ack(previous, tail) &&
            advances_upload_ack(previous, fresh)) {
          lane.back().command = std::move(cmd);
          ++ack_replaced;
          return;
        }
      }
    }
    if (lane.size() < (data ? data_limit : 128u)) {
      lane.push_back({std::move(cmd), {}});
    } else {
      ++pipe_drops;
    }
  };
  std::thread reader([&] {
    if (pipe.wait_for_client()) {
      const radio_link_hello hello{RADIO_LINK_VERSION, channel, static_cast<uint8_t>(dev.width_mhz()),
                                   {pipe.shm_cap_flag(), 0x25}};
      if (pipe.send(RADIO_LINK_HELLO, {reinterpret_cast<const uint8_t *>(&hello), sizeof(hello)})) {
        while (!done() && pipe.connected()) {
          radio_link_header hdr{};
          if (!pipe.read_exact(&hdr, sizeof(hdr)) || hdr.length > RADIO_LINK_MAX_PAYLOAD) break;
          PendingCmd cmd{hdr.type, std::vector<uint8_t>(hdr.length), now_us()};
          if (hdr.length && !pipe.read_exact(cmd.payload.data(), hdr.length)) break;
          enqueue(std::move(cmd));
        }
      }
    }
    link_done = true;
  });

  log_line("MT7925 VHT80 RX streams=" + std::to_string(dev.mt7925_rx_streams()));
  log_line("MT7925 channel " + std::to_string(channel) + " AWDL pipe ready");
  std::array<uint8_t, 6> local{};
  bool enabled = false, ok = true;
  unsigned actions = 0, data = 0, rejected = 0, failures = 0, received = 0;
  unsigned seen_awdl = 0, admitted_data = 0, mdns_rx = 0, mdns_tx = 0, max_batch = 0;
  uint64_t native_amsdu = 0;
  uint64_t unicast_data = 0, ampdu_members = 0, peer_retries = 0;
  uint64_t fresh_ack_count = 0, ack_queue_us = 0, ack_over_2ms = 0;
  uint64_t station_ack_ok = 0, station_ack_failed = 0;
  mtk::RxBaReorder ba_reorder;
  const auto report_rx = [&] {
    const auto &ring = dev.mt7925_rx_ring_snapshot();
    log_line("MT7925 RX: awdl_data=" + std::to_string(admitted_data) +
             " rx2_slots=" + std::to_string(dev.mt7925_rx_ring_size()) +
             " rx2_peak=" + std::to_string(ring.peak()) +
             " rx2_full_samples=" + std::to_string(ring.full_samples()) +
             " awdl_native_amsdu=" + std::to_string(native_amsdu) +
             " unicast_data=" + std::to_string(unicast_data) + " ampdu_members=" + std::to_string(ampdu_members) +
             " peer_retries=" + std::to_string(peer_retries) + " ba_timeouts=" + std::to_string(ba_reorder.timeouts()) +
             " ba_duplicates=" + std::to_string(ba_reorder.duplicates()) +
             " ack_queue_avg_us=" + std::to_string(fresh_ack_count ? ack_queue_us / fresh_ack_count : 0) +
             " ack_queue_over_2ms=" + std::to_string(ack_over_2ms) +
             " station_ack_ok=" + std::to_string(station_ack_ok) + " station_ack_failed=" + std::to_string(station_ack_failed) +
             " rxd_rejected=" + std::to_string(dev.mt7925_rxd_rejected()) +
             " rxd_split_dropped=" + std::to_string(dev.mt7925_split_rx_dropped()));
  };
  unsigned fast_data = 0, aggregates = 0;
  unsigned channel_rx = 0, channel_beacons = 0, awdl_actions_rx = 0;
  unsigned gate_holds = 0, retries_sent = 0, stale_control = 0, expired_retry = 0, superseded_retry = 0;
  struct PeerRate {
    std::array<uint8_t, 6> mac{};
    uint64_t seen_us = 0, capabilities_us = 0, unicast_heard_us = 0;
    AwDlRate rate = AwDlRate::ofdm6;
    mtk::mt7925::AwDlPeerWindow window{};
    mtk::mt7925::AwDlRateLadder ladder{};
    uint16_t seq = 0;
    bool have_seq = false;
  };
  std::array<PeerRate, 16> peers;
  const auto peer_at = [&](const uint8_t *mac) {
    return std::find_if(peers.begin(), peers.end(), [&](const PeerRate &peer) {
      return peer.seen_us && !std::memcmp(mac, peer.mac.data(), peer.mac.size());
    });
  };
  struct BaControl {
    awdl::AddbaRequest request{};
    uint64_t heard = 0;
    bool remove = false;
  };
  struct BaTid {
    bool enabled = false;
    uint8_t token = 0;
    uint16_t ssn = 0, window = 0;
    uint64_t accepted_us = 0;
    bool amsdu = false;
  };
  std::deque<BaControl> ba_commands;
  std::array<BaTid, 8> ba_tids{};
  std::array<uint8_t, 6> ba_peer{};
  bool ba_peer_installed = false;
  uint64_t ba_last_seen_us = 0, ba_retry_retire_us = 0;
  uint16_t ba_management_seq = 0;
  unsigned ba_responses = 0;
  AwDlTxBaFlow tx_flow;
  uint64_t ampdu_queued = 0, ampdu_sample_ok = 0, ampdu_sample_failed = 0;
  uint64_t mib_frames = 0, mib_lease = 0;
  unsigned mib_single_intervals = 0;
  bool mib_baseline = false;
  unsigned tx_ba_requests = 0, tx_ba_agreements = 0, tx_ba_stalls = 0, advertised_ht = 0, received_ht = 0;
  std::vector<uint8_t> qos_frame;
  std::vector<uint8_t> wire;
  wire.reserve(128 * 1024);
  const auto emit_reordered = [&](const uint8_t *frame, size_t bytes, uint8_t ch, int8_t dbm) {
    append_rx_wire({frame, bytes, dbm, ch}, wire);
  };
  mtk::mt7925::RxPhyWindow rx_phy_window;
  const auto on_rx = [&](const mtk::RxFrame &rx) {
    if (rx.channel != channel || rx.len < 20) return;
    ++channel_rx;
    if (rx.data[0] == 0x80) ++channel_beacons;
    if (awdl::is_awdl_action(rx.data, rx.len)) ++awdl_actions_rx;
    const auto heard = now_us();
    const bool is_to_us = !std::memcmp(rx.data + 4, local.data(), local.size());
    const bool from_ba_peer = ba_peer_installed && !std::memcmp(rx.data + 10, ba_peer.data(), ba_peer.size());
    if (rx_ba && rx.data[0] == 0x84) {
      if (is_to_us && from_ba_peer && !(rx.data[16] & 2u)) {
        ba_last_seen_us = heard;
        ba_reorder.on_bar(rx.data, rx.len, heard, emit_reordered);
      }
      return;
    }
    if (!is_awdl_bssid(rx.data, rx.len)) return;
    ++seen_awdl;
    if ((rx_ba || tx_ba) && is_to_us && rx.len >= 26 && rx.data[0] == 0xd0 && !(rx.data[1] & 0xc7) &&
        rx.data[24] == 3 && !(rx.data[10] & 1u)) {
      if (tx_ba && tx_flow.matches(rx.data + 10)) {
        awdl::AddbaAnswer answer{};
        if (awdl::parse_addba_answer(rx.data, rx.len, local.data(), &answer)) {
          const bool ok_ans = tx_flow.accept(rx.data + 10, answer.token, answer.tid, answer.status,
                                             answer.window, answer.immediate, heard);
          log_line("MT7925 TX BA ADDBA_RSP: accepted=" + std::to_string(ok_ans) +
                   " token=" + std::to_string(answer.token) + " status=" + std::to_string(answer.status) +
                   " window=" + std::to_string(answer.window) + " imm=" + std::to_string(answer.immediate) +
                   " ssn=" + std::to_string(tx_flow.next_seq));
          return;
        }
        bool sender = false;
        uint8_t tid = 0;
        if (awdl::parse_delba(rx.data, rx.len, local.data(), &sender, &tid) &&
            !sender && tid == AwDlTxBaFlow::tid) {
          const uint16_t reason = rx.len >= 30 ? static_cast<uint16_t>(rx.data[28] | (rx.data[29] << 8)) : 0xffffu;
          log_line("MT7925 TX BA peer_delba: reason=" + std::to_string(reason) +
                   " phase=" + std::to_string(static_cast<int>(tx_flow.phase)) +
                   " next_seq=" + std::to_string(tx_flow.next_seq) +
                   " ampdu_ok=" + std::to_string(dev.mt7925_awdl_ampdu_ok()) +
                   " ampdu_failed=" + std::to_string(dev.mt7925_awdl_ampdu_failed()) +
                   " tx_pending=" + std::to_string(dev.mt7925_awdl_tx_pending()));
          if (tx_flow.phase == BaPhase::active || tx_flow.phase == BaPhase::ending) {
            tx_flow.phase = BaPhase::ending;
            tx_flow.send_delba = false;
            tx_flow.close_backoff = 1500000u;
          } else if (tx_flow.phase != BaPhase::idle) {
            tx_flow.finish(heard, 1500000u);
          }
          return;
        }
      }
      if (!rx_ba) return;
      if (ba_commands.size() >= 8) return;
      BaControl cmd;
      cmd.heard = heard;
      if (awdl::parse_addba_request(rx.data, rx.len, local.data(), &cmd.request) && cmd.request.immediate) {
        const auto peer = peer_at(cmd.request.peer);
        if ((!ba_peer_installed || from_ba_peer) && peer != peers.end() && heard - peer->seen_us < 3000000u)
          ba_commands.push_back(cmd);
      } else if (from_ba_peer) {
        bool peer_is_sender = false;
        if (awdl::parse_delba(rx.data, rx.len, local.data(), &peer_is_sender, &cmd.request.tid) &&
            peer_is_sender && cmd.request.tid < ba_tids.size()) {
          cmd.remove = true;
          std::copy(ba_peer.begin(), ba_peer.end(), cmd.request.peer);
          ba_commands.push_back(cmd);
        }
      }
      return;
    }
    if (!mtk::mt7925::valid_awdl_rx(rx.data, rx.len) ||
        (!(rx.data[4] & 1u) && !is_to_us) ||
        !std::memcmp(rx.data + 10, local.data(), local.size())) return;
    if (from_ba_peer) ba_last_seen_us = heard;
    ++received;
    if (rx.data[0] != 0xd0) ++admitted_data;
    if (is_to_us && rx.data[0] != 0xd0) {
      ++unicast_data;
      ampdu_members += rx.phy.ampdu_member;
      peer_retries += (rx.data[1] & 8u) != 0;
    }
    if (rx.data[0] == 0x88 && (rx.data[24] & 0x80)) ++native_amsdu;
    const size_t length = awdl::strip_valid_fcs(rx.data, rx.len);
    const size_t header = mtk::mt7925::awdl_header_size(rx.data, length);
    {
      auto peer = peer_at(rx.data + 10);
      if (peer == peers.end() && rx.data[0] == 0xd0) {
        peer = std::min_element(peers.begin(), peers.end(), [](const PeerRate &a, const PeerRate &b) {
          return a.seen_us < b.seen_us;
        });
        *peer = {};
        peer->window.schedule.assumeZero = true;
        std::copy_n(rx.data + 10, peer->mac.size(), peer->mac.begin());
      }
      if (peer != peers.end()) {
        peer->seen_us = heard;
        if (is_to_us && rx.data[0] != 0xd0) peer->unicast_heard_us = heard;
        peer->window.observe(rx.data, length, heard, channel);
        if (is_to_us && !(rx.data[4] & 1u) && rx.data[0] != 0xd0 &&
            rx_phy_window.note(heard, length, rx.phy_mode, rx.phy, rx.dbm)) {
          const auto &schedule = peer->window.schedule;
          log_line(rx_phy_window.finish(schedule.fresh(heard) ? schedule.channels : nullptr,
                                        awdl_sched::kPeerSeqEntries));
        }
        const auto advertised = mtk::mt7925::awdl_peer_rate(rx.data, length);
        const auto observed = rx.phy_mode == 4 ? AwDlRate::vht20 :
            (rx.phy_mode == 2 || rx.phy_mode == 3 ? AwDlRate::ht20 : AwDlRate::ofdm6);
        if (advertised >= AwDlRate::ht20) ++advertised_ht;
        if (observed >= AwDlRate::ht20) ++received_ht;
        const auto supported = mtk::mt7925::awdl_higher_rate(advertised, observed);
        if (supported >= AwDlRate::ht20) {
          if (!peer->capabilities_us || heard - peer->capabilities_us >= 8000000u)
            peer->rate = supported;
          else
            peer->rate = mtk::mt7925::awdl_higher_rate(peer->rate, supported);
          peer->capabilities_us = heard;
        }
      }
    }
    if (mtk::mt7925::awdl_mdns(rx.data, rx.len, header ? header : 40)) ++mdns_rx;
    if (rx_ba && is_to_us && from_ba_peer &&
        ba_reorder.process(rx.data, length, rx.channel, rx.dbm, heard, emit_reordered)) return;
    auto clean = rx;
    clean.len = length;
    append_rx_wire(clean, wire);
  };
  const auto ensure_ba_peer = [&](const uint8_t *peer, uint64_t now) {
    if (ba_peer_installed) return !std::memcmp(peer, ba_peer.data(), ba_peer.size());
    if (!dev.mt7925_add_awdl_ba_peer(peer)) return false;
    std::copy_n(peer, ba_peer.size(), ba_peer.begin());
    ba_peer_installed = true;
    ba_last_seen_us = now;
    return true;
  };
  const auto process_ba_control = [&] {
    if (dev.mt7925_awdl_tx_available() < 12) return;
    const auto now = now_us();
    if (ba_peer_installed && tx_flow.phase == BaPhase::idle &&
        now - ba_last_seen_us > 8000000u && now >= ba_retry_retire_us) {
      ba_retry_retire_us = now + 1000000u;
      if (dev.mt7925_remove_awdl_ba_peer()) {
        ba_peer_installed = false;
        ba_tids = {};
        ba_reorder.clear();
      }
    }
    if (ba_commands.empty()) return;
    const auto cmd = ba_commands.front();
    ba_commands.pop_front();
    if (now - cmd.heard > 500000u) return;
    const auto &request = cmd.request;
    if (ba_peer_installed && std::memcmp(request.peer, ba_peer.data(), ba_peer.size())) return;
    auto &tid = ba_tids[request.tid];
    if (cmd.remove) {
      if (tid.enabled && dev.mt7925_set_awdl_ba(request.tid, 0, 0, false, AwDlBaDirection::rx)) {
        tid.enabled = false;
        ba_reorder.on_delba(ba_peer.data(), request.tid, emit_reordered);
      }
      return;
    }
    if (!ensure_ba_peer(request.peer, now)) return;
    ba_last_seen_us = now;
    const bool duplicate = tid.enabled && tid.token == request.token &&
                           tid.ssn == request.ssn && tid.window == request.window && tid.amsdu == request.amsdu &&
                           now - tid.accepted_us < 1000000u;
    if (!duplicate && !dev.mt7925_set_awdl_ba(request.tid, request.ssn, request.window, true,
                                              AwDlBaDirection::rx, request.amsdu)) return;
    std::array<uint8_t, awdl::kAddbaResponseLength> response{};
    awdl::build_addba_response(response.data(), request, local.data(), ++ba_management_seq, request.amsdu);
    if (dev.mt7925_queue_awdl_frame(response.data(), response.size(), AwDlRate::ofdm6, AwDlTxFormat::block_ack_action)) {
      ++ba_responses;
      dev.mt7925_flush_awdl_tx();
      if (!duplicate) {
        tid = {true, request.token, request.ssn, request.window, now, request.amsdu};
        log_line("MT7925 RX BA agreement: tid=" + std::to_string(request.tid) +
                 " window=" + std::to_string(request.window) + " amsdu=" + std::to_string(request.amsdu));
        ba_reorder.on_addba(ba_peer.data(), request.tid, request.ssn, request.window, now_us(), emit_reordered);
      }
    } else if (!duplicate && dev.mt7925_set_awdl_ba(request.tid, 0, 0, false, AwDlBaDirection::rx)) {
      tid.enabled = false;
      ba_reorder.on_delba(ba_peer.data(), request.tid, emit_reordered);
    }
  };
  const auto advance_tx_ba = [&] {
    const auto now = now_us();
    const auto acked = dev.mt7925_awdl_ampdu_ok();
    const auto failed = dev.mt7925_awdl_ampdu_failed();
    if (tx_flow.phase == BaPhase::active) {
      const auto sample_ok = acked - ampdu_sample_ok;
      const auto sample_failed = failed - ampdu_sample_failed;
      const auto peer = peer_at(tx_flow.peer.data());
      const bool on_channel = peer == peers.end() || peer->window.sample(now);
      bool losses = false;
      if (sample_ok + sample_failed >= 64) {
        losses = on_channel && sample_failed > sample_ok * 2;
        ampdu_sample_ok = acked;
        ampdu_sample_failed = failed;
      }
      bool singles = false;
      if (peer != peers.end() && peer->window.sample(now)) {
        const auto lease = dev.mt7925_awdl_renewals();
        const auto completed = acked + failed;
        const bool reset_mib = !mib_baseline || lease != mib_lease;
        if (reset_mib || completed - mib_frames >= awdl::kBaTxNoBaFallbackFrames) {
          mtk::Mt7925AwDlAggregation sample{};
          const bool valid = dev.mt7925_read_awdl_aggregation(sample);
          if (valid && !reset_mib) {
            const auto without_ba = uint64_t(sample.rx_block_ack) + sample.multi == 0;
            const bool stalled = without_ba && awdl::ba_tx_stalled_as_singles(completed - mib_frames,
                sample.rx_block_ack, sample.zero_or_single);
            mib_single_intervals = stalled ? mib_single_intervals + 1 : 0;
            singles = mib_single_intervals >= 2;
            log_line("MT7925 A-MPDU MIB: single_or_empty=" + std::to_string(sample.zero_or_single) +
                     " multi=" + std::to_string(sample.multi) + " rx_ba=" + std::to_string(sample.rx_block_ack));
          } else {
            mib_single_intervals = 0;
          }
          mib_frames = completed;
          mib_lease = lease;
          mib_baseline = valid;
        }
      }
      if (singles) ++tx_ba_stalls;
      if (losses || singles || now - tx_flow.last_data > 10000000u) {
        log_line("MT7925 TX BA ending: reason=" + std::string(losses ? "loss_majority" : singles ? "singles" : "idle") +
                 " sample_ok=" + std::to_string(sample_ok) + " sample_failed=" + std::to_string(sample_failed));
        tx_flow.phase = BaPhase::ending;
        tx_flow.send_delba = true;
        tx_flow.close_backoff = losses || singles ? 1500000u : 500000u;
      }
    }
    if (tx_flow.phase == BaPhase::ending && acked + failed == ampdu_queued && dev.mt7925_awdl_tx_available() >= 12) {
      if (tx_flow.firmware_enabled &&
          !dev.mt7925_set_awdl_ba(AwDlTxBaFlow::tid, 0, 0, false, AwDlBaDirection::tx)) return;
      tx_flow.firmware_enabled = false;
      const bool had_peer_delba = !tx_flow.send_delba;
      if (tx_flow.send_delba) {
        std::array<uint8_t, awdl::kDelbaLength> delba{};
        awdl::build_delba(delba.data(), tx_flow.peer.data(), local.data(), mtk::mt7925::awdl_bssid.data(),
                          AwDlTxBaFlow::tid, true, ++ba_management_seq);
        if (!dev.mt7925_queue_awdl_frame(delba.data(), delba.size(), AwDlRate::ofdm6, AwDlTxFormat::block_ack_action))
          return;
        dev.mt7925_flush_awdl_tx();
      }
      if (had_peer_delba && ba_peer_installed && !dev.mt7925_awdl_tx_pending()) {
        const bool any_rx_ba = std::any_of(ba_tids.begin(), ba_tids.end(),
                                           [](const auto &t) { return t.enabled; });
        if (!any_rx_ba && dev.mt7925_remove_awdl_ba_peer()) {
          ba_peer_installed = false;
          log_line("MT7925 TX BA: reset STA_REC after peer_delba");
        }
      }
      tx_flow.finish(now, tx_flow.close_backoff);
    }
    if (tx_flow.phase == BaPhase::agreed) {
      if (now - tx_flow.last_data > 1500000u) {
        tx_flow.phase = BaPhase::ending;
        tx_flow.send_delba = true;
        tx_flow.close_backoff = 1500000u;
      } else if (!dev.mt7925_awdl_tx_pending() && dev.mt7925_awdl_tx_available() >= 12) {
        if (ensure_ba_peer(tx_flow.peer.data(), now) &&
            dev.mt7925_set_awdl_ba(AwDlTxBaFlow::tid, tx_flow.next_seq, tx_flow.window, true, AwDlBaDirection::tx)) {
          tx_flow.phase = BaPhase::active;
          tx_flow.firmware_enabled = true;
          ampdu_sample_ok = acked;
          ampdu_sample_failed = failed;
          mib_baseline = false;
          mib_single_intervals = 0;
          ++tx_ba_agreements;
          log_line("MT7925 TX BA active: agreement=" + std::to_string(tx_ba_agreements) +
                   " ssn=" + std::to_string(tx_flow.next_seq) + " window=" + std::to_string(tx_flow.window));
        } else {
          tx_flow.phase = BaPhase::ending;
          tx_flow.send_delba = true;
          tx_flow.close_backoff = 1500000u;
        }
      }
    }
    if (tx_flow.phase != BaPhase::awaiting || now < tx_flow.next_request) return;
    const auto peer = peer_at(tx_flow.peer.data());
    if (peer != peers.end() && peer->window.hold(now)) return;
    if (tx_flow.requests >= 5 || now - tx_flow.last_data > 1500000u) {
      tx_flow.finish(now, 1500000u);
      return;
    }
    // Wait for pre-BA raw frames to drain out of TX0 before sending ADDBA Request.
    if (dev.mt7925_awdl_tx_pending()) return;
    if (!ba_peer_installed && dev.mt7925_awdl_tx_available() < 12) return;
    if (!ensure_ba_peer(tx_flow.peer.data(), now)) {
      tx_flow.finish(now, 1500000u);
      return;
    }
    std::array<uint8_t, awdl::kAddbaResponseLength> request{};
    awdl::build_addba_request(request.data(), tx_flow.peer.data(), local.data(), mtk::mt7925::awdl_bssid.data(),
                              tx_flow.token, AwDlTxBaFlow::tid, AwDlTxBaFlow::max_window, tx_flow.next_seq,
                              ++ba_management_seq);
    if (dev.mt7925_queue_awdl_frame(request.data(), request.size(), AwDlRate::ofdm6, AwDlTxFormat::block_ack_action)) {
      ++tx_ba_requests;
      ++tx_flow.requests;
      tx_flow.next_request = now + 100000u;
      dev.mt7925_flush_awdl_tx();
    }
  };
  const auto rate_cap = [&](const PeerRate &peer, uint64_t now) {
    return peer.capabilities_us && now - peer.capabilities_us < 8000000u ? peer.rate : AwDlRate::ofdm24;
  };
  const auto for_local = [&](std::span<const uint8_t> frame) {
    return !frame.empty() && !std::memcmp(frame.data() + 10, local.data(), local.size());
  };
  const auto ba_stream = [&](std::span<const uint8_t> frame) {
    return tx_ba && tx_flow.phase != BaPhase::idle && tx_flow.matches(frame.data() + 4) &&
           mtk::mt7925::awdl_ba_data_header(frame.data(), frame.size());
  };

  const auto take = [&](std::deque<QueuedTx> &lane, uint64_t now, QueuedTx &out) {
    std::lock_guard<std::mutex> lock(mutex);
    return mtk::mt7925::awdl_take_ready(lane, out, [&](const QueuedTx &item) {
      const auto frame = frame_in(item.command);
      if (!for_local(frame) || item.retry.expired(now)) return true;
      if (!item.retry.ready(now)) return false;
      if (frame[0] == 0xd0 || (frame[4] & 1u)) return true;
      const auto peer = peer_at(frame.data() + 4);
      if (peer != peers.end() && peer->window.hold(now)) {
        // An upload MPDU addressed to us is direct evidence the ACK's peer
        // is listening on this channel. Use it only for an immediate reply.
        if (!peer->unicast_heard_us || now - peer->unicast_heard_us > 1000 || !upload_ack(frame)) {
          ++gate_holds;
          return false;
        }
      }
      if (!ba_stream(frame) || item.retry.attempts) return true;
      const auto completed = dev.mt7925_awdl_ampdu_ok() + dev.mt7925_awdl_ampdu_failed();
      return tx_flow.phase == BaPhase::active && ampdu_queued - completed < tx_flow.window;
    });
  };
  const auto requeue = [&](std::deque<QueuedTx> &lane, QueuedTx item) {
    std::lock_guard<std::mutex> lock(mutex);
    lane.push_front(std::move(item));
  };

  struct InFlight {
    uint16_t token = 0;
    AwDlRate rate = AwDlRate::ofdm6;
    bool sample = false, control = false;
    AwDlTxFormat format = AwDlTxFormat::raw;
    QueuedTx item{};
  };
  std::array<InFlight, mtk::mt7925::AwDlTxSlots::capacity> in_flight;
  std::vector<mtk::Mt7925TxCompletion> completions;
  const auto process_tx_feedback = [&] {
    dev.mt7925_take_awdl_completions(completions);
    const auto now = now_us();
    for (const auto &completion : completions) {
      auto &sent = in_flight[completion.token % in_flight.size()];
      if (sent.token != completion.token) continue;
      const auto frame = frame_in(sent.item.command);
      if (upload_ack(frame)) ++(completion.acked ? ack_ok : ack_failed);
      if (sent.format == AwDlTxFormat::peer_data) ++(completion.acked ? station_ack_ok : station_ack_failed);
      if (sent.sample && !frame.empty()) {
        const auto peer = peer_at(frame.data() + 4);
        // Count failed first attempts only while the current schedule still
        // places the peer on-channel; a slot edge is not rate feedback.
        if (peer != peers.end() && (completion.acked || peer->window.sample(now)))
          peer->ladder.note(completion.acked, sent.rate, rate_cap(*peer, now));
      }
      const bool single_frame = sent.format == AwDlTxFormat::raw || sent.format == AwDlTxFormat::peer_data;
      if (!completion.acked && !frame.empty() && single_frame &&
          !ba_stream(frame) && !sent.item.retry.expired(now) && sent.item.retry.attempts < 6) {
        auto &retry = sent.item.retry;
        retry.next_us = now + awdl::resend_backoff_us(retry.attempts + 1u);
        // The original sequence and retry bit prevent a duplicate delivery.
        sent.item.command.payload[sent.item.command.payload.size() - frame.size() + 1] |= 0x08u;
        auto &lane = sent.control ? controls : payloads;
        std::lock_guard<std::mutex> lock(mutex);
        const auto limit = sent.control ? 128u : data_limit;
        if (lane.size() < limit) lane.push_front(std::move(sent.item));
        else ++pipe_drops;
      }
      sent = {};
    }
  };

  std::array<uint8_t, mtk::mt7925::awdl_amsdu_max> aggregate;
  uint64_t last_active = 0, next_rx_report = now_us() + 15000000;
  uint64_t reported_data = 0, reported_split = 0;

  while (!done()) {
    if (enabled) {
      const auto now = now_us();
      const auto active_peer = peer_at(tx_flow.peer.data());
      const bool peer_away = tx_flow.phase != BaPhase::idle && active_peer != peers.end() &&
                             active_peer->window.renew(now);
      if (!dev.mt7925_maintain_awdl(error, on_rx, peer_away)) {
        ok = false;
        break;
      }
    }

    unsigned worked = 0, queued = 0;
    for (unsigned i = 0; i < 16; ++i) {
      PendingCmd cmd;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (commands.empty()) break;
        cmd = std::move(commands.front());
        commands.pop_front();
      }
      ++worked;
      if (cmd.type != RADIO_LINK_SET_ADDRESS || enabled || cmd.payload.size() != local.size() ||
          (cmd.payload[0] & 3u) != 2u) continue;
      std::copy(cmd.payload.begin(), cmd.payload.end(), local.begin());
      if (!dev.mt7925_start_awdl(local.data(), rx_ba || tx_ba, lease_ms, error)) {
        ok = false;
        break;
      }
      enabled = true;
      log_line("MT7925 AWDL channel " + std::to_string(channel) + " ready for frames");
    }
    if (!ok) break;

    size_t polled = 0;
    if (enabled) {
      // Pipe commands are handled first. A busy SHM producer can neither
      // defer address setup nor consume the receive poll's whole time slice.
      for (unsigned i = 0; i < 64; ++i) {
        const auto now = now_us();
        PendingCmd cmd{RADIO_LINK_TX, {}, now};
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (payloads.size() >= data_limit || controls.size() >= 128) break;
        }
        if (!pipe.pop_shm_tx(cmd.payload)) break;
        enqueue(std::move(cmd));
        ++worked;
      }

      polled = dev.poll(on_rx);
      process_tx_feedback();
      if (rx_ba || tx_ba) process_ba_control();
      if (tx_ba) advance_tx_ba();
      for (unsigned i = 0; i < tx_batch_limit; ++i) {
        const auto free = dev.mt7925_awdl_tx_available();
        if (!free) break;
        const auto now = now_us();
        QueuedTx item;
        const bool control = take(controls, now, item);
        if (!control && (free <= control_reserve || !take(payloads, now, item))) break;
        ++worked;
        const auto frame = frame_in(item.command);
        if (!for_local(frame)) {
          ++rejected;
          continue;
        }
        if (mtk::mt7925::awdl_sync_expired(frame.data(), frame.size(), item.command.at_us, now)) {
          ++stale_control;
          continue;
        }
        if (item.retry.expired(now)) {
          ++expired_retry;
          continue;
        }

        const bool action = frame[0] == 0xd0;
        const bool unicast = !action && !(frame[4] & 1u);
        const auto peer = unicast ? peer_at(frame.data() + 4) : peers.end();
        const bool known = peer != peers.end() && now - peer->seen_us < 3000000u;
        const auto cap = known ? rate_cap(*peer, now) : AwDlRate::ofdm6;
        const bool bulk = mtk::mt7925::awdl_ba_bulk_data(frame.data(), frame.size());
        const bool retry = item.retry.attempts != 0;
        const bool owned = ba_stream(frame);
        if (owned && retry) {
          // A previous raw attempt carries its old sequence. Let the upper
          // layer recover it rather than inject it into a new BA sequence.
          ++superseded_retry;
          continue;
        }
        const bool is_ampdu = owned && tx_flow.phase == BaPhase::active;
        const bool fast_ack = known && !retry && !is_ampdu && cap >= AwDlRate::ht20 && upload_ack(frame);
        const auto rate = fast_ack ? AwDlRate::ofdm24 : fast && known && bulk
                              ? (tx_ba && !is_ampdu ? AwDlRate::ofdm24 : peer->ladder.rate(cap))
                              : AwDlRate::ofdm6;
        const bool manage_seq = !retry && known && tx_ba && mtk::mt7925::awdl_ba_data_header(frame.data(), frame.size());
        const uint16_t input_seq = static_cast<uint16_t>((frame[22] >> 4) | (unsigned(frame[23]) << 4));
        const auto seq = is_ampdu ? tx_flow.next_seq : manage_seq && peer->have_seq ? peer->seq : input_seq;
        auto output = frame;
        auto format = is_ampdu ? AwDlTxFormat::ampdu : AwDlTxFormat::raw;
        if (manage_seq || is_ampdu) {
          if (!mtk::mt7925::build_awdl_ba_qos(frame.data(), frame.size(), seq, qos_frame)) {
            ++rejected;
            continue;
          }
          output = qos_frame;
        }
        // The RX BA agreement has already installed this peer's WCID and rate
        // table. Return its TCP ACK on the data queue with an individual MAC ACK.
        if (fast_ack && manage_seq && ba_peer_installed &&
            !std::memcmp(ba_peer.data(), frame.data() + 4, ba_peer.size()))
          format = AwDlTxFormat::peer_data;

        QueuedTx joined;
        bool have_joined = false;
        if (!control && !tx_ba && !retry && rate >= AwDlRate::ht20 && take(payloads, now, joined)) {
          const auto next = frame_in(joined.command);
          const size_t total = for_local(next) && !joined.retry.attempts && next.size() >= 512 ?
              mtk::mt7925::build_awdl_amsdu(aggregate.data(), aggregate.size(), frame.data(), frame.size(),
                                          next.data(), next.size()) : 0;
          if (total) {
            format = AwDlTxFormat::amsdu;
            output = {aggregate.data(), total};
            have_joined = true;
          } else {
            requeue(payloads, std::move(joined));
          }
        }

        uint16_t token = 0;
        const bool track = unicast && !is_ampdu &&
                           (frame[0] != 0x88 || (frame[24] & 0x60u) == 0);
        if (!dev.mt7925_queue_awdl_frame(output.data(), output.size(), rate, format, track ? &token : nullptr)) {
          if (have_joined) requeue(payloads, std::move(joined));
          if (!dev.mt7925_awdl_tx_available()) {
            requeue(control ? controls : payloads, std::move(item));
            break;
          }
          ++failures;
          continue;
        }

        const auto next_seq = mtk::mt7925::next_awdl_ba_sequence(seq);
        if (const auto *tcp = upload_ack(frame)) {
          if (!retry) {
            const auto queued_us = now - item.command.at_us;
            ++fresh_ack_count;
            ack_queue_us += queued_us;
            ack_over_2ms += queued_us > 2000;
          }
          const uint32_t ack = uint32_t(tcp[8]) << 24 | uint32_t(tcp[9]) << 16 |
                               uint32_t(tcp[10]) << 8 | tcp[11];
          unsigned port = unsigned(tcp[2]) << 8 | tcp[3];
          if (!ack_sent || port != last_peer_port) first_ack = ack;
          ++ack_sent;
          last_peer_port = port;
          latest_ack = ack;
          last_window = unsigned(tcp[14]) << 8 | tcp[15];
        }
        if (manage_seq) {
          peer->seq = next_seq;
          peer->have_seq = true;
        }
        if (is_ampdu) {
          tx_flow.next_seq = next_seq;
          tx_flow.last_data = now;
          ++ampdu_queued;
        }
        if (!retry && tx_ba && bulk && known &&
            (!ba_peer_installed || !std::memcmp(ba_peer.data(), frame.data() + 4, ba_peer.size())))
          tx_flow.note_bulk(frame.data() + 4, next_seq, now);
        if (retry) ++retries_sent;
        ++queued;
        if (action) ++actions;
        else data += have_joined ? 2u : 1u;
        if (have_joined) ++aggregates;
        if (rate != AwDlRate::ofdm6) fast_data += have_joined ? 2u : 1u;
        if (mtk::mt7925::awdl_mdns(frame.data(), frame.size(), frame[0] == 0x88 ? 26 : 24)) ++mdns_tx;
        if (track) {
          if (manage_seq) {
            const auto header = item.command.payload.size() - frame.size();
            item.command.payload.resize(header);
            item.command.payload.insert(item.command.payload.end(), output.begin(), output.end());
          }
          if (!retry) item.retry.first_us = now;
          ++item.retry.attempts;
          const bool sample = fast && !retry && bulk && known && peer->window.sample(now);
          in_flight[token % in_flight.size()] = {token, rate, sample, control, format, std::move(item)};
        }
      }

      max_batch = std::max(max_batch, queued);
      dev.mt7925_flush_awdl_tx();
      if (rx_ba) ba_reorder.flush_expired(now_us(), emit_reordered);
      if (!wire.empty() && !pipe.send_raw(wire)) break;
      wire.clear();

    }
    const auto now = now_us();
    if (enabled && now >= next_rx_report) {
      const auto offered = ack_offered.load();
      if (offered != last_offered) {
        log_line("MT7925 RX upload ACK: offered=" + std::to_string(offered) +
                 " replaced=" + std::to_string(ack_replaced.load()) +
                 " submissions=" + std::to_string(ack_sent) + " raw_mac_ok=" + std::to_string(ack_ok) +
                 " raw_mac_failed=" + std::to_string(ack_failed) + " ack_delta=" + std::to_string(latest_ack-first_ack) +
                 " advertised_window=" + std::to_string(last_window));
        last_offered = offered;
      }
      next_rx_report = now + 15000000;
      if (reported_data != admitted_data || reported_split != dev.mt7925_split_rx_dropped()) {
        report_rx();
        reported_data = admitted_data;
        reported_split = dev.mt7925_split_rx_dropped();
      }
    }
    if (worked || polled) last_active = now;
    else if (last_active && now - last_active < spin_window_us) SwitchToThread();
    else Sleep(1);
  }
  link_done = true;
  reader.join();
  if (enabled) report_rx();
  dev.set_stop_check({});  // Drain and return the channel even after service stop.
  if (!dev.mt7925_stop_awdl(ba_peer_installed) && ok) {
    error = "MT7925 did not acknowledge the AWDL channel release";
    ok = false;
  }
  log_line("MT7925 AWDL link: action_queued=" + std::to_string(actions) +
           " data_queued=" + std::to_string(data) + " tx_rejected=" + std::to_string(rejected) +
           " fast_data=" + std::to_string(fast_data) + " tx_queue_failed=" + std::to_string(failures) +
           " pipe_drops=" + std::to_string(pipe_drops) +
           " max_tx_batch=" + std::to_string(max_batch) +
           " gate_holds=" + std::to_string(gate_holds) + " host_retries=" + std::to_string(retries_sent) +
           " stale_sync=" + std::to_string(stale_control) + " retry_expired=" + std::to_string(expired_retry) +
           " retry_superseded=" + std::to_string(superseded_retry) +
           " ch" + std::to_string(channel) + "_rx=" + std::to_string(channel_rx) + " ch" + std::to_string(channel) + "_beacons=" + std::to_string(channel_beacons) +
           " awdl_actions_rx=" + std::to_string(awdl_actions_rx) +
           " lease_renewals=" + std::to_string(dev.mt7925_awdl_renewals()) +
           " early_renewals=" + std::to_string(dev.mt7925_awdl_early_renewals()) +
           " high_rate_ok=" + std::to_string(dev.mt7925_awdl_fast_ok()) +
           " high_rate_failed=" + std::to_string(dev.mt7925_awdl_fast_failed()) +
           " amsdu_queued=" + std::to_string(aggregates) +
           " tx_ba_requests=" + std::to_string(tx_ba_requests) +
           " tx_ba_agreements=" + std::to_string(tx_ba_agreements) +
           " tx_ba_stalls=" + std::to_string(tx_ba_stalls) +
           " ampdu_queued=" + std::to_string(ampdu_queued) +
           " ampdu_ok=" + std::to_string(dev.mt7925_awdl_ampdu_ok()) +
           " ampdu_failed=" + std::to_string(dev.mt7925_awdl_ampdu_failed()) +
           " rx_ba_responses_queued=" + std::to_string(ba_responses) +
           " rx_ba_reordered=" + std::to_string(ba_reorder.reordered()) +
           " amsdu_ok=" + std::to_string(dev.mt7925_awdl_amsdu_ok()) +
           " amsdu_failed=" + std::to_string(dev.mt7925_awdl_amsdu_failed()) +
           " tx_ok=" + std::to_string(dev.mt7925_awdl_tx_ok()) +
           " tx_failed=" + std::to_string(dev.mt7925_awdl_tx_failed()) + " awdl_rx=" + std::to_string(received) +
           " awdl_bssid_seen=" + std::to_string(seen_awdl) + " awdl_data_rx=" + std::to_string(admitted_data) +
           " advertised_ht=" + std::to_string(advertised_ht) + " received_ht=" + std::to_string(received_ht) +
           " mdns_tx=" + std::to_string(mdns_tx) + " mdns_rx=" + std::to_string(mdns_rx));
  return ok;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  std::filesystem::path firmware, stop_file;
  std::wstring instance, country;
  bool fast = false, indoor = false, passive = false, explicit_channel = false;
  uint8_t channel = 44;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view key(argv[i]);
    if (key == L"--e138-fast") fast = true;
    else if (key == L"--check-passive") passive = true;
    else if (key == L"--indoor") indoor = true;
    else if (key == L"--channel" && i + 1 < argc) {
      explicit_channel = true;
      const std::wstring_view primary(argv[++i]);
      if (primary == L"44") channel = 44;
      else if (primary == L"149") channel = 149;
      else {
        log_line("E138 FastBa --channel accepts 44 or 149");
        return 2;
      }
    }
    else if (key == L"--firmware" && i + 1 < argc) firmware = argv[++i];
    else if (key == L"--stop-file" && i + 1 < argc) stop_file = argv[++i];
    else if (key == L"--country" && i + 1 < argc) country = argv[++i];
    else if (key == L"--instance" && i + 1 < argc) instance = argv[++i];
    else {
      log_line("Unknown or incomplete E138 radio option");
      return 2;
    }
  }
  const bool valid_mode = passive ? (!fast && !indoor && country.empty() && !explicit_channel)
                                  : (fast && indoor && country == L"CN");
  if (!valid_mode || !firmware.is_absolute() || !stop_file.is_absolute() ||
      !mtk::MtkDevice::is_e138_instance(instance)) {
    log_line("Specify --check-passive or --e138-fast --indoor --country CN [--channel 44|149], "
             "plus --firmware <absolute directory> --stop-file <absolute path> "
             "--instance <full E138105B REV_00 PCI instance>");
    return 2;
  }
  const auto stop_requested = [&] {
    if (GetFileAttributesW(stop_file.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const DWORD error = GetLastError();
    return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
  };
  if (stop_requested()) {
    if (passive) log_line("MT7925 E138 passive check stopped before opening the radio");
    return passive ? 1 : 0;
  }
  SetConsoleCtrlHandler(on_console, TRUE);
  std::jthread monitor([&](std::stop_token token) {
    while (!token.stop_requested() && !stopping()) {
      if (stop_requested()) g_stop = true;
      else Sleep(100);
    }
  });
  const auto utf8 = firmware.u8string();
  const std::string firmware_dir(utf8.begin(), utf8.end());
  std::string error;
  if (passive) {
    uint32_t beacons = 0;
    unsigned frames = 0;
    if (!mtk::MtkDevice::check_e138_passive(instance, firmware_dir, stopping, beacons, frames, error)) {
      log_line("MT7925 E138 passive check failed: " + error);
      return 1;
    }
    log_line("MT7925 E138 passive check complete: channel=44 dwell_ms=250 firmware_beacons=" +
             std::to_string(beacons) + " received_frames=" + std::to_string(frames));
    return 0;
  }
  mtk::MtkDevice::Options options;
  options.mt7925_active_lab = true;
  options.instance_id = instance;
  options.country = "CN";
  options.firmware_dir = firmware_dir;
  mtk::MtkDevice dev(log_line);
  dev.set_stop_check(stopping);
  if (!dev.open(options, error) || !dev.is_mt7925() ||
      !dev.mt7925_program_awdl_power(channel, options.country, indoor, true, error) || dev.width_mhz() != 80) {
    log_line("MT7925 E138 FastBa bring-up failed: " + error);
    return 1;
  }
  timeBeginPeriod(1);
  const bool ok = run_mt7925_awdl_link(dev, error);
  timeEndPeriod(1);
  if (!ok) log_line("MT7925 E138 FastBa link failed: " + error);
  return ok ? 0 : 1;
}
