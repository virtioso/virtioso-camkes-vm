# Orin AGX CAmkES VM Porting Guide

This document captures learnings from porting CAmkES VM applications (vm_minimal, vm_qemu_virtio) to NVIDIA Orin AGX (Tegra234).

## Status Summary

| Component | Status | Notes |
|-----------|--------|-------|
| VM boot to init | ✅ Working | VM boots to userspace successfully |
| Kernel boot (earlycon) | ✅ Working | PL011 UART at 0x31d0000 |
| vGIC | ✅ Working | GICD/GICR interception fixed |
| HSP AON | ✅ Working | Shared mailbox for TCU |
| APB MISC | ✅ Working | For tegra_is_silicon() |
| BPMP | ❌ Not Available | Requires proxy - see below |
| TCU console | ❌ Not Available | Depends on BPMP |

## BPMP Virtualization (CRITICAL)

**BPMP (Boot and Power Management Processor) CANNOT be directly passed through to guest VMs.**

### Why BPMP Doesn't Work

BPMP firmware uses an IVC (Inter-VM Communication) protocol over shared SRAM with channel configuration established by UEFI at boot time. When the guest VM tries to communicate:

1. Guest writes request to SRAM (0x40070000 TX, 0x40071000 RX)
2. Guest rings doorbell via HSP Top0 (master 19)
3. **BPMP firmware never responds** - the IVC channels were set up for the host OS, not guest VMs

The guest hangs in `tegra_bpmp_ping()` with a 60-second timeout.

### Solution: BPMP Guest Proxy

The L4T kernel has `CONFIG_TEGRA_BPMP_GUEST_PROXY` (in `drivers/firmware/tegra/bpmp-guest-proxy/`). This redirects BPMP transfers through a shared memory region with a hypervisor proxy.

**To use BPMP in guests:**
1. Build guest kernel with `CONFIG_TEGRA_BPMP_GUEST_PROXY=y`
2. Add `virtual-pa = <0xADDRESS>` property to BPMP DT node
3. Implement hypervisor-side proxy that forwards requests to real BPMP

**For CAmkES/seL4**, this would require:
- A CAmkES component that traps accesses to the proxy region
- Forwarding mechanism to communicate with BPMP on host side
- This is complex and NOT currently implemented

### Current Workaround

**Remove BPMP passthrough entirely.** The VM boots without clock/reset/power control:

```c
/* devices.camkes - NO BPMP nodes */
vm0.dtb = dtb([
    {"path": "/bus@0/misc@100000"},   /* APB MISC */
    {"path": "/bus@0/hsp@c150000"},   /* HSP AON */
    {"path": "/bus@0/serial@31d0000"} /* UARTI */
]);

vm0.dtb_irqs = [
    165,    /* HSP AON shared1 (SPI 133) */
    178,    /* UARTI (SPI 146) */
];
```

**Implications:**
- No dynamic clock/reset control (devices must use static boot config)
- No power domain management
- TCU console unavailable (use UARTI instead)
- Guest kernel logs HSP Top0 probe failure (harmless)

## Key Fixes Made

### 1. HSP mboxes Type Bug (CRITICAL)

**Problem**: BPMP driver failed with "failed to get HSP mailbox: -19" (ENODEV).

**Root Cause**: The seL4 kernel's `orinagx.dts` had incorrect mboxes type value:
```dts
// WRONG - type 2 means "shared semaphore"
mboxes = <&hsp_top0 0x2 0x13>;

// CORRECT - type 0 means "doorbell"
mboxes = <&hsp_top0 0x0 0x13>;
```

**File**: `kernel/tools/dts/orinagx.dts` line 241

**HSP Mailbox Types**:
- Type 0: Doorbell (DB) - for simple notifications
- Type 1: Shared Mailbox (SM) - for data transfer with interrupts
- Type 2: Shared Semaphore (SS) - for synchronization

BPMP uses doorbell (type 0) with master ID 19 (0x13).

### 2. APB MISC Passthrough

**Problem**: After fixing mboxes, BPMP driver hit warning:
```
Tegra APB MISC not yet available
WARNING: CPU: 0 PID: 1 at tegra-apbmisc.c:35 tegra_is_silicon+0x78/0x84
```

**Solution**: Add APB MISC node to seL4 kernel DTS and pass through in CAmkES:

**`kernel/tools/dts/orinagx.dts`**:
```dts
misc@100000 {
    compatible = "nvidia,tegra234-misc";
    reg = <0x0 0x00100000 0x0 0xf000>,    /* MISC base */
          <0x0 0x0010f000 0x0 0x1000>;    /* STRAPPING_OPT_A */
    status = "okay";
};
```

**`devices.camkes`**:
```c
vm0.dtb = dtb([
    {"path": "/bus@0/misc@100000"},   /* APB MISC */
    // ... other devices
]);
```

### 3. vGIC GICD/GICR Interception

The VM needs vGIC to properly virtualize interrupt handling. GICv3 vGIC is installed at:
- GICD: 0x0f400000 (size 0x10000)
- GICR: 0x0f440000 (size 0x200000)

This is logged during boot:
```
vm_install_vgic@vgic_v3.c:623 Installing GICv3 vGIC (GICD=0xf400000, GICR=0xf440000)
```

