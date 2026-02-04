# Orin AGX RAS Errors - Canonical Overview

This document is the **single source of truth** for the RAS error problem statement, fix attempts, and current status on Orin AGX. It summarizes the investigation and links to deep-dive logs and evidence.

## Problem Statement

During `sel4test` on NVIDIA Orin AGX, the system reports **RAS (Reliability, Availability, Serviceability) errors** (SErrors) from the memory subsystem (SCC/ACI). These errors are **software-triggered**, not random hardware noise. The prevailing explanation is **speculative page table walks (PTW)** accessing **invalid physical addresses**, which the hardware reports as RAS errors.

## Two Independent Bugs

Investigation shows **two separate bugs** with distinct signatures:

| Bug | Error Pattern | Primary Trigger | Code Path | RAM Dependent | Status |
|-----|---------------|-----------------|-----------|---------------|--------|
| **A** | `0x7fffxxxx` | FPU0001 (>99%) | `arm_sys_send_recv`, `load_segment` | Yes (near 0x80000000) | **Open** |
| **B** | `0x0fc0/0x0ff0` | CANCEL_BADGED_SENDS (most) | `sel4utils_destroy_process`, `vka_cnode_revoke` | No | **Fixed** |

## Root Cause Summary

- RAS errors are delivered via **SError interrupts** (architecturally asynchronous), but our logs show **ERR<n>STATUS.DE=0** (not deferred). This means the error is reported immediately once signaled, even if it arrives while executing unrelated code.
- The **SERR field** describes the error reason (for example, illegal address) and does **not** indicate deferred vs non-deferred delivery.
- The hardware reports **invalid physical addresses** (below DRAM base or near NULL).
- The most consistent explanation is **speculative PTW reading invalid PTE output addresses**.

## Fix Attempts and Outcomes

| Fix / Hypothesis | Outcome | Notes |
|------------------|---------|-------|
| **Safe invalid PTEs** (Bug B) | **Fixed** | Eliminated near-NULL errors in destruction/revocation paths.
| **DSB in `vcpu_enable()`** | No effect | Kept for correctness but did not resolve Bug A.
| **Cache flush hypothesis** | Disproven | `dc civac` verified working; STNP bypass did not help.
| **ATF/OP-TEE corruption** | Disproven | Delayed verification and firmware memory audits ruled this out.
| **Diagnostic region test** | Disproven | First 200KB intact; corruption elsewhere.
| **SMMU/MC control** | Unverified | vm_minimal without RAS errors is an open lead.

## Current Status

- **Bug B (0x0xxx)**: **Fixed** with safe invalid PTEs in unmap/creation paths.
- **Bug A (0x7fffxxxx)**: **Open**. Appears as stale/overwritten PTE data during syscall/ELF paths. Root cause still unknown.

## Where to Go Next

- **Master investigation log:** [orin-ras-error-investigation.md](investigations/orin-ras-error-investigation.md)
- **Bug B deep dive:** [bug-b-investigation.md](investigations/bug-b-investigation.md)
- **Pattern docs:** [ras-fpu-pattern.md](investigations/ras-fpu-pattern.md), [ras-cancelbadgedsends-pattern.md](investigations/ras-cancelbadgedsends-pattern.md)
- **Hypotheses + experiment plan:** [ras-errors-hypotheses.md](plans/ras-errors-hypotheses.md)
- **Analysis plan:** [speculative-ptw-analysis-plan.md](plans/speculative-ptw-analysis-plan.md)
- **Speculative PTW background:** [arm-speculative-ptw-research.md](reference/arm-speculative-ptw-research.md)

## References

- [Orin AGX Debugging Guide](orin-agx-debugging-guide.md)
- [SDEI + RAS Handling](reference/sdei-ras-error-handling.md)
