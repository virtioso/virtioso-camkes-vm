#
# Copyright 2026, Technology Innovation Institute
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.8.2)

project(vm-qemu-virtio-x86 C)

include(${CAMKES_VM_HELPERS_PATH})
find_package(camkes-vm-linux REQUIRED)
include(${CAMKES_VM_LINUX_HELPERS_PATH})

set(VM_IMAGES_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../vm-images/build/tmp/deploy/images")
set(VM_IMAGE_MACHINE "qemux86-64")
set(VM_IMAGE_BZIMAGE "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/bzImage")
set(VM_IMAGE_ROOTFS "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-boot-${VM_IMAGE_MACHINE}.rootfs.cpio.gz")
set(VM_IMAGE_DRIVER_QCOW2 "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.ext4.qcow2")
set(VM_IMAGE_DRIVER_RAW "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.ext4")

if(EXISTS "${VM_IMAGE_DRIVER_QCOW2}")
    set(VM_IMAGE_DRIVER_SOURCE "${VM_IMAGE_DRIVER_QCOW2}")
    set(VM_IMAGE_DRIVER_FORMAT "qcow2")
    set(VM_IMAGE_DRIVER_NAME "vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.qcow2")
elseif(EXISTS "${VM_IMAGE_DRIVER_RAW}")
    set(VM_IMAGE_DRIVER_SOURCE "${VM_IMAGE_DRIVER_RAW}")
    set(VM_IMAGE_DRIVER_FORMAT "raw")
    set(VM_IMAGE_DRIVER_NAME "vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.ext4")
else()
    message(FATAL_ERROR "No driver root disk found at ${VM_IMAGE_DRIVER_QCOW2} or ${VM_IMAGE_DRIVER_RAW}")
endif()

set(VM_IMAGE_DRIVER_BUNDLE "${CMAKE_BINARY_DIR}/images/${VM_IMAGE_DRIVER_NAME}")
string(
    CONCAT VM_QEMU_EXTRA_ARGS
    "-machine q35,sata=off "
    "-nographic "
    "-vga none "
    "-device intel-iommu,intremap=off,caching-mode=on "
    "-netdev user,id=net0 "
    "-device virtio-net-pci,bus=pcie.0,addr=0x1,netdev=net0,mac=52:55:00:d1:55:01,disable-legacy=on,disable-modern=off,iommu_platform=on "
    "-drive id=disk0,file=images/${VM_IMAGE_DRIVER_NAME},if=none,format=${VM_IMAGE_DRIVER_FORMAT} "
    "-device virtio-blk-pci,bus=pcie.0,addr=0x2,drive=disk0,disable-legacy=on,disable-modern=off,iommu_platform=on "
)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/images")
file(WRITE "${CMAKE_BINARY_DIR}/images/qemu-extra-args" "${VM_QEMU_EXTRA_ARGS}\n")

DecompressLinuxKernel(extract_linux_kernel decompressed_kernel "${VM_IMAGE_BZIMAGE}")
AddToFileServer("bzimage" "${decompressed_kernel}" DEPENDS extract_linux_kernel)
AddToFileServer("rootfs.cpio.gz" "${VM_IMAGE_ROOTFS}")

add_custom_command(
    OUTPUT "${VM_IMAGE_DRIVER_BUNDLE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/images"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VM_IMAGE_DRIVER_SOURCE}" "${VM_IMAGE_DRIVER_BUNDLE}"
    DEPENDS "${VM_IMAGE_DRIVER_SOURCE}"
    VERBATIM
)
add_custom_target(vm_qemu_virtio_driver_disk ALL DEPENDS "${VM_IMAGE_DRIVER_BUNDLE}")

DeclareCAmkESVM(Init0)
DeclareCAmkESVMRootServer(vm_qemu_virtio.camkes)

if(SIMULATION)
    include(simulation)
    GenerateSimulateScript()
endif()
