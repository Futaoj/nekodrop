/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "board-domain.h"
#include <stdbool.h>

static bool Covers20MHz(const mt_regdomain *domain, unsigned center)
{
    bool covered = false;
    const unsigned low = center - 10, high = center + 10;
    for (unsigned i = 0; i < domain->count; ++i) {
        const mt_reg_rule *rule = &domain->rules[i];
        if (!rule->start_mhz || rule->start_mhz >= rule->end_mhz ||
            !rule->bandwidth_mhz || rule->bandwidth_mhz > 320) return false;
        if (rule->start_mhz >= high || rule->end_mhz <= low) continue;
        /* nl80211.h: AUTO_BW is immaterial at this fixed width. Other flags
           include no initiating radiation, indoor/outdoor, DFS and unknown
           restrictions; do not infer that their preconditions were met. */
        const uint32_t autoBandwidth = UINT32_C(1) << 11;
        if (rule->bandwidth_mhz < 20 || (rule->flags & ~autoBandwidth)) return false;
        if (rule->start_mhz <= low && rule->end_mhz >= high) covered = true;
    }
    return covered;
}

unsigned LuoboMt7925WorldCandidateMask(const mt_regdomain *domain)
{
    if (!domain || !domain->count || domain->count > MT_REGD_MAX_RULES ||
        domain->dfs_region > 3) return 0;
    unsigned result = 0;
    if (Covers20MHz(domain, 5180)) result |= LuoboWorldChannel36;
    if (Covers20MHz(domain, 5220)) result |= LuoboWorldChannel44;
    return result;
}
