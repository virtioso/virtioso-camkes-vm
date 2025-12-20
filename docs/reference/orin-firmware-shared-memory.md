# Firmware Shared Memory Communication on Orin AGX

This document describes how UEFI, ATF, and OP-TEE communicate with Linux (EL0-EL2) via shared memory buffers in non-secure DRAM.

## Summary

| Component | Uses Shared Memory? | Location | How Defined/Negotiated |
|-----------|---------------------|----------|------------------------|
| **UEFI** | YES | NS DRAM (runtime) | PCDs or SMC query; communicated via EFI memory map |
| **ATF** | YES | SYSRAM/NS DRAM | MC carveout registers (T234) or static (T194) |
| **OP-TEE** | YES | NS DRAM | Dynamic registration via SMC from Linux |

---

## 1. UEFI Shared Memory

### Mechanism

UEFI uses **MM Communication** buffers to delegate runtime services (variable storage, RAS error logging) to secure firmware (Standalone MM or OP-TEE).

### Two Implementations

1. **FFA-based** (`MmCommunicateFfaDxe`): Queries secure world via STMM SMC calls
   - `STMM_GET_NS_BUFFER` (0xC0270001) - retrieves NS buffer address
   - `STMM_GET_ERST_UNCACHED_BUFFER` (0xC0270002) - ERST error log buffer

2. **OP-TEE-based** (`MmCommunicationOpteeDxe`): Allocates contiguous buffer, registers with OP-TEE
   - Uses `OpteeRegisterShm()` to share pages with secure world

### Address Definition

- **PCDs**: `PcdMmBufferBase`, `PcdMmBufferSize` (in ArmPkg.dec)
- **NVIDIA-specific**: `PcdErstBufferBase`, `PcdErstBufferSize` for RAS

### Communication to Linux

- EFI memory map with `EFI_MEMORY_RUNTIME` attribute
- Configuration tables (`EFI_RT_PROPERTIES_TABLE`)
- Linux parses via `GetMemoryMap()` before `ExitBootServices()`

### Key Files

```
~/nvidia-uefi-docker/nvidia-uefi/edk2-nvidia/Silicon/NVIDIA/Drivers/MmCommunicateFfaDxe/MmCommunication.c
~/nvidia-uefi-docker/nvidia-uefi/edk2-nvidia/Silicon/NVIDIA/Drivers/MmCommunicationOpteeDxe/MmCommunication.c
~/nvidia-uefi-docker/nvidia-uefi/edk2/ArmPkg/ArmPkg.dec (PCD definitions)
~/nvidia-uefi-docker/nvidia-uefi/edk2-nvidia/Silicon/NVIDIA/NVIDIA.dec
```

---

## 2. ATF (ARM Trusted Firmware) Shared Memory

### Mechanism

ATF uses **BPMP IPC** (Inter-Processor Communication) to communicate with the Boot and Power Management Processor. This uses shared memory in SYSRAM with HSP (Hardware Synchronization Primitives) doorbells for signaling.

### Memory Regions

| Platform | TX Buffer | RX Buffer | Size |
|----------|-----------|-----------|------|
| T194 | 0x4004C000 | 0x4004D000 | 4KB each |
| T234 (Orin) | Dynamic (MC reg) | Dynamic (MC reg) | 4KB each |

### Address Definition (T234/Orin)

Read from **Memory Controller carveout registers**:
- `MC_CCPLEX_BPMP_IPC_BASE_LO` (0x23c4)
- `MC_CCPLEX_BPMP_IPC_BASE_HI` (0x23c8)

Programmed by bootloader (BL2) before ATF runs. Validated to be within SYSRAM range (0x40000000-0x50000000).

### IVC Protocol

- 128-byte frames with MRQ (message request) header
- 64-byte aligned channel headers for cache coherency
- Doorbell signaling via HSP registers (0x03C90000)

### Communication to Linux

- **NOT via device tree** - Linux BPMP driver reads same MC registers
- `drivers/firmware/tegra/bpmp.c` mirrors ATF's discovery mechanism

### Key Files

```
~/pkvm/Linux_for_Tegra/source/tegra/optee-src/atf/arm-trusted-firmware/plat/nvidia/tegra/soc/t234/plat_setup.c (lines 544-581)
~/pkvm/Linux_for_Tegra/source/tegra/optee-src/atf/arm-trusted-firmware/plat/nvidia/tegra/drivers/bpmp_ipc/
~/pkvm/Linux_for_Tegra/source/tegra/optee-src/atf/arm-trusted-firmware/plat/nvidia/tegra/include/t234/tegra_def.h
```

---

## 3. OP-TEE Shared Memory

### Mechanism

OP-TEE uses **dynamic shared memory registration** - normal world (Linux) registers memory pages with secure world via SMC, and OP-TEE creates memory objects (MOBJs) to access them.

### Registration Flow

1. Linux app calls `TEEC_RegisterSharedMemory()`
2. Kernel sends `TEE_IOC_SHM_REGISTER` ioctl with physical pages
3. OP-TEE receives `OPTEE_MSG_CMD_REGISTER_SHM`
4. Creates `mobj_reg_shm` with page list, returns cookie
5. Subsequent calls reference memory via cookie

### Address Definition

- **No pre-defined region** - any NS DRAM can be registered
- Physical pages validated against `CORE_MEM_NON_SEC` regions
- Page list format supports non-contiguous memory (511 pages per 4KB descriptor)

### Tegra Configuration

```makefile
CFG_TZDRAM_START = 0x80000000      # TEE RAM (secure)
CFG_CORE_DYN_SHM = y               # Dynamic SHM enabled
CFG_CORE_RESERVED_SHM = n          # No static SHM pool
CFG_CORE_ARM64_PA_BITS = 40        # 40-bit PA (supports >4GB)
```

