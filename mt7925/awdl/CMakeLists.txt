# SPDX-License-Identifier: GPL-3.0-or-later
# AWDL daemon: OWL's library and event core on a Windows I/O and timing layer.
# OWL is GNU C, hence a MinGW build apart from the MSVC radio side.
cmake_minimum_required(VERSION 3.15)
project(winawdl_awdl C)

# The file names in log lines and in the programs themselves start at the
# repository, not at the folder of the disk it was cloned into.
get_filename_component(REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_compile_options("-ffile-prefix-map=${REPOSITORY}=.")

set(WINAWDL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(OWL "${WINAWDL_ROOT}/upstream/owl")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

# compat/ comes first so OWL's Unix includes resolve to the shims.
# -mno-ms-bitfields lays packed structs out as GCC does on Linux.
add_library(owl_build INTERFACE)
target_include_directories(owl_build INTERFACE
    compat "${OWL}/src" "${OWL}/radiotap" "${OWL}/daemon" win
    "${WINAWDL_ROOT}/radio")
target_compile_options(owl_build INTERFACE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/owl_win.h" -mno-ms-bitfields)

add_library(radiotap STATIC "${OWL}/radiotap/radiotap.c")
target_link_libraries(radiotap PRIVATE owl_build)
# Its bounds checks subtract pointers through unsigned long, which is 32 bits
# on Windows. The differences are small, so the truncation cancels out.
target_compile_options(radiotap PRIVATE -Wno-pointer-to-int-cast)

set(OWL_SOURCES log state election sync channel schedule tx rx frame crc32 wire
    peers version hashmap siphash24 circular_buffer)
list(TRANSFORM OWL_SOURCES PREPEND "${OWL}/src/")
list(TRANSFORM OWL_SOURCES APPEND ".c")
add_library(awdl STATIC ${OWL_SOURCES})
target_link_libraries(awdl PUBLIC owl_build radiotap)

# The host adapter is kept clear of owl_build: -mno-ms-bitfields must not
# reach the Windows networking structures it fills in.
add_library(host_if STATIC win/host_if.c win/host_frame.c)
target_include_directories(host_if PUBLIC win)
target_compile_options(host_if PRIVATE -Wall -Wextra -Werror)
target_link_libraries(host_if PRIVATE iphlpapi)

set(WIN_SOURCES
    win/main.c win/ev_win.c win/io_win.c win/netutils_win.c win/radio_client.c
    win/redact.c win/frame_fixup.c win/tcp_merge.c win/service_bridge.c win/slot_policy.c
    compat/ether.c)
add_executable(awdld-mt7925 "${OWL}/daemon/core.c" ${WIN_SOURCES})
set_source_files_properties("${OWL}/daemon/core.c" PROPERTIES
    COMPILE_DEFINITIONS "awdl_is_multicast_eaw=winawdl_is_multicast_eaw;awdl_init_full_action_frame=winawdl_init_full_action_frame")
set_source_files_properties(${WIN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
target_link_libraries(awdld-mt7925 PRIVATE awdl host_if bcrypt)
# Static, so the daemon does not need the MinGW runtime DLLs beside it.
# The wrap routes OWL's log calls through main.c, which can redact them.
target_link_options(awdld-mt7925 PRIVATE -static -Wl,--wrap=log_log)

# Offline regression through the real RXD encoder and OWL receive path.
add_executable(mt7925-rx-frames EXCLUDE_FROM_ALL
    tests/rx_frames.c win/posix_win.c compat/ether.c
    ../driver/raw_rx.c ../driver/rx_ring.c ../driver/core.c)
target_include_directories(mt7925-rx-frames PRIVATE ../driver)
target_link_libraries(mt7925-rx-frames PRIVATE awdl bcrypt)
target_link_options(mt7925-rx-frames PRIVATE -static)
