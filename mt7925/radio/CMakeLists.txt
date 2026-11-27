# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.15)
project(nekodrop_mt7925 LANGUAGES CXX)

add_executable(nekodrop-radio-mt7925 winawdl_mt7925.cpp)
target_compile_features(nekodrop-radio-mt7925 PRIVATE cxx_std_20)
target_link_libraries(nekodrop-radio-mt7925 PRIVATE setupapi bcrypt)
if(MSVC)
    target_compile_options(nekodrop-radio-mt7925 PRIVATE /W4 /WX /utf-8)
    set_property(TARGET nekodrop-radio-mt7925 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
