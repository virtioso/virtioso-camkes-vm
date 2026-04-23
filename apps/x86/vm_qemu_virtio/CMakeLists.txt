#
# Copyright 2026, Unikie
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.8.2)

project(vm-qemu-virtio-x86 C)

include(${CAMKES_VM_HELPERS_PATH})
include(${VIRTIOSO_CAMKES_VM_HELPERS_PATH})
find_package(camkes-vm-linux REQUIRED)
include(${CAMKES_VM_LINUX_HELPERS_PATH})

AddToFileServer("bzimage" "${VM_IMAGE_BZIMAGE}")
AddToFileServer("rootfs.cpio.gz" "${VM_IMAGE_ROOTFS}")

DeclareVirtiosoX86CAmkESVM(Init0)
DeclareVirtiosoX86CAmkESVM(Init1)

DeclareCAmkESVMRootServer(vm_qemu_virtio.camkes)
