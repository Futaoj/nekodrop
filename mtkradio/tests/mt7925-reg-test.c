/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "station-reg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned Calls, Sku, Last, Selected, Selected44, Nack, Timeout;
    unsigned DomainCount, Mask[2], Indoor[2];
    bool Passive[2], Dual44;
    uint8_t Country[2][2];
    int Target, Target44;
} MCU;

static unsigned Get16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

static int Exchange(void *context, uint32_t command, const uint8_t *p, uint32_t bytes,
    int wait, uint32_t *status)
{
    MCU *m = context;
    assert(wait == 1);
    ++m->Calls;
    if (m->Calls == m->Timeout) return MT_FW_TIMEOUT;
    if (m->Calls == m->Nack) { *status = 0x80000002; return MT_OK; }
    if (command == (MT_MCU_UNI | 0x15)) {
        assert(bytes == 24 || bytes == 32);
        assert(Get16(p + 8) == 2 && Get16(p + 10) == bytes - 8);
        assert(!p[12] && p[13] == (bytes - 16) / 8 && !p[14] && !p[15]);
        assert(p[4] == 1 && p[5] == 1 && p[6] == 1 && !p[7]);
        unsigned n = m->DomainCount++;
        assert(n < 2);
        memcpy(m->Country[n], p, 2);
        m->Passive[n] = p[20] == 2;
        for (unsigned i = 16; i < bytes; i += 8) {
            unsigned ch = Get16(p + i);
            assert(ch == 36 || ch == 44);
            assert(!Get16(p + i + 2) && !Get16(p + i + 6) && !(p[i + 5] & ~2u));
            assert(p[i + 4] == (m->Passive[n] ? 2 : 0));
            m->Mask[n] |= ch == 36 ? LuoboWorldChannel36 : LuoboWorldChannel44;
            if (p[i + 5] == 2) m->Indoor[n] |= ch == 36 ? LuoboWorldChannel36 : LuoboWorldChannel44;
        }
        if (!m->Passive[n]) assert(m->Last == 177 && m->Sku == 17 && m->Selected == 1);
    } else {
        assert(command == (MT_MCU_UNI | 0x2c));
        assert(m->DomainCount == 1 && m->Passive[0]);
        assert(bytes == 52u + 450u * p[12] && Get16(p + 4) == 1 && Get16(p + 6) == bytes - 4);
        assert(p[12] >= 1 && p[12] <= 3 && p[13] == 2 && !memcmp(m->Country[0], p + 16, 2));
        for (unsigned j = 0; j < p[12]; ++j) {
            const uint8_t *entry = p + 52 + j * 450;
            assert(entry[0] > m->Last);
            m->Last = entry[0];
            int expected = entry[0] == 36 ? m->Target :
                (m->Dual44 && entry[0] == 44) ? m->Target44 : -127;
            for (unsigned i = 0; i < 449; ++i) {
                bool valid = (i >= 4 && i < 12) || (i >= 44 && i < 61) || i >= 109 ||
                    (i >= 61 && i < 109 && (i - 61) % 12 < 10);
                if (entry[i + 1] != (valid ? (uint8_t)expected : 127)) {
                    fprintf(stderr, "SKU mismatch: call=%u channel=%u rate=%u actual=%u expected=%u limit=%d\n",
                        m->Calls, entry[0], i, entry[i + 1], valid ? (uint8_t)expected : 127, expected);
                }
                assert(entry[i + 1] == (valid ? (uint8_t)expected : 127));
            }
            if (entry[0] == 36) ++m->Selected;
            if (entry[0] == 44 && m->Dual44) ++m->Selected44;
        }
        assert(p[14] == (m->Last == 177));
        ++m->Sku;
    }
    *status = 0;
    return MT_OK;
}

static mt_regdomain Domain(void)
{
    mt_regdomain d = {0};
    d.count = 1;
    d.rules[0] = (mt_reg_rule){5170, 5250, 80, UINT32_MAX, 0};
    return d;
}

