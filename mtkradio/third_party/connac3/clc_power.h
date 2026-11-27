/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef MT7925_CLC_POWER_H
#define MT7925_CLC_POWER_H
#include "firmware.h"
#define MT_CLC_HEADER 84u
#define MT_CLC_SCRATCH (MT_DMA_MAX_LENGTH - 48u)
typedef struct mt_clc_result {
    uint32_t selected_mask, fallback_mask, requests_sent, channel_config;
    int32_t primary_error;
} mt_clc_result;
/* Validate both power blocks before sending. Caller holds all TX gates closed
 * and makes one attempt in a freshly booted generation. Partial firmware
 * programming is fatal to that generation. This does not authorize RF TX. */
MT_API int mt_clc_power_apply(const mt_fw_transport *, const uint8_t *, uint32_t,
    uint32_t hw_encap, const uint8_t alpha2[2], uint32_t environment,
    uint8_t *scratch, uint32_t capacity, mt_clc_result *);
#endif
