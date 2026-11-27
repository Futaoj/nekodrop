/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#pragma once
#include "regulatory.h"

enum { LuoboWorldChannel36 = 1, LuoboWorldChannel44 = 2, LuoboAwdlChannel149 = 4 };

/* A geometry/flag check against the firmware-decoded domain for a 20 MHz
   OFDM channel, without assuming indoor/outdoor operation or DFS clearance.
   The returned bits are candidates only, never an RF enable or power limit. */
unsigned LuoboMt7925WorldCandidateMask(const mt_regdomain *Domain);
