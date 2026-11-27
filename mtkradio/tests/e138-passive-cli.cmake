# SPDX-License-Identifier: GPL-2.0-only
# Existing file makes every invocation stop before a device could be opened.
set(common --instance [=[PCI\VEN_14C3&DEV_7925&SUBSYS_E138105B&REV_00\OFFLINE]=]
  --firmware "${CMAKE_CURRENT_LIST_DIR}" --stop-file "${CMAKE_CURRENT_LIST_FILE}")

function(check_exit expected)
  execute_process(COMMAND "${RADIO}" --check-passive ${ARGN} ${common}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT "${result}" STREQUAL "${expected}")
    message(FATAL_ERROR "Expected ${expected} with [${ARGN}], got ${result}: ${output}${errors}")
  endif()
endfunction()

check_exit(1)
check_exit(2 --e138-fast)
check_exit(2 --indoor)
check_exit(2 --country CN)
check_exit(2 --channel 44)
