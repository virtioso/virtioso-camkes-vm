# MGBE DMA Investigation - seL4 VM Passthrough

## Problem Statement
MGBE Ethernet passthrough to seL4 VM: Link comes up at 1Gbps, but no network traffic works.
DHCP discovery packets are sent by guest but no response received.

## Key Observation
- Physical IRQ 416 (common) fires twice (link up events)
- Physical IRQs 417-421 (DMA channel vm0-vm4) NEVER fire
- Native Linux shows thousands of interrupts on 417-421

## Native Linux Interrupt Counts (Working)
```
IRQ 416 (common_irq): 2
IRQ 417 (vm0): 1291
IRQ 418 (vm1): 290
IRQ 419 (vm2): 283
IRQ 420 (vm3): 417
IRQ 421 (vm4): 219
```

## VM Interrupt Counts (Not Working)
```
IRQ 416: 2 (only from link state changes)
IRQ 417-421: 0 (DMA never completes)
```

## Hardware Configuration

### MGBE Register Bases
- HV (Hypervisor/Wrapper) base: 0x6800000
- MAC base: 0x6810000
- XPCS base: 0x68A0000
- MACsec base: 0x68D0000

### MGBE Wrapper Registers (at 0x6800000 + offset)
- ASID0_CTRL: 0x8400 - AXI Stream ID config
- ASID1_CTRL: 0x8404 - AXI Stream ID config
- ASID2_CTRL: 0x8408 - AXI Stream ID config
- VIRT_INTR_APB_CHX_CNTRL: 0x8200 + (chan * 4) - Per-channel interrupt control
- VIRTUAL_APB_ERR_CTRL: 0x8300 - Virtual error control
- COMMON_INTR_ENABLE: 0x8704 - Common interrupt enable
- COMMON_INTR_STATUS: 0x8708 - Common interrupt status

### SMMU Configuration
- MGBE connected to SMMU at 0x12000000 (smmu_niso1)
- Stream ID: 0x6 (MGBE0_SID)
- IOMMU group: 26

### SID Values (from mgbe_core.h)
```c
MGBE0_SID = 0x6      // Tegra234
MGBE1_SID = 0x49
MGBE2_SID = 0x4A
MGBE3_SID = 0x4B
MGBE0_SID_T264 = 0x0 // Tegra264 - all zeros!
```

## Driver Initialization Flow

### Probe Function Call Order (CRITICAL)
The driver probe function order is:
1. `ether_parse_dt()` - Called FIRST, includes IVC check (hv_base is NULL here!)
2. `ether_init_plat_resources()` - Maps hv_base AFTER parse_dt
3. `ether_open()` - Called when interface goes UP, ASID writes happen here

### Debug Output from VM (2025-01-10)
```
nvethernet 6800000.ethernet: DEBUG: IVC init failed, use_virtualization=0 (DISABLE=0)
nvethernet 6800000.ethernet: DEBUG: hv_base=0000000000000000, will write ASID regs if hv_base!=NULL
nvethernet 6800000.ethernet: DEBUG: tegra_hypervisor_mode=0
nvethernet 6800000.ethernet: DEBUG: hypervisor resource found at 0x6800000
nvethernet 6800000.ethernet: DEBUG: hv_base mapped to ffff80000ba40000
```

This shows:
- hv_base is NULL during IVC check (expected - called before mapping)
- hv_base later gets mapped successfully to 0xffff80000ba40000
- use_virtualization = 0 = OSI_DISABLE (correct)
- tegra_hypervisor_mode = 0 (correct - not NVIDIA hypervisor)

### is_tegra_hypervisor_mode()
- Checks for `nvidia,tegra-hypervisor-mode` property in /chosen
- Returns false in our VM (property not set)
- When false, driver maps "hypervisor" resource (0x6800000)

### use_virtualization Setting
```c
if (!ether_init_ivc(pdata)) {
    osi_core->use_virtualization = OSI_ENABLE;
} else {
    osi_core->use_virtualization = OSI_DISABLE;
}
```
In our VM: IVC init fails, so use_virtualization = OSI_DISABLE

