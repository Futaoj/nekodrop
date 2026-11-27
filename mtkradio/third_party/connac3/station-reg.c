/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* MT7925 command layouts from MediaTek's mt7925_mcu_set_channel_domain and
   mt7925_mcu_rate_txpower_band, mt76 4d7f2bed. Hardware channel flags are
   ieee80211_channel_flags; firmware input rules are nl80211_reg_rule_flags. */
#include "station-reg.h"
#include <string.h>

enum {
    SetDomainInfo = 0x15, SetPowerLimit = 0x2c, DomainHeader = 16, DomainEntry = 8,
    RuleNoCck = 1u << 1, RuleNoOutdoor = 1u << 3, RuleNoIr = 1u << 7, RuleNoIbss = 1u << 8,
    RuleAutoBw = 1u << 11, RuleIrConcurrent = 1u << 12,
    RuleNoWideOrModern = (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19),
    RuleActiveAllow = RuleAutoBw | RuleNoOutdoor | RuleNoCck | RuleIrConcurrent | RuleNoWideOrModern,
    ChanNoIr = 1u << 1, ChanIndoorOnly = 1u << 9
};
static const uint8_t Channels[] = {36, 44};

static void Le16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static unsigned ChannelBit(unsigned channel)
{
    return channel == 36 ? LuoboWorldChannel36 : channel == 44 ? LuoboWorldChannel44 : 0;
}

static int Send(const mt_fw_transport *transport, unsigned command, const uint8_t *data, unsigned bytes)
{
    uint32_t status = UINT32_MAX;
    int error = transport->send(transport->context, MT_MCU_UNI | command, data, bytes, 1, &status);
    if (error) return error;
    return status == 0 ? MT_OK : MT_FW_PROTOCOL;
}

static int Eligible(const mt_regdomain *domain, unsigned allowFlags, unsigned *mask, unsigned *indoor)
{
    unsigned covered = 0, blocked = 0;
    *mask = 0;
    *indoor = 0;
    if (!domain || !domain->count || domain->count > MT_REGD_MAX_RULES || domain->dfs_region > 3)
        return MT_INVALID;
    for (unsigned r = 0; r < domain->count; ++r) {
        const mt_reg_rule *rule = &domain->rules[r];
        if (!rule->start_mhz || rule->start_mhz >= rule->end_mhz ||
            !rule->bandwidth_mhz || rule->bandwidth_mhz > 320) return MT_INVALID;
        for (unsigned i = 0; i < sizeof(Channels); ++i) {
            unsigned bit = ChannelBit(Channels[i]);
            unsigned center = 5000u + 5u * Channels[i];
            if (rule->end_mhz <= center - 10 || rule->start_mhz >= center + 10) continue;
            if ((rule->flags & ~allowFlags) || rule->bandwidth_mhz < 20) blocked |= bit;
            if (rule->flags & RuleNoOutdoor) *indoor |= bit;
            if (rule->start_mhz <= center - 10 && rule->end_mhz >= center + 10) covered |= bit;
        }
    }
    *mask = covered & ~blocked;
    return *mask ? MT_OK : MT_PENDING;
}

static int SetDomain(const mt_fw_transport *transport, const uint8_t country[2],
    unsigned mask, unsigned indoor, bool passive)
{
    uint8_t request[DomainHeader + 2 * DomainEntry] = {0};
    unsigned bytes = DomainHeader;
    request[0] = country[0];
    request[1] = country[1];
    /* BW_20M = 1, including empty bands. The TLV count for other bands is 0. */
    request[4] = request[5] = request[6] = 1;
    Le16(request + 8, 2);
    for (unsigned i = 0; i < sizeof(Channels); ++i) {
        if (!(mask & ChannelBit(Channels[i]))) continue;
        Le16(request + bytes, Channels[i]);
        unsigned flags = passive ? ChanNoIr : 0;
        if (indoor & ChannelBit(Channels[i])) flags |= ChanIndoorOnly;
        Le16(request + bytes + 4, flags);
        bytes += DomainEntry;
        ++request[13];
    }
    Le16(request + 10, bytes - 8);
    return Send(transport, SetDomainInfo, request, bytes);
}

int LuoboMt7925SetPassiveWorldDomain(const mt_fw_transport *transport,
    const mt_regdomain *world, unsigned *applied)
{
    static const uint8_t alpha[2] = {'0','0'};
    if (!applied) return MT_INVALID;
    *applied = 0;
    if (!transport || !transport->send) return MT_INVALID;
    unsigned mask, indoor;
    int error = Eligible(world, RuleAutoBw | RuleNoIr | RuleNoIbss | RuleNoOutdoor, &mask, &indoor);
    if (!error) error = SetDomain(transport, alpha, mask, indoor, true);
    if (!error) *applied = mask;
    return error;
}

int LuoboMt7925QueryChannelEligibility(const mt_regdomain *domain,
    unsigned *passiveMask, unsigned *activeMask)
{
    if (!passiveMask || !activeMask) return MT_INVALID;
    *passiveMask = 0;
    *activeMask = 0;
    unsigned indoor = 0;
    int passive = Eligible(domain, RuleActiveAllow | RuleNoIr | RuleNoIbss,
        passiveMask, &indoor);
    if (passive && passive != MT_PENDING) return passive;
    int active = Eligible(domain, RuleActiveAllow, activeMask, &indoor);
    if (active && active != MT_PENDING) return active;
    return (*passiveMask || *activeMask) ? MT_OK : MT_PENDING;
}

