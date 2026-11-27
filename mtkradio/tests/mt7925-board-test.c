/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "board-sar.h"
#include "board-domain.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void Put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static size_t Wire(const uint8_t *table, unsigned count, uint8_t *raw)
{
    const unsigned length = 12 + count * 8;
    Put32(raw, 0x426f6541);
    Put32(raw + 4, length);
    Put32(raw + 8, count);
    for (unsigned i = 0; i < count; ++i) {
        Put32(raw + 12 + 8 * i, 0x00040000);
        Put32(raw + 16 + 8 * i, table[i]);
    }
    return length;
}

static void Decode(bool geographic, const uint8_t *table, unsigned count, LUOBO_MT7925_SAR_TABLE *out)
{
    uint8_t raw[1024];
    assert(LuoboDecodeMt7925Sar(geographic, 0, raw, Wire(table, count, raw), out));
}

static void VerifyLimits(void)
{
    uint8_t dyn1[] = {'M','T','D','S',1,1,
        0,10,26,19,85,127};
    uint8_t geo1[] = {'M','T','G','S',1,3,
        1,5,0,24,1, 2,6,0,37,3, 3,7,0,0xff,12};
    LUOBO_MT7925_BOARD_SAR oem = {0};
    int cap;
    assert(!LuoboMt7925BoardSarLimit(&oem, 36, 0, 40, &cap));
    Decode(false, dyn1, sizeof(dyn1), &oem.Dynamic);
    Decode(true, geo1, sizeof(geo1), &oem.Geographic);
    assert(LuoboMt7925BoardSarLimit(&oem, 36, 1, 40, &cap) && cap == 24);
    assert(LuoboMt7925BoardSarLimit(&oem, 44, 2, 40, &cap) && cap == 29);
    assert(LuoboMt7925BoardSarLimit(&oem, 44, 3, 40, &cap) && cap == 26);
    assert(LuoboMt7925BoardSarLimit(&oem, 44, 0, 18, &cap) && cap == 18);
    assert(LuoboMt7925BoardSarLimit5g(&oem, 149, 0, 40, &cap) && cap == 40);
    dyn1[11] = 0;
    Decode(false, dyn1, sizeof(dyn1), &oem.Dynamic);
    assert(LuoboMt7925BoardSarLimit5g(&oem, 161, 0, 12, &cap) && cap == 0);
    assert(LuoboMt7925BoardSarLimit5g(&oem, 36, 0, 12, &cap) && cap == 12);
    assert(!LuoboMt7925BoardSarLimit(&oem, 149, 0, 40, &cap));
    assert(!LuoboMt7925BoardSarLimit(&oem, 36, 4, 40, &cap));
    assert(!LuoboMt7925BoardSarLimit(&oem, 36, 0, 127, &cap));
    uint8_t dyn2[] = {'M','T','D','S',1,0,1,
        0,27,0xff,3,4,5,6,7,8,9,10,11};
    uint8_t geo2[] = {'M','T','G','S',2,0,3,
        1,5,0,19,1,0,0, 2,6,0,20,3,0,0, 3,7,0,0xff,12,0,0};
    Decode(false, dyn2, sizeof(dyn2), &oem.Dynamic);
    Decode(true, geo2, sizeof(geo2), &oem.Geographic);
    assert(LuoboMt7925BoardSarLimit(&oem, 36, 1, 40, &cap) && cap == 19);
    assert(LuoboMt7925BoardSarLimit(&oem, 36, 0, 40, &cap) && cap == 40);
    dyn2[4] = 0;
    Decode(false, dyn2, sizeof(dyn2), &oem.Dynamic);
    assert(LuoboMt7925BoardSarLimit(&oem, 36, 1, 40, &cap) && cap == 40);
    assert(LuoboDecodeMt7925Sar(false, 0xc0000034u, NULL, 0, &oem.Dynamic));
    assert(LuoboDecodeMt7925Sar(true, 0xc0000034u, NULL, 0, &oem.Geographic));
    assert(LuoboMt7925BoardSarLimit(&oem, 36, 0, -2, &cap) && cap == -2);
}

static void VerifyFailures(void)
{
    uint8_t dyn[] = {'M','T','D','S',1,1,0,40,20,21,22,23};
    uint8_t raw[1024];
    size_t size = Wire(dyn, sizeof(dyn), raw);
    LUOBO_MT7925_SAR_TABLE parsed;
    assert(!LuoboDecodeMt7925Sar(false, 0xc0000034u, raw, size, &parsed));
    assert(!LuoboDecodeMt7925Sar(false, 0xc00000b5u, NULL, 0, &parsed));
    assert(parsed.State == LuoboSarUnknown);
    assert(!LuoboDecodeMt7925Sar(false, 0, raw, size - 1, &parsed));
    raw[20] = 1;
    assert(!LuoboDecodeMt7925Sar(false, 0, raw, size, &parsed));
    raw[20] = 0;
    raw[17] = 1;
    assert(!LuoboDecodeMt7925Sar(false, 0, raw, size, &parsed));
    dyn[5] = 5;
    assert(!LuoboDecodeMt7925Sar(false, 0, raw, Wire(dyn, sizeof(dyn), raw), &parsed));
    dyn[5] = 1;
    dyn[8] = 0xfe;
    assert(!LuoboDecodeMt7925Sar(false, 0, raw, Wire(dyn, sizeof(dyn), raw), &parsed));
    assert(parsed.State == LuoboSarUnknown);
}

static void VerifyWorld(void)
{
    mt_regdomain world = {0};
    world.count = 1;
    world.rules[0] = (mt_reg_rule){5170, 5230, 80, 0xffffffffu, 0};
    assert(LuoboMt7925WorldCandidateMask(&world) == (LuoboWorldChannel36 | LuoboWorldChannel44));
    world.rules[0].start_mhz = 5171;
    assert(LuoboMt7925WorldCandidateMask(&world) == LuoboWorldChannel44);
    world.count = 2;
    world.rules[1] = (mt_reg_rule){5220, 5300, 80, 0, 1u << 7}; /* overlapping NO_IR */
    assert(!LuoboMt7925WorldCandidateMask(&world));
    world.rules[1].flags = 1u << 11;
    assert(LuoboMt7925WorldCandidateMask(&world) == LuoboWorldChannel44);
    world.rules[1].bandwidth_mhz = 10;
    assert(!LuoboMt7925WorldCandidateMask(&world));
    world.count = MT_REGD_MAX_RULES + 1;
    assert(!LuoboMt7925WorldCandidateMask(&world));
}

int main(void)
{
    VerifyLimits();
    VerifyFailures();
    VerifyWorld();
    puts("MT7925 ACPI SAR and 36/44 firmware-domain intersection checks passed.");
    return 0;
}