### ASID Register Configuration
This happens in `mgbe_dma_chan_to_vmirq_map()` called during `ether_open()`:
```c
if ((osi_core->use_virtualization == OSI_DISABLE) &&
    (osi_core->hv_base != OSI_NULL)) {
    // Write SID to ASID registers
    // MGBE_SID_VAL1(0x6) = 0x06060606
    // MGBE_SID_VAL2(0x6) = 0x0606
    osi_writela(MGBE_SID_VAL1(sid[instance_id]), hv_base + 0x8400); // ASID0
    osi_writela(MGBE_SID_VAL1(sid[instance_id]), hv_base + 0x8404); // ASID1
    osi_writela(MGBE_SID_VAL2(sid[instance_id]), hv_base + 0x8408); // ASID2
}
```

**Key question**: By the time `ether_open()` is called, hv_base SHOULD be valid.
Need to verify with debug logging in `mgbe_dma_chan_to_vmirq_map()`.

Both conditions should be true in our VM when ether_open() runs:
- use_virtualization = OSI_DISABLE (IVC init fails)
- hv_base = mapped (is_tegra_hypervisor_mode() returns false)

## SMMU Shutdown Behavior

When native Linux shuts down:
```c
arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sCR0, ARM_SMMU_sCR0_CLIENTPD);
```
Sets CLIENTPD (Client Port Disable) which should bypass all SMMU translation.

However: UEFI runs between Linux shutdown and seL4 boot - may touch SMMU state.

## Hypotheses

### H1: SMMU Not Actually Bypassed
Even though CLIENTPD is set, individual stream contexts may not be in bypass.
MGBE DMA transactions might be blocked by stale SMMU configuration.

### H2: SID Configuration Wrong for Bypass Mode
The driver writes SID=0x6 to ASID registers. But with SMMU bypassed,
maybe SID should be 0 (like Tegra264 values)?

### H3: vm-irq-config Requires Active Virtualization
The vm-irq-config maps 10 DMA channels to 5 IRQs (vm0-vm4).
This requires hardware register writes to route interrupts.
Maybe these writes only work when SMMU is active?

### H4: DMA Addresses Wrong
Guest allocates DMA buffers at 0x90000000-0xafffffff (VM RAM).
With SMMU bypassed, these should map 1:1 to physical addresses.
But if MGBE expects translated addresses, DMA would fail.

## What Native Linux Does Differently
1. SMMU is active, translating DMA addresses
2. Full SMMU stream context configured for MGBE
3. DMA buffers are allocated and translated through SMMU
4. Works correctly

## Test Results (2025-01-10)

### Driver Debug Output Confirms ASID Write Conditions Met
```
nvethernet 6800000.ethernet: DEBUG: ether_open() about to call osi_hw_core_init
nvethernet 6800000.ethernet: DEBUG: use_virtualization=0, hv_base=ffff80000ba40000, instance_id=0
nvethernet 6800000.ethernet: DEBUG: osi_hw_core_init returned 0 (ASID writes should have happened)
```

All conditions for ASID writes are TRUE:
- `use_virtualization=0` (OSI_DISABLE) ✓
- `hv_base=ffff80000ba40000` (NOT NULL) ✓
- `instance_id=0` ✓
- `osi_hw_core_init returned 0` (SUCCESS) ✓

**Conclusion**: The code path that writes ASID registers IS being executed.

### But DMA Interrupts Still Don't Fire
```
MGBE/Ethernet interrupts (IRQs 416-423, eth0):
 77:          0     GICv3 416 Level     eth0.common_irq
 78:          0     GICv3 417 Level     eth0.vm0
 79:          0     GICv3 418 Level     eth0.vm1
 80:          0     GICv3 419 Level     eth0.vm2
 81:          0     GICv3 420 Level     eth0.vm3
 82:          0     GICv3 421 Level     eth0.vm4
```

