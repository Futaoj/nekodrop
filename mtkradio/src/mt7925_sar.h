// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>

extern "C" {
#include "board-sar.h"
}

namespace mtk {

bool read_mt7925_sar(LUOBO_MT7925_BOARD_SAR &sar, std::string &report, std::string &error);

} // namespace mtk
