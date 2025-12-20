# Orin AGX Debugging Guide

**IMPORTANT: This is the master reference for Orin AGX debugging. Always consult this document and its linked references when investigating issues on Orin AGX.**

## Quick Links - Must Read

| Document | Purpose | Status |
|----------|---------|--------|
| [RAS Error Investigation](orin-ras-error-investigation.md) | **Main investigation log** - 167KB of findings | Active |
| [Memory Layout](sel4-memory-layout-orinagx.md) | Physical memory layout, kernel load addresses | Reference |
| [Tegra Cache Operations](tegra-cache-operations.md) | **CRITICAL**: dc civac vs dc cisw | Reference |

## Current Status (2025-12-20)

### RAS Errors on Orin AGX

**Bug A (0x7fffxxxx errors)**: IN PROGRESS - PTE overwrite mystery
- Page tables initialized correctly but get corrupted DURING test execution
- Corruption contains stale data from previous test iterations
- See Phase 17+ in [RAS Error Investigation](orin-ras-error-investigation.md)

**Bug B (0x0xxx errors)**: FIXED
- Cause: Unsafe (zero) PTEs during unmap operations
- Fix: Safe PTE initialization with `init_pt_with_safe_ptes()`

### Key Findings

1. **Cache operations work correctly** - dc civac flushes data to DRAM
2. **ATF/OP-TEE not corrupting memory** - 5-second delay test proves this
3. **Corruption happens during test execution** - not at boot or idle
4. **Only 7 of 122 tests trigger errors** - specific code paths involved
5. **Primary triggers**: FPU0001 (76%), CANCEL_BADGED_SENDS_0002 (32%)

### What NOT to investigate

- Cache flush operations (proven working)
- Arch_createObject() initialization (verified correct)
- ATF/OP-TEE memory corruption (disproven)
- Basic IPC, scheduling, CNode ops (never trigger errors)

## Documentation Index

### Core Investigation Documents

| Document | Description |
|----------|-------------|
| [orin-ras-error-investigation.md](orin-ras-error-investigation.md) | Complete investigation log with all phases and findings |
| [bug-b-investigation.md](bug-b-investigation.md) | Deep dive into Bug B (0x0xxx errors) - FIXED |
| [diagnostic-region-investigation.md](diagnostic-region-investigation.md) | 200KB diagnostic region test results |
| [speculative-ptw-analysis-plan.md](speculative-ptw-analysis-plan.md) | Analysis plan for speculative page table walks |

### Technical References

| Document | Description |
|----------|-------------|
| [sel4-memory-layout-orinagx.md](sel4-memory-layout-orinagx.md) | Physical memory layout, kernel load addresses, DTS configuration |
| [tegra-cache-operations.md](tegra-cache-operations.md) | **CRITICAL**: Why dc cisw doesn't work on Tegra, must use dc civac |
| [tegra-whole-cache-operations.md](tegra-whole-cache-operations.md) | Whole-cache operations on ARM64 |
| [arm64-cache-maintenance-barriers-smp.md](arm64-cache-maintenance-barriers-smp.md) | Cache maintenance and memory barriers |
| [arm64-mm-register-audit.md](arm64-mm-register-audit.md) | ARM64 memory management register audit |
| [sdei-ras-error-handling.md](sdei-ras-error-handling.md) | SDEI and RAS error handling on ARM |

### Tools and Techniques

| Document | Description |
|----------|-------------|
| [uart_raw_binary_transfer_for_debugging_se_l_4_embedded_systems.md](uart_raw_binary_transfer_for_debugging_se_l_4_embedded_systems.md) | Binary transfer over UART for ftrace data |
| [upstream-addrFromKPPtr-bugs.md](upstream-addrFromKPPtr-bugs.md) | Bugs found in upstream seL4 |

## Build Configurations for Debugging

```bash
# Standard hypervisor mode
make orinagx_defconfig && make sel4test

# With function tracing (ftrace)
make orinagx_ftrace_defconfig && make sel4test

# With ftrace + data cache disabled (for cache debugging)
make orinagx_ftrace_nocache_defconfig && make sel4test

# With 200KB diagnostic region at DRAM start
make orinagx_diag_defconfig && make sel4test
```

## Testing with Autopilot

Always use MCP tools for Orin AGX testing:

```python
# Build
mcp__sel4-autopilot__build_sel4test(mode="el2")  # or "el1", "el2-ftrace"

# Test single run
mcp__sel4-autopilot__test_sel4_binary(binary_path="...")

# Stress test (multiple runs)
mcp__sel4-autopilot__test_sel4_multi_run(binary_path="...", run_count=10)

# Get logs
mcp__sel4-autopilot__get_sel4_log(request_id="...")
```

## Analyzing Results

```bash
# Analyze sel4test log for RAS errors
/home/hlyytine/pkvm/autopilot/analyze_sel4log.py <results>/sel4.log

# Decode ftrace binary data
/home/hlyytine/tii-sel4/kernel/tools/decode_ftrace_binary.py <ftrace_data>
```

## Key Code Locations

| File | Purpose |
|------|---------|
| `kernel/src/arch/arm/64/kernel/vspace.c` | Page table operations, safe PTE init |
| `kernel/src/object/untyped.c` | Untyped retype, memory clearing |
| `kernel/src/benchmark/ftrace.c` | Function tracing, diagnostic region verification |
| `kernel/src/plat/orinagx/machine/cache.c` | Tegra-specific cache operations (dc civac) |
| `kernel/tools/dts/orinagx.dts` | Device tree (normal mode) |
| `kernel/tools/dts/orinagx-diag.dts` | Device tree (diagnostic mode) |

## Remember

1. **Always check [orin-ras-error-investigation.md](orin-ras-error-investigation.md)** before starting new investigation
2. **Read the Summary and latest Phase** to understand current status
3. **Don't re-investigate disproven hypotheses** (cache flush, ATF/OP-TEE)
4. **Use dc civac, never dc cisw** on Tegra platforms
5. **Run stress tests** (10+ iterations) to detect intermittent issues
