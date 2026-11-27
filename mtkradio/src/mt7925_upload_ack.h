// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "mt7925_active.h"
#include <span>

namespace mtk::mt7925 {
// AirDrop's IPv6/TCP ACK on the ordinary single-MSDU AWDL path.
inline const uint8_t *upload_ack(std::span<const uint8_t> frame) {
  const auto header = mtk::mt7925::awdl_header_size(frame.data(), frame.size());
  if (!header || frame[0] == 0xd0 || frame.size() < header + 16 + 60) return nullptr;
  const auto *ip = frame.data() + header + 16;
  const auto *tcp = ip + 40;
  const size_t payload = (unsigned(ip[4]) << 8) | ip[5];
  const size_t tcp_header = (tcp[12] >> 4) * 4;
  if ((ip[0] >> 4) != 6 || ip[6] != 6 || ((unsigned(tcp[0]) << 8) | tcp[1]) != 8771 ||
      (tcp[13] & 0x17u) != 0x10 || tcp_header < 20 || payload != tcp_header ||
      frame.size() < header + 16 + 40 + payload) return nullptr;
  return tcp;
}

// Compare ordinary cumulative ACKs only. Preserve loss signals, window updates,
// distinct flows and TCP options other than a timestamp.
inline bool advances_upload_ack(const uint8_t *a, const uint8_t *b) {
  if (!a || !b || a[13] != 0x10 || b[13] != 0x10 || a[12] != b[12] ||
      (!a[14] && !a[15]) || a[14] != b[14] || a[15] != b[15] ||
      std::memcmp(a - 32, b - 32, 40) != 0) return false; // IPv6 addresses, ports, sequence
  constexpr uint8_t timestamp_options[] = {1, 1, 8, 10};
  const int length = a[12] >> 4;
  if (length != 5 && (length != 8 || std::memcmp(a + 20, timestamp_options, 4) ||
                                     std::memcmp(b + 20, timestamp_options, 4))) return false;
  const auto ack = [](const uint8_t *tcp) {
    return uint32_t(tcp[8]) << 24 | uint32_t(tcp[9]) << 16 | uint32_t(tcp[10]) << 8 | tcp[11];
  };
  const uint32_t delta = ack(b) - ack(a);
  // A pair of 24-segment receive indications can advance by over 64 KiB.
  // Bound the total gap between retained ACKs even when a tail is replaced repeatedly.
  return delta != 0 && delta <= 256u * 1024u;
}

} // namespace mtk::mt7925
