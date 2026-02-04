# Driver VM Notification Callback Bug

## Summary

The Driver VM's RPC notification callback is never invoked because it uses CAmkES's `reg_callback` mechanism, which does not integrate with the VMM's event loop that uses raw `seL4_Recv()`.

## Symptoms

- Guest VM stops receiving interrupts
- Guest VCPU is in `Restart` state but not in the ready queue
- VMM blocks on `seL4_Recv()` indefinitely
- vGIC maintenance interrupts (IRQ 26) fire repeatedly but nobody handles them
- System goes to idle with `bitmap_empty=true`

## Root Cause

### Architecture Background

In vm_qemu_virtio:
- **VM0 (Device VM)**: Runs Linux + QEMU, provides virtio backends
- **VM1 (Driver VM)**: Uses virtio devices, sends MMIO requests via RPC

When the Driver VM's guest faults on virtio MMIO:
1. VMM receives fault via `seL4_Recv()`
2. `mmio_fault_handler()` queues RPC request to Device VM
3. Returns `FAULT_HANDLED` without sending reply (async)
4. VMM loops back to `seL4_Recv()` (blocks)
5. Guest VCPU is in `Restart` state waiting for reply
6. Device VM processes request, sends response, emits notification
7. **Notification arrives but callback is never invoked**
8. Guest VCPU is never resumed

### The Bug

There's a mismatch between how the Device VM and Driver VM handle notifications:

**Device VM (seL4VirtIODeviceVM.template.c)** - Correct:
```c
// Gets badge and registers with VMM's event handler system
connections[...].consume_badge = vm*_ntfn_recv_notification_badge();
err = register_async_event_handler(badge, consume_callback, &connections[...]);
```

**Driver VM (seL4VirtIODriverVM.template.c)** - Bug:
```c
// Uses CAmkES reg_callback mechanism
int err = vm*_ntfn_recv_reg_callback(vm*_ntfn_callback, opaque);
```

### Why CAmkES reg_callback Doesn't Work

1. CAmkES's `reg_callback` mechanism registers callbacks with the CAmkES runtime
2. These callbacks are dispatched when the component calls CAmkES's `seL4_Wait` wrapper
3. The VMM's main loop (`vm_run_arch()`) uses raw `seL4_Recv()`:
   ```c
   // In libsel4vm/src/arch/arm/vm.c
   while (ret > 0) {
       tag = seL4_Recv(vm->host_endpoint, &sender_badge);
       // ...
       if (sender_badge >= MIN_VCPU_BADGE && sender_badge <= MAX_VCPU_BADGE) {
           // Handle VCPU faults
       } else {
           // Handle notifications via handle_async_event()
           vm->run.notification_callback(vm, sender_badge, tag, ...);
       }
   }
   ```
4. `handle_async_event()` only dispatches callbacks registered via `register_async_event_handler()`
5. CAmkES `reg_callback` callbacks are **never invoked**

### Notification Flow Comparison

**Device VM (works):**
```
Notification arrives
    ↓
seL4_Recv() returns with badge
    ↓
handle_async_event() called
    ↓
Finds matching register_async_event_handler() callback
    ↓
consume_callback() invoked
    ↓
Event processed ✓
```

**Driver VM (broken):**
```
Notification arrives
    ↓
seL4_Recv() returns with badge
    ↓
handle_async_event() called
    ↓
No matching register_async_event_handler() callback found!
    ↓
"Unknown badge" warning logged
    ↓
rpc_run() never called
    ↓
Guest VCPU never resumed ✗
```

## Ftrace Evidence

The scheduler ftrace instrumentation captured this pattern:

```
5: KEXIT 0 cycles           # Guest enters
5: SCHEDULE                 # Immediate trap (async event)
5: THREAD -> 0x8115cc3c00   # VMM chosen (higher priority)
5: VCPU_SWITCH active -> NULL
5: VCPU_DISABLE 0x80e303b000
2: KEXIT 0 cycles           # VMM runs
2: KENTRY Syscall Recv      # VMM blocks on seL4_Recv()
2: SCHEDULE
2: THREAD_STATE tcb=IDLE    # Going to idle!
0: SCHED_DECISION chosen=IDLE ... bitmap_empty=true
0: IRQ 26                   # vGIC maintenance fires
0: SCHEDULE                 # But nobody wakes up!
0: SCHED_DECISION chosen=IDLE ... bitmap_empty=true
# ... infinite idle loop ...
```

Key observations:
- VMM goes directly from KEXIT to Syscall Recv (no seL4_Reply in between)
- Guest VCPU (state=Restart) is NOT in ready queue
- System goes idle even though guest should be runnable

## Fix

Change the Driver VM template to use `register_async_event_handler()` like the Device VM:

**File:** `templates/seL4VirtIODriverVM.template.c`

```c
// Add badge getter declaration
extern seL4_Word vm/*? dev.id ?*/_ntfn_recv_notification_badge(void);

// Change callback signature to match async_event_handler_fn_t
static int vm/*? dev.id ?*/_ntfn_handler(vm_t *vm, void *cookie)
{
    io_proxy_t *io_proxy = cookie;

    int err = rpc_run(io_proxy);
    if (err) {
        ZF_LOGF("rpc_run() failed, guest corrupt");
        /* no return */
    }
    return 0;
}

// In module init function, register with VMM event system:
static void vm/*? dev.id ?*/_io_proxy_register_handler(vm_t *vm, void *cookie)
{
    io_proxy_t *io_proxy = cookie;
    seL4_Word badge = vm/*? dev.id ?*/_ntfn_recv_notification_badge();

    int err = register_async_event_handler(badge, vm/*? dev.id ?*/_ntfn_handler, io_proxy);
    ZF_LOGF_IF(err, "Failed to register RPC notification handler");
}

// Remove the old reg_callback mechanism
// - Remove vm*_ntfn_recv_reg_callback() calls
// - Remove vm*_io_proxy_run() that triggered the callback chain
```

## Verification

After fix, the ftrace should show:
1. Guest VCPU faults on MMIO
2. VMM queues RPC request, returns to seL4_Recv()
3. Device VM responds, notification arrives
4. VMM receives notification, handle_async_event() called
5. **rpc_run() processes response**
6. **ioack_vcpu_*() calls advance_vcpu_fault()**
7. **seL4_Send(reply_cap) resumes guest**
8. Guest VCPU runs normally

## Related Files

| File | Role |
|------|------|
| `templates/seL4VirtIODriverVM.template.c` | Driver VM notification handling (BUG) |
| `templates/seL4VirtIODeviceVM.template.c` | Device VM notification handling (correct pattern) |
| `projects/sel4_projects_libs/libsel4vm/src/arch/arm/vm.c` | VMM event loop |
| `projects/vm/components/VM_Arm/src/main.c` | `handle_async_event()` and `register_async_event_handler()` |
| `src/libsel4vm_glue.c` | `rpc_run()` and MMIO fault handling |
| `src/io_proxy.c` | `ioreq_start()` and `ioreq_finish()` |

## Discovery Method

This bug was found using scheduler ftrace instrumentation added to debug why guest VMs stop receiving interrupts. The `SCHED_DECISION` and `THREAD_STATE` markers revealed that the guest VCPU was in `Restart` state but not in the ready queue, while the VMM was blocking on `seL4_Recv()`.

See: `kernel/docs/scheduler-ftrace.md`