IRQ 416 (common) fires for link state changes, but 417-421 (DMA) remain at 0.

### devmem2 Register Reads (2025-01-10)
Added devmem2 to initramfs and verified ASID registers:

```
Using devmem2 to read registers
Reading ASID registers (MGBE wrapper at 0x6800000):
  ASID0_CTRL (0x6808400): 0x06060606 (expect 0x06060606) ✓
  ASID1_CTRL (0x6808404): 0x06060606 (expect 0x06060606) ✓
  ASID2_CTRL (0x6808408): 0x00000606 (expect 0x00000606) ✓
```

**ASID registers ARE being written correctly!** The SID=0x6 is in place.

### But Virtual Interrupt Registers Are All Zero!
```
  COMMON_INTR_ENABLE (0x6808704): 0x00000000
  COMMON_INTR_STATUS (0x6808708): 0x00000000
  VIRT_INTR_APB_CH0_CNTRL (0x6808200): 0x00000000
  VIRT_INTR_APB_CH1_CNTRL (0x6808204): 0x00000000
  ... all channels 0-9 are 0x00000000 ...
```

These registers should be written by `mgbe_dma_chan_to_vmirq_map()` during ether_open().
The code iterates `for (i = 0; i < num_vm_irqs; i++)` and writes `OSI_BIT(vm_num)`.

**If all registers are 0, either:**
1. `num_vm_irqs = 0` - loop didn't run
2. `ether_get_vm_irq_data()` wasn't called or returned error
3. The writes happened but got reset

### DMA Status Shows Stalled State
```
  DMA_CH0_STATUS (0x6813160): 0x00000084
  DMA_CH1_STATUS (0x68131e0): 0x00000084
  DMA_CH2_STATUS (0x6813260): 0x00000084
```

0x84 = bit 7 (RBU - RX Buffer Unavailable) + bit 2 (TI - TX Interrupt)
This indicates DMA is stalled waiting for RX buffers.

## Current Investigation (2026-01-11)

### ~~H5: num_vm_irqs Is Zero~~ DISPROVEN

Debug output shows `num_vm_irqs = 5` is correctly populated:

```
nvethernet 6800000.ethernet: DEBUG: num_vm_irqs=5
nvethernet 6800000.ethernet: DEBUG: irq_data[0]: vm_num=0, num_vm_chans=2
nvethernet 6800000.ethernet: DEBUG: irq_data[1]: vm_num=1, num_vm_chans=2
nvethernet 6800000.ethernet: DEBUG: irq_data[2]: vm_num=2, num_vm_chans=2
nvethernet 6800000.ethernet: DEBUG: irq_data[3]: vm_num=3, num_vm_chans=2
nvethernet 6800000.ethernet: DEBUG: irq_data[4]: vm_num=4, num_vm_chans=2
```

This means:
- `ether_get_vm_irq_data()` succeeded
- `irq_data[]` is properly populated
- 10 DMA channels are mapped to 5 virtual IRQs (vm0-vm4)

### H6: Driver Writes to Wrong Register Address (NEW HYPOTHESIS)

**Key discovery**: The driver uses different base addresses for different registers:

| Register Type | Base Used | Formula | Address |
|---------------|-----------|---------|---------|
| ASID registers | `hv_base` (HV wrapper) | 0x6800000 + 0x8400 | 0x6808400 ✓ |
| VIRT_INTR_APB_CHx | `osi_core->base` (MAC) | 0x6810000 + 0x8200 | **0x6818200** |

But looking at the MGBE hardware documentation, the VIRT_INTR_APB_CHx_CNTRL registers
are described as part of the "wrapper" at offset 0x8200 from HV base (0x6800000).

**The driver code in `mgbe_dma_chan_to_vmirq_map()`:**
```c
osi_writel(OSI_BIT(irq_data->vm_num),
           (nveu8_t *)osi_core->base +           // Uses MAC base!
           MGBE_VIRT_INTR_APB_CHX_CNTRL(chan));  // Offset 0x8200
```

