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

set(ARGISENSE_APP_ROOT ${CMAKE_CURRENT_LIST_DIR}/app)
set(ARGISENSE_COMMON_SRC ${ARGISENSE_APP_ROOT}/common/src)
set(ARGISENSE_MP_SRC ${ARGISENSE_APP_ROOT}/products/methane_pressure/src)
set(ARGISENSE_PH_SRC ${ARGISENSE_APP_ROOT}/products/ph/src)

target_include_directories(app PRIVATE
  ${ARGISENSE_COMMON_SRC}
)

if(CONFIG_BOARD_ARGISENSE_PH_U575RG)
  target_include_directories(app PRIVATE
    ${ARGISENSE_PH_SRC}
  )
  target_sources(app PRIVATE
    ${ARGISENSE_PH_SRC}/main.c
    ${ARGISENSE_PH_SRC}/argisense_registers.c
    ${ARGISENSE_PH_SRC}/argisense_settings.c
    ${ARGISENSE_PH_SRC}/argisense_rs485.c
    ${ARGISENSE_PH_SRC}/argisense_shell.c
    ${ARGISENSE_PH_SRC}/current_loop_output.c
    ${ARGISENSE_PH_SRC}/ph_measurement.c
  )
elseif(CONFIG_BOARD_ARGISENSE_MP_U575RG)
  target_include_directories(app PRIVATE
    ${ARGISENSE_MP_SRC}
  )
  target_sources(app PRIVATE
    ${ARGISENSE_MP_SRC}/main.c
    ${ARGISENSE_MP_SRC}/current_loop_output.c
    ${ARGISENSE_MP_SRC}/argisense_power.c
    ${ARGISENSE_MP_SRC}/argisense_registers.c
    ${ARGISENSE_MP_SRC}/argisense_settings.c
    ${ARGISENSE_MP_SRC}/argisense_rs485.c
    ${ARGISENSE_MP_SRC}/argisense_shell.c
  )
else()
  message(FATAL_ERROR "Unsupported ArgiSense board. Expected argisense_mp_u575rg or argisense_ph_u575rg.")
endif()
target_sources_ifdef(CONFIG_ARGISENSE_RS485_DFU app PRIVATE
  ${ARGISENSE_COMMON_SRC}/argisense_dfu.c
)
target_sources_ifdef(CONFIG_ARGISENSE_USB_DEVICE app PRIVATE
  ${ARGISENSE_COMMON_SRC}/argisense_usb.c
)
argisense_add_module_subdirs()
