# Orin AGX RAS Error Investigation

## Summary

**STATUS: UNDER INVESTIGATION**

This document captures the investigation into intermittent RAS (Reliability, Availability, Serviceability) errors occurring during sel4test execution on NVIDIA Orin AGX. The errors report ADDR values like `0x8000000000000000`, which decode to **PA=0x0 (NULL pointer access)** from non-secure world (bit 63 is the NS flag, not part of the address - see "ARM RAS ADDR Encoding" section).

**Hypothesized Root Cause**: seL4's TLB invalidation code may have set VTTBR_EL2 with a NULL base address (0) when switching VMIDs. Tegra234's memory subsystem (SCC/IOB/ACI) potentially does not tolerate this and generates RAS errors when speculative table walks access PA=0x0.

**Attempted Fix**: Commit `448255694ff79fe1204c968530f6bf374670f969` - Modified `tlb.h` to use `addrFromKPPtr(armKSGlobalUserVSpace)` as the VTTBR base instead of NULL. This provides a valid (empty) page table base that might satisfy Tegra234's speculative table walk requirements.

**Note**: This hypothesis has not been definitively confirmed. The fix showed improvement in testing but the root cause analysis is ongoing.

## Error Pattern

### Observed RAS Error Addresses

During sel4test multi-run testing, the following error addresses were observed:

| Run | ADDR (raw) | Decoded PA (bits 55:0) | NS (bit 63) | Valid DRAM? |
|-----|------------|------------------------|-------------|-------------|
| 1 | `0x8000000000001b00` | `0x0000000000001b00` | 1 (non-secure) | ❌ NO (below 0x80000000) |
| 3 | `0x800000007ffc0800` | `0x0000007ffc0800` | 1 (non-secure) | ❌ NO (below 0x80000000) |
| 3 | `0x800000007ffc0bc0` | `0x0000007ffc0bc0` | 1 (non-secure) | ❌ NO (below 0x80000000) |
| 4 | `0x800000007ffc0040` | `0x0000007ffc0040` | 1 (non-secure) | ❌ NO (below 0x80000000) |
| 4 | `0x800000007ffc0bc0` | `0x0000007ffc0bc0` | 1 (non-secure) | ❌ NO (below 0x80000000) |
| 4 | `0x800000007ffc0c00` | `0x0000007ffc0c00` | 1 (non-secure) | ❌ NO (below 0x80000000) |

**Note**: ALL observed error PAs are below Orin's DRAM base (0x80000000). These are accesses to unmapped memory regions.

### Address Pattern Analysis

**IMPORTANT**: Bit 63 is the NS (Non-Secure) flag, NOT part of the physical address! See "ARM RAS ADDR Encoding" section for details.

- **Bit 63 (NS flag)**: All accesses are from non-secure world
- **Actual PA range**: Decoded PAs are in `0x0` to `0x7ffc0xxx` range
- **ALL PAs are below DRAM base**: Orin DRAM starts at `0x80000000`. All observed error PAs are below this - they are **NOT valid DRAM addresses**
- **PA=0x0 pattern**: Some errors show PA=0x0 (NULL pointer access)
- **PA=0x7ffc0xxx pattern**: Other PAs like `0x7ffc0xxx` are also below DRAM base (unmapped memory region)

### Test Triggering the Error

The RAS error consistently occurs during:
- **Test**: `CANCEL_BADGED_SENDS_0002`
- **Frequency**: ~60% of runs (3 out of 5 in multi-run testing)

## Memory Configuration

### seL4 Memory Layout on Orin AGX

```
Physical Address Space (40-bit):
  physBase:     0x80000000
  Memory size:  8GB (0x200000000)
  PA range:     0x80000000 - 0x280000000

Virtual Address Space (Hypervisor mode):
  PPTR_BASE:        0x0000008000000000 (2^39)
  PPTR_TOP:         0x000000ffc0000000 (2^40 - 2^30)
  PPTR_BASE_OFFSET: 0x0000008000000000
```

### VTCR Configuration

```c
// kernel/include/arch/arm/armv/armv8-a/64/armv/vcpu.h:594-610
vtcr_el2 = VTCR_EL2_T0SZ(24);     // 40-bit input IPA
vtcr_el2 |= VTCR_EL2_PS(PS_1T);   // 40-bit PA size
vtcr_el2 |= VTCR_EL2_SL0(SL0_4K_L1);  // Start at level 1
```

## TLB Barrier Analysis

### PTE Update Sequence

The sequence for unmapping a page (`unmapPage` in `kernel/src/arch/arm/64/kernel/vspace.c:1035`):

