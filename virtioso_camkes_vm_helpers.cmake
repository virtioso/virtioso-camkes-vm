#
# Copyright 2022, 2023, Unikie
#
# SPDX-License-Identifier: Apache-2.0
#

include(${CAMKES_ARM_VM_HELPERS_PATH})

# Let's make assumption for the workspace layout. Can be overridden in cmake
# invocation/cmake cache editor
set(VM_IMAGES_DIR "${VIRTIOSO_CAMKES_VM_DIR}/../../vm-images/build/tmp/deploy/images" CACHE STRING "")
set(VM_IMAGE_LINUX "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/Image" CACHE STRING "VM kernel image")
set(VM_IMAGE_INITRD "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-boot-${VM_IMAGE_MACHINE}.rootfs.cpio.gz" CACHE STRING "VM initramfs")

CAmkESAddImportPath(${KernelARMPlatform})

CAmkESAddTemplatesPath(${VIRTIOSO_CAMKES_VM_DIR}/templates)

CAmkESAddCPPInclude(${VIRTIOSO_CAMKES_VM_DIR})

set(CAmkESCPP ON CACHE BOOL "" FORCE)

set(configure_string "")

add_config_library(virtioso_camkes_vm "${configure_string}")

config_option(
    VmSWIOTLB
    VM_SWIOTLB
    "Compile examples with SWIOTLB enabled"
    DEFAULT
    ON
)

AddCamkesCPPFlag(cpp_flags CONFIG_VARS VmSWIOTLB)

file(
    GLOB
        virtioso_camkes_vm_sources
        ${VIRTIOSO_CAMKES_VM_DIR}/src/camkes/*.c
        ${VIRTIOSO_CAMKES_VM_DIR}/src/camkes/modules/*.c
)

function(DeclareVirtiosoCAmkESVM name)
    DeclareCAmkESARMVM(${name})
    DeclareCAmkESComponent(
        ${name}
        SOURCES
        ${virtioso_camkes_vm_sources}
        LIBS
        virtioso_camkes_vm
        virtioso_camkes_vm_Config
        TEMPLATE_SOURCES
        seL4VirtIODeviceVM.template.c
        seL4VirtIODriverVM.template.c
        pl011.template.c
        hyp_ftrace.template.c
        TEMPLATE_HEADERS
        seL4VirtIODeviceVM.template.h
    )
endfunction(DeclareVirtiosoCAmkESVM)
