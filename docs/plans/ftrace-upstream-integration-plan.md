# Integration Plan: Upstream seL4 Tracing → Virtioso Ftrace

## Executive Summary

This plan describes how to route upstream seL4 benchmark/tracing events into our ftrace
infrastructure, providing a unified trace stream with LZ4 compression.

**Goal**: When both `CONFIG_BENCHMARK_TRACK_KERNEL_ENTRIES` and `CONFIG_KERNEL_FUNCTION_TRACE`
are enabled, kernel entry events (syscalls, interrupts, faults) are logged to ftrace alongside
function call traces.

**Non-Goal**: Replace upstream tracing. The memory-mapped buffer approach remains for
user-space benchmark tools that expect it.

---

## Phase 1: Kernel Entry Markers (Priority: HIGH)

### 1.1 New Marker Definitions

Add to `kernel/include/benchmark/ftrace.h`:

```c
/* Kernel entry markers - integrate with CONFIG_BENCHMARK_TRACK_KERNEL_ENTRIES */

/* KERNEL_ENTRY marker - generic kernel entry event
 * Format: marker + entry_type + details
 *   Entry 0: 0x7FF7 << 1 | 0 = 0xFFEE (marker)
 *   Entry 1: entry_type (3 bits) | syscall_no (4 bits) | cap_type (5 bits) | is_fastpath (1 bit)
 *   Entry 2: invocation_tag[15:0]
 *   Entry 3: invocation_tag[18:16] (3 bits, upper) | reserved
 * Total: 8 bytes (4 entries)
 */
#define FTRACE_KERNEL_ENTRY_MARKER  0xFFEE  /* 0x7FF7 << 1 */
#define FTRACE_KERNEL_ENTRY_ENTRIES 4

/* INTERRUPT marker - external interrupt
 * Format: marker + irq_number
 *   Entry 0: 0x7FF6 << 1 | 0 = 0xFFEC (marker)
 *   Entry 1: irq_number[15:0]
 * Total: 4 bytes (2 entries)
 */
#define FTRACE_INTERRUPT_MARKER     0xFFEC  /* 0x7FF6 << 1 */
#define FTRACE_INTERRUPT_ENTRIES    2

/* KERNEL_EXIT marker - leaving kernel back to user
 * Format: marker + duration
 *   Entry 0: 0x7FF5 << 1 | 0 = 0xFFEA (marker)
 *   Entry 1: duration[15:0]
 *   Entry 2: duration[31:16]
 * Total: 6 bytes (3 entries)
 */
#define FTRACE_KERNEL_EXIT_MARKER   0xFFEA  /* 0x7FF5 << 1 */
#define FTRACE_KERNEL_EXIT_ENTRIES  3

/* Entry type encoding (matches upstream entry_type_t) */
#define FTRACE_ENTRY_UNKNOWN        0
#define FTRACE_ENTRY_INTERRUPT      1
#define FTRACE_ENTRY_UNKNOWN_SYSCALL 2
#define FTRACE_ENTRY_USER_FAULT     3
#define FTRACE_ENTRY_DEBUG_FAULT    4
#define FTRACE_ENTRY_VM_FAULT       5
#define FTRACE_ENTRY_SYSCALL        6
#define FTRACE_ENTRY_VCPU_FAULT     7  /* ARM only */
```

### 1.2 New Ftrace API Functions

Add to `kernel/include/benchmark/ftrace.h`:

```c
/* Log kernel entry event (syscall, fault, interrupt) */
void ftrace_kernel_entry(word_t path, word_t syscall_no, word_t cap_type,
                         word_t is_fastpath, word_t invocation_tag);

/* Log interrupt with IRQ number */
void ftrace_interrupt(word_t irq);

/* Log kernel exit with duration (cycles) */
void ftrace_kernel_exit(word_t duration);
```

### 1.3 Implementation in ftrace.c

Add to `kernel/src/benchmark/ftrace.c`:

```c
void ftrace_kernel_entry(word_t path, word_t syscall_no, word_t cap_type,
                         word_t is_fastpath, word_t invocation_tag)
{
    if (!ftrace_enabled) return;

    /* Pack entry info:
     * Entry 1: path(3) | syscall_no(4) | cap_type(5) | is_fastpath(1) = 13 bits
     */
    word_t packed = (path & 0x7) |
                    ((syscall_no & 0xF) << 3) |
                    ((cap_type & 0x1F) << 7) |
                    ((is_fastpath & 0x1) << 12);

    ftrace_work_buffer[ftrace_work_idx++] = FTRACE_KERNEL_ENTRY_MARKER;
    ftrace_work_buffer[ftrace_work_idx++] = (uint16_t)packed;
    ftrace_work_buffer[ftrace_work_idx++] = (uint16_t)(invocation_tag & 0xFFFF);
    ftrace_work_buffer[ftrace_work_idx++] = (uint16_t)((invocation_tag >> 16) & 0x7);

    ftrace_total_logged += FTRACE_KERNEL_ENTRY_ENTRIES;
    ftrace_check_flush();
}

void ftrace_interrupt(word_t irq)
{
    if (!ftrace_enabled) return;

    ftrace_work_buffer[ftrace_work_idx++] = FTRACE_INTERRUPT_MARKER;
    ftrace_work_buffer[ftrace_work_idx++] = (uint16_t)(irq & 0xFFFF);

    ftrace_total_logged += FTRACE_INTERRUPT_ENTRIES;
    ftrace_check_flush();
}

void ftrace_kernel_exit(word_t duration)
{
    if (!ftrace_enabled) return;

    ftrace_work_buffer[ftrace_work_idx++] = FTRACE_KERNEL_EXIT_MARKER;
    ftrace_work_buffer[ftrace_work_idx++] = (uint16_t)(duration & 0xFFFF);
    ftrace_work_buffer[ftrace_work_idx++] = (uint16_t)((duration >> 16) & 0xFFFF);

    ftrace_total_logged += FTRACE_KERNEL_EXIT_ENTRIES;
    ftrace_check_flush();
}
```

### 1.4 Hook into Upstream Tracing

Modify `kernel/include/benchmark/benchmark_track.h`:

```c
#ifdef CONFIG_KERNEL_FUNCTION_TRACE
#include <benchmark/ftrace.h>
#endif

static inline void benchmark_debug_syscall_start(word_t cptr, word_t msgInfo, word_t syscall)
{
    seL4_MessageInfo_t info = messageInfoFromWord_raw(msgInfo);
    lookupCapAndSlot_ret_t lu_ret = lookupCapAndSlot(NODE_STATE(ksCurThread), cptr);
    ksKernelEntry.path = Entry_Syscall;
    ksKernelEntry.syscall_no = -syscall;
    ksKernelEntry.cap_type = cap_get_capType(lu_ret.cap);
    ksKernelEntry.invocation_tag = seL4_MessageInfo_get_label(info);

#ifdef CONFIG_KERNEL_FUNCTION_TRACE
    ftrace_kernel_entry(Entry_Syscall, -syscall,
                        cap_get_capType(lu_ret.cap),
                        0, /* is_fastpath - set later if applicable */
                        seL4_MessageInfo_get_label(info));
#endif
}
```

Modify `kernel/src/benchmark/benchmark.c` to add exit hook:

```c
void benchmark_track_exit(void)
{
    /* ... existing code ... */

#ifdef CONFIG_KERNEL_FUNCTION_TRACE
    timestamp_t duration = timestamp() - ksEnter;
    ftrace_kernel_exit((word_t)duration);
#endif
}
```

### 1.5 Interrupt Logging

Modify interrupt handler in `kernel/src/arch/arm/kernel/boot.c` or trap handler:

```c
/* In handleInterruptEntry or similar */
#ifdef CONFIG_KERNEL_FUNCTION_TRACE
    ftrace_interrupt(irq);
#endif
```

---

## Phase 2: Decoder Updates (Priority: HIGH)

### 2.1 Update decode_ftrace_binary.py

Add parsing for new markers in `kernel/tools/decode_ftrace_binary.py`:

