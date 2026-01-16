# TCU Console Sporadic Hang Investigation

**Date:** 2026-01-16
**Status:** Open - Root cause unknown

## Problem Description

The TCU (Tegra Combined UART) console sporadically fails after Linux switches from the early bootconsole (pl011) to ttyTCU0. The kernel output stops immediately after:

```
[    1.xxx] printk: console [ttyTCU0] enabled
[    1.xxx] printk: bootconsole [pl11] disabled
```

The hang occurs during `tegra-bpmp` driver probe. In successful boots, this line appears:
```
[    1.294013] tegra-bpmp bpmp: firmware: 39f77b2baaf3f0522607-da583751bbf
```

In failing boots, the kernel hangs before this line, stuck in `tegra186_bpmp_channel_reset()` which loops in `tegra_ivc_notified()` waiting for BPMP firmware to respond.

## Symptoms

- **Sporadic**: Sometimes boots successfully to login prompt, sometimes hangs
- **Consistent hang point**: Always stops at `ledtrig-cpu` message, right before tegra-bpmp probe
- **TCU depends on BPMP**: TCU uses BPMP mailbox, so if BPMP driver hangs, TCU console won't work

## Test Results

| Test ID | Elfloader BPMP | Result | Notes |
|---------|---------------|--------|-------|
| 20260115-235208 | Used (fan control) | SUCCESS | Full boot to login |
| 20260116-094152 | Used (fan control) | HANG | No tegra-bpmp line |
| 20260116-101350 | Used (fan control) | HANG | No tegra-bpmp line |
| 20260116-101827 | IVC reset to SYNC | HANG | No tegra-bpmp line |
| 20260116-102245 | IVC SYNC + doorbell | HANG | No tegra-bpmp line |
| 20260116-102644 | Full IVC handshake | HANG | "IVC re-established" but still hung |
| 20260116-103120 | Disabled (no fan) | HANG | No tegra-bpmp line |

## Hypotheses Tested

### 1. Elfloader BPMP Usage Corrupts IVC State - DISPROVEN

**Theory:** Elfloader uses BPMP IPC for fan control, modifying IVC counters. When Linux's tegra-bpmp driver probes, the corrupted state causes hangs.

**Tests:**
- Reset IVC counters to 0 after elfloader use → Still hung
- Set IVC state to SYNC and ring doorbell → Still hung
- Full IVC handshake (SYNC → ACK → ESTABLISHED) → Still hung
- Disable elfloader BPMP usage entirely → Still hung

**Conclusion:** Elfloader's BPMP usage is NOT the cause. The problem persists even when elfloader doesn't touch BPMP at all.

## Technical Background

### IVC State Machine

The IVC (Inter-VM Communication) protocol uses a state machine:
- ESTABLISHED (0): Normal operating state
- SYNC (1): Reset/synchronization request
- ACK (2): Acknowledgment of sync

State transitions (from `drivers/firmware/tegra/ivc.c`):
```
local   remote  action
SYNC    EST     <none>  ← PROBLEM: No action when remote stays ESTABLISHED
SYNC    ACK     reset counters; move to EST; notify
SYNC    SYNC    reset counters; move to ACK; notify
```

### Linux tegra-bpmp Driver Flow

1. `tegra186_bpmp_channel_reset()` calls `tegra_ivc_reset()` → sets state to SYNC
2. Loops calling `tegra_ivc_notified()` waiting for BPMP to respond
3. If BPMP (remote) stays in ESTABLISHED, no action is taken → infinite loop

### BPMP Communication Path

- HSP Top0 doorbell at 0x3c00000 (IRQ 208)
- Shared memory: TX at 0x40070000, RX at 0x40071000
- BPMP firmware runs on separate processor

## Remaining Hypotheses

1. **HSP doorbell interrupt not delivered**: The doorbell to notify BPMP might not be working in the VM
2. **BPMP firmware state**: BPMP might be in a bad state from previous boot/UEFI
3. **Timing/race condition**: Something about boot timing affects BPMP responsiveness
4. **IRQ passthrough issue**: IRQ 208 (HSP Top0 doorbell) might not be properly configured

## Files Involved

- `/home/hlyytine/tii-sel4/tools/seL4/elfloader-tool/src/plat/orinagx/fan.c` - Elfloader BPMP usage
- `/home/hlyytine/source/kernel/kernel-jammy-src/drivers/firmware/tegra/bpmp-tegra186.c` - Linux BPMP driver
- `/home/hlyytine/source/kernel/kernel-jammy-src/drivers/firmware/tegra/ivc.c` - IVC protocol
- `/home/hlyytine/tii-sel4/projects/tii-sel4-vm/apps/Arm/vm_qemu_virtio/orinagx/devices.camkes` - VM config

## Next Steps

1. Verify HSP doorbell IRQ (208) is properly passed through and delivered
2. Add debug prints to Linux tegra-bpmp driver to see exact hang point
3. Check if BPMP firmware responds to doorbell at all (add polling check)
4. Compare successful vs failed boot UART logs more carefully for timing differences
