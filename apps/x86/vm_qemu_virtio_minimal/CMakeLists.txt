#
# Copyright 2026, Unikie
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.8.2)

project(vm-qemu-virtio-minimal-x86 C)

include(${CAMKES_VM_HELPERS_PATH})
include(${VIRTIOSO_CAMKES_VM_HELPERS_PATH})
find_package(camkes-vm-linux REQUIRED)
include(${CAMKES_VM_LINUX_HELPERS_PATH})

if(EXISTS "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.ext4.qcow2")
    set(VM_IMAGE_DRIVER_SOURCE
        "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.ext4.qcow2")
    set(VM_IMAGE_DRIVER_NAME "vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.qcow2")
    set(VM_IMAGE_DRIVER_FORMAT "qcow2")
elseif(EXISTS "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.ext4")
    set(VM_IMAGE_DRIVER_SOURCE
        "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.ext4")
    set(VM_IMAGE_DRIVER_NAME "vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.ext4")
    set(VM_IMAGE_DRIVER_FORMAT "raw")
else()
    message(FATAL_ERROR
        "No driver root disk found at "
        "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.ext4.qcow2 "
        "or ${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-driver-${VM_IMAGE_MACHINE}.rootfs.ext4")
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

add_custom_command(
    OUTPUT "${VM_IMAGE_DRIVER_BUNDLE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/images"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VM_IMAGE_DRIVER_SOURCE}" "${VM_IMAGE_DRIVER_BUNDLE}"
    DEPENDS "${VM_IMAGE_DRIVER_SOURCE}"
    VERBATIM
)
add_custom_target(vm_qemu_virtio_minimal_driver_disk ALL DEPENDS "${VM_IMAGE_DRIVER_BUNDLE}")

AddToFileServer("bzimage" "${decompressed_kernel}" DEPENDS extract_linux_kernel)
AddToFileServer("rootfs.cpio.gz" "${VM_IMAGE_ROOTFS}")

DeclareVirtiosoX86CAmkESVM(Init0)

DeclareCAmkESVMRootServer(vm_qemu_virtio_minimal.camkes)

if(SIMULATION)
    include(simulation)
    SetSimulationScriptProperty(MEM_SIZE "2048M")
    GenerateSimulateScript()
endif()