```
1. Store PTE:     *(lu_ret.ptSlot) = pte_pte_invalid_new()
2. Cache clean:   dc cvau, %0
3. Memory barrier: dmb sy
4. DSB:           dsb sy (in invalidateLocalTLB_IPA_VMID)
5. VMID switch:   dsb sy → MSR vttbr_el2 → isb (if needed)
6. TLBI stage 2:  tlbi ipas2e1, %0
7. DSB:           dsb sy
8. TLBI stage 1:  tlbi vmalle1
9. DSB:           dsb sy
10. ISB:          isb
```

### Key Functions and Their Barriers

| Function | Location | Barriers |
|----------|----------|----------|
| `cleanByVA_PoU` | mode/machine.h:286 | dc cvau → dmb sy |
| `invalidateLocalTLB_IPA_VMID` | armv/tlb.h:34 | dsb sy at entry |
| `setCurrentUserVSpaceRoot` | mode/machine.h:179 | dsb sy → MSR → isb |
| `invalidateLocalTLB_IPA` | mode/machine.h:269 | tlbi → dsb → tlbi → dsb → isb |

### Analysis Result

**The barrier sequences are correct** according to ARM architecture requirements:
- DSB before TLBI ensures PTE writes are visible
- DSB after TLBI ensures invalidation is complete
- ISB ensures instruction stream sees the changes

## SError Handling

### Configuration

```cmake
# kernel/src/plat/orinagx/config.cmake:20
set(KernelAArch64SErrorIgnore ON)
```

### Behavior

When `KernelAArch64SErrorIgnore` is enabled:
- SError exceptions trigger a simple `eret` return
- No kernel panic or error handling
- RAS errors are reported by NVIDIA firmware, not seL4

```asm
# kernel/src/arch/arm/64/traps.S:144-148
#ifdef CONFIG_AARCH64_SERROR_IGNORE
    eret
#else
    b       invalid_vector_entry
#endif
```

## Possible Root Causes

### 1. ARM RAS ADDR Encoding Clarification

**Note**: The ADDR field uses standard ARM RAS encoding where bit 63 is the NS (Non-Secure) flag, not part of the address. See "ARM RAS ADDR Encoding" section below for details.

- `0x8000000000000000` decodes to: NS=1, **PA=0x0** (NULL pointer access)
- `0x800000007ffc0xxx` decodes to: NS=1, **PA=0x7ffc0xxx** (below DRAM base 0x80000000 - unmapped)

The different patterns between hypervisor and non-hypervisor modes may indicate different speculative access patterns, not different encoding schemes.

### 2. Speculative Memory Access During TLB Flush

The error consistently occurs during thread bind/unbind operations (BIND tests) or endpoint operations (CANCEL_BADGED_SENDS). These operations involve TLB invalidation. Speculative execution on Cortex-A78AE may access memory with stale TLB entries before invalidation fully propagates.

### 3. Page Table Race Condition

Thread binding/unbinding involves page table modifications. A race between CPU speculative access and TLB invalidation could cause the MMU to access invalid entries temporarily.

### 4. Hardware Memory Controller Issue

Could be an ECC error, parity issue, or memory controller bug triggered by specific access patterns during kernel context switching.

### 5. ~~Stage-2 Translation Fault~~ (Ruled Out)

~~In hypervisor mode, stage-2 translation uses IPA (Intermediate Physical Address). An invalid IPA could trigger the memory controller error.~~

**Update**: Testing with ARM_HYP=OFF shows the error occurs MORE frequently without stage-2 translation (100% vs 60%), ruling out stage-2 as the root cause.

## Attempted Fixes

### Boot Delay Test (2 seconds)

Added a 2-second delay at kernel boot (after "Bootstrapping kernel" message) to test if RAS errors are async leftovers from bootloader/UEFI:

```c
// kernel/src/arch/arm/kernel/boot.c
#ifdef CONFIG_PLAT_ORIN_AGX
    uint64_t freq, start, now, target;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    asm volatile("mrs %0, cntpct_el0" : "=r"(start));
    target = start + (freq * 2);  /* 2 second delay */
    printf("DEBUG: Waiting 2 seconds for async RAS errors...\n");
    do {
        asm volatile("mrs %0, cntpct_el0" : "=r"(now));
    } while (now < target);
    printf("DEBUG: Delay complete, continuing boot\n");
#endif
```

**Result**: RAS errors still occur during tests, NOT during the 2-second boot delay. This proves:
- Errors are **synchronous**, triggered by kernel operations
- NOT leftover async errors from bootloader/UEFI
- The triggering operation is in the kernel, not external

### PA Validation in cache.c (Reverted)

Attempted to add physical address validation assertions to catch invalid addresses:

```c
// Early boot crashes prevented this approach
static inline void validate_pa(paddr_t pa) {
    assert(pa >= ORIN_PA_MIN && pa <= ORIN_PA_MAX);
}
```

**Result**: Caused early boot crashes because:
- printf() not available during early boot
- Static variable approach also crashed
- Had to revert all changes

