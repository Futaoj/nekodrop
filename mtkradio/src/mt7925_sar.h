// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>

extern "C" {
#include "board-sar.h"
}

class RegProxy;

namespace mtk {

bool read_mt7925_sar(const RegProxy &proxy, LUOBO_MT7925_BOARD_SAR &sar, std::string &report, std::string &error);

} // namespace mtk
