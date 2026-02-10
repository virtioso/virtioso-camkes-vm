# Orin AGX RAS Errors - Implementation Plan (With Continuous Documentation)

This plan describes how to execute the RAS hypotheses/experiment plan and **document each step as it happens**, not only at the end.

## Goal

Isolate and fix Bug A (0x7fffxxxx) by iterating through ranked hypotheses with concrete experiments, while maintaining a continuous, structured investigation log.

## Continuous Documentation Policy

Every phase and experiment must be documented immediately in the investigation log with the template below. This prevents drift and makes the investigation reproducible.

**Experiment Log Template**:

- **Hypothesis**:
- **Change / Instrumentation**:
- **Expected Outcome**:
- **Run Conditions** (build, configs, test subset, seed):
- **Observed Outcome**:
- **Conclusion** (supported / weakened / inconclusive):
- **Next Step**:

## Phase 0 - Ground Truth and Inputs

1. Collect current build configs (CMake cache, kernel config, build mode).
2. Record exact sel4test binaries and repro steps for Bug A (FPU0001) and Bug B (cancelBadgedSends).
3. Confirm logging/trace infrastructure availability (ftrace, ATF RAS logs, test harness).

**Documentation**:
- Add a "Baseline Inputs" section to the investigation log with build IDs, configs, test subset, and log locations.

## Phase 1 - Code Audit (Spec-Driven)

1. Enumerate all VTTBR/TTBR write paths and VSpace/VMID transitions.
2. Audit barrier and TLBI ordering at each path.
3. Trace PT lifecycle: create/clear/init/publish/free/reuse ordering.
4. Verify memory attributes for PT allocations.
5. Identify any device DMA that can write PT memory without SMMU translation.

**Documentation**:
- Add a "Code Audit Findings" section with file paths and call sites.
- Record any suspected ordering violations or missing barriers.

## Phase 2 - Spec and Errata Cross-Check

1. Compare current ordering to ARM ARM requirements (break-before-make, TLBI + DSB/ISB sequencing).
2. Compare to known Linux/KVM/Xen speculative PTW mitigations.
3. Confirm RAS record interpretation (ERR<n>STATUS, ERR<n>ADDR, SERR).

**Documentation**:
- Add a "Spec/Errata Mismatches" section.
- Record any ambiguous cases to be resolved by experimentation.

## Phase 3 - Instrumentation

1. Add tracepoints around VTTBR writes, VMID changes, TLBI, DSB/ISB.
2. Add PT lifecycle tracing (create/free/first use).
3. Capture correlation between error events and VTTBR/VMID/PT states.

**Documentation**:
- Add an "Instrumentation Plan" section and then "Instrumentation Results" with sample logs.

## Phase 4 - Top 3 Experiments (Fast Signal)

### A1/A3: Barrier and TLBI Hardening
- Insert canonical DSB/ISB + TLBI ordering around all VSpace switch paths.
- Run FPU0001 and cancelBadgedSends.

### B1: PT Pool Isolation
- Allocate PTs from a dedicated pool that is never freed/reused during a test run.
- Run FPU0001 and cancelBadgedSends.

### D1: DMA/SMMU Control
- Disable suspect DMA masters or enforce SMMU translation for device accesses.
- Run FPU0001 and cancelBadgedSends.

**Documentation**:
- Record each experiment using the template. Note exact build flags and logs.

## Phase 5 - Secondary Experiments

1. Full VMID-scoped TLBI on VSpace switch (heavy but diagnostic).
2. PT memory attributes: force non-cacheable or PoC-cleaned PT pages.
3. Delay window tests between PT init and first use.
4. DT/boot memory map audit to rule out overlaps.

**Documentation**:
- Log each experiment and its conclusion using the template.

## Phase 6 - Conclude and Patch

1. Identify the hypothesis that best explains all results.
2. Produce minimal patch set with spec references.
3. Validate via full sel4test and isolated repros.

**Documentation**:
- Add a "Conclusion" section with root cause, fix rationale, and remaining risks.

## References

- Canonical overview: `docs/platforms/orin-agx/ras-errors.md`
- Investigation log: `docs/platforms/orin-agx/investigations/orin-ras-error-investigation.md`
- Hypotheses + experiment plan: `docs/platforms/orin-agx/plans/ras-errors-hypotheses.md`
- SDEI/RAS handling: `docs/platforms/orin-agx/reference/sdei-ras-error-handling.md`
- Speculative PTW research: `docs/platforms/orin-agx/reference/arm-speculative-ptw-research.md`
