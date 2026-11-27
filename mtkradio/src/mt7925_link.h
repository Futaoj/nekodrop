// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "awdl_policy.h"
#include "awdl_schedule.h"

#include <algorithm>
#include <utility>

namespace mtk::mt7925 {

enum class AwDlTxLane { control, data };

inline AwDlTxLane awdl_tx_lane(const uint8_t *frame, size_t bytes) {
  if (bytes >= 24 && (frame[0] == 0x08 || frame[0] == 0x88) && !(frame[4] & 1u))
    return AwDlTxLane::data;
  return AwDlTxLane::control;
}

inline bool awdl_sync_expired(const uint8_t *frame, size_t bytes, uint64_t queued, uint64_t now) {
  return now >= queued && now - queued > 40000 && awdl::is_awdl_action(frame, bytes);
}

// Keep blocked frames in the queue. A bounded lookahead lets a ready peer
// or control frame make progress without repeatedly scanning the entire lane.
template <typename Queue, typename Ready>
bool awdl_take_ready(Queue &lane, typename Queue::value_type &out, Ready ready) {
  const auto end = lane.begin() + std::min<size_t>(lane.size(), 32);
  const auto next = std::find_if(lane.begin(), end, ready);
  if (next == end) return false;
  out = std::move(*next);
  lane.erase(next);
  return true;
}

struct AwDlPeerWindow {
  awdl_sched::PeerSchedule schedule{};

  void observe(const uint8_t *frame, size_t bytes, uint64_t heard, uint8_t channel) {
    schedule.notePeerFrame(heard);
    if (!awdl::is_awdl_action(frame, bytes)) return;
    awdl_sched::PeerSyncInfo info{};
    bool bad_sequence = false;
    if (awdl_sched::peerSyncFromAction(frame, bytes, &info, &bad_sequence))
      schedule.note(info, heard, channel);
    else if (bad_sequence)
      schedule.invalidate();
  }

  bool hold(uint64_t now) const {
    return schedule.sendGate(now, false, 0) == awdl_sched::PeerSchedule::SendGate::Away;
  }

  bool sample(uint64_t now) const {
    // Leave room for firmware retries. A frame close to a slot edge does not
    // provide a reliable rate sample, even if it was safe to submit.
    return schedule.timingUsable(now, false) && !hold(now) && !hold(now + 2500);
  }

  bool renew(uint64_t now) const {
    if (!schedule.timingUsable(now, false) || !hold(now)) return false;
    const auto entry = schedule.eawAt(now, nullptr);
    return !schedule.presentForSend(entry) && schedule.usUntilPresentAw(now) >= 28000;
  }
};

} // namespace mtk::mt7925