static void Passive(void)
{
    MCU m = {0};
    mt_fw_transport port = {&m, Exchange, NULL, NULL};
    mt_regdomain d = Domain();
    d.rules[0].flags = (1u << 7) | (1u << 11) | (1u << 3);
    unsigned applied = 99;
    assert(LuoboMt7925SetPassiveWorldDomain(&port, &d, &applied) == MT_OK);
    assert(applied == 3 && m.DomainCount == 1 && m.Mask[0] == 3 && m.Indoor[0] == 3 && m.Passive[0]);
    assert(!memcmp(m.Country[0], "00", 2));
    d.count = 2;
    d.rules[1] = (mt_reg_rule){5179,5181,20,1,1u<<4};
    assert(LuoboMt7925SetPassiveWorldDomain(&port, &d, &applied) == MT_OK);
    assert(applied == 2 && m.Mask[1] == 2);
    d.rules[1].start_mhz = 1;
    d.rules[1].end_mhz = 6000;
    assert(LuoboMt7925SetPassiveWorldDomain(&port, &d, &applied) == MT_PENDING && !applied);
    d.rules[1].end_mhz = 0;
    assert(LuoboMt7925SetPassiveWorldDomain(&port, &d, &applied) == MT_INVALID && !applied);
    assert(m.Calls == 2);
    d = Domain();
    m.Nack = 3;
    assert(LuoboMt7925SetPassiveWorldDomain(&port, &d, &applied) == MT_FW_PROTOCOL && !applied);
    m.Timeout = 4;
    assert(LuoboMt7925SetPassiveWorldDomain(&port, &d, &applied) == MT_FW_TIMEOUT && !applied);
}

static const uint8_t Country[] = {'U','S'};
/* Channels 36/40/44/48, 20 dBm AP ceiling, 3 dB local power constraint. */
static const uint8_t Ies[] = {0,1,'a',7,6,'U','S',' ',36,4,20,32,1,3};

static void PowerElements(void)
{
    int cap = 91;
    assert(LuoboMt7925ApPowerLimit(Ies,sizeof(Ies),Country,44,&cap) && cap == 34);
    assert(!LuoboMt7925ApPowerLimit(Ies,sizeof(Ies)-1,Country,36,&cap) && !cap);
    assert(!LuoboMt7925ApPowerLimit(Ies,sizeof(Ies),(const uint8_t *)"DE",36,&cap));
    uint8_t mixed[] = {7,10,'U','S','I',1,11,19,36,1,0xfe,0};
    assert(LuoboMt7925ApPowerLimit(mixed,sizeof(mixed),Country,36,&cap) && cap == -4);
    assert(!LuoboMt7925ApPowerLimit(mixed,sizeof(mixed),Country,44,&cap));
    mixed[11] = 1;
    assert(!LuoboMt7925ApPowerLimit(mixed,sizeof(mixed),Country,36,&cap));
    mixed[11] = 0; mixed[5] = 201;
    assert(!LuoboMt7925ApPowerLimit(mixed,sizeof(mixed),Country,36,&cap));
    uint8_t duplicate[2*sizeof(Ies)];
    memcpy(duplicate,Ies,sizeof(Ies)); memcpy(duplicate+sizeof(Ies),Ies,sizeof(Ies));
    assert(!LuoboMt7925ApPowerLimit(duplicate,sizeof(duplicate),Country,36,&cap));
}