static bool CountryLimit(const uint8_t *p, unsigned length, const uint8_t country[2],
    unsigned channel, int *limit)
{
    if (length < 6 || (length & 1) || p[0] != country[0] || p[1] != country[1] ||
        (p[2] != ' ' && p[2] != 'I' && p[2] != 'O')) return false;
    unsigned bytes = length - 3;
    if (bytes % 3 == 1 && p[length - 1] == 0) --bytes;
    if (bytes % 3) return false;
    bool found = false;
    for (unsigned i = 3; i < 3 + bytes; i += 3) {
        unsigned first = p[i], count = p[i + 1];
        /* Operating-class extensions change the meaning of following triplets. */
        if (!first || first >= 201 || !count) return false;
        unsigned step = first <= 14 ? 1 : 4;
        unsigned last = first + (count - 1) * step;
        if (last > (first <= 14 ? 14u : 200u)) return false;
        if (channel < first || channel > last || (channel - first) % step) continue;
        if (found) return false; /* contradictory or repeated AP limits */
        int raw = p[i + 2];
        *limit = raw > 127 ? raw - 256 : raw;
        found = true;
    }
    return found;
}

static bool ParseApLimits(const uint8_t *ies, size_t bytes, const uint8_t country[2],
    uint8_t channel, bool *haveCountry, int *maximum, int *constraint)
{
    if (!ies || !bytes || bytes > 2304 || !country || !ChannelBit(channel) ||
        country[0] < 'A' || country[0] > 'Z' || country[1] < 'A' || country[1] > 'Z') return false;
    bool haveConstraint = false;
    *haveCountry = false;
    *maximum = 0;
    *constraint = 0;
    while (bytes) {
        if (bytes < 2 || ies[1] > bytes - 2) return false;
        unsigned id = ies[0], size = ies[1];
        if (id == 7) { /* Country Information element */
            if (*haveCountry || !CountryLimit(ies + 2, size, country, channel, maximum)) return false;
            *haveCountry = true;
        } else if (id == 32) { /* Local Power Constraint, dB */
            if (haveConstraint || size != 1) return false;
            *constraint = ies[2];
            haveConstraint = true;
        }
        ies += 2 + size;
        bytes -= 2 + size;
    }
    return true;
}

bool LuoboMt7925ApPowerLimit(const uint8_t *ies, size_t bytes, const uint8_t country[2],
    uint8_t channel, int *cap)
{
    if (!cap) return false;
    *cap = 0;
    bool haveCountry;
    int maximum, constraint;
    if (!ParseApLimits(ies, bytes, country, channel, &haveCountry, &maximum, &constraint) ||
        !haveCountry) return false;
    int halfDbm = 2 * (maximum - constraint);
    if (halfDbm < -127 || halfDbm > 126) return false;
    *cap = halfDbm;
    return true;
}

enum { SkuRates = 449, SkuEntry = SkuRates + 1, SkuHeader = 52, BatchLimit = 3 };

static void SetRateCeilingBand(uint8_t rates[SkuRates], unsigned band, int limit)
{
    uint8_t v = (uint8_t)limit;
    /* Preserve reserved rate positions as 127, matching the firmware ABI.
       Band 1 (2.4 GHz) also sets CCK rates 0..3; band 2 (5 GHz) leaves them 127. */
    memset(rates, 127, SkuRates);
    if (band == 1) memset(rates, v, 4);
    memset(rates + 4, v, 8);
    memset(rates + 44, v, 17);
    for (unsigned i = 0; i < 4; ++i) memset(rates + 61 + i * 12, v, 10);
    memset(rates + 109, v, SkuRates - 109);
}

static void SetRateCeiling(uint8_t rates[SkuRates], int limit)
{
    SetRateCeilingBand(rates, 2, limit);
}

static int InstallPowerDual(const mt_fw_transport *transport, const uint8_t country[2],
    uint8_t primary, int primaryLimit, bool include44, int limit44,
    LUOBO_MT7925_STA_POWER_RESULT *result)
{
    static const uint8_t ordered[] = {
        36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,100,102,104,106,
        108,110,112,114,116,118,120,122,124,126,128,132,134,136,138,140,
        142,144,149,151,153,155,157,159,161,163,165,167,169,173,175,177
    };
    uint8_t request[SkuHeader + BatchLimit * SkuEntry] = {0};
    Le16(request + 4, 1);
    request[13] = 2; /* firmware band selector for 5 GHz */
    memcpy(request + 16, country, 2);
    for (unsigned i = 0; i < sizeof(ordered); i += BatchLimit) {
        unsigned count = (unsigned)sizeof(ordered) - i;
        if (count > BatchLimit) count = BatchLimit;
        unsigned bytes = SkuHeader + count * SkuEntry;
        Le16(request + 6, bytes - 4);
        request[12] = (uint8_t)count;
        request[14] = (uint8_t)(i + count == sizeof(ordered));
        for (unsigned j = 0; j < count; ++j) {
            uint8_t *entry = request + SkuHeader + j * SkuEntry;
            entry[0] = ordered[i + j];
            int ceiling = -127;
            if (entry[0] == primary) ceiling = primaryLimit;
            else if (include44 && entry[0] == 44) ceiling = limit44;
            SetRateCeiling(entry + 1, ceiling);
        }
        int error = Send(transport, SetPowerLimit, request, bytes);
        if (error) return error;
        ++result->SkuBatchesAck;
    }
    return MT_OK;
}