## Device Passthrough Configuration

### devices.camkes Structure (Current Working Config)

```c
/* Minimal config - boots to init without BPMP */
vm0.dtb = dtb([
    {"path": "/bus@0/misc@100000"},   /* APB MISC - tegra_is_silicon() */
    {"path": "/bus@0/hsp@c150000"},   /* HSP AON - for TCU mailbox */
    {"path": "/bus@0/serial@31d0000"} /* UARTI - SBSA UART (console) */
]);
```

### IRQ Passthrough

```c
vm0.dtb_irqs = [
    165,    /* HSP AON shared1 (SPI 133) - TCU */
    178,    /* UARTI (SPI 146) - console */
];
```

Note: GIC INTID = SPI number + 32

### Boot Command Line (Working)

```
console=ttyAMA0 earlycon=pl011,mmio32,0x31d0000 keep_bootcon debug rdinit=/init arm64.nopauth maxcpus=1
```

Key parameters:
- `console=ttyAMA0`: Use PL011 UART (not TCU since BPMP unavailable)
- `keep_bootcon`: Keep earlycon active after main console starts (prevents output gap)
- `maxcpus=1`: Single CPU for simplicity

### Device Memory Regions

The CAmkES VM framework uses on-demand device mapping. When guest accesses a device address, the VMM creates device-backed memory:

```
OnDemandInstall: Created device-backed memory for addr 0x100000   # APB MISC
OnDemandInstall: Created device-backed memory for addr 0x10f000   # APB MISC strapping
OnDemandInstall: Created device-backed memory for addr 0x3c00000  # HSP Top0
OnDemandInstall: Created device-backed memory for addr 0xc150000  # HSP AON
OnDemandInstall: Created device-backed memory for addr 0x40070000 # SRAM
```

## Console Configuration

### Working Console (Without BPMP)

Since BPMP is not available, we use PL011/SBSA UART as the only console:

```
console=ttyAMA0 earlycon=pl011,mmio32,0x31d0000 keep_bootcon
```

**Important: `keep_bootcon`** - Without this parameter, there's a gap in console output when the kernel transitions from earlycon to the main console driver. The `keep_bootcon` parameter keeps the boot console active, ensuring continuous output.

### TCU Console (Requires BPMP Proxy)

TCU (Tegra Combined UART) provides a combined console for all cores but requires BPMP for initialization. When BPMP proxy is implemented:

```
console=ttyTCU0 earlycon=pl011,mmio32,0x31d0000 keep_bootcon
```

## DTB Generation vs Provided DTB

CAmkES supports two modes:

1. **Generated DTB** (`generate_dtb: true`, `provide_dtb: false`):
   - fdtgen creates minimal DTB from kernel DTB
   - Only keeps nodes listed in `vm0.dtb`
   - Works well when passthrough is straightforward

2. **Provided DTB** (`generate_dtb: false`, `provide_dtb: true`):
   - Uses pre-built DTB file
   - More control but requires separate DTB maintenance

Currently using generated DTB mode.

## Known Issues

### fdtgen Phandle Handling

fdtgen (libfdtgen) generates new phandles during DTB creation. Properties with phandle references need special handling. Added support for:
- `mboxes` property (uses `#mbox-cells` to determine cell count)
- `shmem` property

See `projects/projects_libs/libfdtgen/fdtgen.c`:
```c
static const char *props_with_dep[] = {
    "phy-handle", "next-level-cache", "interrupt-parent",
    "interrupts-extended", "clocks", "power-domains",
    "mboxes", "shmem"
};
```

### BPMP IPC Status (RESOLVED - Needs Proxy)

BPMP cannot be passed through directly. See "BPMP Virtualization" section above.

**Root cause**: BPMP firmware IVC channels are configured at boot for the host OS. Guest VMs cannot communicate with BPMP without a hypervisor proxy (`CONFIG_TEGRA_BPMP_GUEST_PROXY`).

**Current solution**: Remove BPMP from guest DT. VM boots to init without clock/reset/power control.

## Debugging Tips

### Enable Debug Output

Add to boot cmdline: `debug loglevel=8`

### Add Kernel Driver Debug

For HSP driver, modify `tegra-hsp.c`:
```c
#define DEBUG
```

For BPMP driver, modify `bpmp-tegra186.c`:
```c
// Add before mbox_request_channel():
const void *prop = of_get_property(np, "mboxes", &len);
dev_info(dev, "mboxes raw len=%d bytes:\n", len);
```

### Check Device Tree

From guest Linux:
```bash
ls /proc/device-tree/bus@0/
cat /proc/device-tree/bpmp/mboxes | xxd
```

### Verify IRQ Routing

Check sel4.log for:
- `Undelivered IRQ: N` - IRQ N needs passthrough
- `OnDemandInstall: Created device-backed memory` - device mapping

## References

- [Tegra234 TRM](https://developer.nvidia.com/embedded/downloads) - Technical Reference Manual
- [seL4 CAmkES VM](https://docs.sel4.systems/projects/camkes-vm/) - VM framework documentation
- NVIDIA HSP driver: `drivers/mailbox/tegra-hsp.c`
- NVIDIA BPMP driver: `drivers/firmware/tegra/bpmp-tegra186.c`
