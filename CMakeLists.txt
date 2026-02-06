#
# Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
# Copyright 2022, 2023, Technology Innovation Institute
#
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.12.0)
include(settings.cmake)
project(virtioso-camkes-vm C CXX ASM)

find_package(virtioso-camkes-vm REQUIRED)
virtioso_camkes_vm_setup()

file(
    GLOB
        virtioso_camkes_vm_sources
        src/*.c
)

if(KernelPlatformRpi4)
    file(GLOB plat_deps src/plat/rpi4/*.c)
else()
    file(GLOB plat_deps src/plat/${KernelPlatform}/*.c)
endif()
list(APPEND virtioso_camkes_vm_sources ${plat_deps})


add_library(virtioso_camkes_vm OBJECT ${virtioso_camkes_vm_sources})

target_include_directories(
    virtioso_camkes_vm
    PUBLIC include
)
target_link_libraries(
    virtioso_camkes_vm
    fdt
    sel4_autoconf
    sel4sync
    sel4vm
    sel4vmmplatsupport
    virtioarm
)

# Add VM application
add_subdirectory("apps/${AppArch}/${CAMKES_VM_APP}")
# Should be done adding targets, can now generate the root server and the global configuration
GenerateCAmkESRootserver()