static int InstallPower(const mt_fw_transport *transport, const LUOBO_MT7925_STA_CHANNEL_POLICY *policy,
    int limit, LUOBO_MT7925_STA_POWER_RESULT *result)
{
    return InstallPowerDual(transport, policy->Country, policy->Channel, limit, false, -127, result);
}

int LuoboMt7925ProgramStationChannel(const mt_fw_transport *transport,
    const mt_regdomain *domain, const LUOBO_MT7925_BOARD_SAR *sar,
    const LUOBO_MT7925_STA_CHANNEL_POLICY *policy, LUOBO_MT7925_STA_POWER_RESULT *result)
{
    if (!result || result->Attempted || !transport || !transport->send || !policy ||
        !policy->HostCapKnown || policy->ApEirpBackoffHalfDb > 253 ||
        policy->HostCapHalfDbm < -127 || policy->HostCapHalfDbm > 126) return MT_INVALID;
    memset(result, 0, sizeof(*result));
    unsigned mask, indoor, channel = ChannelBit(policy->Channel);
    int error = Eligible(domain, RuleActiveAllow, &mask, &indoor);
    if (error) return error;
    if (!channel || !(mask & channel)) return MT_PENDING;
    bool haveCountry;
    int apMaximum, constraint, limit;
    if (!ParseApLimits(policy->SelectedApIes, policy->SelectedApIesBytes, policy->Country,
        policy->Channel, &haveCountry, &apMaximum, &constraint)) return MT_INVALID;
    int ceiling = policy->HostCapHalfDbm;
    if (haveCountry) {
        /* The advertised ceiling may be radiated power; convert before
           comparing with the board's independent conducted ceiling. */
        int ap = 2 * apMaximum - (int)policy->ApEirpBackoffHalfDb;
        if (ap < ceiling) ceiling = ap;
    }
    ceiling -= 2 * constraint;
    if (!LuoboMt7925BoardSarLimit(sar, policy->Channel, domain->dfs_region, ceiling, &limit))
        return MT_INVALID;

    result->Attempted = true;
    error = SetDomain(transport, policy->Country, channel, indoor, true);
    if (error) return error;
    result->PassiveAck = channel;
    error = InstallPower(transport, policy, limit, result);
    if (error) return error;
    error = SetDomain(transport, policy->Country, channel, indoor, false);
    if (!error) {
        result->LimitHalfDbm = limit;
        result->ChannelProgrammed = policy->Channel;
    }
    return error;
}

int LuoboMt7925ProgramDualChannel(const mt_fw_transport *transport,
    const mt_regdomain *domain, const LUOBO_MT7925_BOARD_SAR *sar,
    const LUOBO_MT7925_STA_CHANNEL_POLICY *homePolicy, int peer44HostCapHalfDbm,
    LUOBO_MT7925_STA_POWER_RESULT *result)
{
    if (!result || result->Attempted || !transport || !transport->send || !homePolicy ||
        homePolicy->Channel != 36 || !homePolicy->HostCapKnown ||
        homePolicy->ApEirpBackoffHalfDb > 253 ||
        homePolicy->HostCapHalfDbm < -127 || homePolicy->HostCapHalfDbm > 126 ||
        peer44HostCapHalfDbm < -127 || peer44HostCapHalfDbm > 126) return MT_INVALID;
    memset(result, 0, sizeof(*result));
    unsigned mask = 0, indoor = 0;
    const unsigned required = LuoboWorldChannel36 | LuoboWorldChannel44;
    int error = Eligible(domain, RuleActiveAllow, &mask, &indoor);
    if (error) return error;
    if ((mask & required) != required) return MT_PENDING;

    bool haveCountry36 = false, haveCountry44 = false;
    int apMax36 = 0, constraint36 = 0, apMax44 = 0, constraint44 = 0;
    int limit36 = 0, limit44 = 0;
    if (!ParseApLimits(homePolicy->SelectedApIes, homePolicy->SelectedApIesBytes,
            homePolicy->Country, 36, &haveCountry36, &apMax36, &constraint36))
        return MT_INVALID;
    if (haveCountry36 && !ParseApLimits(homePolicy->SelectedApIes, homePolicy->SelectedApIesBytes,
            homePolicy->Country, 44, &haveCountry44, &apMax44, &constraint44))
        return MT_PENDING;
    int ceiling36 = homePolicy->HostCapHalfDbm;
    if (haveCountry36) {
        int ap = 2 * apMax36 - (int)homePolicy->ApEirpBackoffHalfDb;
        if (ap < ceiling36) ceiling36 = ap;
    }
    ceiling36 -= 2 * constraint36;
    int ceiling44 = peer44HostCapHalfDbm;
    if (haveCountry44) {
        int ap = 2 * apMax44 - (int)homePolicy->ApEirpBackoffHalfDb;
        if (ap < ceiling44) ceiling44 = ap;
        ceiling44 -= 2 * constraint44;
    }
    if (!LuoboMt7925BoardSarLimit(sar, 36, domain->dfs_region, ceiling36, &limit36) ||
        !LuoboMt7925BoardSarLimit(sar, 44, domain->dfs_region, ceiling44, &limit44))
        return MT_PENDING;

    result->Attempted = true;
    error = SetDomain(transport, homePolicy->Country, required, indoor, true);
    if (error) return error;
    result->PassiveAck = required;
    error = InstallPowerDual(transport, homePolicy->Country, 36, limit36, true, limit44, result);
    if (error) return error;
    error = SetDomain(transport, homePolicy->Country, required, indoor, false);
    if (!error) {
        result->LimitHalfDbm = limit36;
        result->ChannelProgrammed = 36;
    }
    return error;
}