This writes to 0x6818200, but the registers may actually be at 0x6808200!

**Test needed**: Read from BOTH addresses to verify:
- 0x6808200 (HV base + 0x8200) - where registers should be
- 0x6818200 (MAC base + 0x8200) - where driver writes

### Init script updated to check both

Updated init script reads from both locations:
```
Reading virtual interrupt control registers (HV base + 0x8200):
  VIRT_INTR_APB_CH0_CNTRL (HV:0x6808200): ???
  ...
Reading virtual interrupt control registers (MAC base + 0x8200):
  VIRT_INTR_APB_CH0_CNTRL (MAC:0x6818200): ???
  ...
```

## Summary of Findings

| Item | Status | Notes |
|------|--------|-------|
| ASID registers | ✓ Correct | 0x06060606, 0x06060606, 0x00000606 |
| hv_base mapping | ✓ Correct | Mapped to ffff80000ba30000 |
| use_virtualization | ✓ Correct | = 0 (OSI_DISABLE) |
| num_vm_irqs | ✓ Correct | = 5 |
| irq_data[] | ✓ Correct | 5 entries, 2 chans each |
| VIRT_INTR_APB_CHx | ✓ Correctly set at MAC base | 0x01,0x01,0x02,0x02,0x04,0x04,0x08,0x08,0x10,0x10 |
| DMA status | 0x84 | TI + RBU set (TX/RX activity) |
| DMA interrupts | ✗ Never fire | Despite status flags being set |

## Test Results (2026-01-11 Update)

### VIRT_INTR Registers Are Correctly Configured

Verified registers at MAC base + 0x8200 (correct location):
```
CH0: 0x00000001 (vm0)  CH1: 0x00000001 (vm0)
CH2: 0x00000002 (vm1)  CH3: 0x00000002 (vm1)
CH4: 0x00000004 (vm2)  CH5: 0x00000004 (vm2)
CH6: 0x00000008 (vm3)  CH7: 0x00000008 (vm3)
CH8: 0x00000010 (vm4)  CH9: 0x00000010 (vm4)
```

### DMA Status Shows Activity But No Interrupts

```
DMA_CH0_STATUS: 0x00000084 (TI=TX Interrupt + RBU=RX Buffer Unavailable)
DMA_CH1_STATUS: 0x00000084
DMA_CH2_STATUS: 0x00000084
```

Despite DMA status bits being set, no IRQs 417-421 fire at physical GIC.

### Physical IRQ Reception

Only these IRQs are received by VMM:
- IRQ 328: GPIO (1 time)
- IRQ 416: MGBE common/link state (2 times)
- IRQs 417-421: **NEVER received**

### Spurious Interrupt Investigation

seL4 kernel reports spurious interrupts:
```
getActiveIRQ: IAR1=0x3ff HPPIR1=0x3ff (spurious)
```
IAR1=0x3ff (1023) means no pending interrupt when read. These appear after DHCP
timeouts but may be MCS preemption points rather than MGBE-related.

## NVIDIA UEFI Analysis (2026-01-11)

Explored UEFI sources at ~/nvidia-uefi-docker/nvidia-uefi to understand MGBE setup:

### Key UEFI Behaviors

| Component | UEFI Action | Impact on seL4 |
|-----------|-------------|----------------|
| Clocks | Enables via ATF (AutoEnableClocks=TRUE) | Preserved for seL4 |
| Reset | Resets module via ATF | Module ready to use |
| MMIO | Maps mac (0x6800000) and xpcs bases | Must map same regions |
| DMA | Allocates descriptors at physical addresses | Guest driver reinitializes |
| SMMU | **NOT configured** in driver | Must configure if needed |
| PHY | Resets GPIO, negotiates link | Left operational at exit |
| **Shutdown** | Calls SnpShutdown() on ExitBootServices | Interface stopped |

