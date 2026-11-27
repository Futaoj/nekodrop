/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Firmware layouts from mt76 4d7f2bed mt7925/mcu.[ch], main.c and the
   MediaTek UNI_EVENT_SCAN_DONE_BASIC definition. No probe requests or IEs. */
#include "passive-scan.h"
#include "firmware.h"
#include <string.h>

static void Tlv(uint8_t *p, unsigned tag, unsigned bytes)
{
    p[0] = (uint8_t)tag;
    p[2] = (uint8_t)bytes;
    p[3] = (uint8_t)(bytes >> 8);
}

bool LuoboMt7925EncodePassiveScan(uint8_t p[LuoboPassiveScanBytes], uint8_t id, unsigned mask, uint16_t dwell_ms)
{
    if (!p || !id || id > 127 || !mask || mask > 3) return false;
    if (dwell_ms && (mask == 3 || dwell_ms > 500)) return false;
    memset(p, 0, LuoboPassiveScanBytes);
    p[0] = id; /* Infrastructure BSS 0, main PHY. */
    uint8_t *req = p + 4;
    Tlv(req, 1, 20);
    /* A known AP test uses a continuous dwell with no probes. */
    if (dwell_ms) {
        req[8] = req[10] = (uint8_t)dwell_ms;
        req[9] = req[11] = (uint8_t)(dwell_ms >> 8);
        req[12] = 0xe8;
        req[13] = 3;  /* Firmware scan timeout: 1000 ms. */
    } else {
        req[6] = 1u << 5; /* SPLIT_SCAN; passive, no probes. */
    }
    uint8_t *ssid = req + 20;
    Tlv(ssid, 10, 368);
    ssid[4] = 1;
    uint8_t *bssid = ssid + 368;
    Tlv(bssid, 11, 16);
    memset(bssid + 4, 0xff, 6);
    uint8_t *channel = bssid + 16;
    Tlv(channel, 12, 136);
    channel[4] = 4; /* Specified channels only. */
    for (unsigned i = 0; i < 2; ++i) {
        if (!(mask & (1u << i))) continue;
        unsigned slot = channel[5]++;
        channel[8 + 2 * slot] = 2;
        channel[9 + 2 * slot] = i ? 44 : 36;
    }
    Tlv(channel + 136, 14, 12);
    return true;
}

static unsigned Le16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

int LuoboMt7925PassiveScanEvent(LUOBO_PASSIVE_SCAN *s, uint8_t id, uint8_t seq,
    unsigned mask, const uint8_t *p, uint32_t bytes)
{
    if (!s || !p || !id || id > 127 || !seq || seq > 15 || !mask || mask > 3) return MT_INVALID;
    if (bytes < 44 || (p[3] >> 3) != 7 || (p[2] & 15u) == 1 ||
        Le16(p + 34) != 0x3800 || Le16(p + 32) != bytes - 32) return MT_FW_PROTOCOL;
    if (p[36] == 1 && !(p[38] & 4) && p[37] == seq) {
        if (bytes != 52) return MT_FW_PROTOCOL;
        uint32_t status = 0;
        int ack = mt_mcu_response(MT_MCU_UNI | 0x16, seq, p, bytes, &status);
        if (ack != MT_OK) return ack;
        s->Refusal = status;
        if (status || s->Accepted) return MT_FW_PROTOCOL;
        s->Accepted = true;
    } else if (p[36] == 0x0e && (p[38] & 4)) {
        if (bytes < 48) return MT_FW_PROTOCOL;
        if (p[44] != id) return MT_PENDING;
        LUOBO_PASSIVE_SCAN next = *s;
        for (uint32_t i = 48; i < bytes;) {
            if (bytes - i < 4) return MT_FW_PROTOCOL;
            const unsigned tag = Le16(p + i), n = Le16(p + i + 2);
            if (n < 4 || n > bytes - i) return MT_FW_PROTOCOL;
            if (tag == 0) {
                if (n != 12 || next.Done || p[i + 4] > (mask == 3 ? 2u : 1u)) return MT_FW_PROTOCOL;
                next.Done = true;
                next.CompleteChannels = p[i + 4];
                next.BeaconCount = Le16(p + i + 8) | ((uint32_t)Le16(p + i + 10) << 16);
            } else if (tag == 2) {
                if (n < 8 || next.SawChannelInfo) return MT_FW_PROTOCOL;
                next.SawChannelInfo = true;
                const uint8_t a = p[i + 5], b = p[i + 6];
                if ((a >= 'A' && a <= 'Z' && b >= 'A' && b <= 'Z') || (a == '0' && b == '0')) {
                    next.FirmwareCountry[0] = a;
                    next.FirmwareCountry[1] = b;
                }
            }
            i += n;
        }
        *s = next;
    } else return MT_PENDING;
    return s->Done && s->Accepted ? MT_OK : MT_PENDING;
}
