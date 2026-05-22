# Guest VCPU Not Resumed Investigation (formerly "Guest IRQ Disappearance")

**Status:** FIXED
**Started:** 2026-02-02
**Fixed In:** sel4_projects_lib commit `e1d6ab168068dafbc0a0b53916d0b674992b7a08`
**Last Updated:** 2026-02-04

## Problem Statement

The observed issue was **not** that guest IRQs disappeared. The real failure was that the VMM did not resume guest execution after handling a trap. The guest VCPU would enter and immediately trap, the VMM would block on `seL4_Recv()`, and the guest would never be resumed. The system went idle with vGIC maintenance interrupts (IRQ 26) firing repeatedly but unhandled.

## Resolution

The root cause was fixed in `sel4_projects_lib` commit `e1d6ab168068dafbc0a0b53916d0b674992b7a08`. After this change, the VMM correctly resumes guest execution and the apparent "IRQ disappearance" no longer occurs.

## Key Observations

| Observation | Implication |
|-------------|-------------|
| vm_minimal works | Base VMM code is correct |
| vm_qemu_virtio broken | Something in virtioso-camkes-vm causes the issue |
| vm_qemu_virtio with VM1 removed still broken | NOT an RPC issue between VM0 and VM1 |
| Guest VCPU in Restart state but not in ready queue | Reply never sent to resume guest |
| VMM blocks on seL4_Recv() without calling seL4_Reply() | Fault handling path broken somewhere |

## Ftrace Evidence

Scheduler instrumentation captured this pattern:

```
5: KEXIT 0 cycles           # Guest enters
5: SCHEDULE                 # Immediate trap (no KENTRY = async event)
5: THREAD -> 0x8115cc3c00   # VMM chosen (higher priority)
5: VCPU_SWITCH active -> NULL
5: VCPU_DISABLE 0x80e303b000
2: KEXIT 0 cycles           # VMM runs
2: KENTRY Syscall Recv      # VMM blocks - NO seL4_Reply before this!
2: SCHEDULE
2: THREAD_STATE tcb=IDLE    # Going to idle
0: SCHED_DECISION chosen=IDLE ... bitmap_empty=true
0: IRQ 26                   # vGIC maintenance fires repeatedly
0: SCHED_DECISION chosen=IDLE ... bitmap_empty=true
# ... infinite idle loop ...
```

**Critical insight:** VMM goes from KEXIT directly to Syscall Recv without any seL4_Reply kernel entry in between. This means the guest VCPU fault was either:
1. Not received as a fault (wrong badge?)
2. Handled by a path that doesn't send reply
3. Dropped somehow

## Hypotheses

### Hypothesis 1: Driver VM RPC Notification Callback Bug ❌ DISPROVEN

**Theory:** Driver VM uses CAmkES `reg_callback` which doesn't integrate with VMM's `seL4_Recv()` loop.

**Status:** DISPROVEN - Bug occurs even with VM1 completely removed from CAmkES file.

**Documentation:** [driver-vm-notification-bug.md](driver-vm-notification-bug.md) (kept for reference but not the root cause)

---

### Hypothesis 2: virtioso-camkes-vm Module Interference ⚠️ INVESTIGATING

**Theory:** A module added by virtioso-camkes-vm interferes with VM0's fault handling or notification processing.

**Status:** Under investigation

**Differences between vm_minimal and vm_qemu_virtio (VM0 only):**

| Component | vm_minimal | vm_qemu_virtio |
|-----------|------------|----------------|
| Base VMM | projects/vm/ | projects/vm/ + virtioso-camkes-vm |
| Modules | Standard | + hyp_ftrace, guest_config, trace, fdt_plat_customize |
| Fault handlers | Standard | + hyp_ftrace_fault_handler, potentially others |
| Notification handlers | Standard | + potentially additional handlers |

**virtioso-camkes-vm modules to investigate:**

1. **hyp_ftrace** (`src/camkes/modules/hyp_ftrace.c`)
   - Registers fault handler for memory region
   - Registers vGIC IRQ injection callback
   - Has timer notification handling
   - Calls `advance_vcpu_fault()` - looks correct

2. **guest_config** (`src/camkes/modules/guest_config.c`)
   - Configuration module
   - Unlikely to affect fault handling

3. **trace** (`src/camkes/modules/trace.c`)
   - Tracing infrastructure
   - Need to investigate

4. **fdt_plat_customize** (`src/camkes/modules/fdt_plat_customize.c`)
   - Device tree customization
   - Unlikely to affect fault handling

5. **io_proxy** (`src/camkes/modules/io_proxy.c`)
   - Even without VM1, may be initialized
   - Could register fault handlers that don't resume guest

**Next steps:**
- [ ] Check if io_proxy module is compiled in even without VM1
- [ ] Check what notification handlers are registered by virtioso-camkes-vm
- [ ] Compare `handle_async_event()` callback registration between vm_minimal and vm_qemu_virtio
- [ ] Add logging to identify which code path handles the fault

---