### Critical Finding: Interface Shutdown

UEFI shuts down MGBE on ExitBootServices(). The Linux guest driver must fully
reinitialize the interface. This is happening (link comes up), but DMA
interrupts still don't work.

### UEFI Interrupt Register Offset

UEFI code uses different offset for virtual interrupt control:
```c
#define MGBE_VIRT_INTR_CHX_CNTRL(x)  (0x8600U + ((x) * 8U))  // Per-channel, 8-byte stride
```

Linux driver uses:
```c
#define MGBE_VIRT_INTR_APB_CHX_CNTRL(x)  (0x8200U + ((x) * 4U))  // Per-channel, 4-byte stride
```

**These might be different register sets!** The 0x8600 registers may be additional
virtual interrupt registers that also need configuration.

## Hypothesis (H7): Missing 0x8600 Register Configuration - TESTED

The UEFI code configures VIRT_INTR at 0x8600 offset, but Linux driver only configures
at 0x8200 offset. There may be TWO sets of virtual interrupt control registers that
both need to be configured for interrupts to work.

**Result (2026-01-11)**: The 0x8600 registers ARE configured (CNTRL=0x03 for all channels),
but this doesn't appear to be the issue since STATUS=0x00 for all channels.

## Complete Register State (2026-01-11)

### DMA Channel Registers (all show same pattern)
```
CH0-CH4: STATUS=0x00000084 IER=0x00000041
```
- STATUS 0x84 = TI (TX Interrupt) + RBU (RX Buffer Unavailable) - **Interrupts pending!**
- IER 0x41 = TIE (TX Interrupt Enable) + FBE (Fatal Bus Error Enable) - **Enabled!**

### MAC Level
```
MAC_ISR (0x68100b0): 0x00000000
MAC_IER (0x68100b4): 0x0000B001  (enabled)
```

### Wrapper Level (HV Base 0x6800000)
```
COMMON_INTR_ENABLE (0x6808704): 0x00000000  ← ⚠️ DISABLED!
COMMON_INTR_STATUS (0x6808708): 0x00000000
```

### VIRT_INTR Registers
```
APB (MAC+0x8200): 0x01,0x01,0x02,0x02,0x04,0x04,0x08,0x08,0x10,0x10 ← Correct!
UEFI (MAC+0x8600): CNTRL=0x03, STATUS=0x00 for all channels
```

## Three VIRT_INTR Register Sets (2026-01-11)

MGBE has THREE different virtual interrupt register sets, all at MAC base + offset:

### 1. VIRT_INTR_APB_CHX_CNTRL (0x8200, 4-byte stride)
Maps DMA channels to virtual IRQ lines. Bits 0-4 select vm0-vm4 IRQ.
```
Offset: 0x8200 + (chan * 4)
CH0: 0x01 (vm0), CH1: 0x01 (vm0), CH2: 0x02 (vm1), CH3: 0x02 (vm1)
CH4: 0x04 (vm2), CH5: 0x04 (vm2), CH6: 0x08 (vm3), CH7: 0x08 (vm3)
CH8: 0x10 (vm4), CH9: 0x10 (vm4)
```
**Status**: ✓ Correctly configured in VM

### 2. VIRT_INTR_CHX_CNTRL (0x8600, 8-byte stride)
Per-channel TX/RX interrupt enable. **CRITICAL for interrupt generation!**
```
Offset: 0x8600 + (chan * 8)
Bit 0: VIRT_INTR_CHX_CNTRL_TX - Enable TX interrupt
Bit 1: VIRT_INTR_CHX_CNTRL_RX - Enable RX interrupt
```
**Status**: ✓ All channels show 0x03 (TX+RX enabled)

### 3. VIRT_INTR_CHX_STATUS (0x8604, 8-byte stride)
Per-channel interrupt status. Written to clear pending interrupts.
```
Offset: 0x8604 + (chan * 8)
Bit 0: VIRT_INTR_CHX_STATUS_TX - TX interrupt pending
Bit 1: VIRT_INTR_CHX_STATUS_RX - RX interrupt pending
```
**Status**: ⚠️ All channels show 0x00 (no pending interrupts)

