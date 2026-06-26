# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(ARGISENSE_MODULE_ROOT ${CMAKE_CURRENT_LIST_DIR})

function(argisense_add_module_subdirs)
  get_property(argisense_module_subdirs_added GLOBAL PROPERTY ARGISENSE_MODULE_SUBDIRS_ADDED)

  if(NOT argisense_module_subdirs_added)
    set_property(GLOBAL PROPERTY ARGISENSE_MODULE_SUBDIRS_ADDED TRUE)
    zephyr_include_directories(${ARGISENSE_MODULE_ROOT}/include)
    add_subdirectory(${ARGISENSE_MODULE_ROOT}/drivers ${CMAKE_CURRENT_BINARY_DIR}/argisense_drivers)
    add_subdirectory(${ARGISENSE_MODULE_ROOT}/lib ${CMAKE_CURRENT_BINARY_DIR}/argisense_lib)
  endif()
endfunction()

# When Zephyr includes this repository as a module, only register module content.
if(DEFINED ZEPHYR_CURRENT_MODULE_DIR)
  argisense_add_module_subdirs()
  return()
endif()

set(KCONFIG_ROOT ${CMAKE_CURRENT_LIST_DIR}/app/Kconfig)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR})
list(APPEND SNIPPET_ROOT ${CMAKE_CURRENT_LIST_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(argisense)

target_sources(app PRIVATE
  app/src/main.c
  app/src/current_loop_output.c
  app/src/argisense_power.c
  app/src/argisense_registers.c
  app/src/argisense_settings.c
  app/src/argisense_rs485.c
  app/src/argisense_shell.c
)
target_sources_ifdef(CONFIG_ARGISENSE_RS485_DFU app PRIVATE
  app/src/argisense_dfu.c
)
target_sources_ifdef(CONFIG_ARGISENSE_USB_DEVICE app PRIVATE
  app/src/argisense_usb.c
)
argisense_add_module_subdirs()
