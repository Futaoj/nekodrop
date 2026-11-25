# Applies nekodrop's changes to winawdl's awdld sources before they are built.
# Usage: python patch-awdl.py <winawdl folder>
#
# - awdl0 is retried when Wintun says it already exists (#30).  The service
#   restarts awdld whenever the radio restarts (a channel move, a Wi-Fi join),
#   and the old awdld is killed, so its Wintun adapter (same fixed GUID) can
#   still be on its way out when the new one asks for it.  CreateAdapter then
#   fails with ERROR_ALREADY_EXISTS (183) and awdld runs on without a host
#   interface: every AWDL data frame is dropped, so the iPhone sees the PC but
#   no AirDrop gets through until the next restart.  Waiting for the old one
#   to go fixes it; other errors fail at once, as before.
# - Experimental high-throughput flags (-H for Apple AWDL TLV 12 Wi-Fi 6 HE80
#   capability on 5 GHz 80 MHz; -M <mtu> for Wintun MTU up to 1500, inspired by
#   YiHoooong/owly feat/windows-mtkradio, #65). Both stay off/1450 unless passed.
#
# Idempotent; exits with an error when the source no longer matches.
import pathlib
import sys

root = pathlib.Path(sys.argv[1])


def patch_once(path: pathlib.Path, marker: str, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding='utf-8')
    if marker in text:
        print(f'{path.name}: {label} already patched')
        return
    if text.count(old) != 1:
        sys.exit(f'{path.name}: anchor for {label} moved; update patch-awdl.py')
    path.write_text(text.replace(old, new, 1), encoding='utf-8')
    print(f'{path.name}: {label}')


# 1. host_if.c: retry CreateAdapter while previous awdld adapter is closing (#30)
host_if_c = root / 'awdl' / 'win' / 'host_if.c'
patch_once(
    host_if_c,
    'nekodrop (#30)',
    '\thost->adapter = host->api.CreateAdapter(ADAPTER_NAME, TUNNEL_TYPE, &adapter_guid);\n',
    '\n'.join([
        '\t/* nekodrop (#30): the previous awdld\'s adapter may still be going away. */',
        '\tfor (int tries = 0;; tries++) {',
        '\t\thost->adapter = host->api.CreateAdapter(ADAPTER_NAME, TUNNEL_TYPE, &adapter_guid);',
        '\t\tif (host->adapter || GetLastError() != ERROR_ALREADY_EXISTS || tries >= 40)',
        '\t\t\tbreak;',
        '\t\tSleep(250);',
        '\t}',
        '',
    ]),
    'awdl0 CreateAdapter retry',
)

# 2. host_if.h & host_if.c: configurable Wintun IPv6 MTU (-M 1500, default 1450)
host_if_h = root / 'awdl' / 'win' / 'host_if.h'
patch_once(
    host_if_h,
    'host_if_set_mtu',
    'struct host_if *host_if_open(const uint8_t address[16], char *error, size_t error_len);\n',
    '/* Optional experimental MTU override (clamped 1280..1500; default 1450). */\n'
    'void host_if_set_mtu(unsigned long mtu);\n'
    'struct host_if *host_if_open(const uint8_t address[16], char *error, size_t error_len);\n',
    'host_if_set_mtu declaration',
)
patch_once(
    host_if_c,
    'host_if_set_mtu',
    '#define HOST_MTU 1450\n',
    '#define HOST_MTU 1450\n'
    'static unsigned long host_mtu = HOST_MTU;\n'
    'void host_if_set_mtu(unsigned long mtu) {\n'
    '\tif (mtu < 1280) mtu = 1280;\n'
    '\tif (mtu > 1500) mtu = 1500;\n'
    '\thost_mtu = mtu;\n'
    '}\n',
    'host_if_set_mtu definition',
)
patch_once(
    host_if_c,
    'row.NlMtu = host_mtu;',
    '\trow.NlMtu = HOST_MTU;\n',
    '\trow.NlMtu = host_mtu;\n',
    'configure_ip6 uses host_mtu',
)

