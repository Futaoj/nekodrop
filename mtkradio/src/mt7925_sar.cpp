// SPDX-License-Identifier: GPL-2.0-only
#include <windows.h>
#include <winioctl.h>
#include <cstdio>

#include "../../driver/regproxy/regproxy_ioctl.h"
#include "proxy.h"
#include "mt7925_sar.h"

namespace mtk {

bool read_mt7925_sar(const RegProxy &proxy, LUOBO_MT7925_BOARD_SAR &sar,
                     std::string &report, std::string &error) {
  REGPROXY_MT7925_SAR reply{};
  if (!proxy.read_mt7925_sar(reply, error)) return false;

  char status[96];
  std::snprintf(status, sizeof(status), "MTDS=0x%08x/%u MTGS=0x%08x/%u",
                reply.mtds.status, reply.mtds.size, reply.mtgs.status, reply.mtgs.size);
  report = status;
  LUOBO_MT7925_BOARD_SAR decoded{};
  if (reply.mtds.size > sizeof(reply.mtds.data) ||
      !LuoboDecodeMt7925Sar(false, reply.mtds.status, reply.mtds.data, reply.mtds.size,
                           &decoded.Dynamic)) {
    error = "MT7925 dynamic SAR method unavailable or malformed";
    return false;
  }
  if (decoded.Dynamic.Enabled &&
      (reply.mtgs.size > sizeof(reply.mtgs.data) ||
       !LuoboDecodeMt7925Sar(true, reply.mtgs.status, reply.mtgs.data, reply.mtgs.size,
                            &decoded.Geographic))) {
    error = "MT7925 geographic SAR method unavailable or malformed for the enabled dynamic profile";
    return false;
  }
  sar = decoded;
  report += " dynamic_state=" + std::to_string(sar.Dynamic.State) +
            " geographic_state=" + std::to_string(sar.Geographic.State);
  return true;
}

} // namespace mtk
