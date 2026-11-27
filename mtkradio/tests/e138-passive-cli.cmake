# SPDX-License-Identifier: GPL-2.0-only
# Existing file makes every invocation stop before a device could be opened.
set(common --instance [=[PCI\VEN_14C3&DEV_7925&SUBSYS_E138105B&REV_00\OFFLINE]=]
  --firmware "${CMAKE_CURRENT_LIST_DIR}" --stop-file "${CMAKE_CURRENT_LIST_FILE}")

execute_process(COMMAND "${RADIO}" --help RESULT_VARIABLE result OUTPUT_VARIABLE help)
if(NOT "${result}" STREQUAL "0" OR NOT help MATCHES "MT7921/MT7922")
  message(FATAL_ERROR "Family radio help unavailable: ${help}")
endif()

function(check_exit expected)
  execute_process(COMMAND "${RADIO}" ${ARGN} ${common}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT "${result}" STREQUAL "${expected}")
    message(FATAL_ERROR "Expected ${expected} with [${ARGN}], got ${result}: ${output}${errors}")
  endif()
endfunction()

if(E138_VALIDATION)
  if(NOT help MATCHES "MT7925 E138")
    message(FATAL_ERROR "Validation build omitted E138 from family help")
  endif()
  check_exit(1 --check-passive)
  check_exit(0 --e138-fast --indoor --country CN --channel 149)
  check_exit(2 --check-passive --e138-fast)
  check_exit(2 --check-passive --indoor)
  check_exit(2 --check-passive --country CN)
  check_exit(2 --check-passive --channel 44)
else()
  if(help MATCHES "MT7925 E138")
    message(FATAL_ERROR "Production build advertises E138 validation")
  endif()
  check_exit(2 --check-passive)
  check_exit(2 --e138-fast --indoor --country CN --channel 149)
endif()
check_exit(2)
