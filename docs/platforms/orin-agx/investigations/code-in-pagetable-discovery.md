# Code-in-PageTable Discovery

**Date**: 2025-12-22
**Status**: Active investigation
**Related**: [Boot Section Corruption](boot-section-corruption-investigation.md)

## Summary

Page table memory contains **executable code bytes** from sel4test-driver, proving that memory is being recycled as page tables without being properly cleared.

## Discovery

When adding extra TLBI before VTTBR switch (debugging speculative PTW), massive PT corruption was exposed:

```
PT_CORRUPT: pt[31] paddr=0xac1d2000 entry[0] = 0x540001ac7100141f
```

The corrupt value `0x540001ac7100141f` is **NOT random garbage** - it's two ARM64 instructions:

| Bytes (LE) | Instruction | Location |
|------------|-------------|----------|
| `0x7100141f` | `cmp w0, #0x5` | Lower 32 bits |
| `0x540001ac` | `b.gt 0x34` | Upper 32 bits |

## Exact Match in sel4test-driver

Found exact instruction sequence at multiple locations in sel4test-driver:

```
0x401ed0: 7100141f  cmp  w0, #0x5
0x401ed4: 540001ac  b.gt 401f08 <simple_init_cap+0xb4>

0x401fe8: 7100141f  cmp  w0, #0x5
0x401fec: 540001ac  b.gt 402020 <simple_get_untyped_count+0xb0>

0x4020cc: 7100141f  cmp  w0, #0x5
0x4020d0: 540001ac  b.gt 402104 <simple_get_nth_untyped+0xc0>
```

These are in functions:
- `simple_init_cap`
- `simple_get_untyped_count`
- `simple_get_nth_untyped`

## Implications

1. **Memory recycling bug**: Physical memory previously used for sel4test-driver code is being reused as page tables WITHOUT being zeroed first.

2. **Same root cause as boot section**: This is the same class of bug as the boot section corruption - memory recycling without clearing.

3. **Why extra TLBI exposed this**: The additional TLBI we added changed timing/ordering, causing the scanner to catch the corruption that was previously being overwritten before detection.

4. **The corruption was always there**: We just weren't detecting it because:
   - Previous code paths wrote valid PTEs over the stale code before scanning
   - The extra TLBI created a window where stale data was visible

## Memory Layout

- **Corrupt PT**: PA 0xac1d2000
- **sel4test-driver code segment**: VA 0x400000-0x6e82e8 (loaded at runtime)
- **sel4test-driver BSS**: VA 0x6f0000+

The physical address 0xac1d2000 was at some point mapped to hold sel4test-driver code (around VA 0x401ed0), then that mapping was torn down and the physical memory recycled as a page table.

## Code Path Analysis

The functions containing the matching code are part of the VKA (Virtual Kernel Allocator) and simple pool interfaces:

```c
// In libsel4simple-default or similar
simple_init_cap()        // Initializes capability space
simple_get_untyped_count() // Gets count of untyped caps
simple_get_nth_untyped()   // Gets nth untyped cap
```

These are called during test setup/teardown, suggesting the corruption happens when:
1. Test allocates memory for code/data
2. Test completes and memory is freed
3. Memory is retyped to PageTable
4. **BUG**: Memory not cleared before retype
5. Page table contains stale code bytes

## Verification Commands

```bash
# Find instruction in sel4test-driver
aarch64-linux-gnu-objdump -d sel4test-driver | grep -B1 "540001ac"

# Decode the corrupt value
printf '\x1f\x14\x00\x71\xac\x01\x00\x54' > /tmp/i.bin
aarch64-linux-gnu-objdump -D -b binary -m aarch64 /tmp/i.bin
```

## Relationship to Other Bugs

| Bug | Memory Source | Status |
|-----|---------------|--------|
| Boot section corruption | Kernel .boot section recycled | FIXED (skip recycling) |
| Code-in-PT corruption | User code memory recycled | **NEW - needs fix** |
| Speculative PTW RAS | Stale PTEs during VTTBR switch | In progress |

## Fix Required

The `Arch_createObject()` or memory retype path must ensure physical memory is **zeroed before being used as a page table**, regardless of its previous use.

Current safe PTE initialization may be happening but:
1. Not covering all entry points, OR
2. Race condition where old data is visible before init completes, OR
3. Cache coherency issue where stale data from previous use persists

## Next Steps

1. Audit all paths where memory becomes a PageTable
2. Check if `memzero`/`clearMemory` is called before PT init
3. Verify cache is flushed after clearing and before PT use
4. Consider if the extra TLBI is actually correct and we need to fix the underlying memory clearing bug