static void Station(void)
{
    MCU m = {0};
    mt_fw_transport port = {&m, Exchange, NULL, NULL};
    LUOBO_MT7925_BOARD_SAR sar = {0};
    mt_regdomain d = Domain();
    LUOBO_MT7925_STA_CHANNEL_POLICY p = {{'U','S'},36,true,40,3,Ies,sizeof(Ies)};
    LUOBO_MT7925_STA_POWER_RESULT result = {0};
    assert(LuoboMt7925ProgramStationChannel(&port,&d,&sar,&p,&result) == MT_INVALID);
    assert(!m.Calls && !result.Attempted);
    sar.Dynamic.State = LuoboSarDynamic;
    sar.Dynamic.Enabled = true;
    sar.Dynamic.DynamicLimit = 30;
    sar.Geographic.State = LuoboSarAbsent;
    p.HostCapKnown = false;
    assert(LuoboMt7925ProgramStationChannel(&port,&d,&sar,&p,&result) == MT_INVALID);
    p.HostCapKnown = true;
    d.rules[0].flags = 1u << 7;
    assert(LuoboMt7925ProgramStationChannel(&port,&d,&sar,&p,&result) == MT_PENDING);
    assert(!m.Calls && !result.Attempted);
    d.rules[0].flags = 0;
    m.Target = 30;
    assert(LuoboMt7925ProgramStationChannel(&port,&d,&sar,&p,&result) == MT_OK);
    assert(m.Calls == 19 && m.Mask[0] == 1 && m.Mask[1] == 1 && !m.Passive[1]);
    assert(result.ChannelProgrammed == 36 && result.SkuBatchesAck == 17 && result.LimitHalfDbm == 30);
    assert(LuoboMt7925ProgramStationChannel(&port,&d,&sar,&p,&result) == MT_INVALID);
    for (unsigned fault = 1; fault <= 19; ++fault) {
        memset(&m,0,sizeof(m)); memset(&result,0,sizeof(result));
        m.Nack = fault; m.Target = 30;
        assert(LuoboMt7925ProgramStationChannel(&port,&d,&sar,&p,&result) == MT_FW_PROTOCOL);
        assert(result.Attempted && !result.ChannelProgrammed && m.Calls == fault);
        assert(LuoboMt7925ProgramStationChannel(&port,&d,&sar,&p,&result) == MT_INVALID && m.Calls == fault);
    }

    unsigned passiveMask = 0, activeMask = 0;
    assert(LuoboMt7925QueryChannelEligibility(&d, &passiveMask, &activeMask) == MT_OK);
    assert(passiveMask == 3 && activeMask == 3);
    memset(&m, 0, sizeof(m));
    memset(&result, 0, sizeof(result));
    sar.Dynamic.DynamicLimit = 28;
    m.Dual44 = true; m.Target = 28; m.Target44 = 22;
    p.ApEirpBackoffHalfDb = 0;
    assert(LuoboMt7925ProgramDualChannel(&port, &d, &sar, &p, 28, &result) == MT_OK);
    assert(m.Calls == 19 && m.Mask[0] == 3 && m.Mask[1] == 3 && m.Selected == 1 && m.Selected44 == 1);
    assert(result.ChannelProgrammed == 36 && result.PassiveAck == 3 && result.LimitHalfDbm == 28);
}

static void StandaloneAwdl(void)
{
    MCU m = {0};
    mt_fw_transport port = {&m, Exchange, NULL, NULL};
    mt_regdomain d = Domain();
    d.rules[0].flags = 1u << 3; /* no outdoor transmissions */
    LUOBO_MT7925_BOARD_SAR sar = {0};
    LUOBO_MT7925_STA_POWER_RESULT result = {0};
    assert(LuoboMt7925ProgramAwdlChannels(&port, &d, &sar, Country, 12, true, true, &result) == MT_PENDING);
    assert(!m.Calls && !result.Attempted);
    sar.Dynamic.State = sar.Geographic.State = LuoboSarAbsent;
    assert(LuoboMt7925ProgramAwdlChannels(&port, &d, &sar, Country, 12, false, true, &result) == MT_PENDING);
    assert(!m.Calls && !result.Attempted);
    m.Dual44 = true; m.Target = m.Target44 = 12;
    assert(LuoboMt7925ProgramAwdlChannels(&port, &d, &sar, Country, 12, true, true, &result) == MT_OK);
    assert(result.ChannelProgrammed == 36 && result.PassiveAck == 3 && result.SkuBatchesAck == 17);
    assert(result.LimitHalfDbm == 12 && !m.Passive[1] && m.Indoor[1] == 3);
}

