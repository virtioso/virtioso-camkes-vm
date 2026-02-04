# Stage-2 Device Memory Cacheability Bug

## Summary

Device passthrough memory in seL4 VMs is incorrectly mapped with cacheable stage-2 attributes (S2_NORMAL), causing cache coherency issues when the guest expects non-cacheable device memory. This affects BPMP communication on Orin AGX and potentially any device that requires non-cacheable memory semantics.

## Affected Platform

- NVIDIA Orin AGX (Tegra234)
- Potentially any ARM64 platform with device passthrough requiring non-cacheable memory

## Symptom

BPMP (Boot and Power Management Processor) IVC channel reset succeeds via SRAM polling, but subsequent MRQ (Message Request) commands fail with no response from BPMP. The guest Linux kernel times out waiting for the doorbell interrupt.

## Root Cause Analysis

### 1. Guest Expects Non-Cacheable SRAM

The SRAM device tree node has `no-memory-wc` property:

```dts
sram@40000000 {
    compatible = "nvidia,tegra234-sysram", "mmio-sram";
    reg = <0x0 0x40000000 0x0 0x80000>;
    no-memory-wc;  /* Forces non-cacheable mapping */
    ...
};
```

In `drivers/misc/sram.c:392-400`:
```c
sram->no_memory_wc = of_property_read_bool(pdev->dev.of_node, "no-memory-wc");
if (sram->no_memory_wc)
    sram->virt_base = devm_ioremap_resource(&pdev->dev, res);  // Device-nGnRnE
else
    sram->virt_base = devm_ioremap_resource_wc(&pdev->dev, res);  // Write-Combine
```

This causes guest Linux to map SRAM as **Device-nGnRnE** in stage-1 page tables.

### 2. VMM Maps All Memory as Cacheable

In `libsel4vm/src/guest_memory.c:351`:
```c
reservation_t vspace_reservation = vspace_reserve_deferred_rights_range_at(
    &vm->mem.vm_vspace, (void *)addr, size, 1);  // cacheable=1 ALWAYS
```

This flows through to `kernel/src/arch/arm/64/kernel/vspace.c:778`:
```c
word_t attridx = cacheable ? S2_NORMAL : S2_DEVICE_nGnRnE;
```

So stage-2 uses **S2_NORMAL** (Write-Back Cacheable) for all device passthrough.

### 3. Combined Attribute is Implementation-Defined

According to ARM Architecture Reference Manual (D5.5.4):

> When one stage specifies Device and the other specifies Normal:
> - If the stage 2 output is Device, the combined type is Device
> - If the stage 1 output is Device and the stage 2 output is Normal,
>   the combined type is **IMPLEMENTATION DEFINED**

This means with S1=Device-nGnRnE and S2=Normal-WB:
- Some implementations may use Device (safe)
- Some implementations may use Normal-WB (unsafe - enables caching)
- Behavior is unpredictable

### 4. IVC Relies on Non-Cacheable Semantics

In `drivers/firmware/tegra/bpmp-tegra186.c:129`:
```c
err = tegra_ivc_init(channel->ivc, NULL,  // peer = NULL!
                     priv->rx.virt + offset, priv->rx.phys + offset,
                     priv->tx.virt + offset, priv->tx.phys + offset, ...);
```

When `peer = NULL`, cache flush is skipped in `tegra_ivc_flush()`:
```c
static inline void tegra_ivc_flush(struct tegra_ivc *ivc, dma_addr_t phys)
{
    if (!ivc->peer)
        return;  // NO CACHE FLUSH!
    dma_sync_single_for_device(ivc->peer, phys, TEGRA_IVC_ALIGN, DMA_TO_DEVICE);
}
```

This works on native Linux because SRAM is truly non-cacheable (no stage-2).

## Why Channel Reset Works But MRQ Doesn't

| Operation | Mechanism | Why It Works/Fails |
|-----------|-----------|-------------------|
| Channel Reset | Polls SRAM for SYNC→ACK | Eventually cache evicts, BPMP sees data |
| MRQ Command | Waits for doorbell IRQ | No polling, data stays in cache, BPMP never responds |

The 60-second timeout in the BPMP driver explains why we don't see timeout errors - the autopilot stops capturing after 30 seconds of quiescence.

## Evidence

### Elfloader Fan Control Works

In `elfloader-tool/src/plat/orinagx/fan.c`, BPMP communication works with explicit cache flushes:
```c
static inline void cache_clean(volatile void *addr, size_t size)
{
    for (; p < end; p += 64) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
```

This proves:
1. BPMP hardware works correctly
2. IVC protocol is correct
3. The issue is cache coherency in the VM

### Test Logs Show 3 Doorbell IRQs Only

From `sel4.log`:
- Physical IRQ 208 (HSP doorbell) only fires 3 times during early boot
- All during channel reset phase
- No doorbell IRQs after channel becomes ESTABLISHED

## Stage-2 Memory Attribute Definitions

From `kernel/src/arch/arm/64/kernel/vspace.c:49-69`:

```c
enum mair_s2_types {
    S2_DEVICE_nGnRnE = 0,  // Non-Gathering, Non-Reordering, Non-Early Write Ack
    S2_DEVICE_nGnRE = 1,
    S2_DEVICE_nGRE  = 2,
    S2_DEVICE_GRE = 3,

    S2_NORMAL_INNER_NC_OUTER_NC = 5,
    S2_NORMAL_INNER_WTC_OUTER_NC = 6,
    S2_NORMAL_INNER_WBC_OUTER_NC = 7,
    // ...
    S2_NORMAL = S2_NORMAL_INNER_WBC_OUTER_WBC  // = 15
};
```

For device memory, we need `S2_DEVICE_nGnRnE` (0), not `S2_NORMAL` (15).

## Fix Requirements

1. Device passthrough memory must be mapped with `cacheable=0` in stage-2
2. RAM must continue to use `cacheable=1` for performance
3. The VMM must distinguish between device and RAM frames

## Affected Code Paths

| File | Line | Issue |
|------|------|-------|
| `libsel4vm/src/guest_memory.c` | 351 | `vm_reserve_memory_at()` always uses cacheable=1 |
| `libsel4vm/src/guest_memory.c` | 381 | `vm_memory_make_anon()` always uses cacheable=1 |
| `VM_Arm/src/main.c` | 1143 | `unhandled_mem_fault_callback()` creates cacheable reservation |

## Related Issues

- Native Linux works because no stage-2 translation exists
- Other platforms with device passthrough may have similar issues
- Any device requiring strict memory ordering could be affected

## Test Case

To reproduce:
1. Build vm_minimal for Orin AGX
2. Boot guest Linux
3. Observe BPMP probe stuck at "tegra-bpmp firmware:bpmp: connecting to BPMP..."
4. Guest waits 60 seconds for MRQ timeout

## References

- ARM Architecture Reference Manual, D5.5.4 "Combining stage 1 and stage 2 memory type and cacheability attributes"
- Linux `drivers/misc/sram.c` - SRAM driver handling no-memory-wc
- Linux `drivers/firmware/tegra/ivc.c` - IVC cache flush implementation
- Linux `drivers/firmware/tegra/bpmp-tegra186.c` - BPMP driver IVC initialization
