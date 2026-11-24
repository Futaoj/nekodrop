# SPDX-License-Identifier: GPL-2.0-only
# User-mode MediaTek MT7922 / MT7921 radio daemon for nekodrop / winawdl.
cmake_minimum_required(VERSION 3.20)
project(mtkradio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(MTK_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(MTK_OPTS /W3 /EHsc -Wno-address-of-packed-member -Wno-unused-function)
else()
  set(MTK_OPTS /W3 /EHsc /wd4005)
endif()

add_executable(nekodrop-radio-mtk
  src/mtk_main.cpp
  src/mtk_dev.cpp
  src/mtk_dma.cpp
  ../iwxradio/src/proxy.cpp
  ../iwxradio/src/plat_win.cpp
)
target_include_directories(nekodrop-radio-mtk PRIVATE src ../iwxradio/src ../driver/regproxy)
target_compile_definitions(nekodrop-radio-mtk PRIVATE ${MTK_DEFS})
target_compile_options(nekodrop-radio-mtk PRIVATE ${MTK_OPTS})
target_link_libraries(nekodrop-radio-mtk PRIVATE setupapi winmm advapi32)
