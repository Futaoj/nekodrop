// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT7925 (Wi-Fi 7 Connac3) PCIe registers, fixed BAR0 map,
 * TX_FREE event fields and MCU commands.
 *
 * Register offsets and wire layouts follow OpenWrt mt76 (4d7f2bed):
 * mt7925/pci.c, mt7925/regs.h, mt7925/mcu.h, mt7925/mac.c, mt76_connac3_mac.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "mtk_regs.h"

namespace mtk::mt7925 {

/* --------------------------------------------------------------------------
 * PCIe BAR0 Fixed Address Map (mt7925/pci.c default_fixed_map)
 * -------------------------------------------------------------------------- */
struct FixedMapEntry {
  uint32_t phys;
  uint32_t mapped;
  uint32_t size;
};

static constexpr FixedMapEntry kFixedMap[] = {
    {0x830c0000u, 0x000000u, 0x0001000u}, /* WF_MCU_BUS_CR_REMAP */
    {0x54000000u, 0x002000u, 0x0001000u}, /* WFDMA PCIE0 MCU DMA0 */
    {0x55000000u, 0x003000u, 0x0001000u}, /* WFDMA PCIE0 MCU DMA1 */
    {0x56000000u, 0x004000u, 0x0001000u}, /* WFDMA reserved */
    {0x57000000u, 0x005000u, 0x0001000u}, /* WFDMA MCU wrap CR */
    {0x58000000u, 0x006000u, 0x0001000u}, /* WFDMA PCIE1 MCU DMA0 */
    {0x59000000u, 0x007000u, 0x0001000u}, /* WFDMA PCIE1 MCU DMA1 */
    {0x820c0000u, 0x008000u, 0x0004000u}, /* WF_UMAC_TOP (PLE) */
    {0x820c8000u, 0x00c000u, 0x0002000u}, /* WF_UMAC_TOP (PSE) */
    {0x820cc000u, 0x00e000u, 0x0002000u}, /* WF_UMAC_TOP (PP / MDP) */
    {0x74030000u, 0x010000u, 0x0001000u}, /* PCIe MAC */
    {0x820e0000u, 0x020000u, 0x0000400u}, /* WF_LMAC_TOP BN0 (WF_CFG) */
    {0x820e1000u, 0x020400u, 0x0000200u}, /* WF_LMAC_TOP BN0 (WF_TRB) */
    {0x820e2000u, 0x020800u, 0x0000400u}, /* WF_LMAC_TOP BN0 (WF_AGG) */
    {0x820e3000u, 0x020c00u, 0x0000400u}, /* WF_LMAC_TOP BN0 (WF_ARB) */
    {0x820e4000u, 0x021000u, 0x0000400u}, /* WF_LMAC_TOP BN0 (WF_TMAC) */
    {0x820e5000u, 0x021400u, 0x0000800u}, /* WF_LMAC_TOP BN0 (WF_RMAC) */
    {0x820ce000u, 0x021c00u, 0x0000200u}, /* WF_LMAC_TOP (WF_SEC) */
    {0x820e7000u, 0x021e00u, 0x0000200u}, /* WF_LMAC_TOP BN0 (WF_DMA) */
    {0x820cf000u, 0x022000u, 0x0001000u}, /* WF_LMAC_TOP (WF_PF) */
    {0x820e9000u, 0x023400u, 0x0000200u}, /* WF_LMAC_TOP BN0 (WF_WTBLOFF) */
    {0x820ea000u, 0x024000u, 0x0000200u}, /* WF_LMAC_TOP BN0 (WF_ETBF) */
    {0x820eb000u, 0x024200u, 0x0000400u}, /* WF_LMAC_TOP BN0 (WF_LPON) */
    {0x820ec000u, 0x024600u, 0x0000200u}, /* WF_LMAC_TOP BN0 (WF_INT) */
    {0x820ed000u, 0x024800u, 0x0000800u}, /* WF_LMAC_TOP BN0 (WF_MIB) */
    {0x820ca000u, 0x026000u, 0x0002000u}, /* WF_LMAC_TOP BN0 (WF_MUCOP) */
    {0x820d0000u, 0x030000u, 0x0010000u}, /* WF_LMAC_TOP (WF_WTBLON) */
    {0x40000000u, 0x070000u, 0x0010000u}, /* WF_UMAC_SYSRAM */
    {0x00400000u, 0x080000u, 0x0010000u}, /* WF_MCU_SYSRAM */
    {0x00410000u, 0x090000u, 0x0010000u}, /* WF_MCU_SYSRAM config */
    {0x820f0000u, 0x0a0000u, 0x0000400u}, /* WF_LMAC_TOP BN1 (WF_CFG) */
    {0x820f1000u, 0x0a0600u, 0x0000200u}, /* WF_LMAC_TOP BN1 (WF_TRB) */
    {0x820f2000u, 0x0a0800u, 0x0000400u}, /* WF_LMAC_TOP BN1 (WF_AGG) */
    {0x820f3000u, 0x0a0c00u, 0x0000400u}, /* WF_LMAC_TOP BN1 (WF_ARB) */
    {0x820f4000u, 0x0a1000u, 0x0000400u}, /* WF_LMAC_TOP BN1 (WF_TMAC) */
    {0x820f5000u, 0x0a1400u, 0x0000800u}, /* WF_LMAC_TOP BN1 (WF_RMAC) */
    {0x820f7000u, 0x0a1e00u, 0x0000200u}, /* WF_LMAC_TOP BN1 (WF_DMA) */
    {0x820f9000u, 0x0a3400u, 0x0000200u}, /* WF_LMAC_TOP BN1 (WF_WTBLOFF) */
    {0x820fa000u, 0x0a4000u, 0x0000200u}, /* WF_LMAC_TOP BN1 (WF_ETBF) */
    {0x820fb000u, 0x0a4200u, 0x0000400u}, /* WF_LMAC_TOP BN1 (WF_LPON) */
    {0x820fc000u, 0x0a4600u, 0x0000200u}, /* WF_LMAC_TOP BN1 (WF_INT) */
    {0x820fd000u, 0x0a4800u, 0x0000800u}, /* WF_LMAC_TOP BN1 (WF_MIB) */
    {0x820c4000u, 0x0a8000u, 0x0004000u}, /* WF_LMAC_TOP BN1 (WF_MUCOP) */
    {0x820b0000u, 0x0ae000u, 0x0001000u}, /* [APB2] WFSYS_ON */
    {0x80020000u, 0x0b0000u, 0x0010000u}, /* WF_TOP_MISC_OFF */
    {0x81020000u, 0x0c0000u, 0x0010000u}, /* WF_TOP_MISC_ON */
    {0x7c020000u, 0x0d0000u, 0x0010000u}, /* CONN_INFRA, wfdma */
    {0x7c060000u, 0x0e0000u, 0x0010000u}, /* CONN_INFRA, conn_host_csr_top */
    {0x7c000000u, 0x0f0000u, 0x0010000u}, /* CONN_INFRA */
};

/* --------------------------------------------------------------------------
 * L1 Remap & Chip Control Registers (mt7925/regs.h)
 * -------------------------------------------------------------------------- */
constexpr uint32_t HIF_REMAP_L1 = 0x155024u;
constexpr uint32_t HIF_REMAP_L1_MASK = 0xffff0000u;
constexpr uint32_t HIF_REMAP_L1_OFFSET = 0x0000ffffu;
constexpr uint32_t HIF_REMAP_BASE_L1 = 0x130000u;

constexpr uint32_t CONN_ON_LPCTL = 0x0e0010u;       /* 0x7c060010 */
constexpr uint32_t CONN_ON_MISC = 0x0e00f0u;        /* 0x7c0600f0 */
constexpr uint32_t WFSYS_SW_RST_B = 0x0f0140u;      /* 0x7c000140 */
constexpr uint32_t CBTOP_RGU_WF_SUBSYS_RST = 0x70028600u;
constexpr uint32_t EMI_PROT_ADDR = 0x18011100u;

constexpr uint32_t PCIE_MAC_INT_ENABLE = 0x010188u;
constexpr uint32_t PCIE_MAC_PM = 0x010194u;
constexpr uint32_t PCIE_MAC_PM_L0S_DIS = bit(8);

/* --------------------------------------------------------------------------
 * WFDMA0 Registers & Rings (MT7925)
 * -------------------------------------------------------------------------- */
constexpr uint32_t WFDMA0_RST = 0x0d4100u;
constexpr uint32_t WFDMA0_MCU_INT_ENA = 0x0d41f4u;
constexpr uint32_t WFDMA0_HOST_INT_STA = 0x0d4200u;
constexpr uint32_t WFDMA0_HOST_INT_ENA = 0x0d4204u;
constexpr uint32_t WFDMA0_GLO_CFG = 0x0d4208u;
constexpr uint32_t WFDMA0_RST_DTX_PTR = 0x0d420cu;
constexpr uint32_t WFDMA0_RST_DRX_PTR = 0x0d4280u;
constexpr uint32_t WFDMA0_PAUSE_RX_Q_45_TH = 0x0d4298u;
constexpr uint32_t WFDMA0_PAUSE_RX_Q_67_TH = 0x0d429cu;
constexpr uint32_t WFDMA0_GLO_CFG_EXT0 = 0x0d42b0u;
constexpr uint32_t WFDMA0_GLO_CFG_EXT1 = 0x0d42b4u;
constexpr uint32_t WFDMA0_PRI_DLY_INT_CFG0 = 0x0d42f0u;
constexpr uint32_t WFDMA0_MCU_DMA0_CTL = 0x002120u;
constexpr uint32_t DMASHDL_CONTROL = 0x0d6004u;

constexpr uint32_t TX_RING0_DATA = 0x0d4300u;
constexpr uint32_t TX_RING15_MCU_WM = 0x0d43f0u; /* MT7925 uses TX15, NOT TX17! */
constexpr uint32_t TX_RING16_FWDL = 0x0d4400u;

constexpr uint32_t RX_RING0_MCU = 0x0d4500u;  /* MCU events + TX_FREE + NORMAL_MCU */
constexpr uint32_t RX_RING2_DATA = 0x0d4520u; /* RX 802.11 data / monitor frames */

constexpr uint32_t TX0_EXT_CTRL = 0x0d4600u;
constexpr uint32_t TX1_EXT_CTRL = 0x0d4604u;
constexpr uint32_t TX2_EXT_CTRL = 0x0d4608u;
constexpr uint32_t TX3_EXT_CTRL = 0x0d460cu;
constexpr uint32_t TX15_EXT_CTRL = 0x0d463cu;
constexpr uint32_t TX16_EXT_CTRL = 0x0d4640u;

constexpr uint32_t RX0_EXT_CTRL = 0x0d4680u;
constexpr uint32_t RX1_EXT_CTRL = 0x0d4684u;
constexpr uint32_t RX2_EXT_CTRL = 0x0d4688u;
constexpr uint32_t RX3_EXT_CTRL = 0x0d468cu;

/* --------------------------------------------------------------------------
 * MAC / MDP / WTBL Registers (BAR0 direct offsets via kFixedMap)
 * -------------------------------------------------------------------------- */
constexpr uint32_t MDP_DCR0 = 0x820cc800u;       /* -> 0x0e800 */
constexpr uint32_t MDP_DCR0_DAMSDU_EN = bit(15);
constexpr uint32_t MDP_DCR0_RX_HDR_TRANS_EN = bit(19);
// Keep the complete native MPDU for the AWDL host, including every A-MSDU
// subframe. Reject a device that cannot hold this mode through readback.
template<class ClearBits, class Read>
bool configure_native_awdl_rx(ClearBits clear_bits, Read read) {
  constexpr uint32_t off = MDP_DCR0_RX_HDR_TRANS_EN | MDP_DCR0_DAMSDU_EN;
  clear_bits(MDP_DCR0, off);
  return (read(MDP_DCR0) & off) == 0;
}

constexpr uint32_t MDP_DCR1 = 0x820cc804u;       /* -> 0x0e804 */
constexpr uint32_t MDP_DCR1_MAX_RX_LEN = genmask(15, 3);

constexpr uint32_t WTBL_UPDATE = 0x820d4380u;    /* -> 0x34380 */
constexpr uint32_t WTBL_UPDATE_ADM_CLEAR = bit(14);
constexpr uint32_t WTBL_UPDATE_BUSY = bit(31);
constexpr uint32_t WTBL_ITCR = 0x820d43b0u;      /* -> 0x343b0 */
constexpr uint32_t WTBL_ITCR_WR = bit(16);
constexpr uint32_t WTBL_ITCR_EXEC = bit(31);
constexpr uint32_t WTBL_ITDR0 = 0x820d43b8u;     /* -> 0x343b8 */
constexpr uint32_t WTBL_ITDR1 = 0x820d43bcu;     /* -> 0x343bc */
constexpr uint32_t WTBL_SPE_IDX_SEL = bit(6);
constexpr uint8_t BASIC_RATES_TBL = 14;          /* slots 14..25 = 12 legacy rates */

constexpr uint32_t FW_PATCH_START_ADDR = 0xe0002800u;

// RX0 TX_FREE event fields and MCU commands used by passive monitor mode.
constexpr uint32_t TXFREE0_MSDU_CNT = genmask(25, 16);
constexpr uint32_t TXFREE1_VER = genmask(19, 16);
constexpr uint32_t TXFREE_INFO_PAIR = bit(31);
constexpr uint32_t TXFREE_INFO_HEADER = bit(30);
constexpr uint32_t TXFREE_INFO_STAT = genmask(29, 28);
constexpr uint32_t TXFREE_INFO_COUNT = genmask(27, 24);
constexpr uint32_t TXFREE_INFO_MSDU_ID = genmask(14, 0);
constexpr uint16_t UNI_CMD_EFUSE_CTRL = 0x2d;

} // namespace mtk::mt7925
