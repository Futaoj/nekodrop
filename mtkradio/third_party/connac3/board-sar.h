/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LuoboSarUnknown, LuoboSarAbsent, LuoboSarDynamic, LuoboSarGeographic
} LUOBO_MT7925_SAR_STATE;

typedef struct {
    LUOBO_MT7925_SAR_STATE State;
    bool Enabled;
    uint8_t DynamicLimit;
    uint8_t Dynamic5g[4]; /* 5150-5350, 5350-5470, 5470-5725, 5725-5950 MHz */
    struct { uint8_t Limit, Offset; } Geo[3]; /* FCC, ETSI, other */
} LUOBO_MT7925_SAR_TABLE;

typedef struct {
    LUOBO_MT7925_SAR_TABLE Dynamic, Geographic;
} LUOBO_MT7925_BOARD_SAR;

/* Input is an actual ACPI.sys evaluation of the fixed MTDS/MTGS method on
   this device. Only a missing method with an empty response denotes absence. */
bool LuoboDecodeMt7925Sar(bool Geographic, uint32_t MethodStatus, const uint8_t *Wire,
    size_t Bytes, LUOBO_MT7925_SAR_TABLE *Out);

/* Only 20 MHz channels 36 and 44. The caller first needs permission for the
   channel and a finite per-device cap in half dBm from the regulatory path.
   This function is one additional ceiling; it never enables RF. */
bool LuoboMt7925BoardSarLimit(const LUOBO_MT7925_BOARD_SAR *Sar, unsigned Channel,
    unsigned FirmwareDfsRegion, int RegulatoryCapHalfDbm, int *Limit);

/* Full 5 GHz band (channels 36..177) board SAR ceiling for concurrent STA + AWDL. */
bool LuoboMt7925BoardSarLimit5g(const LUOBO_MT7925_BOARD_SAR *Sar, unsigned Channel,
    unsigned FirmwareDfsRegion, int RegulatoryCapHalfDbm, int *Limit);
