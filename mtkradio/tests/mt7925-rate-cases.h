// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cassert>

#include "mt7925_active.h"

inline void verify_mt7925_rate_cases() {
  using namespace mtk::mt7925;

  std::array<uint8_t, 64> action{};
  action[0] = 0xd0;
  std::copy(awdl_bssid.begin(), awdl_bssid.end(), action.begin() + 16);
  constexpr std::array<uint8_t, 5> vendor{0x7f, 0, 0x17, 0xf2, 8};
  std::copy(vendor.begin(), vendor.end(), action.begin() + 24);
  constexpr size_t ht_end = 49;
  action[40] = 7;
  action[41] = 6;
  action[48] = 0xff;
  for (size_t pad = 0; pad <= 5; ++pad)
    assert(awdl_peer_rate(action.data(), ht_end + pad) == AwDlRate::ht20);
  action[49] = 1;
  assert(awdl_peer_rate(action.data(), ht_end + 1) == AwDlRate::ofdm6);
  action[49] = 0;
  action[50] = 1;
  assert(awdl_peer_rate(action.data(), ht_end + 2) == AwDlRate::ofdm6);
  assert(awdl_peer_rate(action.data(), ht_end + 3) == AwDlRate::ofdm6);
  action[48] = 0x28;
  assert(awdl_peer_rate(action.data(), ht_end) == AwDlRate::ht20_mcs5);
  action[48] = 0x08;
  assert(awdl_peer_rate(action.data(), ht_end) == AwDlRate::ht20_mcs3);

  std::fill(action.begin() + 40, action.end(), uint8_t{0});
  constexpr size_t vht_end = 57;
  action[40] = 17;
  action[41] = 14;
  action[43] = 191;
  action[44] = 12;
  constexpr std::array vht_caps{
      AwDlRate::vht20, AwDlRate::vht20_mcs8, AwDlRate::vht20_mcs8, AwDlRate::ofdm6};
  for (size_t i = 0; i < vht_caps.size(); ++i) {
    action[49] = static_cast<uint8_t>(0xfcu | i);
    action[50] = 0xff;
    assert(awdl_peer_rate(action.data(), vht_end + 2) == vht_caps[i]);
  }
  action[49] = 0xfd;
  assert(awdl_peer_rate(action.data(), vht_end - 1) == AwDlRate::ofdm6);
  action[44] = 13;
  assert(awdl_peer_rate(action.data(), vht_end) == AwDlRate::ofdm6);
  assert(awdl_higher_rate(AwDlRate::ht20_mcs5, AwDlRate::ht20) == AwDlRate::ht20);
  assert(awdl_higher_rate(AwDlRate::vht20, AwDlRate::ht20_mcs3) == AwDlRate::vht20);

  constexpr std::array<uint16_t, 6> codes{0, 0x83, 0x85, 0x87, 0x107, 0x108};
  std::array<uint8_t, 80> data{};
  data[0] = 0x08;
  data[4] = 2;
  std::copy(awdl_bssid.begin(), awdl_bssid.end(), data.begin() + 16);
  constexpr std::array<uint8_t, 17> payload{
      0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0, 3, 4, 0, 0, 0, 0, 0x86, 0xdd, 0x60};
  std::copy(payload.begin(), payload.end(), data.begin() + 24);
  std::array<uint8_t, 144> descriptor{};
  for (size_t i = 0; i < awdl_rate_steps.size(); ++i) {
    const auto rate = awdl_rate_steps[i];
    assert(awdl_fixed_rate_code(rate) == codes[i]);
    assert(write_awdl_txwi(descriptor.data(), descriptor.size(), 0x4000, 1,
                           data.data(), data.size(), rate));
    const auto txd6 = rx_word(descriptor.data() + 24);
    assert(((txd6 >> 16) & 0x3f) == static_cast<uint8_t>(rate));
    assert(((txd6 >> 22) & 0xf) == (i == 0 ? 0u : 8u));  // BW 20 and FIXED_BW when HT/VHT.
  }

  AwDlRateLadder ladder;
  constexpr auto cap = AwDlRate::vht20_mcs8;
  const auto window = [&ladder](AwDlRate sent, AwDlRate supported, unsigned misses) {
    for (unsigned i = 0; i < 32; ++i) ladder.note(i >= misses, sent, supported);
  };
  assert(ladder.rate(AwDlRate::ofdm6) == AwDlRate::ofdm24);
  for (size_t i = 0; i + 1 < awdl_rate_steps.size(); ++i) {
    for (unsigned j = 0; j < 31; ++j) {
      ladder.note(true, awdl_rate_steps[i + 1], cap);  // An out-of-step result must be ignored.
      ladder.note(true, awdl_rate_steps[i], cap);
    }
    assert(ladder.rate(cap) == awdl_rate_steps[i]);
    ladder.note(true, awdl_rate_steps[i], cap);
    assert(ladder.rate(cap) == awdl_rate_steps[i + 1]);
  }
  window(cap, cap, 6);
  assert(ladder.rate(cap) == cap);
  window(cap, cap, 7);
  assert(ladder.rate(cap) == AwDlRate::vht20);
  window(AwDlRate::vht20, cap, 2);
  assert(ladder.rate(cap) == AwDlRate::vht20);
  window(AwDlRate::vht20, cap, 1);
  assert(ladder.rate(cap) == cap);
  constexpr auto lower_cap = AwDlRate::ht20_mcs3;
  window(lower_cap, lower_cap, 0);
  assert(ladder.rate(lower_cap) == lower_cap);
  assert(ladder.rate(cap) == lower_cap);
}
