#
# Copyright 2023, Unikie
#
# SPDX-License-Identifier: BSD-2-Clause
#

set(VIRTIOSO_CAMKES_VM_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE STRING "")
set(VIRTIOSO_CAMKES_VM_HELPERS_PATH "${CMAKE_CURRENT_LIST_DIR}/virtioso_camkes_vm_helpers.cmake" CACHE STRING "")
mark_as_advanced(VIRTIOSO_CAMKES_VM_DIR VIRTIOSO_CAMKES_VM_HELPERS_PATH)

macro(virtioso_camkes_vm_setup)
if(AppArch STREQUAL "Arm")
    find_package(camkes-arm-vm REQUIRED)
    camkes_arm_vm_setup_arm_vm_environment()
else()
    message(FATAL_ERROR "Unsupported")
endif()
endmacro()

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(virtioso-camkes-vm DEFAULT_MSG VIRTIOSO_CAMKES_VM_DIR VIRTIOSO_CAMKES_VM_HELPERS_PATH)