### Linux Driver Usage
The Linux driver (nvethernetrm) uses ALL three register sets:
- `VIRT_INTR_APB_CHX_CNTRL` - Set during driver open (mgbe_dma_chan_to_vmirq_map)
- `VIRT_INTR_CHX_CNTRL` - Enable/disable TX/RX interrupts via intr_fn[]
- `VIRT_INTR_CHX_STATUS` - Clear pending interrupts in interrupt handler

### COMMON_INTR_ENABLE (0x8704)
Wrapper-level interrupt enable for the "common" interrupt (IRQ 416).
```
Bit 2: MAC_SBD_INTR - MAC Side Band interrupt enable
Bit 3: CORE_UNCORRECTABLE_ERR - HSI error interrupt
Bit 4: CORE_CORRECTABLE_ERR - HSI error interrupt
Bit 5: REGISTER_PARITY_ERR - HSI parity error
```
**Status**: 0x00000000 in VM, but this only affects IRQ 416 (common), not 417-421 (DMA)

## 🚨 Current Hypothesis (H9): DMA Not Completing Transfers

**Key observation**: `VIRT_INTR_CHX_STATUS = 0` for all channels means no virtual
interrupts are pending. This is different from the issue we suspected.

Looking at DMA status:
- DMA_CHx_STATUS = 0x84 = TBU (bit 2) + RBU (bit 7)
- TBU = TX Buffer Unavailable
- RBU = RX Buffer Unavailable
- **TI (bit 0) and RI (bit 6) are NOT set!**

This means:
1. TX is not completing successfully (TI not set)
2. RX is failing to get descriptors (RBU set)
3. No actual packet transfers are completing

Yet DHCP discovery packets ARE being sent... Possible explanations:
- TX completes but TI is immediately cleared?
- Driver uses different mechanism?
- SMMU preventing DMA from accessing descriptors?

### Why RBU is Set
RBU (RX Buffer Unavailable) means the receive DMA tried to fetch a descriptor
but couldn't. This suggests:
- RX ring not properly initialized, OR
- DMA can't access RX descriptor memory (SMMU issue?), OR
- Descriptors not marked as owned by DMA

### Next Investigation
1. Check RX descriptor ring initialization
2. Verify SMMU Stream ID matches ASID configuration
3. Check if DMA can access descriptor memory regions

## 🚨 Current Hypothesis (H10): SMMU Blocking DMA Access

**Root cause identified (2026-01-11)**:

### Evidence
1. DMA IS started (TX_CTRL/RX_CTRL bit 0 = 1)
2. Descriptor addresses are valid (0xAB080000 in VM memory range)
3. tx_packets=0, rx_packets=0 - NO packets transferred
4. DMA status = TBU + RBU (buffer unavailable errors)

### seL4 Kernel Configuration
From `kernel/src/plat/orinagx/config.cmake` line 19:
```cmake
set(KernelArmSMMU OFF)
```

This means:
- seL4 does NOT configure SMMU at all
- SMMU remains in whatever state UEFI left it
- UEFI likely has SMMU enabled with specific mappings for MGBE

### SMMU and Stream IDs
- MGBE uses Stream ID 0x6 (TEGRA234_SID_MGBE0)
- ASID registers set this correctly (0x06060606, 0x06060606, 0x00000606)
- But SMMU may require explicit mappings for Stream ID 0x6 to access memory
- Without proper SMMU bypass, DMA fails to access descriptor memory

### VM Cannot Configure SMMU
- VM Linux has `iommu.passthrough=1` in kernel cmdline
- But SMMU hardware is not passed through to VM (not in devices.camkes)
- So VM Linux cannot actually put SMMU in passthrough mode