static int ExchangeWide80(void *context, uint32_t command, const uint8_t *p, uint32_t bytes,
    int wait, uint32_t *status)
{
    MCU *m = context;
    assert(wait == 1);
    ++m->Calls;
    if (command == (MT_MCU_UNI | 0x15)) {
        assert(bytes == 48 && Get16(p + 8) == 2 && Get16(p + 10) == 40);
        assert(p[4] == 1 && p[5] == 2 && p[6] == 1 && p[13] == 4);
        unsigned n = m->DomainCount++;
        assert(n < 2);
        m->Passive[n] = p[20] == 2;
        assert(Get16(p + 16) == 36 && Get16(p + 24) == 40 && Get16(p + 32) == 44 && Get16(p + 40) == 48);
    } else {
        assert(command == (MT_MCU_UNI | 0x2c));
        for (unsigned j = 0; j < p[12]; ++j) {
            const uint8_t *entry = p + 52 + j * 450;
            int expected = (entry[0] >= 36 && entry[0] <= 48) ? m->Target : -127;
            assert(entry[5] == (uint8_t)expected);
            if (entry[0] == 42) ++m->Selected44;
        }
        ++m->Sku;
    }
    *status = 0;
    return MT_OK;
}

static void StandaloneAwdl80(void)
{
    MCU m = {0};
    mt_fw_transport port = {&m, ExchangeWide80, NULL, NULL};
    mt_regdomain d = Domain();
    d.rules[0].flags = 1u << 3;
    LUOBO_MT7925_BOARD_SAR sar = {0};
    sar.Dynamic.State = sar.Geographic.State = LuoboSarAbsent;
    LUOBO_MT7925_STA_POWER_RESULT result = {0};
    m.Target = 12;
    assert(LuoboMt7925ProgramAwdlChannels80(&port, &d, &sar, Country, 36, true, true, &result) == MT_OK);
    assert(result.ChannelProgrammed == 36 && result.AwdlPrimary == 44 && result.SkuBatchesAck == 17 && result.LimitHalfDbm == 12);
    assert(m.Calls == 19 && m.DomainCount == 2 && m.Passive[0] && !m.Passive[1] && m.Selected44 == 1);
}

static int Exchange149(void *context, uint32_t command, const uint8_t *p, uint32_t bytes,
    int wait, uint32_t *status)
{
    MCU *m = context;
    assert(wait == 1);
    ++m->Calls;
    if (command == (MT_MCU_UNI | 0x15)) {
        static const uint8_t expected[] = {36, 149, 153, 157, 161};
        assert(bytes == 56 && p[13] == 5 && p[5] == 2 && !memcmp(p,"CN",2));
        for (unsigned i = 0; i < sizeof(expected); ++i) {
            unsigned flags = Get16(p + 20 + 8 * i);
            assert(Get16(p + 16 + 8 * i) == expected[i]);
            assert(((flags & 2) != 0) == (m->DomainCount == 0));
            assert((flags & (1u << 7)) == (i == 0 ? 1u << 7 : 0));
            assert(flags & (1u << 9));
        }
        ++m->DomainCount;
    } else {
        assert(command == (MT_MCU_UNI | 0x2c) && p[13] == 2 && !memcmp(p + 16,"CN",2));
        assert(bytes == 52u + p[12] * 450u);
        for (unsigned j = 0; j < p[12]; ++j) {
            const uint8_t *entry = p + 52 + j * 450;
            bool inWide = entry[0] >= 149 && entry[0] <= 161;
            int expected = inWide ? m->Target : entry[0] == 36 ? 12 : -127;
            assert(entry[5] == (uint8_t)expected && entry[110] == (uint8_t)expected);
            if (inWide) ++m->Selected;
            if (entry[0] == 155) ++m->Selected44;
        }
        ++m->Sku;
    }
    *status = m->Nack && m->Nack == m->Calls;
    return MT_OK;
}

