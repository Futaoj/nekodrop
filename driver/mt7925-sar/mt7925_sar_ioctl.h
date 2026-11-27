// SPDX-License-Identifier: GPL-2.0-only
#pragma once

// Include the operating system's GUID and IOCTL definitions before this file.
DEFINE_GUID(GUID_DEVINTERFACE_NEKODROP_MT7925_SAR,
            0x6f2d62bd, 0xcb47, 0x427c, 0xbe, 0xc1, 0x7c, 0x4e, 0xca, 0x90, 0x6e, 0x13);

#define IOCTL_NEKODROP_MT7925_READ_SAR CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS)
#define NEKODROP_MT7925_SAR_VERSION 1u
#define NEKODROP_MT7925_SAR_BYTES 1024u

typedef struct _NEKODROP_SAR_METHOD {
  unsigned int status;
  unsigned int size;
  unsigned char data[NEKODROP_MT7925_SAR_BYTES];
} NEKODROP_SAR_METHOD;

typedef struct _NEKODROP_MT7925_SAR {
  unsigned int version;
  NEKODROP_SAR_METHOD mtds;
  NEKODROP_SAR_METHOD mtgs;
} NEKODROP_MT7925_SAR;
