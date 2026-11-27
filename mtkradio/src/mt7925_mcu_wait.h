// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "mt7925_rx.h"
extern "C" {
#include "firmware.h"
}

namespace mtk::mt7925 {

constexpr unsigned roc_abort_ack_ms = 1000;

template<class Submit>
bool abort_roc_once(uint64_t &lease_end, Submit submit) {
  if (!lease_end) return false;
  lease_end = 0;
  return submit(roc_abort_ack_ms);
}

// The caller has already submitted the command once. A timeout may inspect
// already queued records once, without issuing another command or sleeping.
template<class Clock, class Receive, class Idle, class Expired>
int wait_mcu_reply(uint64_t deadline, Clock now, Receive receive, Idle idle, Expired expired) {
  while (now() < deadline) {
    bool received = false;
    const int result = receive(received);
    if (result != MT_PENDING) return result;
    if (!received) idle();
  }
  return expired();
}

struct Rx0Peek {
  bool done, last;
  const uint8_t *data;
  uint32_t size;
};

// Inspect a fixed producer snapshot without acknowledging/refilling an RX
// descriptor. A continuation of a radio frame is never an MCU ACK.
template<class ReadSlot, class Decode>
int peek_mcu_reply(unsigned tail, unsigned producer, unsigned count, bool fragmented,
                   ReadSlot read_slot, Decode decode) {
  if (tail >= count || producer >= count) return MT_FW_PROTOCOL;
  for (unsigned i = tail; i != producer; i = (i + 1) % count) {
    const auto slot = read_slot(i);
    if (!slot.done) break;
    const bool whole = !fragmented && slot.last;
    fragmented = !slot.last;
    if (!whole || slot.size < 40 || rx_kind(slot.data, slot.size) != RxKind::event ||
        slot.data[34] || slot.data[35] != 0x38) continue;
    const int result = decode(slot.data, slot.size);
    if (result != MT_PENDING) return result;
  }
  return MT_FW_TIMEOUT;
}

} // namespace mtk::mt7925