int LuoboMt7925ProgramAwdlChannels(const mt_fw_transport *transport,
    const mt_regdomain *domain, const LUOBO_MT7925_BOARD_SAR *sar,
    const uint8_t country[2], int hostCap, bool indoorVerified, bool activate,
    LUOBO_MT7925_STA_POWER_RESULT *result)
{
    const unsigned requested = LuoboWorldChannel36 | LuoboWorldChannel44;
    if (!result || result->Attempted || !transport || !transport->send || !country ||
        country[0] < 'A' || country[0] > 'Z' || country[1] < 'A' || country[1] > 'Z' ||
        hostCap < -127 || hostCap > 126) return MT_INVALID;
    unsigned active = 0, indoor = 0;
    int error = Eligible(domain, RuleActiveAllow, &active, &indoor);
    if (error) return error;
    if ((active & requested) != requested || (activate && !indoorVerified && (indoor & requested)))
        return MT_PENDING;
    int limit36, limit44;
    if (!LuoboMt7925BoardSarLimit(sar, 36, domain->dfs_region, hostCap, &limit36) ||
        !LuoboMt7925BoardSarLimit(sar, 44, domain->dfs_region, hostCap, &limit44)) return MT_PENDING;
    memset(result, 0, sizeof(*result));
    result->Attempted = true;
    error = SetDomain(transport, country, requested, indoor, true);
    if (error) return error;
    result->PassiveAck = requested;
    error = InstallPowerDual(transport, country, 36, limit36, true, limit44, result);
    if (error) return error;
    result->LimitHalfDbm = limit36 < limit44 ? limit36 : limit44;
    if (activate) {
        error = SetDomain(transport, country, requested, indoor, false);
        if (!error) result->ChannelProgrammed = 36;
    }
    return error;
}

/* Require one decoded firmware rule covering the entire occupied interval.
   Every overlapping rule must also permit this mode. In particular NO_80,
   either HT40 restriction, DFS, NO_IR and unknown flags block wide operation. */
static int EligibleSpan(const mt_regdomain *domain, unsigned centerMhz, unsigned width, bool *indoor)
{
    const unsigned low = centerMhz - width / 2, high = centerMhz + width / 2;
    const unsigned noHt40Or80 = (1u << 13) | (1u << 14) | (1u << 15);
    const unsigned allowFlags = RuleActiveAllow & ~(width == 80 ? noHt40Or80 : 0u);
    bool covered = false;
    *indoor = false;
    if (!domain || !domain->count || domain->count > MT_REGD_MAX_RULES || domain->dfs_region > 3)
        return MT_INVALID;
    for (unsigned r = 0; r < domain->count; ++r) {
        const mt_reg_rule *rule = &domain->rules[r];
        if (!rule->start_mhz || rule->start_mhz >= rule->end_mhz ||
            !rule->bandwidth_mhz || rule->bandwidth_mhz > 320) return MT_INVALID;
        if (rule->end_mhz <= low || rule->start_mhz >= high) continue;
        if ((rule->flags & ~allowFlags) || rule->bandwidth_mhz < width) return MT_PENDING;
        if (rule->flags & RuleNoOutdoor) *indoor = true;
        if (rule->start_mhz <= low && rule->end_mhz >= high) covered = true;
    }
    return covered ? MT_OK : MT_PENDING;
}

static int SetAwdlWideDomain(const mt_fw_transport *transport, const uint8_t country[2],
    const uint8_t *channels, unsigned count, bool wideIndoor, bool homeIndoor, bool passive)
{
    uint8_t request[DomainHeader + 5 * DomainEntry] = {0};
    unsigned bytes = DomainHeader;
    request[0] = country[0];
    request[1] = country[1];
    request[4] = 1; /* 2.4 GHz: BW_20M */
    request[5] = 2; /* 5 GHz: BW_20_40_80M */
    request[6] = 1; /* 6 GHz: BW_20M */
    Le16(request + 8, 2);
    for (unsigned i = 0; i < count; ++i) {
        Le16(request + bytes, channels[i]);
        unsigned flags = passive ? ChanNoIr : 0;
        /* In the high-band plan, channel 36 is the 20 MHz return channel. */
        const bool homeOnly = count == 5 && i == 0;
        if (homeOnly) flags |= (1u << 4) | (1u << 5) | (1u << 7) | (1u << 8);
        if (homeOnly ? homeIndoor : wideIndoor) flags |= ChanIndoorOnly;
        Le16(request + bytes + 4, flags);
        bytes += DomainEntry;
        ++request[13];
    }
    Le16(request + 10, bytes - 8);
    return Send(transport, SetDomainInfo, request, bytes);
}

static int InstallAwdlWidePower(const mt_fw_transport *transport, const uint8_t country[2],
    unsigned first, int wideLimit, int homeLimit, LUOBO_MT7925_STA_POWER_RESULT *result)
{
    static const uint8_t ordered[] = {
        36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,100,102,104,106,
        108,110,112,114,116,118,120,122,124,126,128,132,134,136,138,140,
        142,144,149,151,153,155,157,159,161,163,165,167,169,173,175,177
    };
    uint8_t request[SkuHeader + BatchLimit * SkuEntry] = {0};
    Le16(request + 4, 1);
    request[13] = 2; /* 5 GHz */
    memcpy(request + 16, country, 2);
    for (unsigned i = 0; i < sizeof(ordered); i += BatchLimit) {
        unsigned count = (unsigned)sizeof(ordered) - i;
        if (count > BatchLimit) count = BatchLimit;
        unsigned bytes = SkuHeader + count * SkuEntry;
        Le16(request + 6, bytes - 4);
        request[12] = (uint8_t)count;
        request[14] = (uint8_t)(i + count == sizeof(ordered));
        for (unsigned j = 0; j < count; ++j) {
            uint8_t *entry = request + SkuHeader + j * SkuEntry;
            entry[0] = ordered[i + j];
            /* The seven ordered 20/40/80 MHz centers include primary and secondary 20s. */
            int ceiling = (entry[0] >= first && entry[0] <= first + 12) ? wideLimit : -127;
            if (first != 36 && entry[0] == 36) ceiling = homeLimit;
            SetRateCeiling(entry + 1, ceiling);
        }
        int error = Send(transport, SetPowerLimit, request, bytes);
        if (error) return error;
        ++result->SkuBatchesAck;
    }
    return MT_OK;
}

