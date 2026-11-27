/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2023 MediaTek Inc.
 * MTDS/MTGS formats and per-band calculation: mt76 mt792x_acpi_sar.{c,h}
 * and mac80211.c at 4d7f2bed49cde7f1aa3b6e6e2d0bd8dc869f722e.
 */
#include "board-sar.h"
#include <string.h>

enum { MaxTableBytes = 104 };

static uint32_t Le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool Fits(const uint8_t *p, unsigned n, unsigned header, unsigned stride, unsigned minimum)
{
    if (n < header) return false;
    unsigned count = p[header - 1];
    return count >= minimum && count <= 8 && n - header == count * stride;
}

bool LuoboDecodeMt7925Sar(bool geographic, uint32_t methodStatus,
    const uint8_t *wire, size_t bytes, LUOBO_MT7925_SAR_TABLE *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (methodStatus == UINT32_C(0xc0000034) && bytes == 0) {
        out->State = LuoboSarAbsent;
        return true;
    }
    if (methodStatus || !wire || bytes < 12 || bytes > 12 + MaxTableBytes * 8 ||
        Le32(wire) != UINT32_C(0x426f6541) || Le32(wire + 4) != bytes) return false;

    uint32_t count = Le32(wire + 8);
    if (count < 4 || count > MaxTableBytes || bytes != 12 + count * 8) return false;
    uint8_t table[MaxTableBytes];
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *arg = wire + 12 + 8 * i;
        /* ACPI_METHOD_ARGUMENT_INTEGER, DataLength=4, little-endian ULONG. */
        if (Le32(arg) != UINT32_C(0x00040000) || Le32(arg + 4) > 255) return false;
        table[i] = arg[4];
    }
    const unsigned minimum = geographic ? 3 : 1;
    unsigned header = 6, stride = geographic ? 5 : 6;
    if (!Fits(table, count, header, stride, minimum)) {
        header = 7;
        stride = geographic ? 7 : 12;
        if (!Fits(table, count, header, stride, minimum)) return false;
    }
    for (unsigned row = header; row < count; row += stride) {
        for (unsigned offset = 1; offset < stride; offset += geographic ? 2 : 1) {
            uint8_t power = table[row + offset];
            if (power > 127 && power != 0xff) return false;
        }
    }
    if (geographic) {
        for (unsigned i = 0; i < 3; ++i) {
            /* row id, 2.4 GHz {max,offset}, then 5 GHz {max,offset} */
            out->Geo[i].Limit = table[header + i * stride + 3];
            out->Geo[i].Offset = table[header + i * stride + 4];
        }
        out->State = LuoboSarGeographic;
    } else {
        out->Enabled = table[4] != 0;
        /* mt76 range indices 1..4 (5150-5350, 5350-5470, 5470-5725, 5725-5950 MHz)
           in the first dynamic profile. */
        out->DynamicLimit = table[header + 2];
        for (unsigned i = 0; i < 4; ++i) {
            out->Dynamic5g[i] = table[header + 2 + i];
        }
        out->State = LuoboSarDynamic;
    }
    return true;
}

static bool EvalBoardSar5g(const LUOBO_MT7925_BOARD_SAR *sar, unsigned subband,
    unsigned region, int cap, int *limit)
{
    const LUOBO_MT7925_SAR_TABLE *d = &sar->Dynamic, *g = &sar->Geographic;
    if (d->State != LuoboSarAbsent && d->State != LuoboSarDynamic) return false;
    int board = 127;
    if (d->State == LuoboSarDynamic && d->Enabled) {
        /* mt792x_init_acpi_sar_power consults geography only for an enabled
           dynamic profile. An unread geographical table can tighten this path. */
        if (g->State != LuoboSarAbsent && g->State != LuoboSarGeographic) return false;
        /* Zero is a valid half-dBm ceiling. Only 0xff denotes no limit. */
        uint8_t dyn = subband == 0 ? d->DynamicLimit : d->Dynamic5g[subband];
        board = dyn == 0xff ? 127 : dyn;
        if (g->State == LuoboSarGeographic) {
            unsigned row = region == 1 ? 0 : region == 2 ? 1 : 2;
            if (g->Geo[row].Limit != 0xff) {
                board += g->Geo[row].Offset;
                if (board > g->Geo[row].Limit) board = g->Geo[row].Limit;
            }
        }
    }
    *limit = cap < board ? cap : board;
    return true;
}

bool LuoboMt7925BoardSarLimit(const LUOBO_MT7925_BOARD_SAR *sar, unsigned channel,
    unsigned region, int cap, int *limit)
{
    if (!limit) return false;
    *limit = 0;
    if (!sar || (channel != 36 && channel != 40 && channel != 44 && channel != 48) || region > 3 ||
        cap < -127 || cap > 126) return false;
    return EvalBoardSar5g(sar, 0, region, cap, limit);
}

bool LuoboMt7925BoardSarLimit5g(const LUOBO_MT7925_BOARD_SAR *sar, unsigned channel,
    unsigned region, int cap, int *limit)
{
    if (!limit) return false;
    *limit = 0;
    if (!sar || channel < 36 || channel > 177 || region > 3 ||
        cap < -127 || cap > 126) return false;
    unsigned subband = channel <= 64 ? 0 : (channel <= 144 ? 2 : 3);
    return EvalBoardSar5g(sar, subband, region, cap, limit);
}
