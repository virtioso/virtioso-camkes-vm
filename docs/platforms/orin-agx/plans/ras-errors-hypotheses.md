# Orin AGX RAS Errors - Hypotheses and Experiment Plan

This document turns the RAS investigation status into a ranked hypothesis list with concrete experiments. It is scoped to **full stack** (kernel/VMM, firmware/ATF, SMMU/MC, boot/DT, DMA).

## Goal

Deliver a focused, reproducible path to isolate **Bug A** (0x7fffxxxx) and confirm remaining root causes.

## Known Facts (Condensed)

- RAS errors are delivered via **SError interrupts** (asynchronous exception class) but our logs show **ERR<n>STATUS.DE=0** (not deferred).
- Two independent bugs:
  - **Bug A**: `0x7fffxxxx` pattern; FPU0001/thread-switch heavy; RAM near 0x80000000; **open**.
  - **Bug B**: `0x0fc0/0x0ff0` pattern; cancelBadgedSends + cleanup paths; **fixed** via safe invalid PTEs.
- Errors are associated with **speculative PTW** hitting invalid/stale PTEs.
- Tests pass architecturally; the issue is in error reporting and memory subsystem interactions.

## Ranked Hypotheses (Bug A Focus)

Priority is a mix of likelihood, leverage, and effort.

1. **Missing/incorrect barrier or TLBI ordering in VSpace/VMID switch paths**
   - Rationale: Triggered by rapid context switching, consistent with speculative PTW hazards.
2. **PT reuse race window (clear/init vs PTW)**
   - Rationale: Stale/garbage PTEs observed; PTs reused quickly under churn.
3. **Stale TLB/walk-cache retention across VSpace reuse**
   - Rationale: Errors show up later during cleanup; TLB invalidation might be incomplete.
4. **Uncontrolled DMA or device writeback corrupting PT memory**
   - Rationale: vm_minimal (with SMMU/MC passthrough) shows no errors; sel4test likely no SMMU.
5. **Memory attribute/coherency mismatch for PT pages**
   - Rationale: Orin MC strictness; speculative PTW could see stale lines.
6. **Boot/DT memory map/reserved region overlap**
   - Rationale: Rare but catastrophic; would explain address patterns and reproducibility.
7. **Firmware/ATF interaction (error routing or register state confusion)**
   - Rationale: ATF reports RAS/SError; could mask or reorder reporting (less likely root cause).

## Experiment Matrix

Each experiment should run with `KernelAArch64SErrorIgnore OFF` when possible and include ATF RAS logs.

### A. Kernel/VMM - Barriers and TLBI

- **A1: Audit and trace VTTBR/TTBR writes + DSB/ISB + TLBI ordering**
  - Add tracepoints around all VSpace switch paths (`setVMRoot`, thread resume/switch).
  - Expected: missing barrier path correlates with errors.
- **A2: Force full TLBI for VMID on switch**
  - Temporary over-invalidation; check if error rate drops.
- **A3: Insert DSB/ISB before and after VTTBR writes in all paths**
  - Compare with existing `vcpu_enable()` DSB-only change.

### B. Page Table Reuse and Lifecycle

- **B1: PT pool isolation (no reuse)**
  - Allocate PTs from a dedicated region never freed during run.
  - Expected: If reuse race is the cause, errors drop or vanish.
- **B2: Delay window test**
  - Add short delay between PT clear/init and first use.
  - Expected: If PTW races with init, error rate shifts with delay.
- **B3: Instrument PT lifecycle**
  - Track create/destroy and log if any PT accessed after free.

### C. TLB/Walk Cache Persistence

- **C1: Forced full system TLBI on VSpace switch**
  - Heavy but diagnostic; if errors vanish, stale translation is likely.
- **C2: Explicit invalidate on PT free**
  - Add TLBI after freeing PTs.

### D. DMA / SMMU / MC

- **D1: Disable suspect DMA masters**
  - Boot with reduced device set or SMMU blocking.
  - Expected: If DMA corrupts PTs, errors drop.
- **D2: Run sel4test with SMMU configured**
  - Compare against current default (no SMMU setup).
- **D3: MC config parity with vm_minimal**
  - Replicate MC settings from vm_minimal (if known) and compare.

### E. Boot/DT/Memory Map

- **E1: Audit reserved regions against PT allocation ranges**
  - Cross-check DTB, firmware reserved, and PT allocator ranges.
- **E2: DRAM base offset sweep**
  - Repeat boundary sweep to validate RAM-range dependency.

### F. Firmware / ATF / RAS Logging

- **F1: Capture raw RAS record fields (STATUS/ADDR/MISC)**
  - Confirm DE=0 across all errors and verify ADDR decode.
- **F2: Validate ESR_EL2 EC in ATF logs**
  - Ensure EC=0x2F only when truly handling SError.

## Minimal Repro Harness

- Run isolated **FPU0001** and **CANCEL_BADGED_SENDS_0002** with:
  - Fixed VSpace
  - Controlled thread count and priorities
  - Repeatable RAS log collection

## Success Criteria

- Identify at least one change that **eliminates Bug A** without masking (no `SErrorIgnore`).
- Provide a minimal patch set with a clear causal narrative.

## Runbook (Initial Top 3)

1. **A1 + A3**: Instrument + barrier insertion in VSpace switch paths.
2. **B1**: PT pool isolation to remove reuse race.
3. **D1**: Disable DMA sources or enforce SMMU to rule out external corruption.

## References

- Canonical overview: `docs/platforms/orin-agx/ras-errors.md`
- Investigation log: `docs/platforms/orin-agx/investigations/orin-ras-error-investigation.md`
- SDEI/RAS handling: `docs/platforms/orin-agx/reference/sdei-ras-error-handling.md`
- Speculative PTW research: `docs/platforms/orin-agx/reference/arm-speculative-ptw-research.md`
