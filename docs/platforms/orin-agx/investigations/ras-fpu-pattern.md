# RAS Error Pattern: FPU0001 Thread Creation and Wake-up

**Date**: 2025-12-22
**Status**: Reproducible pattern identified

## Summary

RAS errors on Orin AGX are also triggered by the FPU0001 test during thread creation and subsequent wake-up. This is a distinct pattern from the CancelBadgedSends pattern but shares the same root cause: rapid thread switching with VSpace transitions.

## The Pattern

The FPU0001 test creates multiple threads that perform FPU operations. The RAS error occurs during thread configuration and wake-up:

```
[Memory setup: UntypedRetype + ARMPageMap alternating]
→ TCBSetPriority(6) → TCBWriteRegisters(3) → TCBSetTLSBase(19) → TCBResume(12)
→ THREAD switch → Recv → THREAD switch → ... → RAS_ERROR
```

### Syscall Tags

| Tag | Invocation | Description |
|-----|------------|-------------|
| 1 | `UntypedRetype` | Create new kernel objects |
| 42 | `ARMPageMap` | Map pages into VSpace |
| 6 | `TCBSetPriority` | Set thread priority |
| 3 | `TCBWriteRegisters` | Write thread registers |
| 19 | `TCBSetTLSBase` | Set TLS base address |
| 12 | `TCBResume` | Resume thread execution |

### Test Code Reference

From `projects/sel4test/apps/sel4test-tests/src/tests/fpu.c`:

```c
// test_fpu_multithreaded - creates NUM_THREADS threads doing FPU ops
for (int i = 0; i < NUM_THREADS; i++) {
    // Allocate thread resources (UntypedRetype, ARMPageMap)
    // Configure thread (TCBSetPriority, TCBWriteRegisters, TCBSetTLSBase)
    // Resume thread (TCBResume) → thread switches begin
}
```

## Evidence

### Run 1 (2025-12-22 01:21)
- **Test iteration**: 103
- **Ftrace events**: 6,512,140
- **Interrupted PC**: `0x4007c4`
- **Request ID**: 20251222-012113

### Ftrace Trace Before Error

```
[6512019] KENTRY Syscall Call cap=1 tag=42    (ARMPageMap)
[6512022] KENTRY Syscall Call cap=2 tag=1     (UntypedRetype)
[6512025] KENTRY Syscall Call cap=1 tag=42    (ARMPageMap)
[6512028] KENTRY Syscall Call cap=12 tag=6    (TCBSetPriority)
[6512031] KENTRY Syscall Call cap=12 tag=3    (TCBWriteRegisters)
[6512034] KENTRY Syscall Call cap=12 tag=19   (TCBSetTLSBase)
[6512037] KENTRY Syscall Call cap=12 tag=12   (TCBResume)
[6512039] THREAD tcb=0x80ac22c800
[6512040] VSPACE paddr=0xac002000
[6512042] KENTRY Syscall Recv cap=4 tag=0
[6512044] THREAD tcb=0x8080ac8800
[6512045] VSPACE paddr=0xac002000
[6512051] THREAD tcb=0x8080aa6800
[6512058] THREAD tcb=0x8080ae4800
[6512065] THREAD tcb=0x80afffa800
...
[6512137] RAS_ERROR event=44 cpu=0 elr=0x4007c4
```

### Key Observations

| Attribute | Value |
|-----------|-------|
| VSpace | 0xac002000 (same as CancelBadgedSends) |
| Trigger phase | TCBResume followed by thread switches |
| Thread count | Multiple FPU worker threads |
| RAS event type | SCC Uncorrectable Error |

## Comparison with CancelBadgedSends Pattern

| Aspect | FPU0001 | CANCEL_BADGED_SENDS_0002 |
|--------|---------|--------------------------|
| Trigger | TCBResume (thread start) | CancelBadgedSends (thread wake) |
| Setup phase | UntypedRetype + ARMPageMap | Already set up |
| Syscall pattern | SetPriority→WriteRegs→SetTLS→Resume | CNodeRevoke→CancelBadgedSends |
| Thread action | Recv after resume | Recv after wake |
| VSpace | 0xac002000 | 0xac002000 |
| Common factor | **Rapid thread switching** | **Rapid thread switching** |

## Analysis

Both patterns share the same fundamental trigger:

1. **Rapid context switching** between multiple threads
2. **Same VSpace** (0xac002000) across all switches
3. **Recv syscalls** causing threads to block/unblock
4. **RAS error** during the switching cascade

The difference is how the thread cascade is initiated:
- FPU0001: New thread creation and resume
- CANCEL_BADGED_SENDS: Blocked threads woken by cancelBadgedSends

## Hypothesis

The RAS errors occur during rapid VSpace/VMID transitions. Both patterns create a "storm" of context switches where:

1. Multiple threads become runnable simultaneously
2. Each switch requires VSpace validation/transition
3. Speculative page table walks may encounter stale or transitional state
4. SCC (System Cache Controller) detects an error condition

This aligns with the need for DSB barriers before VTTBR changes, as implemented in Linux/KVM/Xen for similar speculative PTW issues.

## Build Configuration

Same as CancelBadgedSends testing - `el2-ras` build mode:
- `KernelFtraceBuffer=ON` - enables ftrace buffer for RAS event logging
- `KernelFunctionTrace=OFF` - no function enter/exit tracing (low overhead)
- `KernelArmSdeiRas=ON` - SDEI RAS error handling
- `KernelBenchmarks=track_kernel_entries` - kernel entry/exit tracking

## Reproduction

```bash
# Build with el2-ras mode
mcp__sel4-autopilot__build_sel4test(mode="el2-ras")

# Run test - will halt on first RAS error with ftrace dump
mcp__sel4-autopilot__test_sel4_binary(binary_path="...", timeout=600)

# Query ftrace for FPU-related patterns
mcp__sel4-autopilot__query_ftrace(request_id="...", summary=True)
```

## Next Steps

1. **Unify with CancelBadgedSends investigation**
   - Both patterns point to context switch path issues
   - Focus on VTTBR write barriers

2. **Add instrumentation around TCBResume**
   - Track VSpace switches during thread resume
   - Compare with cancelBadgedSends wake-up path

3. **Test with additional barriers**
   - Same fix should address both patterns
   - DSB before VTTBR_EL2 writes

## Related Documents

- [CancelBadgedSends Pattern](ras-cancelbadgedsends-pattern.md) - The other reproducible pattern
- [ARM Speculative PTW Research](../reference/arm-speculative-ptw-research.md) - Linux/KVM/Xen fixes
- [orin-ras-error-investigation.md](orin-ras-error-investigation.md) - Full investigation log
- [arm64-speculative-ptw-safe-invalid-pte.md](../../../../../../kernel/docs/arm64-speculative-ptw-safe-invalid-pte.md) - Speculative PTW fix details
