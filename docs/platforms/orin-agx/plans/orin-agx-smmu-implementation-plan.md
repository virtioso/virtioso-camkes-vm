# Orin AGX SMMU Support Implementation Plan for seL4/CAmkES VM

**Date**: 2025-12-31
**Status**: Planning
**Author**: Claude Code (Unikie)

## Executive Summary

Supporting Orin AGX (Tegra234) SMMUs on seL4 requires significant work due to the platform's unique architecture: **three independent SMMUv2 instances** tightly coupled with the **Memory Controller (MC)** for stream ID management. The existing seL4 SMMUv2 driver (TX2) provides a foundation but needs substantial extension.

**Estimated effort**: ~10 weeks full scope, ~4 weeks MVP

---

## Table of Contents

1. [Hardware Architecture Overview](#hardware-architecture-overview)
2. [Implementation Phases](#implementation-phases)
3. [Key Technical Challenges](#key-technical-challenges)
4. [Resource Estimates](#resource-estimates)
5. Recommended Initial Scope
6. [References](#references)

---

## Hardware Architecture Overview

### SMMU Instances (All ARM MMU-500, SMMUv2)

| Instance | Base Address | Size | Devices |
|----------|-------------|------|---------|
| smmu_niso1 | 0x08000000 | 16MB | GPU, VIC, NVDEC, USB, PCIe, SDMMC |
| smmu_niso0 | 0x12000000 | 16MB | Display, NVENC, DLA, Ethernet |
| smmu_iso | 0x10000000 | 16MB | Secure display path |

**Key characteristic**: Each `niso0`/`niso1` has **dual register bases** that must be programmed identically (NVIDIA arm-smmu-nvidia.c handles this).

### Memory Controller Integration

Tegra234 **does NOT use SMMU's Stream Match Registers (SMR)** for device-to-SID mapping. Instead:

```
Device -> MC Client ID -> MC SID Override Reg (0x02c00000+offset) -> Stream ID -> SMMU
```

The MC has **93 client entries** in `tegra234_mc_clients[]`, each mapping a hardware client to a stream ID.

### Stream ID Programming Mechanisms

Tegra234 has **three distinct mechanisms** for programming device Stream IDs:

| Mechanism | Description | Clients | pKVM Status |
|-----------|-------------|---------|-------------|
| MC SID Override Registers | Centralized in Memory Controller | 93 clients (VIC, NVDEC, PCIe, SDMMC, etc.) | Validated by EL2 |
| Device-Specific SID Registers | Per-device registers | host1x, crypto, PVA, DCE, ADSP, Ethernet | NOT validated |
| Firmware-Set SIDs | Configured by MB1/MB2 | XUSB | No runtime programming |

### MC Client Examples

```c
// From tegra234_mc_clients[] in Linux drivers/memory/tegra/tegra234.c
TEGRA234_MC_CLIENT_VICSRD    = 0x6c  // VIC read
TEGRA234_MC_CLIENT_VICSWR    = 0x6d  // VIC write
TEGRA234_MC_CLIENT_NVDECSRD  = 0x9c  // NVDEC read
TEGRA234_MC_CLIENT_GPUSRD    = 0x4c  // GPU read
TEGRA234_MC_CLIENT_GPUSWR    = 0x4d  // GPU write
// ... 93 total clients
```

---

## Implementation Phases

### Phase 1: Kernel SMMU Driver Extension (Weeks 1-3)

**Goal**: Extend seL4 SMMUv2 driver to support Orin AGX hardware.

#### 1.1 Multi-Instance SMMU Support

The current driver assumes a single SMMU at `SMMU_TX2_PADDR`. Needs:

```c
// New: Per-instance state
struct smmu_instance {
    pptr_t base;
    pptr_t secondary_base;  // For NVIDIA dual-base
    uint32_t num_cbanks;
    uint32_t num_stream_ids;
    uint32_t num_stream_map_groups;
    struct smmu_feature features;
};

// Support 3 instances for Orin
#define MAX_SMMU_INSTANCES 3
static struct smmu_instance smmu_instances[MAX_SMMU_INSTANCES];

// Instance-aware API
void smmu_cb_assign_vspace_inst(uint32_t inst, word_t cb, vspace_root_t *vspace, asid_t asid);
void smmu_sid_bind_cb_inst(uint32_t inst, word_t sid, word_t cb);
```

**Files to modify**:
- `kernel/src/drivers/smmu/smmuv2.c` - Multi-instance support
- `kernel/include/drivers/smmu/smmuv2.h` - Instance abstraction

#### 1.2 Orin AGX Platform Configuration

**File**: `kernel/src/plat/orinagx/config.cmake`

```cmake
# Change from:
set(KernelArmSMMU OFF)

# To:
set(KernelArmSMMU ON)
set(KernelArmSMMUOrinAGX ON)  # New config for Orin-specific features
set(KernelOrinSMMUInstances 3) # Number of SMMU instances
```

**New file**: `kernel/src/plat/orinagx/machine/smmu.c`

```c
#include <drivers/smmu/smmuv2.h>
#include <plat/machine/smmu.h>

// Orin AGX SMMU base addresses
#define SMMU_NISO1_PADDR        0x08000000
#define SMMU_NISO1_SECONDARY    0x09000000
#define SMMU_NISO0_PADDR        0x12000000
#define SMMU_NISO0_SECONDARY    0x11000000
#define SMMU_ISO_PADDR          0x10000000
#define SMMU_ISO_SECONDARY      0x0  // No secondary for iso

// Platform-specific SMMU initialization for Tegra234
BOOT_CODE void plat_smmu_init(void) {
    // Initialize three SMMU instances
    smmu_instance_init(0, SMMU_NISO1_PADDR, SMMU_NISO1_SECONDARY);
    smmu_instance_init(1, SMMU_NISO0_PADDR, SMMU_NISO0_SECONDARY);
    smmu_instance_init(2, SMMU_ISO_PADDR, SMMU_ISO_SECONDARY);

    // Probe and reset each instance
    for (int i = 0; i < 3; i++) {
        smmu_config_probe(&smmu_instances[i]);
        smmu_mapping_init(&smmu_instances[i]);
        smmu_dev_reset(&smmu_instances[i]);
    }
}
```

#### 1.3 Memory Controller (MC) Integration

**Critical**: seL4 must program MC SID override registers when binding devices.

**New file**: `kernel/src/plat/orinagx/machine/mc.c`

```c
#include <plat/machine/mc.h>

// MC register base
#define TEGRA234_MC_BASE    0x02c00000
#define MC_PPTR             /* kernel virtual address for MC */

// MC client structure (derived from tegra234_mc_clients[])
struct mc_client {
    uint32_t id;              // Client ID (e.g., 0x6c for VICSRD)
    uint32_t sid_override_reg; // Register offset for SID override
    uint32_t sid_override_sec; // Secondary register offset (if any)
    uint8_t smmu_instance;    // Which SMMU instance (0-2)
    uint8_t default_sid;      // Default stream ID
};

// Tegra234 MC client table (subset - full table has 93 entries)
static const struct mc_client tegra234_mc_clients[] = {
    { .id = 0x6c, .sid_override_reg = 0x1b0, .smmu_instance = 0, .default_sid = 0x03 }, // VICSRD
    { .id = 0x6d, .sid_override_reg = 0x1b4, .smmu_instance = 0, .default_sid = 0x03 }, // VICSWR
    { .id = 0x9c, .sid_override_reg = 0x270, .smmu_instance = 0, .default_sid = 0x04 }, // NVDECSRD
    // ... etc
};

// Program MC SID override for a client
void mc_set_sid_override(uint32_t client_id, uint32_t sid) {
    const struct mc_client *client = mc_find_client(client_id);
    if (!client) return;

    uint32_t val = (sid & 0xFF) | MC_SID_OVERRIDE_ENABLE;
    mc_write_reg32(client->sid_override_reg, val);

    // Write to secondary base if present
    if (client->sid_override_sec) {
        mc_write_reg32(client->sid_override_sec, val);
    }
}

// Get SMMU instance for a client
uint32_t mc_get_smmu_instance(uint32_t client_id) {
    const struct mc_client *client = mc_find_client(client_id);
    return client ? client->smmu_instance : 0;
}
```

#### 1.4 Dual-Base Register Programming

NVIDIA's SMMUv2 requires synchronous writes to dual bases:

```c
// In smmuv2.c
static void smmu_write_reg32_dual(struct smmu_instance *inst, uint32_t offset, uint32_t val) {
    smmu_write_reg32(inst->base, offset, val);
    if (inst->secondary_base) {
        smmu_write_reg32(inst->secondary_base, offset, val);
    }
}

// Use for SMR and S2CR registers which must be synchronized
void smmu_sid_bind_cb_inst(uint32_t inst_id, word_t sid, word_t cb) {
    struct smmu_instance *inst = &smmu_instances[inst_id];

    uint32_t reg = S2CR_PRIVCFG_SET(S2CR_PRIVCFG_DEFAULT);
    reg |= S2CR_TYPE_SET(S2CR_TYPE_CB);
    reg |= S2CR_CBNDX_SET(cb);

    // Must write to both bases
    smmu_write_reg32_dual(inst, SMMU_S2CRn(sid), reg);

    if (inst->features.stream_match) {
        reg = SMR_VALID_SET(SMR_VALID_EN) | SMR_ID_SET(sid);
        smmu_write_reg32_dual(inst, SMMU_SMRn(sid), reg);
    }
}
```

#### 1.5 DTS and Hardware Description

**File**: `kernel/tools/dts/orinagx.dts` additions

```dts
/ {
    smmu_niso1: iommu@8000000 {
        compatible = "nvidia,tegra234-smmu";
        reg = <0x0 0x08000000 0x0 0x1000000>,
              <0x0 0x09000000 0x0 0x1000000>;  /* Secondary base */
        #iommu-cells = <1>;
        status = "okay";
    };

    smmu_niso0: iommu@12000000 {
        compatible = "nvidia,tegra234-smmu";
        reg = <0x0 0x12000000 0x0 0x1000000>,
              <0x0 0x11000000 0x0 0x1000000>;
        #iommu-cells = <1>;
        status = "okay";
    };

    smmu_iso: iommu@10000000 {
        compatible = "nvidia,tegra234-smmu";
        reg = <0x0 0x10000000 0x0 0x1000000>;
        #iommu-cells = <1>;
        status = "okay";
    };

    mc: memory-controller@2c00000 {
        compatible = "nvidia,tegra234-mc";
        reg = <0x0 0x02c00000 0x0 0x10000>;
        status = "okay";
    };
};
```

---

### Phase 2: Kernel API Extensions (Weeks 3-4)

**Goal**: Extend SMMU syscall API for multi-instance and MC integration.

#### 2.1 Extended SID Management

Current API assumes global SID space. Need instance-aware SID allocation:

**File**: `kernel/libsel4/arch_include/arm/interfaces/object-api-arch.xml` additions

```xml
<!-- New: Instance-aware SID binding -->
<method id="ARMSIDBindCBInst" name="BindCB" manual_name="Bind CB (Instance)">
    <brief>Bind a stream ID to a context bank on a specific SMMU instance</brief>
    <param dir="in" name="cb" type="seL4_ARMCB">Context bank capability</param>
    <param dir="in" name="smmu_instance" type="seL4_Word">SMMU instance (0-2 for Orin)</param>
    <param dir="in" name="mc_client_id" type="seL4_Word">MC client ID for SID override</param>
</method>

<!-- New: MC SID override control -->
<method id="ARMMCSetSID" name="SetSID" manual_name="Set MC SID Override">
    <brief>Program MC SID override register for a client</brief>
    <param dir="in" name="mc_client_id" type="seL4_Word">MC client ID</param>
    <param dir="in" name="stream_id" type="seL4_Word">Stream ID to assign</param>
</method>
```

#### 2.2 New Capability Types

**File**: `kernel/include/arch/arm/arch/64/mode/object/structures.bf` additions

```bitfield
-- Extended SID cap with instance information
block sid_cap {
    field capSID            16      -- Stream ID
    field capSMMUInstance   2       -- SMMU instance (0-2)
    field capMCClientID     8       -- MC client ID
    padding                 6
    field capType           5
}
```

#### 2.3 MC Client ID to SID Mapping Header

**New file**: `kernel/include/plat/orinagx/plat/machine/smmu.h`

```c
#pragma once

/* SMMU instance indices */
#define SMMU_INST_NISO1     0
#define SMMU_INST_NISO0     1
#define SMMU_INST_ISO       2

/* MC client IDs (from tegra234-mc.h) */
#define TEGRA234_MC_CLIENT_VICSRD       0x6c
#define TEGRA234_MC_CLIENT_VICSWR       0x6d
#define TEGRA234_MC_CLIENT_NVDECSRD     0x9c
#define TEGRA234_MC_CLIENT_NVDECSWR     0x9d
#define TEGRA234_MC_CLIENT_NVJPGSRD     0xa4
#define TEGRA234_MC_CLIENT_NVJPGSWR     0xa5
#define TEGRA234_MC_CLIENT_SDMMCRAB     0x45
#define TEGRA234_MC_CLIENT_SDMMCWAB     0x46
#define TEGRA234_MC_CLIENT_PCIE0R       0x70
#define TEGRA234_MC_CLIENT_PCIE0W       0x71
/* ... more clients */

/* Stream IDs (from tegra234-sid.h) */
#define TEGRA234_SID_VIC        0x03
#define TEGRA234_SID_NVDEC      0x04
#define TEGRA234_SID_GPU        0x05
#define TEGRA234_SID_NVJPG      0x06
#define TEGRA234_SID_SDMMC      0x02
#define TEGRA234_SID_PCIE0      0x12
/* ... more SIDs */

/* Max values */
#define SMMU_MAX_INSTANCES      3
#define SMMU_MAX_SID_PER_INST   128
#define SMMU_MAX_CB_PER_INST    64
#define MC_MAX_CLIENTS          128
```

---

### Phase 3: CAmkES Integration (Weeks 4-6)

**Goal**: Enable CAmkES VM to manage SMMU domains for guest VMs.

#### 3.1 Multi-Instance SMMU Template

**New file**: `projects/vm/templates/seL4SMMUV2Orin.template.c`

```c
/*
 * Copyright 2024, Unikie
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Orin AGX multi-instance SMMU support for CAmkES VM
 */

#include <sel4/sel4.h>
#include <camkes.h>

/*# Per-device SMMU configuration #*/
/*- for device in me.configuration.get('passthrough_devices', []) -*/

/*- set device_name = device.get('name') -*/
/*- set smmu_inst = device.get('smmu_instance', 0) -*/
/*- set stream_id = device.get('stream_id') -*/

/*- set sid_cap = alloc(name=device_name + '_sid', type=seL4_ARMSID) -*/
/*- set cb_cap = alloc(name=device_name + '_cb', type=seL4_ARMCB) -*/

struct smmu_device_config /*? device_name ?*/_smmu_config = {
    .name = "/*? device_name ?*/",
    .smmu_instance = /*? smmu_inst ?*/,
    .stream_id = /*? stream_id ?*/,
    .sid_cap = /*? sid_cap ?*/,
    .cb_cap = /*? cb_cap ?*/,
};

/*- endfor -*/

/* Get SMMU config for a device by name */
struct smmu_device_config *camkes_get_smmu_config(const char *name) {
    /*- for device in me.configuration.get('passthrough_devices', []) -*/
    if (strcmp(name, "/*? device.get('name') ?*/") == 0) {
        return &/*? device.get('name') ?*/_smmu_config;
    }
    /*- endfor -*/
    return NULL;
}
```

#### 3.2 VMM IOMMU Manager

**New file**: `projects/virtioso-camkes-vm/src/iommu_manager.c`

```c
/*
 * IOMMU Manager for CAmkES VM on Orin AGX
 *
 * Manages SMMU domains and MC SID overrides for VM device passthrough.
 */

#include <sel4vm/guest_vm.h>
#include <sel4vmmplatsupport/guest_memory.h>
#include "iommu_manager.h"

struct iommu_domain {
    uint32_t smmu_instance;
    uint32_t context_bank;
    seL4_CPtr sid_cap;
    seL4_CPtr cb_cap;
    vspace_root_t *vspace;
    bool active;
};

static struct iommu_domain domains[MAX_IOMMU_DOMAINS];

/**
 * Initialize IOMMU domain for a VM
 *
 * @param vm          Target VM
 * @param smmu_inst   SMMU instance (0-2)
 * @param stream_id   Stream ID for the device
 * @param mc_clients  Array of MC client IDs
 * @param num_clients Number of MC clients
 * @return 0 on success
 */
int iommu_domain_init(vm_t *vm, uint32_t smmu_inst, uint32_t stream_id,
                      const uint32_t *mc_clients, size_t num_clients) {
    struct iommu_domain *domain = &domains[vm->vm_id];

    // Get caps from CAmkES
    domain->sid_cap = camkes_get_smmu_sid_cap(vm->vm_id);
    domain->cb_cap = camkes_get_smmu_cb_cap(vm->vm_id);
    domain->smmu_instance = smmu_inst;

    // Assign VM's vspace to context bank
    seL4_Error err = seL4_ARM_CB_AssignVspace(domain->cb_cap, vm->vspace_root);
    if (err != seL4_NoError) {
        ZF_LOGE("Failed to assign vspace to CB: %d", err);
        return -1;
    }

    // Bind SID to CB
    err = seL4_ARM_SID_BindCB(domain->sid_cap, domain->cb_cap);
    if (err != seL4_NoError) {
        ZF_LOGE("Failed to bind SID to CB: %d", err);
        return -1;
    }

    // Program MC SID overrides for all clients
    for (size_t i = 0; i < num_clients; i++) {
        mc_set_sid_override(mc_clients[i], stream_id);
    }

    domain->active = true;
    return 0;
}

/**
 * Map a physical address range into IOMMU domain
 */
int iommu_map_range(vm_t *vm, paddr_t paddr, size_t size, int flags) {
    // The VM's vspace is used directly by SMMU (stage-2 translation)
    // Mapping happens through normal VM memory management
    return vm_map_guest_phys(vm, paddr, size, flags);
}
```

#### 3.3 CAmkES VM Configuration DSL Extension

**Example**: `apps/Arm/vm_pcie_passthrough/orinagx/devices.camkes`

```camkes
import <VM/vm.camkes>;

assembly {
    composition {
        component VM driver_vm;
    }

    configuration {
        driver_vm.linux_image = "linux-orin";
        driver_vm.linux_ram_base = 0x90000000;
        driver_vm.linux_ram_size = 0x10000000;  /* 256MB */

        /* PCIe NVMe passthrough with SMMU protection */
        driver_vm.passthrough_devices = [
            {
                "name": "nvme0",
                "smmu_instance": 0,  /* smmu_niso1 */
                "stream_id": 0x12,   /* TEGRA234_SID_PCIE0 */
                "mc_clients": [0x70, 0x71],  /* PCIE0R, PCIE0W */
                "paddr_ranges": [
                    {"base": 0x14100000, "size": 0x10000},  /* NVMe BAR */
                ],
                "irqs": [46],  /* PCIe MSI */
            }
        ];

        /* SMMU configuration */
        driver_vm.smmu_enabled = true;
    }
}
```

---

### Phase 4: Device-Specific SID Handling (Weeks 6-8)

**Goal**: Handle non-MC SID programming for complex devices.

#### 4.1 host1x Channel SID

host1x programs SID per-channel at offset 0x084. This bypasses MC.

**Challenge**: host1x has 64 channels, each with its own SID register.

**Option A: VMM MMIO Trap** (Recommended for security)

```c
/* In VMM fault handler */
int handle_host1x_mmio_fault(vm_t *vm, fault_t *fault) {
    uintptr_t offset = fault->addr - HOST1X_BASE;

    /* Channel SID register: base + channel*0x100 + 0x84 */
    if ((offset & 0xFF) == 0x84) {
        uint32_t channel = offset / 0x100;
        uint32_t requested_sid = fault->data;

        /* Validate SID matches VM's assigned SID */
        if (requested_sid != vm->assigned_host1x_sid) {
            ZF_LOGE("VM %d attempted invalid host1x SID %u",
                    vm->vm_id, requested_sid);
            return -1;
        }

        /* Allow the write */
        return vm_mmio_write(vm, fault->addr, fault->data);
    }

    return vm_mmio_forward(vm, fault);
}
```

**Option B: Kernel Syscall** (Simpler but less flexible)

```c
/* New syscall for host1x-specific SID */
seL4_Error seL4_ARM_Host1x_SetChannelSID(
    seL4_CPtr host1x_cap,
    seL4_Word channel_id,
    seL4_Word stream_id
);
```

#### 4.2 Device-Specific SID Register Summary

| Device | Register | MMIO Base | Offset | Priority |
|--------|----------|-----------|--------|----------|
| host1x | HOST1X_CHANNEL_SMMU_STREAMID | 0x13e00000 | 0x084/channel | HIGH (GPU path) |
| tegra-se | SE_STREAM_ID | 0x15810000 | varies | MEDIUM |
| PVA | cfg_priv_sid, cfg_vps_sid | 0x16000000 | 0x240000 | LOW |
| DCE | ast_ast0_streamid_ctl | varies | 0x020 | LOW |
| ADSP | AST_STREAMID_CTL | varies | 0x020 | LOW |
| MGBE | MGBE_WRAP_AXI_ASID*_CTRL | varies | 0x8400+ | LOW |

#### 4.3 Implementation Strategy

For initial release:
1. **Supported (MC path)**: VIC, NVDEC, NVENC, PCIe, SDMMC, DLA
2. **Partial support (with VMM trap)**: host1x (for basic GPU)
3. **Not supported initially**: PVA, DCE, ADSP, crypto

---

### Phase 5: Testing and Validation (Weeks 8-10)

#### 5.1 sel4test SMMU Tests

**File**: `projects/sel4test/apps/sel4test-driver/src/tests/smmu_orin.c`

```c
#include <sel4test/test.h>
#include <plat/machine/smmu.h>

/* Test multi-instance SMMU initialization */
static int test_orin_smmu_multi_instance(struct env *env) {
    /* Verify all three instances initialized */
    for (int i = 0; i < 3; i++) {
        seL4_CPtr sid_cap = get_smmu_sid_cap(env, i, 0);
        test_assert(sid_cap != seL4_CapNull);

        seL4_CPtr cb_cap = get_smmu_cb_cap(env, i, 0);
        test_assert(cb_cap != seL4_CapNull);
    }
    return sel4test_get_result();
}
DEFINE_TEST(SMMU_ORIN_0001, "Multi-instance SMMU init", test_orin_smmu_multi_instance, true);

/* Test MC SID override programming */
static int test_mc_sid_override(struct env *env) {
    /* Program VIC client SID */
    seL4_Error err = seL4_ARM_MC_SetSID(
        env->mc_cap,
        TEGRA234_MC_CLIENT_VICSRD,
        TEGRA234_SID_VIC
    );
    test_assert(err == seL4_NoError);

    /* Verify by reading back (if supported) */
    return sel4test_get_result();
}
DEFINE_TEST(SMMU_ORIN_0002, "MC SID override", test_mc_sid_override, true);

/* Test SMMU translation */
static int test_smmu_translation(struct env *env) {
    /* Create a vspace, assign to CB, bind SID, perform DMA test */
    vspace_t *vs = create_test_vspace(env);

    seL4_Error err = seL4_ARM_CB_AssignVspace(env->cb_cap, vs->root);
    test_assert(err == seL4_NoError);

    err = seL4_ARM_SID_BindCB(env->sid_cap, env->cb_cap);
    test_assert(err == seL4_NoError);

    /* Map test buffer */
    void *buf = vspace_alloc(vs, PAGE_SIZE);
    test_assert(buf != NULL);

    /* Would need actual hardware DMA to fully test */
    return sel4test_get_result();
}
DEFINE_TEST(SMMU_ORIN_0003, "SMMU translation", test_smmu_translation, true);
```

#### 5.2 Integration Test: PCIe NVMe DMA

```c
/* Test NVMe DMA through SMMU */
static int test_nvme_dma_isolation(struct env *env) {
    /* 1. Configure CB with test vspace */
    /* 2. Program PCIE0 SID via MC */
    /* 3. Initialize NVMe controller */
    /* 4. Submit read command */
    /* 5. Verify data arrived in mapped buffer */
    /* 6. Verify unmapped addresses fault */
    return sel4test_get_result();
}
```

#### 5.3 Negative Tests

```c
/* Test that unmapped DMA causes fault */
static int test_smmu_fault_handling(struct env *env) {
    /* Configure SMMU with restricted mapping */
    /* Attempt DMA to unmapped address */
    /* Verify SMMU fault is reported */

    uint32_t fault_status, fault_addr;
    seL4_ARM_CB_GetFault(env->cb_cap, &fault_status, &fault_addr);
    test_assert(fault_status != 0);

    return sel4test_get_result();
}
```

---

## Key Technical Challenges

### 1. MC-SMMU Coupling Complexity

Unlike standard ARM SMMU where stream matching is done in SMMU itself, Tegra234 requires MC to program SID overrides. This means:
- seL4 needs MC driver awareness
- SID assignment is a two-step process (SMMU CB setup + MC override)
- System resume requires MC re-programming

**Mitigation**: Create unified `iommu_domain_init()` that handles both SMMU and MC.

### 2. Multiple SMMU Instances

Current seL4 SMMUv2 driver is single-instance. Extending to multi-instance requires:
- Per-instance state management
- Instance selection in all SMMU operations
- Careful handling of context bank allocation across instances
- Global vs per-instance TLB invalidation

**Mitigation**: Refactor driver to use instance array from the start.

### 3. Dual Register Base Synchronization

NVIDIA's implementation requires programming identical values to two MMIO bases. Missing this causes undefined SMMU behavior.

**Mitigation**: Create `smmu_write_reg32_dual()` wrapper, use consistently.

### 4. Device-Specific SID Paths

host1x, crypto, PVA, etc. bypass MC. Supporting these for passthrough requires:
- MMIO trap and emulate in VMM (most secure)
- Kernel extensions per device type (simpler)
- Restricting passthrough to MC-path devices only (easiest)

**Mitigation**: Start with MC-path devices, add host1x trap for GPU.

### 5. Firmware Handoff

Some clients (display) start in firmware-configured passthrough mode. seL4 must either:
- Preserve firmware SID configuration
- Carefully coordinate handoff (similar to Linux's probe_finalize)

**Mitigation**: For initial release, disable display passthrough.

### 6. Context Bank Allocation

With 3 SMMU instances and potentially many VMs, CB allocation strategy matters:
- Each instance has ~64 context banks
- Each device needs a CB bound to a SID
- Multiple devices can share a CB if they share a vspace

**Mitigation**: Use simple per-VM CB allocation, optimize later.

---

## Resource Estimates

| Phase | Description | Effort | Dependencies |
|-------|-------------|--------|--------------|
| 1 | Kernel SMMU Driver Extension | 3 weeks | Hardware docs, Tegra234 TRM |
| 2 | Kernel API Extensions | 1 week | Phase 1 |
| 3 | CAmkES Integration | 2 weeks | Phase 2 |
| 4 | Device-Specific SID Handling | 2 weeks | Phase 3 |
| 5 | Testing and Validation | 2 weeks | All phases |

**Total: ~10 weeks** for full SMMU support with device passthrough capability.

---

## Recommended Initial Scope (MVP)

For Minimum Viable Product (~4 weeks):

### Included in MVP

1. **Single SMMU instance** (smmu_niso1 @ 0x08000000)
2. **MC-path devices only**:
   - PCIe controllers (NVMe, network cards)
   - SDMMC (storage)
   - VIC, NVDEC, NVENC (video engines)
3. **Basic kernel API** (instance-aware SID/CB)
4. **Simple CAmkES integration** (manual configuration)
5. **sel4test validation**

### Excluded from MVP

1. Multi-instance (smmu_niso0, smmu_iso)
2. GPU passthrough (requires host1x SID handling)
3. Display passthrough (firmware handoff complexity)
4. Complex devices (PVA, DCE, ADSP)
5. Automatic MC client discovery

### MVP Deliverables

1. `kernel/src/plat/orinagx/machine/smmu.c` - Orin SMMU init
2. `kernel/src/plat/orinagx/machine/mc.c` - MC SID override
3. Modified `kernel/src/drivers/smmu/smmuv2.c` - Instance support
4. `kernel/include/plat/orinagx/plat/machine/smmu.h` - Constants
5. Basic sel4test tests
6. Documentation

---

## References

### Internal Documentation

- `/home/hlyytine/pkvm/jetson-pkvm/docs/pkvm_pkvm_smmu_notes.md` - pKVM architecture
- `/home/hlyytine/pkvm/jetson-pkvm/docs/sid_programming_mechanisms.md` - SID paths
- `/home/hlyytine/pkvm/jetson-pkvm/docs/why-mc-coupled-with-smmu.md` - MC coupling
- `/home/hlyytine/pkvm/jetson-pkvm/docs/smmuv2_pkvm_architecture.md` - pKVM design

### seL4 Source Files

- `kernel/src/drivers/smmu/smmuv2.c` - Existing SMMUv2 driver
- `kernel/include/drivers/smmu/smmuv2.h` - SMMU header
- `kernel/src/arch/arm/object/smmu.c` - SMMU kernel objects
- `kernel/src/plat/tx2/` - TX2 platform (reference)
- `projects/vm/templates/seL4SMMUV2.template.c` - CAmkES template

### Linux Source Files (Reference)

- `drivers/iommu/arm/arm-smmu/arm-smmu-nvidia.c` - NVIDIA SMMU impl
- `drivers/memory/tegra/tegra186.c` - MC driver
- `drivers/memory/tegra/tegra234.c` - MC client table
- `include/dt-bindings/memory/tegra234-mc.h` - MC client IDs
- `include/dt-bindings/memory/tegra234-smmu-streamid.h` - Stream IDs

### Hardware Documentation

- NVIDIA Tegra234 Technical Reference Manual (TRM)
- ARM MMU-500 Technical Reference Manual
- ARM System MMU Architecture Specification (SMMUv2)
