#
# Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
# Copyright 2022, 2023, Unikie
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

if(AppArch STREQUAL "Arm")
    if(KernelPlatformRpi4)
        file(GLOB plat_deps src/plat/rpi4/*.c)
    else()
        file(GLOB plat_deps src/plat/${KernelPlatform}/*.c)
    endif()
    list(APPEND virtioso_camkes_vm_sources ${plat_deps})
else()
    set(
        virtioso_camkes_vm_sources
        src/emulated_device.c
        src/io_proxy.c
        src/irq_line.c
        src/irq_reservations.c
        src/libsel4vm_glue.c
        src/list.c
        src/mmio_reservations.c
        src/msi.c
        src/ram_dataport.c
        src/shared_irq_line.c
    )
endif()

add_library(virtioso_camkes_vm OBJECT ${virtioso_camkes_vm_sources})

set(
    VIRTIOSO_CONTRACTS_INCLUDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../sources/virtioso-contracts/include"
    CACHE PATH "Path to shared Virtioso contract headers"
)

target_include_directories(
    virtioso_camkes_vm
    PUBLIC
        include
        ${VIRTIOSO_CONTRACTS_INCLUDE_DIR}
)
target_link_libraries(
    virtioso_camkes_vm
    sel4_autoconf
    sel4sync
    sel4vm
    sel4vmmplatsupport
)

if(AppArch STREQUAL "Arm")
    target_link_libraries(
        virtioso_camkes_vm
        fdt
        virtioarm
    )
endif()

# Add VM application
add_subdirectory("apps/${AppArch}/${CAMKES_VM_APP}")
# Should be done adding targets, can now generate the root server and the global configuration
GenerateCAmkESRootserver()