int LuoboMt7925ProgramAwdlChannels80(const mt_fw_transport *transport,
    const mt_regdomain *domain, const LUOBO_MT7925_BOARD_SAR *sar,
    const uint8_t country[2], int hostCap, bool indoorVerified, bool activate,
    LUOBO_MT7925_STA_POWER_RESULT *result)
{
    return LuoboMt7925ProgramAwdlChannels80ForPrimary(transport, domain, sar, country, 44,
        hostCap, indoorVerified, activate, result);
}

int LuoboMt7925ProgramAwdlChannels80ForPrimary(const mt_fw_transport *transport,
    const mt_regdomain *domain, const LUOBO_MT7925_BOARD_SAR *sar,
    const uint8_t country[2], unsigned primary, int hostCap, bool indoorVerified, bool activate,
    LUOBO_MT7925_STA_POWER_RESULT *result)
{
    static const uint8_t unii1[] = {36, 40, 44, 48};
    static const uint8_t homeAndUnii3[] = {36, 149, 153, 157, 161};
    if (!result || result->Attempted || !transport || !transport->send || !country ||
        country[0] < 'A' || country[0] > 'Z' || country[1] < 'A' || country[1] > 'Z' ||
        hostCap < -127 || hostCap > 126 || (primary != 44 && primary != 149)) return MT_INVALID;
    const bool high = primary == 149;
    if (high && (memcmp(country, "CN", 2) || !indoorVerified)) return MT_PENDING;
    const uint8_t *channels = high ? homeAndUnii3 : unii1;
    const unsigned count = high ? sizeof(homeAndUnii3) : sizeof(unii1);
    const unsigned first = high ? 149 : 36;
    bool wideIndoor = false, homeIndoor = false;
    int error = EligibleSpan(domain, 5000 + 5 * (first + 6), 80, &wideIndoor);
    if (error) return error;
    if (high) {
        error = EligibleSpan(domain, 5180, 20, &homeIndoor);
        if (error) return error;
        wideIndoor = true;
    }
    if (activate && !indoorVerified && (wideIndoor || homeIndoor)) return MT_PENDING;
    if (hostCap > 12) hostCap = 12; /* E138 conducted power ceiling, half dBm */
    int homeLimit, wideLimit;
    if (!LuoboMt7925BoardSarLimit5g(sar, 36, domain->dfs_region, hostCap, &homeLimit) ||
        !LuoboMt7925BoardSarLimit5g(sar, primary, domain->dfs_region, hostCap, &wideLimit))
        return MT_PENDING;
    memset(result, 0, sizeof(*result));
    result->Attempted = true;
    error = SetAwdlWideDomain(transport, country, channels, count, wideIndoor, homeIndoor, true);
    if (error) return error;
    result->PassiveAck = LuoboWorldChannel36 | (high ? LuoboAwdlChannel149 : LuoboWorldChannel44);
    error = InstallAwdlWidePower(transport, country, first, wideLimit, homeLimit, result);
    if (error) return error;
    result->LimitHalfDbm = homeLimit < wideLimit ? homeLimit : wideLimit;
    if (activate) {
        error = SetAwdlWideDomain(transport, country, channels, count, wideIndoor, homeIndoor, false);
        if (!error) {
            result->ChannelProgrammed = 36;
            result->AwdlPrimary = primary;
        }
    }
    return error;
}

static bool RuleCoversChannel5g(const mt_regdomain *domain, unsigned channel,
    bool *radar, bool *noIr, bool *indoor)
{
    const unsigned center = 5000u + 5u * channel;
    bool covered = false;
    *radar = false;
    *noIr = false;
    *indoor = false;
    for (unsigned r = 0; r < domain->count; ++r) {
        const mt_reg_rule *rule = &domain->rules[r];
        if (rule->end_mhz <= center - 10 || rule->start_mhz >= center + 10) continue;
        if (rule->bandwidth_mhz < 20) return false;
        if (rule->flags & (1u << 4)) *radar = true;       /* NL80211_RRF_DFS */
        if (rule->flags & (RuleNoIr | RuleNoIbss)) *noIr = true;
        if (rule->flags & RuleNoOutdoor) *indoor = true;
        if (rule->start_mhz <= center - 10 && rule->end_mhz >= center + 10) covered = true;
    }
    return covered;
}

