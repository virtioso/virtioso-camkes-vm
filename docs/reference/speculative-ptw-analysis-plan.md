# Speculative PTW Analysis Plan

## Background: Critical Correction (2025-12-17)

### What We Learned

Testing with ARM_HYPERVISOR_SUPPORT=OFF revealed that **speculative page table walks (PTW) are NOT EL2-specific** - they occur at ALL exception levels on modern AArch64 SoCs:

| Configuration | Safe PTEs | RAS Errors per Run |
|---------------|-----------|-------------------|
| ARM_HYP=OFF | No | **556** |
| ARM_HYP=OFF | Yes | **0** |
| ARM_HYP=ON | Yes | **0-4** |

The ~200x higher error rate in EL1-only mode proved that EL2 mode was *masking* the bug (due to earlier safe PTE fixes being EL2-only), not causing it.

### Fix Applied

Commit `619e1848d` removed `#ifdef CONFIG_ARM_HYPERVISOR_SUPPORT` guards from:
- `vspace.c`: `pte_safe_invalid_new()`
- `objecttype.c`: VSpaceObject and PageTableObject initialization

---

## Consequences of This Correction

### 1. Architectural Understanding

**Previous assumption**: Speculative PTW was primarily an EL2/Stage-2 hypervisor concern due to:
- ARM errata focusing on KVM/hypervisor scenarios
- HCR_EL2.VM workaround specifically for Stage-2
- Most Linux patches targeting KVM paths

**Corrected understanding**: Speculative PTW is a **universal ARM64 phenomenon**:
- Occurs at EL1 (non-hypervisor kernels)
- Occurs at EL2 (hypervisor mode)
- Likely occurs at EL3 (secure firmware) though not tested
- Any zero PTE can trigger hardware accesses to PA=0

### 2. Broader Platform Implications

If speculative PTW occurs universally on Cortex-A78AE (Orin AGX), it likely affects:

| Platform | CPU | Likely Affected |
|----------|-----|-----------------|
| NVIDIA Xavier | Carmel (custom ARM) | Possibly |
| NVIDIA Orin | Cortex-A78AE | **Confirmed** |
| Other Cortex-A78 variants | A78, A78C | Probable |
| Later ARM cores | Cortex-X1, X2, A710, etc. | Unknown |
| Apple Silicon | M1, M2, etc. | Unknown |

**Note**: Cortex-A78AE has RAS support that *reports* the error. Other platforms may silently corrupt or have different behavior.

### 3. Code Paths Not Yet Audited

The safe PTE fix was applied to:
- [x] `unmapPage()` - page unmap
- [x] `unmapPageTable()` - page table unmap
- [x] VSpaceObject creation
- [x] PageTableObject creation
- [x] `armKSGlobalUserVSpace` initialization

**Still need audit**:
- [ ] Any other PTE clearing paths in `vspace.c`
- [ ] ASID pool management (uses zero)
- [ ] Page table retyping/recycling paths
- [ ] `clearMemory()` operations on page table memory
- [ ] Initial boot-time page table setup

---

## Further Analysis Needed

### Priority 1: Code Audit

**Goal**: Ensure ALL paths that clear/zero PTEs use safe invalid values.

**Files to audit**:
```
kernel/src/arch/arm/64/kernel/vspace.c
kernel/src/arch/arm/64/object/objecttype.c
kernel/include/arch/arm/arch/64/mode/object/structures_gen.h
kernel/src/arch/arm/kernel/boot.c (ARM64 portion)
```

**What to look for**:
- Calls to `pte_pte_invalid_new()` - should these use `pte_safe_invalid_new()`?
- Direct writes of 0 to PTE memory
- `memset()` or `clearMemory()` on page table structures
- Any PTE initialization that doesn't go through safe functions

### Priority 2: Other ARM Platforms

**Question**: Does this affect other seL4-supported ARM64 platforms?

**Test plan**:
1. Run sel4test on Raspberry Pi 4 (Cortex-A72) with ARM_HYP=OFF
2. Check for RAS errors (if Cortex-A72 reports them)
3. Run sel4test on QEMU ARM virt with ARM_HYP=OFF
4. Profile error patterns

**Note**: Raspberry Pi 4's Cortex-A72 may not have RAS reporting, so errors might be silent.

### Priority 3: ARM Documentation Research

**Questions to answer**:
1. Is there official ARM documentation on speculative PTW across exception levels?
2. Are there Cortex-A78AE errata specifically mentioning EL1 speculation?
3. Does ARM recommend safe invalid PTE patterns in their architecture guides?
4. How do other hypervisors (Xen, Hafnium) handle this?

**Resources to check**:
- ARM Cortex-A78AE TRM (Technical Reference Manual)
- ARM Architecture Reference Manual (DDI 0487)
- ARM Errata database for Cortex-A78/A78AE
- Xen and Hafnium source code for comparison

### Priority 4: Performance Impact Assessment

**Concern**: Safe PTEs contain a valid physical address - could this affect:
- Cache behavior (speculative accesses hit cache instead of error)
- TLB behavior (does hardware cache "invalid" entries differently?)
- Memory bandwidth (speculative reads to safe memory)

**Test plan**:
1. Run performance benchmarks with/without safe PTEs
2. Monitor cache hit/miss ratios
3. Check for any regression in context switch times

---

## Upstream Considerations

### Should This Be Upstreamed to seL4?

**Arguments for**:
1. Bug affects all ARM64 platforms, not just Orin
2. Safe PTEs are a defense-in-depth improvement
3. Zero PTEs are architecturally unsafe per ARM speculative behavior
4. The fix has no functional impact (PTEs remain invalid)

**Arguments against**:
1. Only confirmed on one platform (Orin AGX with Cortex-A78AE)
2. May add minor overhead on platforms without the issue
3. Original seL4 developers may have had reasons for zero PTEs

**Recommendation**:
1. First, test on additional platforms to confirm breadth
2. Gather performance data
3. Then propose as platform-specific change or global fix depending on findings

### Patch Format

If upstreaming, the change should:
- Be a single commit for all safe PTE changes
- Include detailed commit message explaining:
  - The speculative PTW phenomenon
  - Test results showing the issue
  - Why zero PTEs are unsafe
  - Which platforms were tested
- Reference ARM documentation if available

---

## Remaining Bug A Investigation

**Status**: Bug A (0x7fffxxxx errors in FPU/syscall path) remains partially unresolved.

**Current understanding**:
- Triggered primarily by FPU0001 test
- Only occurs when RAM starts near 0x80000000
- Related to address calculation in syscall path
- HCR_EL2.VM workaround helps but doesn't eliminate

**Next steps for Bug A**:
1. Investigate `arm_sys_send_recv` code path
2. Check `load_segment` ELF loading for address issues
3. Consider whether Bug A is a separate VTTBR-related race

---

## Summary: Immediate Actions

| Action | Priority | Status |
|--------|----------|--------|
| Apply safe PTE fix to EL1 paths | P0 | **Done** (commit 619e1848d) |
| Update documentation | P1 | **Done** |
| Audit remaining PTE paths | P2 | Not started |
| Test on other ARM64 platforms | P3 | Not started |
| Research ARM documentation | P3 | Not started |
| Performance assessment | P4 | Not started |
| Upstream proposal | P5 | Blocked on P2-P4 |

---

## Document History

| Date | Change |
|------|--------|
| 2025-12-17 | Initial plan created after EL1 speculative PTW confirmation |