```python
# New marker constants
MARKER_KERNEL_ENTRY = 0xFFEE
MARKER_INTERRUPT = 0xFFEC
MARKER_KERNEL_EXIT = 0xFFEA

# Entry type names
ENTRY_TYPES = {
    0: "Unknown",
    1: "Interrupt",
    2: "UnknownSyscall",
    3: "UserFault",
    4: "DebugFault",
    5: "VMFault",
    6: "Syscall",
    7: "VCPUFault",
}

def decode_kernel_entry(entries, idx):
    """Decode KERNEL_ENTRY marker"""
    packed = entries[idx + 1]
    invoc_lo = entries[idx + 2]
    invoc_hi = entries[idx + 3]

    path = packed & 0x7
    syscall_no = (packed >> 3) & 0xF
    cap_type = (packed >> 7) & 0x1F
    is_fastpath = (packed >> 12) & 0x1
    invocation_tag = invoc_lo | ((invoc_hi & 0x7) << 16)

    entry_name = ENTRY_TYPES.get(path, f"Unknown({path})")

    if path == 6:  # Syscall
        return (f"KERNEL_ENTRY: {entry_name} syscall={syscall_no} "
                f"cap_type={cap_type} fastpath={is_fastpath} tag={invocation_tag}")
    else:
        return f"KERNEL_ENTRY: {entry_name}"

def decode_interrupt(entries, idx):
    """Decode INTERRUPT marker"""
    irq = entries[idx + 1]
    return f"INTERRUPT: IRQ {irq}"

def decode_kernel_exit(entries, idx):
    """Decode KERNEL_EXIT marker"""
    duration_lo = entries[idx + 1]
    duration_hi = entries[idx + 2]
    duration = duration_lo | (duration_hi << 16)
    return f"KERNEL_EXIT: {duration} cycles"
```

### 2.2 Marker Dispatch Table

Update the marker dispatch in the decoder:

```python
MARKER_HANDLERS = {
    0xFFFE: (4, decode_safe_pte),
    0xFFFC: (2, decode_syscall),
    0xFFFA: (4, decode_vspace),
    0xFFF8: (4, decode_thread),
    0xFFF6: (5, decode_retype),
    0xFFF4: (5, decode_init_pt),
    0xFFF2: (5, decode_create_obj),
    0xFFF0: (8, decode_pt_map),
    # New markers
    0xFFEE: (4, decode_kernel_entry),
    0xFFEC: (2, decode_interrupt),
    0xFFEA: (3, decode_kernel_exit),
}
```

---

## Phase 3: Fault Event Markers (Priority: MEDIUM)

### 3.1 VM Fault Details

Add marker for VM fault information:

```c
/* VM_FAULT marker - page fault details
 * Format: marker + fault_type + address
 *   Entry 0: 0x7FF4 << 1 | 0 = 0xFFE8 (marker)
 *   Entry 1: fault_type (read/write/exec, permission/unmapped)
 *   Entry 2: fault_addr[15:0]
 *   Entry 3: fault_addr[31:16]
 *   Entry 4: fault_addr[47:32]
 * Total: 10 bytes (5 entries)
 */
#define FTRACE_VM_FAULT_MARKER      0xFFE8  /* 0x7FF4 << 1 */
#define FTRACE_VM_FAULT_ENTRIES     5

void ftrace_vm_fault(word_t fault_type, word_t fault_addr);
```

### 3.2 User Fault Details

```c
/* USER_FAULT marker - user-level fault
 *   Entry 0: 0x7FF3 << 1 | 0 = 0xFFE6 (marker)
 *   Entry 1: fault_number
 *   Entry 2: fault_code[15:0]
 *   Entry 3: fault_code[31:16]
 * Total: 8 bytes (4 entries)
 */
#define FTRACE_USER_FAULT_MARKER    0xFFE6  /* 0x7FF3 << 1 */
#define FTRACE_USER_FAULT_ENTRIES   4

void ftrace_user_fault(word_t fault_number, word_t fault_code);
```

---

## Phase 4: Tracepoint Integration (Priority: LOW)

### 4.1 Route TRACE_POINT Events

If timing measurements are desired in ftrace, modify `kernel/include/benchmark/benchmark.h`:

```c
/* TRACEPOINT marker - named timing measurement
 *   Entry 0: 0x7FF2 << 1 | 0 = 0xFFE4 (marker)
 *   Entry 1: tracepoint_id
 *   Entry 2: duration[15:0]
 *   Entry 3: duration[31:16]
 *   Entry 4: duration[47:32]
 * Total: 10 bytes (5 entries)
 */
#define FTRACE_TRACEPOINT_MARKER    0xFFE4  /* 0x7FF2 << 1 */
#define FTRACE_TRACEPOINT_ENTRIES   5

#ifdef CONFIG_KERNEL_FUNCTION_TRACE
#define TRACE_POINT_STOP(id) do { \
    word_t __duration = benchmark_tracepoint_log_stop(id); \
    ftrace_tracepoint((id), __duration); \
} while(0)
#else
/* Original implementation */
#endif
```