int LuoboMt7925ProgramConcurrent5G(const mt_fw_transport *transport,
    const mt_regdomain *domain, const LUOBO_MT7925_BOARD_SAR *sar,
    const uint8_t country[2], int hostCap, bool indoorVerified, bool activate,
    LUOBO_MT7925_STA_POWER_RESULT *result)
{
    static const uint8_t all5g[] = {
        36, 40, 44, 48, 52, 56, 60, 64,
        100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
        149, 153, 157, 161, 165
    };
    static const uint8_t ordered[] = {
        36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,100,102,104,106,
        108,110,112,114,116,118,120,122,124,126,128,132,134,136,138,140,
        142,144,149,151,153,155,157,159,161,163,165,167,169,173,175,177
    };
    enum { ChanRadar = 1u << 2 };

    if (!result || result->Attempted || !transport || !transport->send || !domain ||
        !domain->count || domain->count > MT_REGD_MAX_RULES || domain->dfs_region > 3 ||
        !country || country[0] < 'A' || country[0] > 'Z' || country[1] < 'A' || country[1] > 'Z' ||
        hostCap < -127 || hostCap > 126) return MT_INVALID;
    for (unsigned r = 0; r < domain->count; ++r) {
        const mt_reg_rule *rule = &domain->rules[r];
        if (!rule->start_mhz || rule->start_mhz >= rule->end_mhz ||
            !rule->bandwidth_mhz || rule->bandwidth_mhz > 320) return MT_INVALID;
    }

    uint8_t domReq[DomainHeader + sizeof(all5g) * DomainEntry] = {0};
    unsigned activeCount = 0;
    unsigned chCount = 0;
    uint8_t chList[sizeof(all5g)] = {0};
    uint16_t chFlags[sizeof(all5g)] = {0};
    int lowestLimit = hostCap;

    for (unsigned i = 0; i < sizeof(all5g); ++i) {
        bool radar = false, noIr = false, indoor = false;
        if (!RuleCoversChannel5g(domain, all5g[i], &radar, &noIr, &indoor)) continue;
        int chLimit = 0;
        if (!LuoboMt7925BoardSarLimit5g(sar, all5g[i], domain->dfs_region, hostCap, &chLimit))
            return MT_PENDING;
        uint16_t flags = 0;
        if (radar) flags |= (uint16_t)ChanRadar | (uint16_t)ChanNoIr;
        else if (noIr || (indoor && !indoorVerified)) flags |= (uint16_t)ChanNoIr;
        if (indoor) flags |= (uint16_t)ChanIndoorOnly;
        if (!(flags & ChanNoIr)) {
            ++activeCount;
            if (chLimit < lowestLimit) lowestLimit = chLimit;
        }
        chList[chCount] = all5g[i];
        chFlags[chCount] = flags;
        ++chCount;
    }
    if (!chCount || !activeCount) return MT_PENDING;

    memset(result, 0, sizeof(*result));
    result->Attempted = true;

    /* 1. Passive domain setup across all legal 5 GHz channels (80 MHz capable). */
    domReq[0] = country[0];
    domReq[1] = country[1];
    domReq[4] = 1; /* 2.4 GHz: 20 MHz */
    domReq[5] = 2; /* 5 GHz: 20/40/80 MHz */
    domReq[6] = 1; /* 6 GHz: 20 MHz */
    Le16(domReq + 8, 2);
    unsigned bytes = DomainHeader;
    for (unsigned i = 0; i < chCount; ++i) {
        Le16(domReq + bytes, chList[i]);
        Le16(domReq + bytes + 4, chFlags[i] | ChanNoIr);
        bytes += DomainEntry;
        ++domReq[13];
    }
    Le16(domReq + 10, bytes - 8);
    int error = Send(transport, SetDomainInfo, domReq, bytes);
    if (error) return error;
    result->PassiveAck = LuoboWorldChannel36 | LuoboWorldChannel44;

    /* 2. Install SAR-bounded SKU power ceilings for every covered 5 GHz channel/center. */
    uint8_t skuReq[SkuHeader + BatchLimit * SkuEntry] = {0};
    Le16(skuReq + 4, 1);
    skuReq[13] = 2; /* 5 GHz */
    memcpy(skuReq + 16, country, 2);
    for (unsigned i = 0; i < sizeof(ordered); i += BatchLimit) {
        unsigned count = (unsigned)sizeof(ordered) - i;
        if (count > BatchLimit) count = BatchLimit;
        unsigned skuBytes = SkuHeader + count * SkuEntry;
        Le16(skuReq + 6, skuBytes - 4);
        skuReq[12] = (uint8_t)count;
        skuReq[14] = (uint8_t)(i + count == sizeof(ordered));
        for (unsigned j = 0; j < count; ++j) {
            uint8_t *entry = skuReq + SkuHeader + j * SkuEntry;
            entry[0] = ordered[i + j];
            bool radar = false, noIr = false, indoor = false;
            int ceiling = -127;
            if (RuleCoversChannel5g(domain, entry[0], &radar, &noIr, &indoor)) {
                int chLimit = -127;
                if (LuoboMt7925BoardSarLimit5g(sar, entry[0], domain->dfs_region, hostCap, &chLimit))
                    ceiling = chLimit;
            }
            SetRateCeiling(entry + 1, ceiling);
        }
        error = Send(transport, SetPowerLimit, skuReq, skuBytes);
        if (error) return error;
        ++result->SkuBatchesAck;
    }
    result->LimitHalfDbm = lowestLimit;

    /* 3. Activate non-DFS 5 GHz channels (and keep DFS channels passive/radar-flagged). */
    if (activate) {
        memset(domReq, 0, sizeof(domReq));
        domReq[0] = country[0];
        domReq[1] = country[1];
        domReq[4] = 1;
        domReq[5] = 2;
        domReq[6] = 1;
        Le16(domReq + 8, 2);
        bytes = DomainHeader;
        for (unsigned i = 0; i < chCount; ++i) {
            Le16(domReq + bytes, chList[i]);
            Le16(domReq + bytes + 4, chFlags[i]);
            bytes += DomainEntry;
            ++domReq[13];
        }
        Le16(domReq + 10, bytes - 8);
        error = Send(transport, SetDomainInfo, domReq, bytes);
        if (!error) result->ChannelProgrammed = chList[0];
    }
    return error;
}

