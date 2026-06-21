# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(ARGISENSE_MODULE_ROOT ${CMAKE_CURRENT_LIST_DIR})

function(argisense_add_module_subdirs)
  if(NOT ARGISENSE_MODULE_SUBDIRS_ADDED)
    set(ARGISENSE_MODULE_SUBDIRS_ADDED TRUE CACHE INTERNAL "ArgiSense module subdirectories added")
    add_subdirectory(${ARGISENSE_MODULE_ROOT}/drivers ${CMAKE_CURRENT_BINARY_DIR}/argisense_drivers)
    add_subdirectory(${ARGISENSE_MODULE_ROOT}/lib ${CMAKE_CURRENT_BINARY_DIR}/argisense_lib)
  endif()
endfunction()

# When Zephyr includes this repository as a module, only register module content.
if(DEFINED ZEPHYR_CURRENT_MODULE_DIR)
  argisense_add_module_subdirs()
  return()
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(argisense)

target_sources(app PRIVATE app/src/main.c)
argisense_add_module_subdirs()