static void StandaloneAwdl149(void)
{
    const uint8_t cn[] = {'C', 'N'};
    MCU m = {0};
    mt_fw_transport port = {&m, Exchange149, NULL, NULL};
    mt_regdomain d = Domain();
    d.count = 2;
    d.rules[0] = (mt_reg_rule){5170,5190,20,UINT32_MAX,1u << 3};
    d.rules[1] = (mt_reg_rule){5735,5815,80,UINT32_MAX,0};
    LUOBO_MT7925_BOARD_SAR sar = {0};
    LUOBO_MT7925_STA_POWER_RESULT result = {0};
    assert(LuoboMt7925ProgramAwdlChannels80ForPrimary(&port,&d,&sar,cn,149,12,true,true,&result) == MT_PENDING);
    sar.Dynamic.State = sar.Geographic.State = LuoboSarAbsent;
    assert(LuoboMt7925ProgramAwdlChannels80ForPrimary(&port,&d,&sar,cn,149,12,false,true,&result) == MT_PENDING);
    assert(LuoboMt7925ProgramAwdlChannels80ForPrimary(&port,&d,&sar,Country,149,12,true,true,&result) == MT_PENDING);
    --d.rules[1].end_mhz;
    assert(LuoboMt7925ProgramAwdlChannels80ForPrimary(&port,&d,&sar,cn,149,12,true,true,&result) == MT_PENDING);
    ++d.rules[1].end_mhz;
    d.rules[0].flags |= 1u << 7;
    assert(LuoboMt7925ProgramAwdlChannels80ForPrimary(&port,&d,&sar,cn,149,12,true,true,&result) == MT_PENDING);
    d.rules[0].flags = 1u << 3;
    d.count = 3;
    const unsigned blocked[] = {4, 7, 8, 13, 14, 15, 23};
    for (unsigned i = 0; i < sizeof(blocked) / sizeof(blocked[0]); ++i) {
        d.rules[2] = (mt_reg_rule){5800,5801,80,UINT32_MAX,1u << blocked[i]};
        assert(LuoboMt7925ProgramAwdlChannels80ForPrimary(&port,&d,&sar,cn,149,12,true,true,&result) == MT_PENDING);
    }
    d.rules[2].flags = 0;
    d.rules[2].bandwidth_mhz = 40;
    assert(LuoboMt7925ProgramAwdlChannels80ForPrimary(&port,&d,&sar,cn,149,12,true,true,&result) == MT_PENDING);
    assert(!m.Calls && !result.Attempted);
    d.count = 2;
    sar.Dynamic.State = LuoboSarDynamic;
    sar.Dynamic.Enabled = true;
    sar.Dynamic.DynamicLimit = 29;
    sar.Dynamic.Dynamic5g[3] = 0;
    m.Target = 0; /* fourth subband's zero is a real limit, independent of home */
    assert(LuoboMt7925ProgramAwdlChannels80ForPrimary(&port,&d,&sar,cn,149,36,true,true,&result) == MT_OK);
    assert(m.Calls == 19 && m.Sku == 17 && m.DomainCount == 2 && m.Selected == 7 && m.Selected44 == 1);
    assert(result.PassiveAck == 5 && result.ChannelProgrammed == 36 && result.AwdlPrimary == 149 && result.LimitHalfDbm == 0);
    memset(&m,0,sizeof(m)); memset(&result,0,sizeof(result));
    sar.Dynamic.State = LuoboSarAbsent;
    m.Target = 12; m.Nack = 19;
    assert(LuoboMt7925ProgramAwdlChannels80ForPrimary(&port,&d,&sar,cn,149,36,true,true,&result) == MT_FW_PROTOCOL);
    assert(result.Attempted && result.SkuBatchesAck == 17 && !result.AwdlPrimary && !result.ChannelProgrammed);
}

int main(void)
{
    Passive(); PowerElements(); Station(); StandaloneAwdl(); StandaloneAwdl80(); StandaloneAwdl149();
    puts("MT7925 passive firmware domain, selected AP limits and ordered station SKU checks passed.");
    return 0;
}
