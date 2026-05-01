#
# Copyright 2022, 2023, Unikie
#
# SPDX-License-Identifier: Apache-2.0
#

# Let's make assumption for the workspace layout. Can be overridden in cmake
# invocation/cmake cache editor
set(VM_IMAGES_DIR "${VIRTIOSO_CAMKES_VM_DIR}/../../vm-images/build/tmp/deploy/images" CACHE STRING "")
set(VM_IMAGE_LINUX "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/Image" CACHE STRING "VM kernel image")
set(VM_IMAGE_INITRD "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-boot-${VM_IMAGE_MACHINE}.rootfs.cpio.gz" CACHE STRING "VM initramfs")
set(VM_IMAGE_BZIMAGE "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/bzImage" CACHE STRING "VM bzImage")
set(VM_IMAGE_ROOTFS "${VM_IMAGES_DIR}/${VM_IMAGE_MACHINE}/vm-image-boot-${VM_IMAGE_MACHINE}.rootfs.cpio.gz" CACHE STRING "VM rootfs/initramfs")

if(DEFINED KernelARMPlatform)
    CAmkESAddImportPath(${KernelARMPlatform})
endif()

CAmkESAddTemplatesPath(${VIRTIOSO_CAMKES_VM_DIR}/templates)

CAmkESAddCPPInclude(${VIRTIOSO_CAMKES_VM_DIR})

set(CAmkESCPP ON CACHE BOOL "" FORCE)

set(configure_string "")

add_config_library(virtioso_camkes_vm "${configure_string}")