### 4.2 Tracepoint Name Dictionary

Add tracepoint names to binary output header:

```
=== TRACEPOINT NAMES ===
0:KS_ENTRY
1:KS_EXIT
2:FASTPATH_CALL
...
===
```

---

## Phase 5: Testing & Validation

### 5.1 Unit Tests

Add sel4test tests that:
1. Enable both CONFIG options
2. Make syscalls and verify kernel entry markers appear
3. Trigger interrupts and verify interrupt markers
4. Verify durations are reasonable (non-zero, not huge)

### 5.2 Integration Test

```bash
# Build with both tracing options
cmake -DKernelFunctionTrace=ON -DKernelBenchmarkTrackKernelEntries=ON ...

# Run sel4test, extract ftrace
./decode_ftrace_binary.py < uart_log > trace.txt

# Verify kernel events appear
grep "KERNEL_ENTRY: Syscall" trace.txt
grep "KERNEL_EXIT:" trace.txt
grep "INTERRUPT:" trace.txt
```

### 5.3 Performance Impact

Measure overhead with microbenchmark:
- Baseline: upstream tracing only
- With ftrace: both systems enabled
- Expected: <5% overhead on syscall latency

---

## Implementation Order

| Step | Description | Files Modified | Effort |
|------|-------------|----------------|--------|
| 1 | Add marker definitions | ftrace.h | Small |
| 2 | Implement ftrace_kernel_entry/exit/interrupt | ftrace.c | Medium |
| 3 | Hook benchmark_debug_syscall_start | benchmark_track.h | Small |
| 4 | Hook benchmark_track_exit | benchmark.c | Small |
| 5 | Add interrupt logging | trap handlers | Medium |
| 6 | Update decoder for new markers | decode_ftrace_binary.py | Medium |
| 7 | Add fault detail markers (Phase 3) | ftrace.c, fault handlers | Medium |
| 8 | Tracepoint routing (Phase 4) | benchmark.h | Low priority |
| 9 | Testing | sel4test | Medium |

**Total estimated effort**: 2-3 days for Phases 1-2 (core functionality)

---

## Marker Allocation Summary

Current allocation after integration:

| Marker | Value | Entries | Purpose |
|--------|-------|---------|---------|
| SAFE_PTE | 0xFFFE | 4 | pte_pte_invalid_new() PC |
| SYSCALL | 0xFFFC | 2 | Syscall number (existing) |
| VSPACE | 0xFFFA | 4 | VSpace switch |
| THREAD | 0xFFF8 | 4 | Thread switch |
| RETYPE | 0xFFF6 | 5 | Memory retype |
| INIT_PT | 0xFFF4 | 5 | PT init phase |
| CREATE_OBJ | 0xFFF2 | 5 | Arch_createObject |
| PT_MAP | 0xFFF0 | 8 | PT mapped to parent |
| **KERNEL_ENTRY** | 0xFFEE | 4 | Kernel entry (new) |
| **INTERRUPT** | 0xFFEC | 2 | IRQ number (new) |
| **KERNEL_EXIT** | 0xFFEA | 3 | Exit + duration (new) |
| **VM_FAULT** | 0xFFE8 | 5 | Fault details (new) |
| **USER_FAULT** | 0xFFE6 | 4 | User fault (new) |
| **TRACEPOINT** | 0xFFE4 | 5 | Timing point (new) |
| (reserved) | 0xFFE2-0xFF00 | - | Future use |
| VMID | 0x0000-0x01FE | 1 | VMID markers (even values) |

---

## Benefits Summary

1. **Unified trace stream**: Function calls + kernel events in one timeline
2. **Compressed storage**: 4-5x more events than raw upstream format
3. **Context correlation**: See which syscall triggered which functions
4. **Single retrieval**: One ftrace dump gets everything
5. **Debugging power**: Kernel entry context for crash analysis
6. **No breaking changes**: Upstream memory-mapped interface still works

---

## Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| Circular dependency ftrace↔benchmark | Careful header organization, forward declarations |
| Increased overhead | Make ftrace logging conditional on ftrace_enabled |
| Buffer overflow with many events | Already handled by streaming compression |
| Decoder complexity | Modular marker handlers, good test coverage |
