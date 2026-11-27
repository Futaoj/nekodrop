// SPDX-License-Identifier: GPL-2.0-only
/*
 * Layer-2/control-plane MAC translation between Windows' TAP adapter and the
 * station's over-the-air MAC address (#96).
 *
 * OpenVPN's tap-windows6 driver rejects any NetworkAddress registry override
 * that lacks the locally-administered bit (0x02), which rules out every real
 * factory OUI Wi-Fi MAC. Keeping the TAP adapter's own address untouched and
 * translating Ethernet + ARP + DHCPv4 (chaddr / Option 61) + ICMPv6 NDP
 * (Source/Target Link-Layer Address) at the TAP boundary lets the router see
 * the PC's exact Windows Wi-Fi MAC with zero adapter-restart delay.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sta_mac_nat {

inline uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

inline uint32_t csum_add(uint32_t sum, const uint8_t *p, size_t n) {
  for (size_t i = 0; i + 1 < n; i += 2) sum += static_cast<uint32_t>((p[i] << 8) | p[i + 1]);
  if (n & 1) sum += static_cast<uint32_t>(p[n - 1] << 8);
  return sum;
}

inline uint16_t csum_fold(uint32_t sum) {
  while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}

inline bool payload_is_dhcp(const uint8_t *p, size_t len, bool tx) {
  if (len < 28 || (p[0] >> 4) != 4 || p[9] != 17) return false;
  const size_t ihl = static_cast<size_t>(p[0] & 0x0f) * 4;
  if (ihl < 20 || ihl + 8 > len) return false;
  const uint16_t sport = be16(p + ihl);
  const uint16_t dport = be16(p + ihl + 2);
  return tx ? (sport == 68 && dport == 67) : (sport == 67 && dport == 68);
}

inline bool translate_payload(uint16_t ethertype, uint8_t *payload, size_t len,
                              const uint8_t from_mac[6], const uint8_t to_mac[6], bool tx) {
  if (!payload || std::memcmp(from_mac, to_mac, 6) == 0) return false;
  if (ethertype == 0x0806) {
    /* ARP (RFC 826): Ethernet(1) / IPv4(0x0800), hlen=6, plen=4. */
    if (len < 28 || be16(payload) != 1 || be16(payload + 2) != 0x0800 || payload[4] != 6 || payload[5] != 4)
      return false;
    uint8_t *field = tx ? (payload + 8) : (payload + 18); /* SHA on TX, THA on RX */
    if (std::memcmp(field, from_mac, 6) == 0) {
      std::memcpy(field, to_mac, 6);
      return true;
    }
    return false;
  }
  if (ethertype == 0x0800) {
    /* IPv4 DHCP (BOOTP/UDP 68->67 on TX, 67->68 on RX): rewrite chaddr and Option 61. */
    if (len < 20 || (payload[0] >> 4) != 4 || payload[9] != 17) return false;
    const size_t ihl = static_cast<size_t>(payload[0] & 0x0f) * 4;
    if (ihl < 20 || ihl + 8 > len || (be16(payload + 6) & 0x1fff) != 0) return false;
    uint8_t *udp = payload + ihl;
    const size_t udp_len = be16(udp + 4);
    if (udp_len < 8 + 236 || ihl + udp_len > len) return false;
    const uint16_t sport = be16(udp), dport = be16(udp + 2);
    uint8_t *bootp = udp + 8;
    const size_t bootp_len = udp_len - 8;
    if (tx) {
      if (sport != 68 || dport != 67 || bootp[0] != 1 || bootp[1] != 1 || bootp[2] != 6) return false;
    } else {
      if (sport != 67 || dport != 68 || bootp[0] != 2 || bootp[1] != 1 || bootp[2] != 6) return false;
    }
    bool changed = false;
    if (std::memcmp(bootp + 28, from_mac, 6) == 0) {
      std::memcpy(bootp + 28, to_mac, 6);
      changed = true;
    }
    if (bootp_len >= 240 && bootp[236] == 0x63 && bootp[237] == 0x82 && bootp[238] == 0x53 && bootp[239] == 0x63) {
      size_t pos = 240;
      while (pos < bootp_len) {
        const uint8_t opt = bootp[pos];
        if (opt == 255) break;
        if (opt == 0) {
          pos++;
          continue;
        }
        if (pos + 2 > bootp_len) break;
        const uint8_t olen = bootp[pos + 1];
        if (pos + 2 + olen > bootp_len) break;
        if (opt == 61 && olen == 7 && bootp[pos + 2] == 1 &&
            std::memcmp(bootp + pos + 3, from_mac, 6) == 0) {
          std::memcpy(bootp + pos + 3, to_mac, 6);
          changed = true;
        }
        pos += 2 + olen;
      }
    }
    if (changed && be16(udp + 6) != 0) {
      udp[6] = 0;
      udp[7] = 0;
      uint32_t sum = csum_add(0, payload + 12, 8); /* IPv4 src + dst */
      sum += 17u + static_cast<uint32_t>(udp_len);
      sum = csum_add(sum, udp, udp_len);
      uint16_t cs = csum_fold(sum);
      if (cs == 0) cs = 0xffff;
      udp[6] = static_cast<uint8_t>(cs >> 8);
      udp[7] = static_cast<uint8_t>(cs);
    }
    return changed;
  }
  if (ethertype == 0x86dd) {
    /* IPv6 NDP (ICMPv6 133/135/136): rewrite Source/Target Link-Layer Address options. */
    if (len < 48 || (payload[0] >> 4) != 6 || payload[6] != 58) return false;
    const size_t plen = be16(payload + 4);
    if (plen < 8 || 40 + plen > len) return false;
    uint8_t *icmp = payload + 40;
    const uint8_t type = icmp[0];
    const size_t opt_off = (type == 133) ? 8 : (type == 135 || type == 136) ? 24 : 0;
    if (!opt_off || opt_off >= plen) return false;
    bool changed = false;
    size_t pos = opt_off;
    while (pos + 2 <= plen) {
      const uint8_t otype = icmp[pos];
      const uint8_t ounits = icmp[pos + 1];
      if (ounits == 0) break;
      const size_t olen = static_cast<size_t>(ounits) * 8;
      if (pos + olen > plen) break;
      if ((otype == 1 || otype == 2) && olen == 8 && std::memcmp(icmp + pos + 2, from_mac, 6) == 0) {
        std::memcpy(icmp + pos + 2, to_mac, 6);
        changed = true;
      }
      pos += olen;
    }
    if (changed) {
      icmp[2] = 0;
      icmp[3] = 0;
      uint32_t sum = csum_add(0, payload + 8, 32); /* IPv6 src + dst */
      sum += static_cast<uint32_t>(plen) + 58u;
      sum = csum_add(sum, icmp, plen);
      const uint16_t cs = csum_fold(sum);
      icmp[2] = static_cast<uint8_t>(cs >> 8);
      icmp[3] = static_cast<uint8_t>(cs);
    }
    return changed;
  }
  return false;
}

} // namespace sta_mac_nat