function(VirtiosoGenerateCAmkESStreamRegistry name camkes_config)
    cmake_parse_arguments(
        PARSE_ARGV
        2
        STREAM_REGISTRY
        ""
        ""
        "CPP_FLAGS;CPP_INCLUDES"
    )

    if(STREAM_REGISTRY_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown arguments: ${STREAM_REGISTRY_UNPARSED_ARGUMENTS}")
    endif()

    if(IS_ABSOLUTE "${camkes_config}")
        set(stream_registry_camkes "${camkes_config}")
    else()
        set(stream_registry_camkes "${CMAKE_CURRENT_LIST_DIR}/${camkes_config}")
    endif()

    if(NOT EXISTS "${stream_registry_camkes}")
        message(FATAL_ERROR "CAmkES file not found: ${stream_registry_camkes}")
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(VIRTIO_CAMKES_CPP cpp)
    if(NOT VIRTIO_CAMKES_CPP)
        message(FATAL_ERROR "cpp not found; cannot generate CAmkES stream registry")
    endif()

    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" stream_registry_target "${name}")
    set(stream_registry_out_dir
        "${CMAKE_BINARY_DIR}/generated/virtioso_camkes_stream_registry/${stream_registry_target}"
    )
    set(stream_registry_json "${stream_registry_out_dir}/console-stream-registry.json")
    set(stream_registry_header "${stream_registry_out_dir}/console_stream_ids.h")
    set(stream_registry_source "${stream_registry_out_dir}/console_stream_registry.c")

    set(stream_registry_includes
        "${CMAKE_CURRENT_LIST_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${VIRTIOSO_CAMKES_VM_DIR}"
        "${CAMKES_VM_DIR}/components/VM"
        "${CAMKES_VM_DIR}/components/VM/configurations"
        "${CAMKES_VM_DIR}/components/VM_Arm"
        "${CAMKES_VM_DIR}/components/VM_Arm/configurations"
        ${STREAM_REGISTRY_CPP_INCLUDES}
    )

    set(stream_registry_include_args "")
    foreach(include_dir IN LISTS stream_registry_includes)
        if(include_dir)
            list(APPEND stream_registry_include_args --cpp-include "${include_dir}")
        endif()
    endforeach()

    set(stream_registry_cpp_flag_args "")
    foreach(cpp_flag IN LISTS STREAM_REGISTRY_CPP_FLAGS)
        if(cpp_flag)
            list(APPEND stream_registry_cpp_flag_args --cpp-flag "${cpp_flag}")
        endif()
    endforeach()

    add_custom_command(
        OUTPUT
            "${stream_registry_json}"
            "${stream_registry_header}"
            "${stream_registry_source}"
        COMMAND
            ${Python3_EXECUTABLE}
            "${VIRTIOSO_CAMKES_VM_DIR}/tools/generate_camkes_stream_registry.py"
            --camkes "${stream_registry_camkes}"
            --name "${name}"
            --out-dir "${stream_registry_out_dir}"
            --cpp "${VIRTIO_CAMKES_CPP}"
            ${stream_registry_include_args}
            ${stream_registry_cpp_flag_args}
        DEPENDS
            "${stream_registry_camkes}"
            "${VIRTIOSO_CAMKES_VM_DIR}/tools/generate_camkes_stream_registry.py"
        COMMENT "Generating CAmkES stream registry for ${name}"
        VERBATIM
    )

    add_custom_target(
        "virtioso_camkes_stream_registry_${stream_registry_target}"
        ALL
        DEPENDS
            "${stream_registry_json}"
            "${stream_registry_header}"
            "${stream_registry_source}"
    )

    set(VIRTIO_CAMKES_STREAM_REGISTRY_JSON "${stream_registry_json}" PARENT_SCOPE)
    set(VIRTIO_CAMKES_STREAM_REGISTRY_HEADER "${stream_registry_header}" PARENT_SCOPE)
    set(VIRTIO_CAMKES_STREAM_REGISTRY_SOURCE "${stream_registry_source}" PARENT_SCOPE)
endfunction()

config_option(
    VmSWIOTLB
    VM_SWIOTLB
    "Compile examples with SWIOTLB enabled"
    DEFAULT
    ON
)

if(COMMAND AddCamkesCPPFlag)
    AddCamkesCPPFlag(cpp_flags CONFIG_VARS VmSWIOTLB)
endif()

file(
    GLOB
        virtioso_camkes_vm_sources
        ${VIRTIOSO_CAMKES_VM_DIR}/src/camkes/*.c
        ${VIRTIOSO_CAMKES_VM_DIR}/src/camkes/modules/*.c
)

function(DeclareVirtiosoArmCAmkESVM name)
    include(${CAMKES_ARM_VM_HELPERS_PATH})
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
        TEMPLATE_HEADERS
        seL4VirtIODeviceVM.template.h
    )
endfunction(DeclareVirtiosoArmCAmkESVM)

function(DeclareVirtiosoX86CAmkESVM name)
    cmake_parse_arguments(
        PARSE_ARGV 1 VM_COMP "" ""
        "EXTRA_SOURCES;EXTRA_INCLUDES;EXTRA_LIBS;EXTRA_C_FLAGS;EXTRA_LD_FLAGS"
    )
    DeclareCAmkESVM(
        ${name}
        EXTRA_SOURCES
        ${VM_PROJECT_DIR}/components/VM_Arm/src/modules/init_ram.c
        ${VIRTIOSO_CAMKES_VM_DIR}/src/camkes/modules/init_dataport_ram.c
        ${VIRTIOSO_CAMKES_VM_DIR}/src/camkes/modules/io_proxy.c
        ${VM_COMP_EXTRA_SOURCES}
        EXTRA_INCLUDES
        ${VIRTIOSO_CAMKES_VM_DIR}/include
        ${VIRTIOSO_CONTRACTS_INCLUDE_DIR}
        ${VM_COMP_EXTRA_INCLUDES}
        EXTRA_LIBS
        virtioso_camkes_vm
        virtioso_camkes_vm_Config
        ${VM_COMP_EXTRA_LIBS}
        EXTRA_C_FLAGS
        ${VM_COMP_EXTRA_C_FLAGS}
        EXTRA_LD_FLAGS
        ${VM_COMP_EXTRA_LD_FLAGS}
    )
    DeclareCAmkESComponent(
        ${name}
        TEMPLATE_SOURCES
        seL4VMParameters.template.c
        seL4VirtIODeviceVM.template.c
        seL4VirtIODriverVM.template.c
        TEMPLATE_HEADERS
        seL4VMParameters.template.h
        seL4VirtIODeviceVM.template.h
    )
endfunction(DeclareVirtiosoX86CAmkESVM)

function(DeclareVirtiosoCAmkESVM name)
    DeclareVirtiosoArmCAmkESVM(${name})
endfunction(DeclareVirtiosoCAmkESVM)