### Communication to Linux

- Device tree node for optee (standard bindings)
- TEE client API (`libteec`) for registration
- Kernel TEE subsystem manages mappings

### Key Files

```
~/pkvm/Linux_for_Tegra/source/tegra/optee-src/optee/optee_os/core/arch/arm/mm/mobj_dyn_shm.c
~/pkvm/Linux_for_Tegra/source/tegra/optee-src/optee/optee_os/core/tee/entry_std.c
~/pkvm/Linux_for_Tegra/source/tegra/optee-src/optee/optee_os/core/arch/arm/plat-tegra/conf.mk
~/pkvm/Linux_for_Tegra/source/tegra/optee-src/optee/optee_os/core/include/optee_msg.h
```

---

## Relevance to seL4

### UEFI and BPMP IPC: Safe

1. **UEFI runtime regions**: Located at ~32GB (well above seL4's 2-2.75GB)
2. **BPMP IPC**: Uses SYSRAM (0x40000000 range) or MC-carved regions, not main DRAM at 0x80000000

### OP-TEE: NO CONFLICT WITH seL4

#### Actual TZDRAM Location (Verified)

Despite OP-TEE source showing `CFG_TZDRAM_START = 0x80000000`, the **actual TZDRAM is at 0x50000000**, not 0x80000000.

**ATF platform configuration for T234 (Orin AGX):**

From `plat/nvidia/tegra/soc/t234/platform_t234.mk`:
```makefile
PLAT_BL31_BASE := 0x50000000
```

From `plat/nvidia/tegra/include/platform_def.h`:
```c
#define TZDRAM_SIZE  0x00400000  // 4MB only!
#define TZDRAM_END   (PLAT_BL31_BASE + TZDRAM_SIZE)
```

**Actual TZDRAM region: 0x50000000 - 0x50400000 (4MB)**

This is in the SYSRAM/secure region below 0x80000000, NOT in main DRAM.

#### Linux Memory Map Confirms This

From `/proc/iomem` on Orin AGX:
```
80000000-fffdffff : System RAM      # <-- Linux sees this as available!
100000000-818a14fff : System RAM
```

Linux sees System RAM starting at 0x80000000, confirming TZDRAM does NOT carve out this region.

#### Does OP-TEE Access NS DRAM Without Registration?

**NO.** OP-TEE only accesses NS DRAM when Linux explicitly registers shared memory:
- `CFG_CORE_DYN_SHM = y` - Dynamic shared memory only
- `CFG_CORE_RESERVED_SHM = n` - No static SHM pool

OP-TEE does NOT automatically access any NS DRAM at 0x80000000 or elsewhere.

#### Memory Layout Summary

| Region | Address Range | Size | Owner |
|--------|--------------|------|-------|
| MMIO / Peripherals | 0x00000000 - 0x4FFFFFFF | ~1.25GB | Hardware |
| **TZDRAM (ATF+OP-TEE)** | **0x50000000 - 0x50400000** | **4MB** | **Secure World** |
| System RAM (NS) | 0x80000000 - 0xFFFFFFFF | ~2GB | Normal World |
| System RAM (NS) | 0x100000000 - ~0x900000000 | ~30GB+ | Normal World |

**Conclusion:** seL4's load address at 0x80000000 does NOT conflict with OP-TEE's TZDRAM at 0x50000000.

### ATF SMC Handling: NO NS DRAM ACCESS

**ATF runs at EL3** when handling SMC calls. Investigation of T234 ATF code shows it does NOT access NS DRAM at 0x80000000+ during normal operation.

#### PSCI Handlers (plat_psci_handlers.c)

PSCI operations (`tegra_soc_cpu_standby`, `tegra_soc_pwr_domain_suspend`, etc.) only:
- Interact with MCE (Microcontroller Engine) via ARI
- Access percpu data in **TZDRAM** (secure memory at 0x50000000)
- Cache flush operations are on TZDRAM only:
  ```c
  clean_dcache_range((uint64_t)&t234_percpu_data[cpu], sizeof(...));
  ```
  This is secure memory, NOT NS DRAM.

#### Tegra SiP SMC Handlers

| SMC | Function | NS DRAM Access? |
|-----|----------|-----------------|
| `TEGRA_SIP_NEW_VIDEOMEM_REGION` (0x82000003) | Programs MC registers | No (only stores addresses in MC regs) |
| `TEGRA_SIP_FIQ_NS_ENTRYPOINT` (0x82000005) | Stores FIQ handler pointer | No |
| `TEGRA_SIP_FIQ_NS_GET_CONTEXT` (0x82000006) | Returns context in registers | No |
| `TEGRA_SIP_WRITE_PFG_REGS` (T234 only) | RAS fault injection (debug) | No |

#### RAS Error Handling

RAS handlers read error status from GIC600AE registers (MMIO), not DRAM. Error information is returned in GP registers, not written to NS memory.

#### What SMCs Does seL4 Make?

seL4 kernel at EL2:
- Does NOT directly make PSCI calls for CPU power management
- Provides SMC invocation mechanism for user applications
- May forward PSCI from EL1 guests to EL3

**Conclusion:** ATF does not access NS DRAM during normal seL4 operation. Cache operations during SMC handling are on TZDRAM (0x50000000) only.

## See Also

- [seL4 Memory Layout on Orin AGX](sel4-memory-layout-orinagx.md)
- [Tegra Cache Operations](tegra-cache-operations.md)
- [Orin RAS Error Investigation](orin-ras-error-investigation.md)