### Current State

- `kernel/src/plat/orinagx/machine/cache.c` uses dc civac (correct for Tegra)
- TLB barriers are correct
- SErrors are ignored
- RAS errors still occur intermittently (~60% of runs)

## Files Examined

| File | Purpose |
|------|---------|
| `kernel/src/plat/orinagx/machine/cache.c` | Tegra-specific cache operations |
| `kernel/src/arch/arm/64/kernel/vspace.c` | Page table and TLB operations |
| `kernel/include/arch/arm/arch/64/mode/machine.h` | TLB invalidation functions |
| `kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h` | Hypervisor TLB functions |
| `kernel/include/arch/arm/armv/armv8-a/64/armv/vcpu.h` | VTCR configuration |
| `kernel/src/plat/orinagx/config.cmake` | Platform configuration |

## Recommendations

### Short Term

1. **Continue with SError ignore**: The current `KernelAArch64SErrorIgnore ON` setting allows tests to complete despite RAS errors
2. **Monitor frequency**: Track if the error rate changes with kernel modifications

### Medium Term

1. **NVIDIA consultation**: Contact NVIDIA about the RAS error address format and meaning
2. **Firmware investigation**: Check if UEFI/TF-A configuration affects RAS reporting
3. **Memory test**: Run Linux memtest on the same hardware to rule out hardware issues

### Long Term

1. **RAS error handler**: Implement proper SError handling that logs the error details without crashing
2. **Page table validation**: Add runtime checks for page table integrity (with boot-safe logging)
3. **Hardware debugging**: Use NVIDIA debugging tools to trace memory accesses

## Test Results Summary

### Hypervisor Mode (ARM_HYP=ON)

| Test Configuration | Result |
|-------------------|--------|
| sel4test multi-run (5 runs) | 3/5 had RAS errors (60%) |
| Triggered by | CANCEL_BADGED_SENDS_0002 |
| Error Address Pattern | `0x800000007ffc0xxx` |
| Tests pass despite errors | Yes |
| Kernel crashes | No (SError ignored) |

### Non-Hypervisor Mode (ARM_HYP=OFF)

