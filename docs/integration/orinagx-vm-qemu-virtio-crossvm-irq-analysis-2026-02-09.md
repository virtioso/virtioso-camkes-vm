# Orin AGX vm_qemu_virtio: Cross-VM IRQ Delivery Analysis (2026-02-09)

## Scope

This document captures the debugging session for VM1 boot hang symptoms where:

- VM1 logs are routed to `tty0` (not `vm.log`).
- VM0 receives cross-VM notification (`consume_callback`) but expected guest-side IRQ activity is missing.
- The focus is the `guest-device-1` cross-VM dataport path.

## Environment

- Platform: Orin AGX
- Target: `vm_qemu_virtio`
- Test profile: `vm-qemu-virtio`
- Key runs:
  - `20260209-174530`
  - `20260209-180310`
  - `20260209-183327` (instrumented for IRQ mapping verification)

## Observed Runtime Sequence (from `tty0`)

In successful and hang-adjacent runs, VM1 reaches backend-ready:

- `QEMU_OP_START_VM received, backend marked ready`
- `PCI backend up, continuing`

Later, VM1 sends notify to VM0:

- `vm1: ... notify: sending to device VM`
- `vm0: consume_callback ... badge=134217730 name=guest-device-1`

With added instrumentation in run `20260209-183327`, VM0 also reports:

- `consume_connection_event ... vm_inject_irq irq=432 rc=0`

So event reception and `vm_inject_irq()` succeed at VMM level.

## Instrumentation Added for Verification

### 1) Cross-VM consume path

- `projects/virtioso-camkes-vm/templates/seL4VirtIODeviceVM.template.c`
- `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c`

Added guarded prints around:

- `consume_callback()`
- badge matching
- event register increment
- `vm_inject_irq()` return code

### 2) IRQ config + DT map generation

- `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c`
- `projects/sel4_projects_libs/libsel4vmmplatsupport/src/arch/arm/devices/vpci.c`

Added guarded prints for:

- configured `connection_irq`
- PCI config `interrupt_line` byte value
- generated `interrupt-map` values used in guest DT

## Critical Finding

Run `20260209-183327` shows:

1. Cross-VM connection IRQ is **432**:

   - `crossvm pci cfg[0] ... connection_irq=432`

2. PCI config-space `interrupt_line` becomes **176**:

   - same log line: `cfg_interrupt_line=176`

3. DT `interrupt-map` is built from that byte value:

   - `vpci fdt irq-map dev=1 pin=1 line=176 -> gic_spi=144`

4. Event injection still uses full IRQ **432**:

   - `consume_connection_event ... vm_inject_irq irq=432 rc=0`

This is a direct mismatch:

- guest DT maps PCI INTx to SPI derived from `176` (`144` after `-32`)
- VMM injects IRQ `432`

## Why This Happens

`interrupt_line` in PCI config is an 8-bit field:

- `projects/sel4_projects_libs/libsel4vmmplatsupport/include/sel4vmmplatsupport/drivers/pci_helper.h`
  - `uint8_t interrupt_line;`

The cross-VM code stores `connection_irq` there, which truncates values above 255.

`fdt_generate_vpci_node()` reads PCI config `PCI_INTERRUPT_LINE` and emits the DT `interrupt-map` from that truncated value:

- `projects/sel4_projects_libs/libsel4vmmplatsupport/src/arch/arm/devices/vpci.c`

### Why `interrupt-parent` is not enough here

`interrupt-parent` selects the interrupt controller/domain, but for PCI child
functions the per-device routing comes from the host bridge `interrupt-map`.

In this path, VPCI explicitly generates `interrupt-map` entries. If those
entries are derived from a truncated 8-bit `PCI_INTERRUPT_LINE`, changing
`interrupt-parent` cannot restore the lost high bits.

So this bug must be addressed at the IRQ value source used when building the
PCI `interrupt-map` (or by constraining IRQ allocation to 8-bit-safe values).

## Impact Hypothesis

Highly likely root cause for missing guest IRQ handling after `consume_callback`:

- VM0 injects IRQ 432 correctly.
- Guest IRQ routing (from DT) expects a different IRQ path.
- kmod ISR may never fire for the injected interrupt.

## Candidate Solution Directions

### Option A: Decouple DT interrupt-map generation from 8-bit PCI `interrupt_line`

Generate `interrupt-map` using a full-width IRQ source (not config byte), while keeping PCI header field for compatibility.

Pros:

- Correct mapping for IRQ values >255.
- Preserves existing high IRQ allocation strategy.

Cons:

- Requires API/data-path changes so `fdt_generate_vpci_node()` can see full IRQ values per device.

### Option B: Constrain cross-VM IRQ allocation to <=255

Force `get_crossvm_irq_num()`/allocator to provide IRQs representable in PCI config byte.

Pros:

- Minimal changes to DT generation path.

Cons:

- Platform IRQ availability/collision risk.
- Can conflict with other virtual/physical interrupt reservations.

### Option C: Dedicated explicit IRQ property for vpci-generated node

Keep `interrupt_line` as legacy field but add/consume explicit mapping data for DT generation.

Pros:

- Cleaner semantics.
- Avoids overloading PCI byte field.

Cons:

- Moderate refactor across vpci/crossvm plumbing.

## Recommended Next Step

Prototype Option A first:

- Preserve existing runtime IRQ injection (`432` etc.).
- Modify DT map generation to use full IRQ values.
- Re-test and verify:
  - guest sees IRQ events in `/proc/interrupts`
  - `sel4-rpcdbg` IRQ handler prints on guest side
  - VM1 boot progresses past current hang point.
