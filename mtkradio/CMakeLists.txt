# SPDX-License-Identifier: GPL-2.0-only
# User-mode MediaTek MT7922 / MT7921 radio daemon for nekodrop / winawdl.
cmake_minimum_required(VERSION 3.20)
project(mtkradio C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(MTK_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(MTK_OPTS /W3 /EHsc -Wno-address-of-packed-member -Wno-unused-function)
else()
  set(MTK_OPTS /W3 /EHsc /wd4005 /utf-8)
endif()

set(IWX_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../iwxradio/src)
set(MT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../mtradio/src)

# Shared Wi-Fi station (join, WPA2/WPA3, TAP bridge) from iwxradio, driven via
# StaDevice (iwxradio/src/sta_device.h) by mtk_station.cpp with mtradio's host CCMP.
add_library(mtk_stashared STATIC
  ${IWX_SRC}/wifi_sta.cpp
  ${IWX_SRC}/tap.cpp
  ${IWX_SRC}/crypto.cpp
  ${IWX_SRC}/wpa3.cpp
)
target_include_directories(mtk_stashared PUBLIC ${IWX_SRC})
target_compile_definitions(mtk_stashared PRIVATE ${MTK_DEFS})
target_compile_options(mtk_stashared PRIVATE ${MTK_OPTS})
target_link_libraries(mtk_stashared PUBLIC bcrypt setupapi)

set(CONNAC3_SRC third_party/connac3)
add_library(mtk_connac3 STATIC
  ${CONNAC3_SRC}/core.c
  ${CONNAC3_SRC}/firmware.c
  ${CONNAC3_SRC}/nic_caps.c
  ${CONNAC3_SRC}/efuse_type.c
  ${CONNAC3_SRC}/regulatory.c
  ${CONNAC3_SRC}/clc_power.c
  ${CONNAC3_SRC}/board-domain.c
  ${CONNAC3_SRC}/board-sar.c
  ${CONNAC3_SRC}/station-reg.c
  ${CONNAC3_SRC}/passive-scan.c
)
target_include_directories(mtk_connac3 PUBLIC ${CONNAC3_SRC})
set_target_properties(mtk_connac3 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

option(NEKODROP_E138_VALIDATION "Include E138 validation modes in the MediaTek family radio" OFF)
set(mtk_daemon nekodrop-radio-mtk)

add_executable(${mtk_daemon}
  src/mtk_main.cpp
  src/mtk_dev.cpp
  src/mtk_dev_7925.cpp
  src/mtk_dev_7925_tx.cpp
  src/mt7925_sar.cpp
  src/mtk_dma.cpp
  src/mtk_station.cpp
  ${MT_SRC}/ccmp.cpp
  ${IWX_SRC}/proxy.cpp
  ${IWX_SRC}/plat_win.cpp
)
target_include_directories(${mtk_daemon} PRIVATE src ${IWX_SRC} ${MT_SRC} ../driver/regproxy)
target_compile_definitions(${mtk_daemon} PRIVATE ${MTK_DEFS})
target_compile_options(${mtk_daemon} PRIVATE ${MTK_OPTS})
target_link_libraries(${mtk_daemon} PRIVATE mtk_stashared mtk_connac3 setupapi winmm advapi32 bcrypt)
if(NEKODROP_E138_VALIDATION)
  target_sources(${mtk_daemon} PRIVATE src/e138_main.cpp)
  target_compile_definitions(${mtk_daemon} PRIVATE NEKODROP_E138_VALIDATION)
  target_link_libraries(${mtk_daemon} PRIVATE shell32)
endif()

add_executable(ccmp_test
  ../mtradio/tests/ccmp_test.cpp
  ${MT_SRC}/ccmp.cpp
)
target_include_directories(ccmp_test PRIVATE ${MT_SRC})
target_compile_definitions(ccmp_test PRIVATE ${MTK_DEFS})
target_compile_options(ccmp_test PRIVATE ${MTK_OPTS})
target_link_libraries(ccmp_test PRIVATE bcrypt)

enable_testing()
add_test(NAME mtk_family_cli COMMAND ${CMAKE_COMMAND}
  -DRADIO=$<TARGET_FILE:${mtk_daemon}> -DE138_VALIDATION=${NEKODROP_E138_VALIDATION}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/e138-passive-cli.cmake)
add_test(NAME ccmp COMMAND ccmp_test)

add_executable(mt7925_transport_test tests/mt7925-transport-test.cpp)
target_include_directories(mt7925_transport_test PRIVATE src ${IWX_SRC})
target_compile_options(mt7925_transport_test PRIVATE /UNDEBUG ${MTK_OPTS})
target_link_libraries(mt7925_transport_test PRIVATE mtk_connac3)
add_test(NAME mt7925_transport COMMAND mt7925_transport_test)

foreach(test_name IN ITEMS board reg)
  add_executable(mt7925_${test_name}_test tests/mt7925-${test_name}-test.c)
  set_target_properties(mt7925_${test_name}_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_compile_options(mt7925_${test_name}_test PRIVATE /UNDEBUG)
  target_link_libraries(mt7925_${test_name}_test PRIVATE mtk_connac3)
  add_test(NAME mt7925_${test_name} COMMAND mt7925_${test_name}_test)
endforeach()

add_executable(mtk_patch_test tests/mtk_patch_test.cpp)
target_include_directories(mtk_patch_test PRIVATE src)
target_compile_options(mtk_patch_test PRIVATE /UNDEBUG ${MTK_OPTS})
add_test(NAME mtk_patch COMMAND mtk_patch_test)