| Test Configuration | Result |
|-------------------|--------|
| sel4test multi-run (5 runs) | 5/5 had RAS errors (100%) |
| Triggered by | BIND0001 (occasionally BIND0003) |
| Error Address Pattern | `0x8000000000000000` or `0x8000000000000010` |
| Tests pass despite errors | Yes (tests interrupted but don't fail) |
| Kernel crashes | No (SError ignored) |

### Comparative Analysis

| Aspect | Hypervisor Mode | Non-Hypervisor Mode |
|--------|-----------------|---------------------|
| Error Rate | 60% (3/5) | 100% (5/5) |
| Triggering Test | CANCEL_BADGED_SENDS | BIND tests |
| Address Pattern | physBase in upper bits | Clean bit 63 only |
| Stage-2 Translation | Yes | No |

**Key Finding**: RAS errors are NOT hypervisor-specific. They actually occur MORE frequently without hypervisor support, suggesting the root cause is in core kernel operations related to thread management or endpoint operations, not stage-2 translation.

### Single-Core DTS Test (ARM_HYP=ON)

Disabled 11 of 12 CPU cores in the device tree to test if multi-core cache coherency is the cause:

| Run | RAS Errors | Triggering Test |
|-----|------------|-----------------|
| 1 | 2 errors | IPC1002 (inter-AS Call/ReplyRecv) |
| 2 | 0 errors | Clean run |
| 3 | 2 errors | IPC1004 |
| 4 | 5 errors | CANCEL_BADGED_SENDS_0001 |
| 5 | 2 errors | IPC1001 |

**Result**: Error rate **increased** from 60% to 80% with single-core DTS. Errors now occur in different tests (IPC tests, not just CANCEL_BADGED_SENDS). Different RAS sources: IOB (0xe010000) in addition to SCC.

**Key Finding**: Disabling secondary cores in DTS does NOT help. Note that `status = "disabled"` only tells seL4 not to use those cores - TF-A firmware may still have all 12 cores running in WFI loops. This rules out seL4 multi-core coherency as the cause.

## Additional Experiments

### Boot Delay Test (2-4 seconds)

Added a delay loop at kernel boot to test if RAS errors are async leftovers from bootloader/UEFI:

**Result**: No RAS errors occurred during the delay. Errors only occur during test execution. This proves errors are **synchronous** - triggered by kernel operations, not async from bootloader.

### DMB Barrier in cancelBadgedSends

Added `dmb sy` memory barrier in the cancelBadgedSends loop to test speculative execution hypothesis:

**Result**: No change in error rate (still ~60%). Memory barrier in this location does not help.

## Summary of Ruled-Out Causes

| Hypothesis | Test | Result |
|------------|------|--------|
| Async errors from bootloader | 4-second boot delay | Ruled out - errors only during tests |
| Stage-2 translation (hypervisor) | ARM_HYP=OFF test | Ruled out - errors worse without hyp |
| seL4 multi-core coherency | Single-core DTS | Ruled out - errors worse with single-core |
| Speculative execution in cancelBadgedSends | DMB barrier | Ruled out - no change |

## HYPOTHESIZED ROOT CAUSE

### TLB Invalidation with NULL VTTBR_EL2 Base Address

**Hypothesis**: seL4 intentionally sets VTTBR_EL2 with a NULL base address (0) when switching VMIDs for TLB invalidation. Tegra234's memory subsystem may not tolerate this, generating RAS errors when speculative table walks access PA=0x0. This hypothesis is supported by evidence but not definitively confirmed.

#### Call Path

```
unmapPage
  → invalidateTLBByASIDVA(asid, vptr)
    → invalidateTranslationSingle((hw_asid << 48) | vaddr >> seL4_PageBits)
      → invalidateTranslationSingleLocal(vptr)
        → invalidateLocalTLB_IPA_VMID(vptr)
          → setCurrentUserVSpaceRoot(ttbr_new(vmid, 0))  // BUG: base=0!
          → invalidateLocalTLB_IPA(ipa)
            → tlbi ipas2e1, %0
            → dsb sy
            → tlbi vmalle1    // <-- RAS ERROR OCCURS HERE
```

#### ATF Debug Output Proof

Added debug code to TF-A's `tegra234_ras_handler()` to capture EL2/EL3 state:

```
ERROR:   ======= RAS INTERRUPT CONTEXT =======
ERROR:   ELR_EL3 (seL4 PC when interrupted): 0x808001a974
ERROR:   SPSR_EL3: 0x800003c9 (mode=9)  // EL2h
ERROR:   ELR_EL2: 0x40bff8
ERROR:   ESR_EL2: 0x56000000
ERROR:   FAR_EL2: 0x515910
ERROR:   VTTBR_EL2: 0x1000000000000     // VMID=1, BASE=0 (NULL!)
ERROR:   ======================================
```

**VTTBR_EL2 Analysis:**
- Bits [63:48] = VMID = 0x0001 (valid)
- Bits [47:0] = Base = 0x0000_0000_0000 (**NULL - INVALID!**)

**Kernel Instruction at ELR_EL3:**
```
808001a970:  d5033f9f   dsb sy
808001a974:  d508871f   tlbi vmalle1    <-- RAS interrupt taken here
808001a978:  d5033f9f   dsb sy
```

#### seL4 Code Analysis

In `kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h`:

```c
static inline void invalidateLocalTLB_IPA_VMID(word_t ipa_plus_vmid)
{
    word_t vttbr = getVTTBR();
    word_t v = (vttbr >> 48);
    word_t vmid = ipa_plus_vmid >> 48;
    word_t ipa = ipa_plus_vmid & 0xfffffffff;
    dsb();
    if (v != vmid) {
        setCurrentUserVSpaceRoot(ttbr_new(vmid, 0));  // BUG: sets base=0
    }
    invalidateLocalTLB_IPA(ipa);
    // ...
}
```

The comment in the seL4 code says:
> "Note that an invalid address is used, and it seems fine."

**This is NOT fine on Tegra234!**

#### Why This Happens

According to ARM architecture, TLBI operations should not require a valid page table - they just invalidate TLB entries. However, Tegra234's SCC/IOB/ACI hardware appears to:

1. Trigger internal memory accesses during `tlbi vmalle1`
2. Use the VTTBR_EL2 base address (even if null)
3. Generate RAS errors when speculative accesses to PA=0x0 occur (ADDR reports NS=1, PA=0)

This is either:
- A Tegra234 hardware errata
- NVIDIA-specific behavior not documented in ARM specs
- A stricter implementation of TLB maintenance operations

### Implemented Fix

**Commit**: `448255694ff79fe1204c968530f6bf374670f969`

Modified `kernel/include/arch/arm/armv/armv8-a/64/armv/tlb.h` to use `armKSGlobalUserVSpace` as the VTTBR base instead of NULL:

```c
static inline void invalidateLocalTLB_VMID(word_t vmid)
{
    word_t vttbr = getVTTBR();
    word_t v = (vttbr >> 48);
    dsb();
    /* We need to switch to the target VMID for flushing
     * the TLB if necessary.
     * Note: We use armKSGlobalUserVSpace as a valid empty page table base.
     * Using base=0 causes RAS errors on some platforms (e.g., Tegra234/Orin)
     * where the hardware does speculative table walks during TLBI.
     */
    if (v != vmid) {
        setCurrentUserVSpaceRoot(ttbr_new(vmid, addrFromKPPtr(armKSGlobalUserVSpace)));
    }
    invalidateLocalTLB_VMALLS12E1();
    if (v != vmid) {
        /* Restore the previous VTTBR value */
        setCurrentUserVSpaceRoot((ttbr_t) {
            .words[0] = vttbr
        });
    }
}
```

The same pattern was applied to `invalidateLocalTLB_IPA_VMID()`.

**Why this works**: `armKSGlobalUserVSpace` is an empty page table that exists throughout kernel execution. Using its address as the VTTBR base provides a valid pointer for Tegra234's speculative table walk hardware, preventing RAS errors while still allowing the TLB invalidation to work correctly.

### Fix Verification

| Test | Before Fix | After Fix |
|------|------------|-----------|
| 5-run multi-run | 3/5 RAS crashes (60%) | 1/5 (rare FPU0000 error) |
| 10-run multi-run | N/A | 0/10 RAS crashes (0%) |

The fix eliminates the RAS errors caused by NULL VTTBR base during TLB invalidation. ATF debug output after fix shows valid VTTBR:
```
VTTBR_EL2: 0x1000080d26000  // VMID=1, BASE=0x80d26000 (valid!)
```

## ARM RAS Error Record Address Register (ERR<n>ADDR) Encoding

**IMPORTANT**: The ADDR field in RAS error reports is NOT a raw 64-bit physical address. The upper bits contain status flags!

### Bit Field Layout

| Bits | Field | Description |
|------|-------|-------------|
| **63** | **NS** | Non-secure attribute (1 = non-secure access) |
| 62 | SI | Secure Incorrect (1 = NS field may be wrong) |
| 61 | AI | Address Incorrect (1 = PADDR may be wrong) |
| 60 | VA | Virtual Address (1 = address is VA, not PA) |
| 59 | NSE | Physical Address Space (FEAT_RME only) |
| 58:56 | RES0 | Reserved, read-as-zero |
| **55:0** | **PADDR** | Actual physical address (56-bit max) |

### Decoding Examples

#### Example 1: `ADDR = 0x8000000000000000`

```
Binary: 1000 0000 0000 0000 ... (64 zeros after bit 63)

Bit 63 (NS) = 1    → Non-secure access
Bit 62 (SI) = 0    → Secure field is valid
Bit 61 (AI) = 0    → Address is VALID (not incorrect!)
Bit 60 (VA) = 0    → Physical address (not virtual)
Bits 55:0 (PADDR) = 0x0000_0000_0000 → Physical address is ZERO (NULL!)
```

**Interpretation**: A non-secure access to **physical address 0x0** - this is a NULL pointer access!

#### Example 2: `ADDR = 0x800000007fff4540`

```
Bit 63 (NS) = 1    → Non-secure access
Bit 62 (SI) = 0    → Secure field is valid
Bit 61 (AI) = 0    → Address is VALID
Bit 60 (VA) = 0    → Physical address
Bits 55:0 (PADDR) = 0x0000_7fff_4540 → PA ~2GB (but below DRAM base!)
```

**Interpretation**: A non-secure access to **physical address 0x7fff4540** - this is **BELOW** Orin's DRAM range (DRAM starts at 0x80000000). This address is in an unmapped memory region.

#### Example 3: `ADDR = 0xC000000080100000`

```
Bit 63 (NS) = 1    → Non-secure access
Bit 62 (SI) = 1    → Secure field may be incorrect!
Bit 61 (AI) = 0    → Address is valid
Bits 55:0 (PADDR) = 0x0000_0080_1000_00 → PA 0x80100000
```

### Common Misinterpretation

When seeing error addresses like `0x8000000000000000`, it's easy to think "bit 63 is set, which is impossible for a 40-bit physical address space" and assume it's address corruption.

**WRONG**: Bit 63 is corrupted/invalid
**RIGHT**: Bit 63 is the NS flag; the actual PA is in bits 55:0

### Source References

- [ARM ERR<n>ADDR Register](https://arm.jonpalmisc.com/latest_sysreg/ext-errnaddr) - Jon's ARM Reference
- [ARM Developer Documentation](https://developer.arm.com/documentation/107734/0001/External-registers/External-RAS-registers-summary/ERR0ADDR--Error-Record--n--Address-Register) - Official ARM docs
- ARM Architecture Reference Manual, RAS Extension chapter

## Ongoing Investigation: Remaining RAS Errors (2025-12-14)

### Status: INVESTIGATING

After the initial VTTBR NULL base fix, RAS errors still occur intermittently (~40% of runs pass). Further investigation reveals new patterns.

### New Error Pattern: PA=0 Access

Recent captures show a different error signature:

```
ERROR:   RAS Uncorrectable Error in IOB, base=0xe010000:
ERROR:          Status = 0xec000612
ERROR:   SERR = Error response from slave: 0x12
ERROR:          IERR = CBB Interface Error: 0x6
ERROR:          ADDR = 0x8000000000000000

ERROR:   RAS Uncorrectable Error in ACI, base=0xe01a000:
ERROR:          Status = 0xe8000904
ERROR:   SERR = Assertion failure: 0x4
ERROR:          IERR = FillWrite Error: 0x9
ERROR:          ADDR = 0x8000000000000000
```

**Decoded**: ADDR = 0x8000000000000000 → NS=1, **PADDR = 0x0** (NULL pointer access!)

### Tegra234 Memory Map

Physical address 0x0 has **no memory** on Tegra234:
- SYSRAM starts at 0x40000000 (per [tegra234.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/nvidia/tegra234.dtsi))
- DRAM starts at 0x80000000
- Any access to PA=0 causes a bus error → RAS interrupt

### Hypothesis: Speculative Table Walk with Zero PTEs

The `armKSGlobalUserVSpace` page table is BSS-initialized (all zeros). When used as VTTBR base during TLB operations:

1. Hardware does speculative table walk
2. Reads PTE entry = 0x0 (all zeros, bit[0]=0 = invalid)
3. ARM architecture says invalid entries should cause translation fault
4. **However**, some ARM cores (documented for Neoverse N1) "use the fixed value of 0" when faults occur during speculation
5. Speculative prefetch to PA=0 triggers RAS error on Tegra234's CBB fabric

### Cortex-A78AE Errata for Non-ARM Interconnects

NVIDIA's Orin uses their own **CBB (Control Back Bone)** interconnect, not ARM's interconnect IP. There's a relevant errata:

**ERRATA_A78_AE_2712574**: Affects Cortex-A78AE on systems without ARM interconnect IP.
- Revisions affected: r0p0, r0p1, r0p2 (Orin is r0p1)
- Issue: FWB (Force Write-Back) feature doesn't work correctly
- Effect: "A KVM guest could fetch stale/junk data instead of instructions"
- Workaround: Disable FWB, perform explicit cache maintenance

**Status**: Not enabled in NVIDIA's ATF build. Testing in progress.

### ATF Debug Build Configuration

Built ATF with debug options to get detailed RAS output:

```makefile
# ~/pkvm/Linux_for_Tegra/source/tegra/optee-src/atf/nvbuild.sh
DEBUG=1
LOG_LEVEL=50
```

### CPU Errata Enabled

Added missing errata workarounds for Cortex-A78AE r0p1:

```makefile
# platform_t234.mk - TII additions
ERRATA_A78_AE_1941500 := 1
ERRATA_A78_AE_1951502 := 1
ERRATA_A78_AE_2376748 := 1
ERRATA_A78_AE_2395408 := 1
ERRATA_A78_AE_2712574 := 1  # For non-ARM interconnect (CBB)
```

### ATF Fixes Applied

1. **Removed spammy EHF logs** in `bl31/ehf.c` (lines 257, 308, 356)
2. **Fixed backtrace crash** in `plat/nvidia/tegra/soc/t234/plat_ras.c`:
   - ATF's EL3 page tables don't map seL4's memory region
   - Backtrace code was crashing when trying to walk seL4's stack
   - Modified to just print register values without dereferencing

### Debugging Resources and Techniques

#### Challenge: Asynchronous Nature of SErrors

SErrors are asynchronous - an incorrect memory access may not immediately trigger an SError. The fault might be reported later when the AXI transaction completes, by which time the CPU may have transitioned to a different exception level. This makes correlating errors with their source difficult.

#### KVM Speculative Page Table Walk Issue (Highly Relevant)

Linux KVM encountered a similar issue:

> "The page table walker is allowed to carry on with speculative walks started from EL1&0 while running at EL2... If the hypervisor modifies EL1&0 system registers before these walks complete, the PTW may access stale or incorrect translation tables."

**KVM Fix**: Insert `DSB` barriers before modifying translation regime registers.

Source: [KVM: arm64: Synchronise speculative page table walks](https://lore.kernel.org/linux-arm-kernel/ZDdIWIIogROyg1zD@linux.dev/T/)

#### seL4 SError Handling Discussion

From seL4 mailing list: "People who care deeply about this issue should make sure they have ARMv8.2 hardware with FEAT_RAS and use that to implement proper SError handling in seL4 for such systems."

Source: [seL4 Devel: ARM64 SError handling](https://www.mail-archive.com/devel@sel4.systems/msg03091.html)

#### Debugging Techniques

1. **Leverage RAS error records** - examine ADDR field (remember bit 63 = NS flag)
2. **Software context tracking** - record EL states during transitions
3. **Robust diagnostic logging** - capture registers and faulting addresses
4. **Compare with Linux** - run KVM on same hardware to see if similar issues occur

#### Potential Root Causes Still Under Investigation

1. **armKSGlobalUserVSpace contains zero PTEs** - speculative walks may extract PA=0 from invalid entries
2. **Insufficient DSB barriers** - may need more barriers than currently implemented
3. **NVIDIA CBB-specific behavior** - proprietary interconnect may have stricter requirements

### SDEI (Software Delegated Exception Interface) and RAS Dispatch

#### Why ATF Reports "sdei_dispatch_event returned -1"

When RAS errors occur, NVIDIA's ATF tries to dispatch an SDEI event to notify the running OS. The `-1` return means **SDEI events are masked on this PE**.

**Source Code** (`atf/arm-trusted-firmware/services/std_svc/sdei/sdei_intr_mgmt.c:601-604`):
```c
/* Can't dispatch if events are masked on this PE */
state = sdei_get_this_pe_state();
if (state->pe_masked)
    return -1;
```

#### SDEI Architecture Overview

SDEI is an ARM standard for delivering asynchronous events (like RAS errors) from firmware to the OS. The flow:

1. **OS Registration**: OS calls `SDEI_EVENT_REGISTER` to register handler entry points
2. **PE Unmask**: OS calls `SDEI_PE_UNMASK` to enable event delivery on each PE
3. **Event Dispatch**: Firmware calls `sdei_dispatch_event()` to invoke OS handler
4. **OS Handling**: OS handler runs at registered entry point, processes error
5. **Event Complete**: OS calls `SDEI_EVENT_COMPLETE` to return control

**Key Insight**: "All PEs start with SDEI events masked" (`sdei_main.c:80`). The **client** (OS) must call `SDEI_PE_UNMASK` to receive events.

#### Why seL4 Gets -1

seL4 is a bare-metal microkernel that doesn't implement SDEI:
- Never calls `SDEI_PE_UNMASK` (SMC 0xC400002C)
- Never registers SDEI event handlers
- PE remains masked → dispatch fails → ATF logs `-1`

#### Consequence of SDEI Failure

When `sdei_dispatch_event()` returns -1:
1. ATF logs the error but **continues processing**
2. For uncorrectable errors, `system_is_not_recoverable = true`
3. ATF powers off the core after processing all errors

**The SDEI failure doesn't cause the crash** - the uncorrectable RAS error does. SDEI just means the OS can't handle it gracefully.

#### How Linux Handles SDEI

Linux kernel implements full SDEI support for RAS handling (`drivers/firmware/arm_sdei.c`):

**1. Probe/Init** (during boot):
```c
static int sdei_probe(struct platform_device *pdev)
{
    // Get SDEI version from firmware
    err = sdei_api_get_version(&ver);

    // Get arch-specific entry point (assembly handler)
    sdei_entry_point = sdei_arch_get_entry_point(conduit);

    // Register CPU hotplug callbacks - this is key!
    err = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "SDEI",
                            &sdei_cpuhp_up, &sdei_cpuhp_down);
}
```

**2. PE Unmask** (called when each CPU comes online via hotplug):
```c
static int sdei_cpuhp_up(unsigned int cpu)
{
    // Re-register private events for this CPU...

    // UNMASK THIS CPU FOR SDEI EVENTS
    return sdei_unmask_local_cpu();
}

int sdei_unmask_local_cpu(void)
{
    // SMC call: SDEI_1_0_FN_SDEI_PE_UNMASK (0xC400002C)
    return invoke_sdei_fn(SDEI_1_0_FN_SDEI_PE_UNMASK, 0, 0, 0, 0, 0, NULL);
}
```

**3. GHES Registration** (for RAS error events):
```c
// drivers/acpi/apei/ghes.c uses this to register for RAS notifications
int sdei_register_ghes(struct ghes *ghes, sdei_event_callback *normal_cb,
                       sdei_event_callback *critical_cb)
{
    event_num = ghes->generic->notify.vector;  // From ACPI table
    err = sdei_event_register(event_num, cb, ghes);
    if (!err)
        err = sdei_event_enable(event_num);
    return err;
}
```

**4. Event Handler** (arch-specific assembly, `arch/arm64/kernel/sdei.c`):
```c
// Entry: __sdei_asm_handler (arch/arm64/kernel/entry.S)
// - Switches to SDEI-specific stack
// - Saves interrupted context
// - Calls do_sdei_event()

unsigned long do_sdei_event(struct pt_regs *regs, struct sdei_registered_event *arg)
{
    // Retrieve clobbered registers
    for (i = 0; i < clobbered_registers; i++)
        sdei_api_event_context(i, &regs->regs[i]);

    // Call the registered callback (e.g., GHES handler)
    err = sdei_event_handler(regs, arg);

    // Return to interrupted code or IRQ vector
    return SDEI_EV_HANDLED;
}
```

**5. Mask on CPU Down** (power management, reboot, idle):
```c
static int sdei_cpuhp_down(unsigned int cpu)
{
    // Unregister private events
    // MASK THIS CPU
    return sdei_mask_local_cpu();
}
```

**Key Insight**: Linux's SDEI lifecycle:
- Boot: `sdei_probe()` → registers hotplug callbacks
- Each CPU online: `sdei_cpuhp_up()` → calls `sdei_unmask_local_cpu()`
- RAS event: ATF calls `sdei_dispatch_event()` → Linux handler invoked
- CPU offline/idle: `sdei_cpuhp_down()` → calls `sdei_mask_local_cpu()`

#### Options for seL4

1. **Implement SDEI support** (complex):
   - Add SMC calls for PE_UNMASK during boot
   - Create assembly entry points for SDEI events
   - Implement RAS error handler

2. **Continue ignoring SErrors** (current approach):
   - `KernelAArch64SErrorIgnore ON`
   - Let ATF power off core on uncorrectable errors
   - Prevent errors at source (valid VTTBR, safe PTEs)

3. **Modify ATF** (not recommended):
   - Change RAS handler to not power off core
   - Would mask real hardware errors

### References

- [TF-A CPU Build Macros](https://trustedfirmware-a.readthedocs.io/en/latest/design/cpu-specific-build-macros.html)
- [FWB Errata Patch Series](https://lore.kernel.org/linux-arm-kernel/20230330165128.3237939-1-james.morse@arm.com/T/)
- [NVIDIA RAS IOB Error Forum](https://forums.developer.nvidia.com/t/ras-uncorrectable-error-in-iob-base-0xe010000/322875)
- [OE4T RAS Discussion](https://github.com/orgs/OE4T/discussions/1624)
- [ARM RAS Error Reporting](https://developer.arm.com/documentation/107790/latest/RAS-error-reporting-flow)
- [Detecting SError Origin](https://www.systemonchips.com/detecting-serror-interrupt-origin-in-arm-exception-levels-el0-el1-el2-el3/)
- [KVM Translation Table Walk RAS](https://patchwork.kernel.org/project/linux-arm-kernel/patch/1511988524-30240-1-git-send-email-gengdongjiu@huawei.com/)
- [Linux SDEI ARM64 Patch](https://patchwork.kernel.org/project/linux-arm-kernel/patch/20170922182614.27885-10-james.morse@arm.com/)
- [SDEI Platform Error Injection](https://static.linaro.org/connect/yvr18/presentations/yvr18-107.pdf)

## References

- ARM Architecture Reference Manual - ARMv8-A (Cache and TLB maintenance)
- NVIDIA Orin TRM (Tegra234 Technical Reference Manual)
- seL4 kernel source code
- `tegra-cache-operations.md` - Related cache issue documentation

## Build Configurations Used

### Hypervisor Mode Build
```bash
make orinagx_defconfig   # Sets ARM_HYP=ON
make sel4test
```

### Non-Hypervisor Mode Build
```bash
make orinagx_nohyp_defconfig   # Sets ARM_HYP=OFF
make sel4test
```

Config differences:
- `ARM_HYP=ON`: `CONFIG_ARCH_ARM_HYP` enabled, uses EL2, stage-2 translation
- `ARM_HYP=OFF`: No hypervisor support, runs at EL1, only stage-1 translation

## Changelog

| Date | Change |
|------|--------|
| 2025-12-14 | Documented SDEI architecture and why `sdei_dispatch_event returned -1` occurs |
| 2025-12-14 | Added detailed Linux kernel SDEI handling flow (PE_UNMASK, GHES, hotplug) |
| 2025-12-14 | Documented ARM RAS ADDR register bit field encoding (bit 63 = NS flag, not address) |
| 2025-12-14 | Identified PA=0 access pattern in remaining RAS errors |
| 2025-12-14 | Added ERRATA_A78_AE_2712574 for non-ARM interconnect (CBB) |
| 2025-12-14 | Enabled missing CPU errata: 1941500, 1951502, 2376748, 2395408 |
| 2025-12-14 | Fixed ATF backtrace crash when accessing seL4 memory from EL3 |
| 2025-12-14 | Built ATF with DEBUG=1 LOG_LEVEL=50 for detailed RAS output |
| 2025-12-13 | **Hypothesis identified**: NULL VTTBR_EL2 base address during TLB invalidation |
| 2025-12-13 | Added ATF debug code to capture EL2/EL3 state on RAS interrupt |
| 2025-12-13 | Added single-core DTS test - ruled out seL4 multi-core coherency |
| 2025-12-13 | Added DMB barrier test in cancelBadgedSends - no effect |
| 2025-12-13 | Added boot delay test - proved errors are synchronous |
| 2025-12-13 | Added non-hypervisor mode test results, ruled out stage-2 as root cause |
| 2025-12-13 | Initial investigation and documentation |