# 3. frame_fixup.h & frame_fixup.c: optional AWDL TLV 12 Wi-Fi 6 HE80 capability (-H)
frame_fixup_h = root / 'awdl' / 'win' / 'frame_fixup.h'
frame_fixup_c = root / 'awdl' / 'win' / 'frame_fixup.c'
patch_once(
    frame_fixup_h,
    'frame_fixup_set_he',
    'void frame_fixup_announce(int two_streams, int short_guard_interval, int width_mhz);\n',
    'void frame_fixup_announce(int two_streams, int short_guard_interval, int width_mhz);\n'
    '/* Optional experimental Wi-Fi 6 HE80 flag in AWDL Data Path State TLV (12). */\n'
    'void frame_fixup_set_he(int enable);\n',
    'frame_fixup_set_he declaration',
)
patch_once(
    frame_fixup_c,
    'announce_he',
    'static int announce_width_mhz = 20;\n',
    'static int announce_width_mhz = 20;\n'
    'static int announce_he;\n'
    '#define TLV_DATA_PATH_STATE 12\n'
    'void frame_fixup_set_he(int enable) {\n'
    '\tannounce_he = enable ? 1 : 0;\n'
    '}\n',
    'frame_fixup_set_he definition',
)
patch_once(
    frame_fixup_c,
    'dp_val[14] |= 0x01u;',
    '\t/* Only a frame that ends on a TLV, as OWL\'s do, can take one more. */\n'
    '\tif (announce_width_mhz < 80 || fcs_len || end != length || length + VHT_TLV_LEN > capacity)\n'
    '\t\treturn length;\n'
    '\twrite_vht_tlv(frame + length);\n'
    '\treturn length + VHT_TLV_LEN;\n',
    '\t/* Experimental Wi-Fi 6 HE80 flag in Data Path State TLV (12): ext_flags |= 0x0100\n'
    '\t * (bit 0 of high byte at TLV value offset 14 when flags bit 0x8000 is set, #65). */\n'
    '\tif (announce_he && announce_width_mhz >= 80) {\n'
    '\t\tint dp = -1;\n'
    '\t\twalk_tlvs(frame, offset, length - fcs_len, TLV_DATA_PATH_STATE, &dp);\n'
    '\t\tif (dp >= 0) {\n'
    '\t\t\tint dp_len = (int) read_le16(frame + dp + 1);\n'
    '\t\t\tuint8_t *dp_val = frame + dp + TLV_HEADER_LEN;\n'
    '\t\t\tif (dp_len >= 15 && (read_le16(dp_val) & 0x8000u))\n'
    '\t\t\t\tdp_val[14] |= 0x01u;\n'
    '\t\t}\n'
    '\t}\n'
    '\t/* Only a frame that ends on a TLV, as OWL\'s do, can take one more. */\n'
    '\tif (announce_width_mhz < 80 || fcs_len || end != length || length + VHT_TLV_LEN > capacity)\n'
    '\t\treturn length;\n'
    '\twrite_vht_tlv(frame + length);\n'
    '\treturn length + VHT_TLV_LEN;\n',
    'TLV 12 HE80 ext_flags patch',
)

# 4. main.c: parse -H (HE80) and -M <mtu> CLI flags
main_c = root / 'awdl' / 'win' / 'main.c'
patch_once(
    main_c,
    'int enable_he = 0;',
    '\tint short_guard_interval = 1;\n'
    '\tint option;\n'
    '\twinawdl_join_segments(DEFAULT_JOIN_WAIT_US);\n'
    '\twhile ((option = getopt(argc, argv, "c:nrs:lj:v")) != -1) {\n',
    '\tint short_guard_interval = 1;\n'
    '\tint enable_he = 0;\n'
    '\tunsigned long host_mtu = 1450;\n'
    '\tint option;\n'
    '\twinawdl_join_segments(DEFAULT_JOIN_WAIT_US);\n'
    '\twhile ((option = getopt(argc, argv, "c:nrs:lj:HM:v")) != -1) {\n',
    'main.c getopt -H and -M',
)
patch_once(
    main_c,
    'option == \'H\'',
    '\t\t} else if (option == \'j\') {\n'
    '\t\t\twinawdl_join_segments(atoi(optarg));\n'
    '\t\t} else if (option == \'v\') {\n',
    '\t\t} else if (option == \'j\') {\n'
    '\t\t\twinawdl_join_segments(atoi(optarg));\n'
    '\t\t} else if (option == \'H\') {\n'
    '\t\t\tenable_he = 1;\n'
    '\t\t} else if (option == \'M\') {\n'
    '\t\t\thost_mtu = (unsigned long) atoi(optarg);\n'
    '\t\t\thost_if_set_mtu(host_mtu);\n'
    '\t\t} else if (option == \'v\') {\n',
    'main.c handle -H and -M',
)
patch_once(
    main_c,
    'frame_fixup_set_he(enable_he)',
    '\tframe_fixup_announce(streams == 2, short_guard_interval,\n'
    '\t\t\t     radio_client_width_mhz(winawdl_radio()));\n',
    '\tframe_fixup_announce(streams == 2, short_guard_interval,\n'
    '\t\t\t     radio_client_width_mhz(winawdl_radio()));\n'
    '\tframe_fixup_set_he(enable_he);\n'
    '\tif (enable_he || host_mtu != 1450)\n'
    '\t\tlog_info("experimental high-throughput: HE80=%d, MTU=%lu", enable_he, host_mtu);\n',
    'main.c apply HE80 and log high-throughput',
)
