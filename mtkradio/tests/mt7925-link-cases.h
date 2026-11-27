// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "mt7925_link.h"

#include <cassert>
#include <deque>

inline void verify_mt7925_link_cases() {
  using namespace mtk::mt7925;
  constexpr uint64_t now = 100000;
  AwDlPeerWindow window;
  assert(!window.hold(now) && !window.sample(now) && !window.renew(now));

  awdl_sched::PeerSyncInfo sync{};
  sync.awPeriodTU = 16;
  sync.downCounterTU = 48;
  sync.channels[0] = sync.channels[2] = 44;
  sync.channels[1] = 149;
  window.schedule.assumeZero = true;
  window.schedule.note(sync, now, 44);
  const uint64_t next = now + 48 * 1024;
  assert(window.sample(now));
  assert(window.hold(next - 1000) && !window.renew(next - 1000));
  assert(window.hold(next + 5000) && window.renew(next + 5000));
  assert(window.hold(next + 60000) && !window.renew(next + 60000));
  assert(window.sample(next + 64000 + 3000));
  assert(!window.hold(now + awdl_sched::kPeerScheduleStaleUS));

  std::deque<int> queue{1, 2, 3};
  int sent = 0;
  const auto ready = [&](int frame) { return frame != 1 || !window.hold(next + 5000); };
  assert(awdl_take_ready(queue, sent, ready) && sent == 2);
  assert((queue == std::deque<int>{1, 3}));
  assert(awdl_take_ready(queue, sent, [](int) { return true; }) && sent == 1);
  assert((queue == std::deque<int>{3}));
  assert(!awdl_take_ready(queue, sent, [](int) { return false; }));
  assert((queue == std::deque<int>{3}));

  uint8_t frame[26]{};
  frame[0] = 0xd0;
  assert(awdl_tx_lane(frame, sizeof(frame)) == AwDlTxLane::control);
  frame[0] = 0x88;
  frame[4] = 1;
  assert(awdl_tx_lane(frame, sizeof(frame)) == AwDlTxLane::control);
  frame[4] = 0;
  assert(awdl_tx_lane(frame, sizeof(frame)) == AwDlTxLane::data);
  assert(awdl_sync_expired(frame, sizeof(frame), now, now + 41000) == false);
  frame[0] = 0xd0;
  frame[24] = 0x7f;
  assert(!awdl_sync_expired(frame, sizeof(frame), now, now + 41000));
  uint8_t sync_frame[40]{};
  sync_frame[0] = 0xd0;
  sync_frame[24] = 0x7f;
  sync_frame[26] = 0x17;
  sync_frame[27] = 0xf2;
  sync_frame[28] = 0x08;
  assert(!awdl_sync_expired(sync_frame, sizeof(sync_frame), now, now + 40000));
  assert(awdl_sync_expired(sync_frame, sizeof(sync_frame), now, now + 41000));
}
