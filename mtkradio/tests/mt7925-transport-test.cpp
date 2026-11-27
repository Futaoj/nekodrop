// SPDX-License-Identifier: GPL-2.0-only
#include "mt7925_active.h"
#include "mt7925_ba.h"
#include "mt7925_mcu_wait.h"
#include "mt7925_regs.h"
#include "mt7925_rx.h"
#include "mt7925-ba-cases.h"
#include "mt7925-rate-cases.h"

extern "C" {
#include "clc_power.h"
#include "nic_caps.h"
#include "passive-scan.h"
#include "regulatory.h"
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

void put32_le(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

void verify_rx_and_dual_band() {
  uint32_t mdp_dcr0 = 0xfedcba98u | MDP_DCR0_DAMSDU_EN | MDP_DCR0_RX_HDR_TRANS_EN;
  const auto original = mdp_dcr0;
  const auto read = [&](uint32_t address) {
    assert(address == 0x820cc800u);
    return mdp_dcr0;
  };
  assert(configure_native_awdl_rx(
      [&](uint32_t address, uint32_t mask) {
        assert(address == 0x820cc800u && mask == 0x88000u);
        mdp_dcr0 &= ~mask;
      },
      read));
  assert(mdp_dcr0 == (original & ~0x88000u));
  mdp_dcr0 |= MDP_DCR0_DAMSDU_EN;
  assert(!configure_native_awdl_rx([](uint32_t, uint32_t) {}, read));

  RxDmaSnapshot snapshot;
  unsigned hardware_index = 5;
  const auto index = [&] { return hardware_index; };
  for (unsigned i = 0; i < 5; ++i) assert(snapshot.ready(i, 8, index));
  assert(snapshot.register_reads() == 1 && snapshot.consumed() == 5);
  assert(!snapshot.ready(5, 8, index));
  hardware_index = 2;
  for (unsigned i : {5u, 6u, 7u, 0u, 1u}) assert(snapshot.ready(i, 8, index));
  assert(snapshot.register_reads() == 3 && snapshot.consumed() == 10);
  hardware_index = 0xffffffffu;
  assert(!snapshot.ready(2, 8, index));
  hardware_index = 3;
  assert(snapshot.ready(2, 8, index));

  std::array<uint8_t, 0x109> rx{};
  put32_le(rx.data(), 0x38010109u);
  put32_le(rx.data() + 4, 0x80870013u);
  put32_le(rx.data() + 8, 0x60008c00u);
  put32_le(rx.data() + 12, 0x004324d3u); /* ch 36 (0x24) */
  put32_le(rx.data() + 16, 0x81c60000u);
  put32_le(rx.data() + 48, 0x12345678u); /* Group 2 timestamp */
  put32_le(rx.data() + 64, 0x00000089u);
  put32_le(rx.data() + 72, 0x0000200au);
  rx[76] = 100;
  rx[77] = 112;
  rx[88] = 0x80; /* Beacon */

  MonitorFrame frame{};
  assert(rx_kind(rx.data(), rx.size()) == RxKind::frame);
  assert(parse_concurrent_frame(rx.data(), rx.size(), 36, 80, frame));
  assert(frame.offset == 88 && frame.length == 0x109 - 88 && frame.channel == 36 && frame.dbm == -54);
  assert(frame.has_timestamp && frame.timestamp_us == 0x12345678u);
  assert(frame.phy.width_mhz == 80 && frame.phy.mcs == 9 && frame.phy.nss == 2 && frame.phy.short_gi);

  /* 2.4 GHz channel 6 (0x06) must be accepted cleanly by parse_concurrent_frame(). */
  put32_le(rx.data() + 12, 0x004306d3u);
  assert(parse_concurrent_frame(rx.data(), rx.size(), 6, 20, frame));
  assert(frame.channel == 6);

  /* 2.4 GHz channel 0 in RXD falls back to tuned_channel (6). */
  put32_le(rx.data() + 12, 0x004300d3u);
  assert(parse_concurrent_frame(rx.data(), rx.size(), 6, 20, frame));
  assert(frame.channel == 6);

  /* Invalid channel 15 must be rejected. */
  put32_le(rx.data() + 12, 0x00430fd3u);
  assert(!parse_concurrent_frame(rx.data(), rx.size(), 6, 20, frame));

  /* 5 GHz 80 MHz center channel (42 = 0x2a) maps to tuned primary (44). */
  put32_le(rx.data() + 12, 0x00432ad3u);
  assert(parse_concurrent_frame(rx.data(), rx.size(), 44, 80, frame));
  assert(frame.channel == 44);
}

void verify_dual_band_roc_and_sta() {
  /* 2.4 GHz ROC request & grant (channel 6, band 1, 20 MHz, cw=0, sco=0). */
  const auto req2g = roc_request(6, 7, 1, 0, roc_max_awdl_ms, 80);
  assert(req2g[8] == 0 && req2g[9] == 7 && req2g[10] == 6 && req2g[11] == 0);
  assert(req2g[12] == 1 && req2g[13] == 0 && req2g[14] == 6 && req2g[19] == 1);

  std::array<uint8_t, 68> grant2g{};
  grant2g[32] = 36;
  grant2g[36] = 0x27;
  grant2g[38] = 4; /* unsolicited */
  grant2g[50] = 20;
  grant2g[52] = 0;  /* bss 0 */
  grant2g[53] = 7;  /* token 7 */
  grant2g[55] = 6;  /* ch 6 */
  grant2g[56] = 0;  /* sco 0 */
  grant2g[57] = 1;  /* band 1 (2.4 GHz) */
  grant2g[58] = 0;  /* cw 0 */
  grant2g[59] = 6;  /* center 6 */
  grant2g[61] = 1;  /* type 1 */
  put32_le(grant2g.data() + 64, 4000u);
  uint32_t ms = 0;
  assert(roc_grant(grant2g.data(), static_cast<uint32_t>(grant2g.size()), 6, 7, 1, ms, 0, 4000, 80) ==
         Grant::granted);
  assert(ms == 4000u);

  /* Band mismatch (band=2 for ch 6) must be rejected. */
  grant2g[57] = 2;
  assert(roc_grant(grant2g.data(), static_cast<uint32_t>(grant2g.size()), 6, 7, 1, ms, 0, 4000, 80) ==
         Grant::invalid);

  /* 5 GHz 80 MHz ROC request & grant (channel 149, band 2, cw=1, sco=1, center=155). */
  const auto req5g = roc_request(149, 9, 1, 0, roc_max_awdl_ms, 80);
  assert(req5g[10] == 149 && req5g[11] == 1 && req5g[12] == 2 && req5g[13] == 1 && req5g[14] == 155);

  std::array<uint8_t, 68> grant5g = grant2g;
  grant5g[53] = 9;
  grant5g[55] = 149;
  grant5g[56] = 1;
  grant5g[57] = 2;
  grant5g[58] = 1;
  grant5g[59] = 155;
  ms = 0;
  assert(roc_grant(grant5g.data(), static_cast<uint32_t>(grant5g.size()), 149, 9, 1, ms, 0, 4000, 80) ==
         Grant::granted);
  assert(ms == 4000u);

  /* Dual-band STA_REC builder (2.4 GHz vs 5 GHz). */
  const uint8_t peer[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
  const uint8_t mcs[2] = {0xff, 0x00};
  const auto sta2g = sta_rec_add_vht(0, 1, peer, 0x01ef, 0x03, mcs, true, 0x038001b2u, 0xfffa, 2, 1, true);
  assert(sta2g[2] == 7); /* VHT TLV omitted on 2.4 GHz */
  assert(sta2g[8 + 20 + 4] == 0x0f && sta2g[8 + 20 + 6] == 0x13);
  assert(sta2g[8 + 20 + 12 + 8 + 4] == 0xcf && sta2g[8 + 20 + 12 + 8 + 5] == 0x3f);

  const uint8_t mcs2[2] = {0xff, 0xff};
  const auto sta5g = sta_rec_add_vht(1, 17, peer, 0x01ef, 0x03, mcs2, true, 0x038001b2u, 0xfffa, 2, 2, false);
  assert(sta5g[2] == 8);
  assert(sta5g[8 + 20 + 4] == 0x15 && sta5g[8 + 20 + 6] == 0x38);
  assert(sta5g[8 + 20 + 12 + 8 + 16 + 4] == 0xc0 && sta5g[8 + 20 + 12 + 8 + 16 + 5] == 0x3f);

  /* General TXWI encoding (management fixed rate + QoS data firmware RA). */
  std::array<uint8_t, 256> page{};
  std::array<uint8_t, 36> mgmt{};
  mgmt[0] = 0xd0;
  std::fill_n(mgmt.begin() + 4, 6, 0xff);
  assert(write_general_txwi(page.data(), page.size(), 0x2000u, 42, mgmt.data(), mgmt.size(), 0, 19,
                            true, 18, true, true, false));
  assert((rx_word(page.data() + 32) & 0x9fffu) == (0x8000u | 42u));
  assert(((rx_word(page.data() + 24) >> 16) & 0x3fu) == 18u);

  std::array<uint8_t, 64> qos{};
  qos[0] = 0x88;
  qos[4] = 0x02;
  assert(write_general_txwi(page.data(), page.size(), 0x2000u, 43, qos.data(), qos.size(), 0, 1,
                            false, 0, false, false, true));
  assert((rx_word(page.data() + 4) & 0x0fffu) == 1u);
  assert((rx_word(page.data() + 12) & (1u << 28)) == 0u); /* BA enabled */
}

void verify_mcu_wait_and_slots() {
  AwDlTxSlots slots;
  const uint16_t t0 = slots.next_token();
  slots.prepare(t0);
  slots.publish(100);
  assert(slots.pending() == 1 && slots.unfetched() == 1);
  assert(slots.complete(t0));
  assert(slots.observe_fetch(1));
  assert(slots.reclaim() == 1 && slots.pending() == 0);

  uint64_t lease = 500;
  unsigned calls = 0;
  assert(abort_roc_once(lease, [&](unsigned timeout) {
    ++calls;
    assert(timeout == roc_abort_ack_ms);
    return true;
  }));
  assert(lease == 0 && calls == 1);
  assert(!abort_roc_once(lease, [&](unsigned) {
    ++calls;
    return true;
  }));
  assert(calls == 1);
}

void verify_firmware_images(const std::string &patch_path, const std::string &ram_path) {
  auto load = [](const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    assert(f);
    return std::vector<uint8_t>{std::istreambuf_iterator<char>(f), {}};
  };
  const auto patch = load(patch_path);
  const auto ram = load(ram_path);

  mt_fw_plan patch_plan{}, ram_plan{};
  assert(mt_fw_validate(patch.data(), static_cast<uint32_t>(patch.size()), MT_FW_PATCH, &patch_plan) == MT_OK);
  assert(mt_fw_validate(ram.data(), static_cast<uint32_t>(ram.size()), MT_FW_RAM, &ram_plan) == MT_OK);

  /* Validate signed regdomain request extraction for US, CN, JP, and world 00. */
  std::array<uint8_t, 4048> req{};
  for (const char *cc : {"00", "US", "CN", "JP"}) {
    const uint8_t alpha[2] = {static_cast<uint8_t>(cc[0]), static_cast<uint8_t>(cc[1])};
    const int n = mt_regd_request(ram.data(), static_cast<uint32_t>(ram.size()), 0, alpha,
                                  req.data(), static_cast<uint32_t>(req.size()));
    assert(n > static_cast<int>(MT_REGD_REQUEST_HEADER));
  }

  /* Validate CLC power block preflight + mock transport apply for US and CN. */
  unsigned clc_calls = 0;
  const mt_fw_transport mock{
      &clc_calls,
      [](void *ctx, uint32_t cmd, const uint8_t *, uint32_t bytes, int wait, uint32_t *st) -> int {
        assert(cmd == (MT_MCU_UNI | 0x2cu) && wait == 1 && bytes >= MT_CLC_HEADER);
        ++(*static_cast<unsigned *>(ctx));
        *st = 0;
        return MT_OK;
      },
      nullptr,
      nullptr,
  };
  for (const char *cc : {"00", "US", "CN"}) {
    clc_calls = 0;
    const uint8_t alpha[2] = {static_cast<uint8_t>(cc[0]), static_cast<uint8_t>(cc[1])};
    mt_clc_result clc{};
    const int r = mt_clc_power_apply(&mock, ram.data(), static_cast<uint32_t>(ram.size()), 0, alpha,
                                     1, req.data(), static_cast<uint32_t>(req.size()), &clc);
    assert(r == MT_OK && clc.requests_sent > 0 && clc.requests_sent == clc_calls);
  }
  std::cout << "Verified real MT7925 firmware patch (" << patch.size() << " B) & RAM (" << ram.size()
            << " B): regd + CLC OK.\n";
}

} // namespace

int main(int argc, char **argv) {
  verify_rx_and_dual_band();
  verify_dual_band_roc_and_sta();
  verify_mcu_wait_and_slots();
  verify_mt7925_ba_cases();
  verify_mt7925_rate_cases();
  if (argc >= 3) {
    verify_firmware_images(argv[1], argv[2]);
  }
  std::cout << "MT7925 transport, dual-band RX/ROC/STA/TXWI, BA, and rate tests passed.\n";
  return 0;
}
