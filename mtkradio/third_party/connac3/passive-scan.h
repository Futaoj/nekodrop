/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { LuoboPassiveScanBytes = 556 };
typedef struct {
    bool Accepted, Done, SawChannelInfo;
    uint8_t CompleteChannels, FirmwareCountry[2];
    uint32_t BeaconCount, Refusal;
} LUOBO_PASSIVE_SCAN;

bool LuoboMt7925EncodePassiveScan(uint8_t Payload[LuoboPassiveScanBytes],
    uint8_t ScanId, unsigned ChannelMask, uint16_t DwellMs);
/* Run only in a single D0 scan lease. MT_OK requires both the command ACK and
   a scan-done BASIC TLV for this scan ID, in either arrival order. */
int LuoboMt7925PassiveScanEvent(LUOBO_PASSIVE_SCAN *Scan, uint8_t ScanId,
    uint8_t CommandSequence, unsigned ChannelMask, const uint8_t *Record, uint32_t Bytes);