static bool RuleCoversChannel2g(const mt_regdomain *domain, unsigned channel,
    bool *noIr, bool *indoor)
{
    if (channel < 1 || channel > 13) return false;
    const unsigned center = 2407u + 5u * channel;
    bool covered = false;
    *noIr = false;
    *indoor = false;
    for (unsigned r = 0; r < domain->count; ++r) {
        const mt_reg_rule *rule = &domain->rules[r];
        if (rule->end_mhz <= center - 10 || rule->start_mhz >= center + 10) continue;
        if (rule->bandwidth_mhz < 20) return false;
        if (rule->flags & (RuleNoIr | RuleNoIbss)) *noIr = true;
        if (rule->flags & RuleNoOutdoor) *indoor = true;
        if (rule->start_mhz <= center - 10 && rule->end_mhz >= center + 10) covered = true;
    }
    return covered;
}

int LuoboMt7925ProgramConcurrentDualBand(const mt_fw_transport *transport,
    const mt_regdomain *domain, const LUOBO_MT7925_BOARD_SAR *sar,
    const uint8_t country[2], int hostCap, bool indoorVerified, bool activate,
    LUOBO_MT7925_STA_POWER_RESULT *result)
{
    static const uint8_t all2g[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    static const uint8_t all5g[] = {
        36, 40, 44, 48, 52, 56, 60, 64,
        100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
        149, 153, 157, 161, 165
    };
    static const uint8_t ordered5g[] = {
        36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,100,102,104,106,
        108,110,112,114,116,118,120,122,124,126,128,132,134,136,138,140,
        142,144,149,151,153,155,157,159,161,163,165,167,169,173,175,177
    };
    enum { ChanRadar = 1u << 2 };

    if (!result || result->Attempted || !transport || !transport->send || !domain ||
        !domain->count || domain->count > MT_REGD_MAX_RULES || domain->dfs_region > 3 ||
        !country ||
        !((country[0] == '0' && country[1] == '0') ||
          (country[0] >= 'A' && country[0] <= 'Z' && country[1] >= 'A' && country[1] <= 'Z')) ||
        hostCap < -127 || hostCap > 126) return MT_INVALID;
    for (unsigned r = 0; r < domain->count; ++r) {
        const mt_reg_rule *rule = &domain->rules[r];
        if (!rule->start_mhz || rule->start_mhz >= rule->end_mhz ||
            !rule->bandwidth_mhz || rule->bandwidth_mhz > 320) return MT_INVALID;
    }

    uint8_t domReq[DomainHeader + (sizeof(all2g) + sizeof(all5g)) * DomainEntry] = {0};
    unsigned activeCount = 0;
    unsigned ch2Count = 0, ch5Count = 0;
    uint8_t ch2List[sizeof(all2g)] = {0};
    uint16_t ch2Flags[sizeof(all2g)] = {0};
    uint8_t ch5List[sizeof(all5g)] = {0};
    uint16_t ch5Flags[sizeof(all5g)] = {0};
    int lowestLimit = hostCap;

    for (unsigned i = 0; i < sizeof(all2g); ++i) {
        bool noIr = false, indoor = false;
        if (!RuleCoversChannel2g(domain, all2g[i], &noIr, &indoor)) continue;
        int chLimit = 0;
        if (!LuoboMt7925BoardSarLimitDualBand(sar, all2g[i], domain->dfs_region, hostCap, &chLimit))
            return MT_PENDING;
        uint16_t flags = 0;
        if (noIr || (indoor && !indoorVerified)) flags |= (uint16_t)ChanNoIr;
        if (indoor) flags |= (uint16_t)ChanIndoorOnly;
        if (!(flags & ChanNoIr)) {
            ++activeCount;
            if (chLimit < lowestLimit) lowestLimit = chLimit;
        }
        ch2List[ch2Count] = all2g[i];
        ch2Flags[ch2Count] = flags;
        ++ch2Count;
    }

    for (unsigned i = 0; i < sizeof(all5g); ++i) {
        bool radar = false, noIr = false, indoor = false;
        if (!RuleCoversChannel5g(domain, all5g[i], &radar, &noIr, &indoor)) continue;
        int chLimit = 0;
        if (!LuoboMt7925BoardSarLimitDualBand(sar, all5g[i], domain->dfs_region, hostCap, &chLimit))
            return MT_PENDING;
        uint16_t flags = 0;
        if (radar) flags |= (uint16_t)ChanRadar | (uint16_t)ChanNoIr;
        else if (noIr || (indoor && !indoorVerified)) flags |= (uint16_t)ChanNoIr;
        if (indoor) flags |= (uint16_t)ChanIndoorOnly;
        if (!(flags & ChanNoIr)) {
            ++activeCount;
            if (chLimit < lowestLimit) lowestLimit = chLimit;
        }
        ch5List[ch5Count] = all5g[i];
        ch5Flags[ch5Count] = flags;
        ++ch5Count;
    }
    if ((!ch2Count && !ch5Count) || !activeCount) return MT_PENDING;

    memset(result, 0, sizeof(*result));
    result->Attempted = true;

    /* 1. Passive domain setup across covered 2.4 GHz (domReq[12]) + 5 GHz (domReq[13]). */
    domReq[0] = country[0];
    domReq[1] = country[1];
    domReq[4] = 1; /* 2.4 GHz: 20 MHz */
    domReq[5] = 2; /* 5 GHz: 20/40/80 MHz */
    domReq[6] = 1; /* 6 GHz: 20 MHz */
    Le16(domReq + 8, 2);
    unsigned bytes = DomainHeader;
    for (unsigned i = 0; i < ch2Count; ++i) {
        Le16(domReq + bytes, ch2List[i]);
        Le16(domReq + bytes + 4, ch2Flags[i] | ChanNoIr);
        bytes += DomainEntry;
        ++domReq[12];
    }
    for (unsigned i = 0; i < ch5Count; ++i) {
        Le16(domReq + bytes, ch5List[i]);
        Le16(domReq + bytes + 4, ch5Flags[i] | ChanNoIr);
        bytes += DomainEntry;
        ++domReq[13];
    }
    Le16(domReq + 10, bytes - 8);
    int error = Send(transport, SetDomainInfo, domReq, bytes);
    if (error) return error;
    result->PassiveAck = LuoboWorldChannel36 | LuoboWorldChannel44;

    /* 2a. Install 2.4 GHz SKU power ceilings (band 1, channels 1..14) FIRST. */
    uint8_t skuReq[SkuHeader + BatchLimit * SkuEntry] = {0};
    Le16(skuReq + 4, 1);
    skuReq[13] = 1; /* 2.4 GHz */
    memcpy(skuReq + 16, country, 2);
    for (unsigned i = 0; i < 14; i += BatchLimit) {
        unsigned count = 14u - i;
        if (count > BatchLimit) count = BatchLimit;
        unsigned skuBytes = SkuHeader + count * SkuEntry;
        Le16(skuReq + 6, skuBytes - 4);
        skuReq[12] = (uint8_t)count;
        skuReq[14] = (uint8_t)(i + count == 14u);
        for (unsigned j = 0; j < count; ++j) {
            uint8_t *entry = skuReq + SkuHeader + j * SkuEntry;
            entry[0] = (uint8_t)(i + j + 1u);
            bool noIr = false, indoor = false;
            int ceiling = -127;
            if (RuleCoversChannel2g(domain, entry[0], &noIr, &indoor)) {
                int chLimit = -127;
                if (LuoboMt7925BoardSarLimitDualBand(sar, entry[0], domain->dfs_region, hostCap, &chLimit))
                    ceiling = chLimit;
            }
            SetRateCeilingBand(entry + 1, 1, ceiling);
        }
        error = Send(transport, SetPowerLimit, skuReq, skuBytes);
        if (error) return error;
        ++result->SkuBatchesAck;
    }

    /* 2b. Install 5 GHz SKU power ceilings (band 2, channels/centers 36..177). */
    memset(skuReq, 0, sizeof(skuReq));
    Le16(skuReq + 4, 1);
    skuReq[13] = 2; /* 5 GHz */
    memcpy(skuReq + 16, country, 2);
    for (unsigned i = 0; i < sizeof(ordered5g); i += BatchLimit) {
        unsigned count = (unsigned)sizeof(ordered5g) - i;
        if (count > BatchLimit) count = BatchLimit;
        unsigned skuBytes = SkuHeader + count * SkuEntry;
        Le16(skuReq + 6, skuBytes - 4);
        skuReq[12] = (uint8_t)count;
        skuReq[14] = (uint8_t)(i + count == sizeof(ordered5g));
        for (unsigned j = 0; j < count; ++j) {
            uint8_t *entry = skuReq + SkuHeader + j * SkuEntry;
            entry[0] = ordered5g[i + j];
            bool radar = false, noIr = false, indoor = false;
            int ceiling = -127;
            if (RuleCoversChannel5g(domain, entry[0], &radar, &noIr, &indoor)) {
                int chLimit = -127;
                if (LuoboMt7925BoardSarLimitDualBand(sar, entry[0], domain->dfs_region, hostCap, &chLimit))
                    ceiling = chLimit;
            }
            SetRateCeilingBand(entry + 1, 2, ceiling);
        }
        error = Send(transport, SetPowerLimit, skuReq, skuBytes);
        if (error) return error;
        ++result->SkuBatchesAck;
    }
    result->LimitHalfDbm = lowestLimit;

    /* 3. Activate legal 2.4 GHz + non-DFS 5 GHz channels (DFS stays Radar + No-IR). */
    if (activate) {
        memset(domReq, 0, sizeof(domReq));
        domReq[0] = country[0];
        domReq[1] = country[1];
        domReq[4] = 1;
        domReq[5] = 2;
        domReq[6] = 1;
        Le16(domReq + 8, 2);
        bytes = DomainHeader;
        for (unsigned i = 0; i < ch2Count; ++i) {
            Le16(domReq + bytes, ch2List[i]);
            Le16(domReq + bytes + 4, ch2Flags[i]);
            bytes += DomainEntry;
            ++domReq[12];
        }
        for (unsigned i = 0; i < ch5Count; ++i) {
            Le16(domReq + bytes, ch5List[i]);
            Le16(domReq + bytes + 4, ch5Flags[i]);
            bytes += DomainEntry;
            ++domReq[13];
        }
        Le16(domReq + 10, bytes - 8);
        error = Send(transport, SetDomainInfo, domReq, bytes);
        if (!error) result->ChannelProgrammed = ch5Count ? ch5List[0] : ch2List[0];
    }
    return error;
}