### Hypothesis 3: Notification Badge Mismatch ⚠️ TO INVESTIGATE

**Theory:** A notification arrives with a badge that doesn't match any registered handler, causing `handle_async_event()` to log "Unknown badge" and return without processing.

**Status:** To investigate

**Evidence needed:**
- Check for "Unknown badge" log messages in test output
- Add logging to `handle_async_event()` to see all badges received

---

### Hypothesis 4: VCPU Fault Not Delivered as Fault ⚠️ TO INVESTIGATE

**Theory:** The VCPU fault message is delivered with a badge outside the VCPU badge range, causing it to be treated as a notification instead of a fault.

**Status:** To investigate

**In vm_run_arch():**
```c
if (sender_badge >= MIN_VCPU_BADGE && sender_badge <= MAX_VCPU_BADGE) {
    // Handle as VCPU fault - calls handler which sends seL4_Reply()
} else {
    // Handle as notification - NO reply sent
    vm->run.notification_callback(vm, sender_badge, tag, ...);
}
```

If the badge is somehow wrong, the fault would be treated as a notification and no reply would be sent.

**Next steps:**
- [ ] Add logging to see what badge is received when guest traps
- [ ] Verify VCPU badge configuration

---

## Related Documentation

| Document | Description |
|----------|-------------|
| ftrace.md | Kernel ftrace tooling used to capture the bug pattern |
| [driver-vm-notification-bug.md](driver-vm-notification-bug.md) | Disproven hypothesis about RPC notifications |
| [orin-agx-debugging-guide.md](../orin-agx-debugging-guide.md) | Master debugging guide |

## Test Configurations

### Configuration A: vm_minimal (WORKS)
- Single VM
- Uses projects/vm/ only
- Standard VMM modules

### Configuration B: vm_qemu_virtio with VM0+VM1 (BROKEN)
- Two VMs with virtio RPC
- Uses projects/vm/ + virtioso-camkes-vm
- Additional modules from virtioso-camkes-vm

### Configuration C: vm_qemu_virtio with VM0 only (BROKEN)
- Single VM (VM1 removed from CAmkES)
- Uses projects/vm/ + virtioso-camkes-vm
- Still has virtioso-camkes-vm modules

**Key insight:** Configuration C is broken while Configuration A works. The difference is virtioso-camkes-vm code, NOT the presence of VM1 or RPC.

## Investigation Log

### 2026-02-02: Initial Investigation

1. Added scheduler ftrace instrumentation (SCHED_DECISION, THREAD_STATE markers)
2. Captured trace showing guest VCPU not being resumed
3. Identified that VMM blocks on seL4_Recv() without sending seL4_Reply()
4. Initial hypothesis: Driver VM RPC notification callback bug
5. User reported bug occurs even with VM1 removed → hypothesis disproven
6. New direction: investigate virtioso-camkes-vm modules that affect VM0

### 2026-02-02: Comparing vm_minimal vs vm_qemu_virtio

Key differences identified:

| Aspect | vm_minimal | vm_qemu_virtio |
|--------|------------|----------------|
| Config include | `configurations/vm.h` | `configurations/virtioso/vm.h` |
| VM macro | `VM_INIT_DEF()` | `VM_VIRTIOSO_INIT_DEF()` |
| Extra attributes | None | tracebuffer, ramoops, hyp_ftrace_timer, virtio channels |

**Virtioso modules compiled in (even with VM1 removed):**
- `trace.c` - tracebuffer/ramoops shared memory setup
- `hyp_ftrace.c` - hypervisor ftrace control, registers vGIC IRQ callback
- `guest_config.c` - guest configuration
- `fdt_plat_customize.c` - device tree customization

**hyp_ftrace module analysis:**
- Registers fault handler for MMIO region at `hyp_ftrace` address
- Calls `vgic_set_irq_inject_callback(hyp_ftrace_irq_injected)` during init
- The callback only updates a counter - shouldn't cause issues
- Fault handler calls `advance_vcpu_fault()` correctly

**Existing ftrace data issue:**
- Available ftrace captures (20260201-232751, 20260202-012838) do NOT contain SCHED_DECISION/THREAD_STATE markers
- Those markers were added during this debugging session
- Need to rebuild and run new test to capture the actual scheduler bug pattern

### Next Steps

1. **Build new kernel with scheduler instrumentation** - `el2-ras` or `el2-ftrace` mode
2. **Run vm_qemu_virtio test** and capture ftrace with SCHED_DECISION markers
3. **Analyze the trace** to find exactly when/why seL4_Reply() is not called
4. **Compare with vm_minimal** - run same scheduler instrumentation on working config
5. **Add logging to handle_async_event()** to see badge values and handler dispatch

### Questions to Answer

1. What badge value is received when the guest traps and VMM doesn't resume it?
2. Is the message being treated as a notification instead of a VCPU fault?
3. Is there a Virtioso module fault handler that's "eating" faults without resuming?
4. Does the hyp_ftrace module's vGIC callback somehow interfere with interrupt delivery?
