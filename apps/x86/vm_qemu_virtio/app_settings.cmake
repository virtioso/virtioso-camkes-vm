#
# Copyright 2026, Unikie
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.8.2)

if(NOT "${KernelPlatform}" STREQUAL "pc99")
    message(FATAL_ERROR "KernelPlatform: ${KernelPlatform} not supported. Supported: pc99")
endif()

set(VM_IMAGE_MACHINE "qemux86-64")
set(CAmkESVMGuestDMAIommu ON CACHE BOOL "" FORCE)
set(KernelSel4Arch x86_64 CACHE STRING "" FORCE)
set(KernelX86_64VTX64BitGuests ON CACHE BOOL "" FORCE)
set(KernelMaxNumNodes 1 CACHE STRING "" FORCE)
set(LibSel4VMMUseHPET ON CACHE BOOL "" FORCE)
