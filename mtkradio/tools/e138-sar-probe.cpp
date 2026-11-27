// SPDX-License-Identifier: GPL-2.0-only
// Read the two E138 SAR method results through UMDF. This probe never starts
// firmware, arms DMA, changes channel or transmits. The radio separately
// decodes the tables and intersects them with its regional and firmware limits.
#include <windows.h>
#include <winioctl.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include "../../driver/regproxy/regproxy_ioctl.h"
#include "../../iwxradio/src/proxy.h"

namespace {
bool report_method(const char *name, const REGPROXY_SAR_METHOD &method) {
  std::printf("%s status=0x%08x bytes=%u\n", name, method.status, method.size);
  if (method.status == 0xc0000034u) return method.size == 0; // object name not found
  if (method.status != 0 || method.size < 12 || method.size > sizeof(method.data)) return false;
  // ACPI_EVAL_OUTPUT_BUFFER_V1 header: signature, length, argument count.
  uint32_t header[3];
  std::memcpy(header, method.data, sizeof(header));
  return header[0] == 0x426f6541u && header[1] == method.size && header[2] > 0;
}
}

int wmain(int argc, wchar_t **argv) {
  constexpr char usage[] = "Usage: e138-sar-probe [--instance <full E138 PCI instance ID>]";
  std::wstring instance;
  if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
    std::puts(usage);
    return 0;
  }
  if (argc != 1) {
    constexpr std::wstring_view prefix = L"PCI\\VEN_14C3&DEV_7925&SUBSYS_E138105B&REV_00\\";
    if (argc != 3 || std::wstring_view(argv[1]) != L"--instance" ||
        std::wstring_view(argv[2]).size() <= prefix.size() ||
        _wcsnicmp(argv[2], prefix.data(), prefix.size()) != 0) {
      std::fprintf(stderr, "%s\n", usage);
      return 2;
    }
    instance = argv[2];
  }
  RegProxy proxy;
  std::string error;
  REGPROXY_MT7925_SAR reply{};
  if (!proxy.open(error, instance) || !proxy.read_mt7925_sar(reply, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const bool dynamic = report_method("MTDS", reply.mtds);
  const bool geographic = report_method("MTGS", reply.mtgs);
  return dynamic && geographic ? 0 : 1;
}