### Possible Solutions
1. **Disable SMMU in elfloader/seL4 boot** - Write SMMU_CR0 to disable translation
2. **Enable seL4 SMMU support and configure bypass** - Complex, requires seL4 changes
3. **Pass SMMU through to VM** - Risk: VM could misconfigure SMMU affecting all devices
4. **Modify UEFI to disable SMMU** - Requires NVIDIA UEFI modification

### Test to Verify
Add SMMU status/control register reads to init script:
- SMMU base: 0x12000000 (smmu_niso1 for MGBE)
- SMMU_CR0: Control register (bit 0 = enable)
- SMMU_IDR0: Identification register

## ❌ H10 DISPROVEN: SMMU Is Already Bypassed (2026-01-11)

### Test Results

Passed through SMMU MMIO regions to VM via `untyped_mmios` (NOT in dtb() to avoid driver loading):

```camkes
vm0.untyped_mmios = [
    /* SMMU/MC regions for debug access via devmem2 */
    "0x02c00000:20",      /* MC - Memory Controller (1MB) */
    "0x07000000:24",      /* smmu_niso1 shadow regs (16MB) */
    "0x08000000:24",      /* smmu_niso1 main regs (16MB) */
    "0x10000000:24",      /* smmu_iso (16MB) */
    "0x11000000:24",      /* smmu_niso0 shadow regs (16MB) */
    "0x12000000:24",      /* smmu_niso0 main regs (16MB) */
];
```

Also added SMMU nodes to kernel DTS (`kernel/tools/dts/orinagx.dts`) with `status = "disabled"`.

### SMMU Register Dump (ARM SMMU-500/SMMUv2)

```
smmu_niso1 (0x8000000) - MGBE uses this:
  sCR0: 0x00200001 (bit0: CLIENTPD - 1=bypass, 0=active)
  sCR1: 0x00000000
  IDR0: 0x7C013E80
  IDR1: 0x00400020

smmu_iso (0x10000000):
  sCR0: 0x00200001 (bit0: CLIENTPD - 1=bypass)

smmu_niso0 (0x12000000):
  sCR0: 0x00200001 (bit0: CLIENTPD - 1=bypass)

MC (0x2c00000):
  ERR_STATUS: 0x0000007F (multiple error bits!)
```

### Key Findings

1. **All three SMMUs are in bypass mode** (sCR0 bit0 = 1 = CLIENTPD)
2. **Memory Controller has errors** (ERR_STATUS = 0x7F)
3. **Elfloader CAN access SMMU** - But only after elfloader's own MMU is enabled (see below)

### Elfloader MMU Page Table Discovery (2026-01-11)

Initial attempts to access SMMU from elfloader caused Synchronous Exceptions. Root cause:

**Boot sequence and MMU state:**
1. `main()` - runs with **UEFI's page tables** (SMMU not mapped!)
2. `efi_exit_boot_services()` - still uses UEFI's page tables
3. `orinagx_fan_init()` - works because UEFI maps PWM/HSP/SRAM addresses
4. `continue_boot()` → `arm_enable_hyp_mmu()` - **elfloader's MMU enabled here**
5. After this point, elfloader's identity-mapped page tables are active

**Solution:** Move SMMU access to `continue_boot()` AFTER `arm_enable_hyp_mmu()`:

```c
// In tools/seL4/elfloader-tool/src/arch-arm/sys_boot.c, continue_boot():
    if (is_hyp_mode()) {
        printf("Enabling hypervisor MMU and paging\n");
        arm_enable_hyp_mmu();
    } else {
        printf("Enabling MMU and paging\n");
        arm_enable_mmu();
    }

#if defined(CONFIG_PLAT_ORIN_AGX)
    /* Now that elfloader's identity-mapped page tables are active,
     * we can access all device memory including SMMU registers. */
    extern void orinagx_smmu_disable(void);
    orinagx_smmu_disable();
#endif
```

