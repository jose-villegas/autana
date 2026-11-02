# Sets board_dir, the folder under main/board/ of the board being built:
# CONFIG_BOARD_DIR on the device, and on a host, which has no sdkconfig, the
# BOARD_DIR of the Kconfig choice BOARD's default (main/Kconfig.projbuild).
# tools/build/packages.py and packages.sh read the same default.

if(DEFINED CONFIG_BOARD_DIR)
    set(board_dir "${CONFIG_BOARD_DIR}")
else()
    set(board_dir "")
    set(board_choice "")
    set(board_in_choice FALSE)
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../Kconfig.projbuild" board_kconfig_lines)
    foreach(board_line IN LISTS board_kconfig_lines)
        if(board_line MATCHES "^ *choice BOARD$")
            set(board_in_choice TRUE)
        elseif(board_in_choice AND board_line MATCHES "^ *endchoice")
            set(board_in_choice FALSE)
        elseif(board_in_choice AND NOT board_choice AND board_line MATCHES "^ *default ([A-Z0-9_]+)$")
            set(board_choice "${CMAKE_MATCH_1}")
        elseif(board_choice AND board_line MATCHES "^ *default \"([a-z0-9_]+)\" if ${board_choice}$")
            set(board_dir "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endif()
if(NOT board_dir)
    message(FATAL_ERROR "no board folder: main/Kconfig.projbuild's choice BOARD names none")
endif()
