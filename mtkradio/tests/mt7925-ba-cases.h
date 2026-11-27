// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "../src/mt7925_ba.h"

#include <cassert>
#include <vector>

inline void verify_mt7925_ba_cases() {
  using namespace mtk::mt7925;

  auto data_frame = [](uint8_t subtype, size_t bytes) {
    std::vector<uint8_t> frame(bytes, 0x5a);
    frame[0] = subtype;
    frame[1] = 0x08;  // Retry on the input's old sequence number
    frame[4] = 0x02;
    std::copy(awdl_bssid.begin(), awdl_bssid.end(), frame.begin() + 16);
    frame[22] = 0xb0;
    frame[23] = 0x35;
    const size_t header = subtype == 0x88 ? 26 : 24;
    if (header == 26) {
      frame[24] = 0x60;  // Block Ack policy on the input
      frame[25] = 0x67;
    }
    const uint8_t snap[]{0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0, 3, 4, 0, 0, 0, 0, 0x86, 0xdd, 0x60};
    std::copy(std::begin(snap), std::end(snap), frame.begin() + header);
    return frame;
  };

  std::vector<uint8_t> output;
  std::array<uint8_t, 4096> tx_page{};
  for (const uint8_t subtype : {uint8_t{0x08}, uint8_t{0x88}}) {
    const size_t header = subtype == 0x88 ? 26 : 24;
    const auto frame = data_frame(subtype, 512);
    assert(awdl_ba_bulk_data(frame.data(), frame.size()));
    assert(!awdl_ba_bulk_data(frame.data(), frame.size() - 1));
    assert(build_awdl_ba_qos(frame.data(), frame.size(), 4095, output) == frame.size() + 26 - header);
    assert(output[0] == 0x88 && output[1] == 0);
    assert(std::equal(frame.begin() + 2, frame.begin() + 22, output.begin() + 2));
    assert(output[22] == 0xf0 && output[23] == 0xff);
    assert(output[24] == 0 && output[25] == (header == 26 ? frame[25] : 0));
    assert(std::equal(frame.begin() + header, frame.end(), output.begin() + 26, output.end()));
    assert(write_awdl_txwi(tx_page.data(), tx_page.size(), 0x1000, 128, output.data(), output.size(),
                           AwDlRate::ht20, AwDlTxFormat::ampdu));

    const auto small_frame = data_frame(subtype, header + 56);
    assert(!awdl_ba_bulk_data(small_frame.data(), small_frame.size()));
    assert(build_awdl_ba_qos(small_frame.data(), small_frame.size(), next_awdl_ba_sequence(4095), output) == 82);
    assert(output[22] == 0 && output[23] == 0);

    const auto max_frame = data_frame(subtype, 2304 - (26 - header));
    assert(awdl_ba_bulk_data(max_frame.data(), max_frame.size()));
    assert(build_awdl_ba_qos(max_frame.data(), max_frame.size(), next_awdl_ba_sequence(0), output) == 2304);
    assert(output[22] == 0x10 && output[23] == 0);
    const auto oversized = data_frame(subtype, max_frame.size() + 1);
    assert(!awdl_ba_bulk_data(oversized.data(), oversized.size()));
    assert(build_awdl_ba_qos(oversized.data(), oversized.size(), 0, output) == 0);
    assert(output.size() == 2304 && output[22] == 0x10);
  }

  const auto native = data_frame(0x88, 512);
  auto expect_rejected = [&](const uint8_t *frame, size_t bytes) {
    assert(!awdl_ba_bulk_data(frame, bytes));
    assert(build_awdl_ba_qos(frame, bytes, 0, output) == 0);
    assert(output.size() == 2304 && output[22] == 0x10);
  };
  expect_rejected(nullptr, native.size());
  expect_rejected(native.data(), 26);
  constexpr struct { size_t index; uint8_t value; } invalid_bytes[]{
      {0, 0x48}, {1, 0x40}, {4, 0x03}, {16, 0xff}, {22, 0xb1}, {24, 0x03}, {24, 0x80}, {40, 0}};
  for (const auto &[index, value] : invalid_bytes) {
    auto corrupted = native;
    corrupted[index] = value;
    expect_rejected(corrupted.data(), corrupted.size());
  }

  AwDlTxBaFlow flow;
  for (uint64_t now = 0; now < 2; ++now) {
    if (awdl_ba_bulk_data(native.data(), native.size())) flow.note_bulk(native.data() + 4, 4095, now);
  }
  assert(flow.phase == AwDlTxBaFlow::Phase::awaiting && flow.next_seq == 4095);
}
