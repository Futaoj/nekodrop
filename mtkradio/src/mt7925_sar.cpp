// SPDX-License-Identifier: GPL-2.0-only
#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#include <initguid.h>
#include <cstdio>
#include <vector>

#include "../../driver/mt7925-sar/mt7925_sar_ioctl.h"
#include "mt7925_sar.h"

namespace mtk {

bool read_mt7925_sar(LUOBO_MT7925_BOARD_SAR &sar, std::string &report, std::string &error) {
  const GUID &guid = GUID_DEVINTERFACE_NEKODROP_MT7925_SAR;
  HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) {
    error = "SAR device enumeration failed: " + std::to_string(GetLastError());
    return false;
  }

  SP_DEVICE_INTERFACE_DATA iface{};
  iface.cbSize = sizeof(iface);
  HANDLE handle = INVALID_HANDLE_VALUE;
  if (SetupDiEnumDeviceInterfaces(set, nullptr, &guid, 0, &iface)) {
    SP_DEVICE_INTERFACE_DATA extra{};
    extra.cbSize = sizeof(extra);
    if (SetupDiEnumDeviceInterfaces(set, nullptr, &guid, 1, &extra)) {
      SetupDiDestroyDeviceInfoList(set);
      error = "more than one MT7925 E138 SAR device is present";
      return false;
    }
    DWORD bytes = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &bytes, nullptr);
    if (bytes >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) && bytes <= 65536) {
      std::vector<uint8_t> storage(bytes);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
      detail->cbSize = sizeof(*detail);
      if (SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, bytes, nullptr, nullptr))
        handle = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
  }
  const DWORD open_error = GetLastError();
  SetupDiDestroyDeviceInfoList(set);
  if (handle == INVALID_HANDLE_VALUE) {
    error = "MT7925 SAR interface could not be opened: " + std::to_string(open_error);
    return false;
  }

  NEKODROP_MT7925_SAR reply{};
  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(handle, IOCTL_NEKODROP_MT7925_READ_SAR, nullptr, 0,
                                  &reply, sizeof(reply), &returned, nullptr);
  const DWORD query_error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (!ok || returned != sizeof(reply) || reply.version != NEKODROP_MT7925_SAR_VERSION) {
    error = "MT7925 SAR query failed: Win32=" + std::to_string(query_error) +
            " bytes=" + std::to_string(returned);
    return false;
  }

  char status[96];
  std::snprintf(status, sizeof(status), "MTDS=0x%08x/%u MTGS=0x%08x/%u",
                reply.mtds.status, reply.mtds.size, reply.mtgs.status, reply.mtgs.size);
  report = status;
  LUOBO_MT7925_BOARD_SAR decoded{};
  if (reply.mtds.size > sizeof(reply.mtds.data) || reply.mtgs.size > sizeof(reply.mtgs.data) ||
      !LuoboDecodeMt7925Sar(false, reply.mtds.status, reply.mtds.data, reply.mtds.size,
                           &decoded.Dynamic) ||
      !LuoboDecodeMt7925Sar(true, reply.mtgs.status, reply.mtgs.data, reply.mtgs.size,
                           &decoded.Geographic)) {
    error = "MT7925 SAR methods returned unavailable or malformed data";
    return false;
  }
  sar = decoded;
  report += " dynamic_state=" + std::to_string(sar.Dynamic.State) +
            " geographic_state=" + std::to_string(sar.Geographic.State);
  return true;
}

} // namespace mtk
