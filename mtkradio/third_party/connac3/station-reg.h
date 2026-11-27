/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#pragma once
#include "board-domain.h"
#include "board-sar.h"

/* Passive MCU domain only; does not tune or open a transmit queue. A positive
   result is published only after the firmware acknowledges this exact command.
   MT_PENDING means neither narrow channel is eligible. Hardware ownership and
   MCU serialization belong to the caller. */
int LuoboMt7925SetPassiveWorldDomain(const mt_fw_transport *Transport,
    const mt_regdomain *World, unsigned *AppliedMask);
int LuoboMt7925QueryChannelEligibility(const mt_regdomain *Domain,
    unsigned *PassiveMask, unsigned *ActiveMask);

/* These IEs must belong to the current selected infrastructure AP, captured
   on the actual channel. A Country IE or power constraint can only reduce the
   independent board/OS cap below. Its country must match the trusted domain. */
bool LuoboMt7925ApPowerLimit(const uint8_t *Ies, size_t Bytes, const uint8_t Country[2],
    uint8_t Channel, int *CapHalfDbm);

typedef struct {
    uint8_t Country[2], Channel;
    bool HostCapKnown;
    int HostCapHalfDbm; /* finite host rate-table ceiling under applicable EFUSE/CLC */
    unsigned ApEirpBackoffHalfDb; /* policy backoff for AP-advertised radiated limits */
    const uint8_t *SelectedApIes;
    size_t SelectedApIesBytes;
} LUOBO_MT7925_STA_CHANNEL_POLICY;

typedef struct {
    bool Attempted;
    unsigned PassiveAck, SkuBatchesAck, ChannelProgrammed;
    int LimitHalfDbm;
    unsigned AwdlPrimary; /* populated only after the wide AWDL active domain is acknowledged */
} LUOBO_MT7925_STA_POWER_RESULT;

/* Caller owns a fresh, zeroed Result per hardware generation. Before calling,
   gate ALL transmitters, drain any previous traffic, verify the location and
   apply the matching country CLC, then obtain CountryDomain from the MCU in
   this generation. Unknown policy/SAR makes no MCU request. An attempted
   operation can leave partially applied firmware state and requires reset
   before retry. This does not grant an on-channel interval or enable TX. */
int LuoboMt7925ProgramStationChannel(const mt_fw_transport *Transport,
    const mt_regdomain *CountryDomain, const LUOBO_MT7925_BOARD_SAR *Sar,
    const LUOBO_MT7925_STA_CHANNEL_POLICY *Policy, LUOBO_MT7925_STA_POWER_RESULT *Result);
int LuoboMt7925ProgramDualChannel(const mt_fw_transport *Transport,
    const mt_regdomain *CountryDomain, const LUOBO_MT7925_BOARD_SAR *Sar,
    const LUOBO_MT7925_STA_CHANNEL_POLICY *HomePolicy, int Peer44HostCapHalfDbm,
    LUOBO_MT7925_STA_POWER_RESULT *Result);

/* Standalone AWDL (no infrastructure BSS). Caller confirms current country,
   applies that firmware CLC and holds every TX queue closed. A successful
   program is a power ceiling; the caller still needs a firmware channel grant. */
int LuoboMt7925ProgramAwdlChannels(const mt_fw_transport *Transport,
    const mt_regdomain *CountryDomain, const LUOBO_MT7925_BOARD_SAR *Sar,
    const uint8_t Country[2], int HostCapHalfDbm, bool Indoor,
    bool ActivateDomain, LUOBO_MT7925_STA_POWER_RESULT *Result);

/* Standalone AWDL with 80 MHz UNII-1 (channels 36, 40, 44, 48; 40/80 MHz
   centers 38, 42, 46). Returns MT_PENDING without touching MCU if the domain
   does not allow active 80 MHz UNII-1 operation. */
int LuoboMt7925ProgramAwdlChannels80(const mt_fw_transport *Transport,
    const mt_regdomain *CountryDomain, const LUOBO_MT7925_BOARD_SAR *Sar,
    const uint8_t Country[2], int HostCapHalfDbm, bool Indoor,
    bool ActivateDomain, LUOBO_MT7925_STA_POWER_RESULT *Result);

/* E138 wide AWDL. Primary 44 uses channels 36..48. Primary 149 requires
   verified CN indoor operation, the entire 149..161 block and a separately
   eligible 20 MHz home channel 36. The caller must have applied matching CLC. */
int LuoboMt7925ProgramAwdlChannels80ForPrimary(const mt_fw_transport *Transport,
    const mt_regdomain *CountryDomain, const LUOBO_MT7925_BOARD_SAR *Sar,
    const uint8_t Country[2], unsigned PrimaryChannel, int HostCapHalfDbm, bool Indoor,
    bool ActivateDomain, LUOBO_MT7925_STA_POWER_RESULT *Result);

/* Full 5 GHz concurrent STA + AWDL channel domain (up to 80 MHz) bounded by
   firmware country regulatory rules and OEM SAR tables. DFS/radar channels are
   marked No-IR / Radar; UNII-1/UNII-3 channels allowed by the domain are active. */
int LuoboMt7925ProgramConcurrent5G(const mt_fw_transport *Transport,
    const mt_regdomain *CountryDomain, const LUOBO_MT7925_BOARD_SAR *Sar,
    const uint8_t Country[2], int HostCapHalfDbm, bool Indoor,
    bool ActivateDomain, LUOBO_MT7925_STA_POWER_RESULT *Result);
