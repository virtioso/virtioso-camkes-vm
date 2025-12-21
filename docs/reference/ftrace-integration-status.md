# Ftrace Upstream Integration Status

**Date**: 2025-12-21
**Status**: Phase 1 Complete - Pending Decoder Performance Improvements

## Summary

Successfully integrated upstream seL4 benchmark tracing (CONFIG_BENCHMARK_TRACK_KERNEL_ENTRIES) into TII's ftrace implementation. Kernel entry/exit events are now logged to the compressed ftrace stream.

## Verified Working

From test `20251221-163258`:
- **Total entries**: 90,310,000
- **Kernel entries**: 30,834 ✓
- **Kernel exits**: 33,577 ✓
- **Interrupts**: 0 (not hooked yet)
- **Ftrace dump triggered**: Yes (BINARY TRANSFER markers appear)

## Files Modified

### Kernel
| File | Changes |
|------|---------|
| `kernel/include/benchmark/ftrace.h` | Added KERNEL_ENTRY/INTERRUPT/KERNEL_EXIT marker definitions and function declarations |
| `kernel/src/benchmark/ftrace.c` | Implemented `ftrace_kernel_entry()`, `ftrace_interrupt()`, `ftrace_kernel_exit()` |
| `kernel/include/benchmark/benchmark_track.h` | Added ftrace hook in `benchmark_debug_syscall_start()` |
| `kernel/src/benchmark/benchmark_track.c` | Added `ftrace_kernel_exit()` call in `benchmark_track_exit()` |
| `kernel/src/benchmark/benchmark.c` | Added `ftrace_dump_binary()` call in `handle_SysBenchmarkFinalizeLog()` |
| `kernel/src/arch/arm/kernel/boot.c` | Re-enabled `ftrace_start()` at boot (was disabled with `#if 0`) |

### Decoder
| File | Changes |
|------|---------|
| `kernel/tools/decode_ftrace_binary.py` | Added KERNEL_ENTRY_MARKER (0xFFEE), INTERRUPT_MARKER (0xFFEC), KERNEL_EXIT_MARKER (0xFFEA) parsing; updated FtraceEntry dataclass; added format_entry support |

### Build System
| File | Changes |
|------|---------|
| `tii_sel4_build/scripts/build_sel4.sh` | Added `BENCHMARK_TRACK` variable support |
| `tii_sel4_build/Makefile` | Added `BENCHMARK_TRACK=ON` to ftrace defconfig |

### Test Driver
| File | Changes |
|------|---------|
| `projects/sel4test/apps/sel4test-driver/src/main.c` | Added `seL4_BenchmarkFinalizeLog()` call at test completion; reduced RAS_STRESS_REPEAT to 1 |

## Marker Format

### KERNEL_ENTRY (0xFFEE) - 8 bytes (4 entries)
```
Entry 0: 0xFFEE (marker)
Entry 1: packed = (path & 0x7) | ((syscall_no & 0xF) << 3) | ((cap_type & 0x1F) << 7) | ((is_fastpath & 0x1) << 12)
Entry 2: invocation_tag[15:0]
Entry 3: invocation_tag[18:16]
```

### KERNEL_EXIT (0xFFEA) - 6 bytes (3 entries)
```
Entry 0: 0xFFEA (marker)
Entry 1: duration[15:0]
Entry 2: duration[31:16]
```

### INTERRUPT (0xFFEC) - 4 bytes (2 entries)
```
Entry 0: 0xFFEC (marker)
Entry 1: IRQ number
```

## Entry Path Types
- 0: Unknown
- 1: Interrupt
- 2: UnknownSyscall
- 3: UserFault
- 4: DebugFault
- 5: VMFault
- 6: Syscall
- 7: VCPUFault

## Next Steps (After Decoder Improvements)

1. **Verify KENTRY/KEXIT format** - Decode sample entries to confirm data is correct
2. **Hook interrupt path** - Add `ftrace_interrupt()` call in interrupt handler
3. **Hook fastpath** - Currently only slowpath is instrumented via benchmark_track
4. **Performance analysis** - Measure overhead of ftrace logging in hot paths

## Test Results Location

Latest test with kernel entry/exit logging:
```
/home/hlyytine/pkvm/autopilot/results/20251221-163258/
├── ftrace.bin      # 180MB raw binary
├── ftrace.meta     # JSON metadata
├── sel4.log        # Console output
└── uart-raw.log    # Raw UART capture
```

## Fast Indexed Format (NEW)

Converted from variable-length ftrace.bin to fixed 16-byte records for O(1) access.

**Tools:**
- `ftrace-index-rs` (Rust) - Fast converter: ftrace.bin → ftrace.idx (~3 sec)
- `ftrace_to_indexed.py` (Python) - Slower fallback (~2 min)
- `ftrace_indexed.py` - Query indexed file with instant random access

**Performance comparison (90M events):**
| Operation | Python | Rust | Speedup |
|-----------|--------|------|---------|
| Conversion + index | 1m52s + 7s | 2.1s + 0.8s | **38×** |
| Random access event N | N/A | 28ms | ∞ |
| Filter KERNEL_ENTRY | 8 sec | 35ms | 228× |

**Usage:**
```bash
# Convert with Rust (fast, recommended)
ftrace-index-rs --binary ftrace.bin --meta ftrace.meta --output ftrace.idx --build-index

# Or with Python (slower fallback)
./ftrace_to_indexed.py --binary ftrace.bin --meta ftrace.meta -o ftrace.idx
./ftrace_indexed.py ftrace.idx --build-index

# Query specific event
./ftrace_indexed.py ftrace.idx --event 50000000

# Filter by type (instant with type index)
./ftrace_indexed.py ftrace.idx --type KERNEL_ENTRY --limit 30

# Show context around event
./ftrace_indexed.py ftrace.idx --event 51517274 --context 10

# Summary
./ftrace_indexed.py ftrace.idx --summary
```

## How to Resume

Verification of KENTRY/KEXIT format complete! Example output:
```
?:[51517274] KENTRY Syscall Call cap=1 tag=42
?:[51517376] KEXIT duration=0
```

**Findings:**
- KENTRY shows: path type, syscall name, cap type, invocation tag
- KEXIT duration is 0 - timing values (ksEnter/ksExit) not being set properly
- 30,834 kernel entries logged
- 33,577 kernel exits logged

## Known Issues

1. **KEXIT duration=0**: The `ksEnter`/`ksExit` timestamps aren't being captured properly.
   Need to check if timing is enabled in the kernel config.

2. **No interrupts logged**: `ftrace_interrupt()` isn't hooked into the interrupt path yet.