**Elfloader output after fix:**
```
Orin AGX: Disabling SMMUs for device passthrough...
SMMU niso0 (0x12000000): sCR0=0x200001 - already bypassed
SMMU niso1 (0x8000000): sCR0=0x200001 - already bypassed
SMMU iso (0x10000000): sCR0=0x200001 - already bypassed
Orin AGX: SMMU bypass complete
```

### Conclusion

**SMMU is NOT the problem** - it's already bypassed. The DMA failure must be caused by:
1. **Memory Controller (MC) blocking access** - MC shows error status 0x7F
2. **Stream ID configuration in MC** - MC programs SID override registers
3. **Some other memory protection** - Firewall, TrustZone, etc.

### ARM SMMU-500 Register Layout (corrected)

Note: Initial investigation had wrong register labels. Correct offsets:
- Offset 0x000: **sCR0** (Secure Configuration Register 0) - bit0 is CLIENTPD
- Offset 0x004: sCR1
- Offset 0x020: **IDR0** (Identification Register 0)
- Offset 0x024: IDR1
- Offset 0x044: sGFSYNR2 (NOT GBPA - that's SMMUv3)

## 🚨 Current Hypothesis (H11): Memory Controller Blocking Access

**Updated root cause hypothesis (2026-01-11)**:

### Evidence
1. SMMU is bypassed (CLIENTPD=1) - NOT blocking DMA
2. MC ERR_STATUS = 0x7F - Multiple error flags set
3. DMA still fails (TBU + RBU)

### Tegra234 Memory Controller Architecture

The Memory Controller (MC) at 0x2c00000:
- Programs Stream ID (SID) override registers for each device
- Provides memory access control independent of SMMU
- Even with SMMU bypassed, MC might block access if SID isn't configured

### Next Investigation
1. Dump more MC registers (especially SID override registers)
2. Check if native Linux programs MC differently
3. Look for MC firewall/TrustZone configuration

## Updated Next Steps
1. ~~Check ASID registers~~ DONE - correctly written
2. ~~Add devmem2~~ DONE - working
3. ~~Debug num_vm_irqs value~~ DONE - correct (5)
4. ~~Check VIRT_INTR at both addresses~~ DONE - correctly set at MAC base
5. ~~Check DMA_CHx_IER (Interrupt Enable) registers~~ DONE - 0x41 (enabled)
6. ~~Check 0x8600 VIRT_INTR registers~~ DONE - CNTRL=0x03 (enabled)
7. ~~Enable COMMON_INTR_ENABLE at HV wrapper~~ DONE
8. ~~Check SMMU bypass status~~ DONE - SMMU is bypassed!
9. **IN PROGRESS**: Investigate Memory Controller (MC) errors
10. **PENDING**: Dump MC SID override registers

## Files Modified
- kernel/include/arch/arm/arch/machine/gic_v3.h - Added spurious IRQ debug
- projects/vm/components/VM_Arm/src/main.c - Added IRQ handler debug
- vm-images/.../init - Added register debug for DMA IER, MAC ISR, SMMU registers
- vm-images/build/.../ether_linux.c - Added debug prints for num_vm_irqs
- **tools/seL4/elfloader-tool/src/plat/orinagx/platform_init.c** - Added orinagx_smmu_disable()
- **tools/seL4/elfloader-tool/src/arch-arm/sys_boot.c** - Call SMMU disable after MMU enabled
- **kernel/tools/dts/orinagx.dts** - Added SMMU/MC nodes (status=disabled)
- **projects/vm-examples/apps/Arm/vm_minimal/orinagx/devices.camkes** - Added SMMU/MC to untyped_mmios

## Key Source Files
- nvidia-oot/drivers/net/ethernet/nvidia/nvethernet/ether_linux.c
- nvidia-oot/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm/osi/core/mgbe_core.c
- nvidia-oot/drivers/net/ethernet/nvidia/nvethernet/nvethernetrm/osi/core/mgbe_core.h
- **nvidia-uefi/edk2-nvidia/Silicon/NVIDIA/Drivers/EqosDeviceDxe/** - UEFI driver
