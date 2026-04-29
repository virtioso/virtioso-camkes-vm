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

## Latest Status (2026-04-29)

### UARTI routing run: `20260429-111850`

After moving UARTI from VM0 to VM1 in
`apps/Arm/vm_qemu_virtio/orinagx/devices.camkes`, a clean Orin AGX rebuild and
Autopilot run confirmed that VM1 earlycon now appears on Autopilot `tty1`:

- Build sequence:
  - `make mrproper`
  - `make orinagx_defconfig`
  - `make vm_qemu_virtio`
- Built image:
  - `orinagx_vm_qemu_virtio/images/capdl-loader-image-arm-orinagx`
- Autopilot request:
  - `20260429-111850`
  - chain: `vm-qemu-virtio`
  - final status: `failed`
  - test verdict: `fail`
  - `tty0=/dev/ttyACM0`
  - `tty1=/dev/ttyACM1`

VM1 setup generated the intended UARTI boot path:

- `stdout=/bus@0/serial@31d0000`
- `console=ttyAMA0,115200n8`
- `earlycon=pl011,mmio32,0x031d0000`
- DTB node `/bus@0/serial@31d0000`

`tty1.ansi.log` now contains VM1 Linux earlycon output:

- `earlycon: pl11 at MMIO32 0x00000000031d0000`
- `Kernel command line: console=ttyAMA0,115200n8 earlycon=pl011,mmio32,0x031d0000 ...`

So the specific "VM1 earlycon does not appear on UARTI / Autopilot tty1"
problem is resolved by passing UARTI (`serial@31d0000`) to VM1 instead of VM0.

The overall Autopilot test still fails later:

- `AUTOPILOT_FAIL: USERVM_READY_TIMEOUT`
- `AUTOPILOT_INFO: USERVM_CONSOLE_SOURCE=tty1`
- `AUTOPILOT_USERVM_STATUS: status=exited`

The VM1 Linux log stops during PCI enumeration:

- `pci 0000:00:00.0: [5e14:0042] type 00 class 0x060000`

### UARTI earlycon and VM1 image/getty verification: `20260429-132803`

Follow-up testing corrected two earlier assumptions.

First, VM1 should not carry a hard-coded `earlycon=pl011,...` bootarg for this
path. UARTI is exposed to VM1 as the SBSA UART node:

- `stdout=/bus@0/serial@31d0000`
- `console=ttyAMA0,115200n8`
- `earlycon`

The VM1 log still prints:

- `earlycon: pl11 at MMIO32 0x00000000031d0000`

This is Linux's earlycon implementation detail for `arm,sbsa-uart`; it does not
mean the command line is forcing the wrong UART. The same log confirms the real
device path:

- `31d0000.serial: ttyAMA0 at MMIO 0x31d0000 ... is a SBSA`
- `printk: bootconsole [pl11] disabled`

Second, Autopilot did deploy the driver VM rootfs for this run. The chain record
for request `20260429-132803` includes:

- `prepare_vm_image_dir`
- `upload_driver_vm_rootfs`
- `upload_efi`
- `boot_efi`

`upload_driver_vm_rootfs` finished `ok` before EFI upload. The uploaded source
artifact was:

- `vm-images/build/tmp/deploy/images/vm-jetson-agx-orin/vm-image-driver-vm-jetson-agx-orin.rootfs.ext4`

The driver image contains:

- `/var/lib/virt/images/user-vm.qcow2`
- size: `101646336`

The embedded qcow2 was extracted with `debugfs` and compared against the deploy
directory user image:

- embedded: `/tmp/autopilot-user-vm.qcow2`
- deploy: `vm-images/build/tmp/deploy/images/vm-jetson-agx-orin/vm-image-user-vm-jetson-agx-orin.rootfs.ext4.qcow2`
- sha256 for both:
  `077f26787adcd700b50e7f289266dbe7bf337a76a9cbff88c8d5ad98106ea80c`

After converting the embedded qcow2 to raw and inspecting it with `debugfs`, the
user VM `/etc/inittab` contains the expected UARTI getty:

```text
AMA0:12345:respawn:/usr/sbin/ttyrun ttyAMA0 /bin/start_getty 115200 ttyAMA0 vt102
```

Therefore the missing VM1 login prompt in `20260429-132803` is not explained by
Autopilot skipping the driver image upload, stale eMMC contents, or a missing
`SERIAL_CONSOLES`/inittab change in the packaged user VM image.

The current failure boundary is later:

- VM1 starts `/sbin/init`
- VM1 prints only `INIT: version 3.14 booting`
- VM1 does not reach `Starting udev`, `INIT: Entering runlevel`, or `login:`
  before Autopilot timeout

This points at an early userspace/rcS stall or VM/QEMU stall after init starts.
There is also still a real kernel-time gap before root mount:

- `3.324558`: `msm_serial: driver initialized`
- `54.617393`: `cacheinfo: Unable to detect cache hierarchy for CPU 0`
- `64.724981`: `Run /sbin/init as init process`

No `virtio_console_init` line appears in this run before VM1 exits, so the
current runtime boundary has moved earlier than the previous virtio-console
stall marker.

### VM1 sluggishness source-path analysis: 2026-04-29

This analysis was done from the source path rather than another hardware run.

Conclusion: the VM1 slowness is no longer primarily explained by UART output,
but there are two different slow phases and they should not be collapsed into
one root cause.

1. If the delay is while the CAmkES VMM prints `Loading Kernel`, that is before
   VM1 Linux starts. This path is ordinary VMM image loading and cache
   maintenance, not QEMU/virtio emulation.
2. If the delay is after VM1 Linux timestamps begin, the strongest source-backed
   explanation is the nested QEMU/seL4 device-model path. VM1 then pays for many
   synchronous VM1 -> seL4 VMM -> `sel4_virt` kmod -> VM0/QEMU -> QEMU device
   model -> kmod/VMM acknowledgements during PCI/virtio probing and rootfs I/O.
   VM0 does not use that path for its own kernel/rootfs boot.

Runtime alignment from `20260429-132803`:

- UARTI earlycon is now correctly routed and the boot console is disabled after
  the SBSA UART driver binds.
- The large gap from about `3.324s` to `54.617s` occurs during generic device
  and bus initialization before `virtio_blk` reports `/dev/vda`.
- Root mount appears around `64.590s`, then `/sbin/init` starts around
  `64.724s`.
- This timing points at trap-heavy device discovery and block/rootfs access, not
  serial throughput.

Source-path evidence:

- `devices.camkes` makes VM0 a 512 MiB direct VM with initrd, one-to-one
  mapping, and physical device passthrough; VM0 runs QEMU and provides the
  virtio backends.
- The same file makes VM1 a 256 MiB generated-DTB VM with `root=/dev/vda`,
  UARTI console, `clean_cache=true`, `swiotlb=512`, an 8 MiB SWIOTLB bounce
  area at `0xC0000000`, and a virtio driver channel to VM0.
- `projects/vm/components/VM_Arm/src/main.c` prints `Loading Kernel` immediately
  before calling `vm_load_guest_kernel()`. On ARM,
  `libsel4vmmplatsupport/src/arch/arm/guest_image.c` loads the image through
  `vm_ram_touch()` page callbacks. For normal images, each callback reads a page
  from the file descriptor into mapped guest memory; for LZ4 images, each
  callback copies a decompressed page. With `vm->mem.clean_cache` set, both paths
  then call `seL4_ARM_Page_CleanInvalidate_Data()` for that 4 KiB page. This can
  plausibly make the pre-kernel `Loading Kernel` phase slow.
- `qemu-rnd-helper` launches VM1 as `/usr/bin/qemu-system-aarch64 --accel sel4
  -M virt`, with a qcow2 `virtio-blk-pci` root disk,
  `disable-legacy=on,iommu_platform=on`, 9p `-virtfs`,
  `virtio-serial-pci`, optional `virtconsole`, and for `VMID=1` a `tap0`
  `virtio-net-pci` device. It also copies `/var/lib/virt/images/user-vm.qcow2`
  into `/tmp/vm1` by default before QEMU starts.
- The qcow2 copy can delay VM1 launch, but it happens before the guest kernel
  timestamps. It cannot explain the later in-kernel 50+ second gap once Linux is
  already printing timestamps.
- QEMU's seL4 accelerator registers the VM fd with `qemu_set_fd_handler()`.
  When the fd is readable, QEMU drains the forwarded RPC queue in the QEMU main
  loop. `QEMU_OP_MMIO` requests are handled synchronously by either generic
  memory-region access or PCI config-space access and then acknowledged back to
  the driver RPC ring.
- The QEMU-side ioeventfd registration currently records `addr_space =
  AS_GLOBAL`. The kmod-side ioeventfd fast path only handles matching MMIO
  writes; all MMIO reads deliberately fall through to userspace. PCI config
  reads/writes also go through QEMU userspace handling.
- The kmod upcall path uses one global work item and a workqueue with
  `max_active=1`; that work scans all VMs and forwards unhandled requests to
  the userspace RPC queue before waking the QEMU fd poll path.
- `virt-sel4.c` exposes the seL4 PCI host around the GICv2m, PCI ECAM, PCI MMIO
  window, and PCI I/O window. VM1's modern virtio PCI devices therefore
  exercise this emulated PCI path during probe.

Likely contributors, ranked:

1. Synchronous userspace exits for VM1 PCI config and virtio MMIO, especially
   reads that cannot use the ioeventfd write fast path.
2. Heavy default VM1 QEMU device surface: qcow2 virtio-blk, tap virtio-net,
   virtio-serial/virtconsole, 9p, `iommu_platform=on`, SWIOTLB, and vPCI/MSI
   plumbing.
3. kmod upcall serialization through a single global work item and single-active
   workqueue.
4. Pre-kernel `Loading Kernel` time is likely dominated by VMM image load plus
   per-page cache clean/invalidate when `clean_cache=true`. This needs separate
   timing around `vm_load_guest_kernel()` and should not be attributed to QEMU.
   VM0 also uses `clean_cache=true`, so if VM0's kernel load is materially
   faster, compare image size, compression type, and exact load timing before
   treating cache maintenance as the whole explanation.
5. UART output is now a low-probability primary cause because the observed gap
   is before root mount and after early console routing has been corrected.

Next useful experiments should avoid printk floods:

- Default-off VMM image-load timing has been added as `VmImageLoadTiming` /
  `CONFIG_VM_IMAGE_LOAD_TIMING`. When enabled, `VM_Arm` emits one
  `VM_IMAGE_LOAD_TIMING` line per kernel/initrd/DTB load with instance, phase,
  image name, byte count, CNTPCT cycles/frequency, computed microseconds, error
  code, and `clean_cache` state. The default build keeps it off.
- Add default-off counters/timing, not repeated printk, for forwarded RPC
  counts by VM, op, address space, direction, and latency in both
  `sources/kmod-sel4-virt` and `sources/qemu/accel/sel4`.
- Run a minimal VM1 QEMU surface: no tap net, no 9p, no virtio-serial/console,
  raw block instead of qcow2 if possible, and only the virtio-blk root device.
- If the counters confirm RPC pressure, consider a kmod per-VM work item/queue
  and a broader fast path for virtqueue kicks or PCI/virtio accesses that do not
  need full QEMU userspace handling.
- Keep the earlier getty/stale-rootfs/UARTI questions closed unless a future
  image sha256 or generated bootarg contradicts the evidence above.

### VMM image-load timing run: `20260429-145048`

The first timing-enabled run that emitted observable timing lines was
`20260429-145048`. It was built from a clean Orin AGX configuration with
`CONFIG_VM_IMAGE_LOAD_TIMING=y`.

Timing evidence:

| VM | Phase | Bytes | usec | Approx wall time | clean_cache |
| --- | --- | ---: | ---: | ---: | ---: |
| VM0 | kernel `linux` | 44352000 | 45939 | 45.9 ms | 1 |
| VM0 | initrd `linux-initrd` | 2694584 | 4086 | 4.1 ms | 1 |
| VM0 | generated DTB | 327680 | 1671 | 1.7 ms | 1 |
| VM1 | kernel `linux` | 44352000 | 21698201 | 21.7 s | 1 |
| VM1 | generated DTB | 327680 | 159612 | 159.6 ms | 1 |

This confirms the user's observation: VM1's VMM-side Linux kernel load is
already very slow before VM1 Linux starts. The same kernel image size loads in
about `45.9 ms` for VM0 and about `21.7 s` for VM1, a roughly `472x`
difference.

This does not contradict the later QEMU/virtio hypothesis; it splits the problem
into two measured phases:

1. VM1 pre-kernel load is slow in the CAmkES VMM image-load path.
2. VM1 post-kernel boot can still be slow in the nested QEMU/seL4 virtio path.

Runtime evidence also shows a key VM0/VM1 memory-path difference:

- VM0: `Guest RAM mapped from untyped memory (unity stage-2 mapping)`
- VM1: `Guest RAM mapped from allocator pool (NO unity stage-2 mapping)`

That is now the primary source-backed suspect for the pre-kernel load gap. Both
VMs have `clean_cache=1` and load the same kernel image, but VM0's image load is
fast while VM1's is slow. The next source review should focus on why
`vm_ram_touch()` plus `seL4_ARM_Page_CleanInvalidate_Data()` is hundreds of
times slower for VM1's allocator-pool/no-unity mapping path than for VM0's
untyped/unity mapping path.

### Inter-VM RPC during VM1 image load

The image-load timing does not prove that the RAM path is the only active
factor. Source order shows that VM1 installs devices and initializes VMM modules
before loading the guest images:

1. `init_modules(&vm, __start__vmm_module, __stop__vmm_module)`
2. `load_vm_images(&vm, &vm_config)`
3. `dump_dtb_base64(gen_dtb_buf)`
4. `vcpu_start(vm_vcpu)`
5. `vm_run(&vm)`

The `cross_vm_connections` module registers its async consume handler in
`init_cross_vm_connections()` and depends on `vpci_init`, so a VM0-origin
cross-VM notification can in principle arrive during VM1 `load_vm_images()`.
However, VM1 Linux is not started until after `load_vm_images()` and the DTB
dump, so VM1 cannot yet be probing PCI, touching virtqueues, or producing
guest-origin RPC at the measured `21.7 s` kernel-load point.

The Linux-side `kmod-sel4-virt` path is also event-driven rather than an obvious
empty-queue spin loop:

- `sel4_dt_irqhandler()` returns `IRQ_NONE` when the consume event register is
  zero and queues work only through the common interrupt path when the IRQ is
  handled.
- `sel4_vm_upcall_notify()` queues a single work item on `sel4_ioreq_wq`.
- `sel4_vm_process_ioreqs()` walks concrete driver RPC requests and wakes
  userspace only when `sel4_ioreq_pending()` says a userspace request is ready.
- `sel4_vm_poll()` waits on `ioreq_wait` and reports readable only when a
  userspace RPC request is pending.
- `sel4_ioeventfd_match()` scans registered ioeventfds only while processing an
  actual MMIO request.

Therefore, an inter-VM/RPC CPU-burn bug is still a valid second suspect if an
event register is stuck asserted, an IRQ is retriggering, or VM0 userspace keeps
polling a permanently-ready fd. But that would be a VM0-origin/runtime
contention issue during VM1 VMM image load, not VM1 guest PCI probing. It should
be measured separately with default-off counters for doorbells, handled IRQs,
queued work, forwarded RPCs, empty workqueue passes, and userspace poll-ready
returns, correlated with the `VM_IMAGE_LOAD_TIMING` window.

Instrumentation added for that follow-up:

- `CONFIG_KMOD_SEL4_VIRT_STATS` in the workspace Kconfig, default `n`.
- Yocto passes it to `kernel-module-sel4-virt` as `VIRTIO_VM_STATS=1`.
- `sources/kmod-sel4-virt` emits rate-limited `sel4_virt_stats:` lines with
  `dt_doorbell`, `dt_irq_handled`, `irq_handled`, `notify`, `work_pass`,
  `work_empty`, `forwarded`, `wake_userspace`, and `poll_ready`.

This option is intentionally separate from `CONFIG_VIRTIO_VM_DEBUG`: it should
show whether there is a high-rate event/poll/workqueue loop without restoring
the verbose per-event printk flood.

### Combined timing/stats run: `20260429-161305`

Run `20260429-161305` was built after refreshing the driver-VM Yocto image so
the embedded `sel4_virt.ko` included `CONFIG_KMOD_SEL4_VIRT_STATS` support.
The final seL4 image had both:

- `CONFIG_VM_IMAGE_LOAD_TIMING=y`
- `CONFIG_KMOD_SEL4_VIRT_STATS=y`

VMM image-load timing:

| VM | Phase | Bytes | usec | Approx wall time | clean_cache |
| --- | --- | ---: | ---: | ---: | ---: |
| VM0 | kernel `linux` | 44352000 | 33235 | 33.2 ms | 1 |
| VM0 | initrd `linux-initrd` | 2695784 | 2429 | 2.4 ms | 1 |
| VM0 | generated DTB | 327680 | 399 | 0.4 ms | 1 |
| VM1 | kernel `linux` | 44352000 | 731398 | 0.73 s | 1 |
| VM1 | generated DTB | 327680 | 1988 | 2.0 ms | 1 |

This still shows VM1's VMM-side kernel load is slower than VM0's, but not the
earlier `21.7 s` outlier from run `20260429-145048`. The pre-kernel load gap is
therefore real but variable; it no longer explains the large VM1 Linux boot gap
by itself.

VM1 Linux log timing:

- VM1 reaches UART console enable around `1.756s` and disables bootconsole at
  `1.822s`.
- VM1 enumerates and enables virtio PCI devices from about `2.201s` to
  `3.223s`.
- The next visible VM1 kernel line is not until `58.844s`
  (`cacheinfo: Unable to detect cache hierarchy for CPU 0`).
- `virtio_blk virtio0: [vda] ...` appears at `61.243s`.
- Rootfs mounts at `71.447s`, and `/sbin/init` starts at `71.672s`.

That means the observed "near 1.8s then much later" behavior should not be
interpreted as VM1's guest clock being frozen in this run: the guest timestamp
eventually advances to `58s`/`71s`. The symptom is a long quiet kernel interval
after PCI/virtio enumeration, not merely delayed UART output.

The `sel4_virt_stats:` counters also rule out an empty-workqueue spin for this
run. Around the quiet interval:

- `work_empty` remains `0`.
- `forwarded` tracks handled IRQ/work passes one-for-one.
- `dt_irq_handled`, `irq_handled`, `notify`, `work_pass`, `forwarded`, and
  `wake_userspace` keep increasing.
- `poll_ready` also increases, but at a lower rate.

Examples:

- At `23.290s`: `forwarded=793`, `work_empty=0`, `poll_ready=11`.
- At `60.221s`: `forwarded=1600`, `work_empty=0`, `poll_ready=730`.
- At `105.769s`: `forwarded=2510`, `work_empty=0`, `poll_ready=1497`.

So the post-kernel bottleneck is backed by real forwarded VM1 requests, not a
kernel workqueue burning CPU on empty queues. The next useful split is to
classify which QEMU/virtio MMIO operations dominate those forwarded requests
during the `3s` to `61s` interval, preferably with rate-limited per-op/per-device
stats rather than per-event printk.

### QEMU wait-thread A/B run: `20260429-165235`

Run `20260429-165235` tested the hypothesis that QEMU commit
`26be78e2a8` (`sel4: handle rpc through vmfd readiness`) caused the VM1
slowness by moving seL4 RPC servicing from the older dedicated `SEL4_WAIT_IO`
thread into QEMU's vmfd main-loop readiness handler.

To make the hypothesis falsifiable, the workspace gained a diagnostic build
switch:

- `CONFIG_QEMU_SEL4_RPC_WAIT_THREAD=y`

That switch builds the QEMU seL4 accelerator with the older dedicated wait
thread shape while leaving the rest of the Orin image path unchanged. The run
also kept:

- `CONFIG_VM_IMAGE_LOAD_TIMING=y`
- `CONFIG_KMOD_SEL4_VIRT_STATS=y`

The result does **not** support the vmfd-readiness change as the primary
slowness cause.

VMM image-load timing:

| VM | Phase | Bytes | usec | Approx wall time | clean_cache |
| --- | --- | ---: | ---: | ---: | ---: |
| VM0 | kernel `linux` | 44352000 | 46024 | 46.0 ms | 1 |
| VM0 | initrd `linux-initrd` | 2695784 | 4088 | 4.1 ms | 1 |
| VM0 | generated DTB | 327680 | 1670 | 1.7 ms | 1 |
| VM1 | kernel `linux` | 44352000 | 21716773 | 21.7 s | 1 |
| VM1 | generated DTB | 327680 | 159763 | 160 ms | 1 |

VM1 Linux still booted slowly after that. The guest reached PCI/virtio setup
around `2.738s` to `4.053s`, then was quiet until `cacheinfo` at `55.340s`.
`virtio_blk` appeared at `57.481s`, rootfs mounted at `67.133s`, and
`/sbin/init` started at `67.356s`. Autopilot still failed with
`USERVM_READY_TIMEOUT`.

The kmod stats in the wait-thread run are also qualitatively different from the
vmfd run in one important way: `poll_ready` stayed `0` because QEMU was no
longer using vmfd poll readiness. Despite that, the same slowness class
remained, with real forwarded requests and `work_empty=0`.

Conclusion: keep `CONFIG_QEMU_SEL4_RPC_WAIT_THREAD` as a useful A/B diagnostic,
but do not treat the QEMU vmfd-readiness conversion as the current primary
suspect. The stronger remaining suspects are the VM1 RAM/data/control mapping
and cache-maintenance path, plus the exact high-volume operation class behind
the forwarded requests during virtio PCI probing.

### GICv3 and seL4 IPI candidate check: 2026-04-29

Two additional source-level candidates were checked after the vmfd A/B result:

1. Orin AGX GICv3 CAmkES VM support may still have bugs.
2. The seL4 kernel IPI path may still be slow or incorrect.

The current generated Orin image is SMP-capable:

- `orinagx_vm_qemu_virtio/kernel/gen_config/kernel/gen_config.h`
  - `CONFIG_MAX_NUM_NODES 12`
  - `CONFIG_ENABLE_SMP_SUPPORT 1`

However, the generated CapDL for this app places the relevant CAmkES threads on
CPU affinity 0, and both VM components have one vCPU:

- `orinagx_vm_qemu_virtio/vm_qemu_virtio.cdl`
  - `vm0_*` TCBs: `affinity: 0`
  - `vm1_*` TCBs: `affinity: 0`
- `orinagx_vm_qemu_virtio/devices.camkes.cpp`
  - `vm0.num_vcpus = 1`
- `orinagx_vm_qemu_virtio/vm1/include/camkes-component-vm1.h`
  - `num_vcpus_DEF 1`

`libsel4vm/src/arch/arm/boot.c` does call `seL4_TCB_SetAffinity()` for vCPU
TCBs when `CONFIG_MAX_NUM_NODES > 1`, but the affinity it sets is `vcpu_id`.
For this one-vCPU VM0/VM1 configuration, both guest vCPU TCBs remain on CPU 0.
No runtime affinity reassignment was found in the app path.

Conclusion for the IPI candidate: the kernel IPI fixes are real and relevant
to multi-core Orin support, but this generated `vm_qemu_virtio` app should not
depend on cross-core IPIs for the VM0/VM1/VMM hot path. IPI slowness is
therefore a weak explanation for the current VM1 slowdown unless a later change
pins components or vCPUs away from CPU 0.

The GICv3/vGIC candidate remains more plausible for the post-kernel stall:

- The current GICv3 vGIC implementation is custom Orin-era work in
  `projects/sel4_projects_libs/libsel4vm/src/arch/arm/vgic/vgic_v3.c`.
- It uses hard-coded `NUM_LIST_REGS 4` from
  `projects/sel4_projects_libs/libsel4vm/src/arch/arm/vgic/virq.h`.
- It relies on vGIC maintenance exits to free list-register shadow state and
  reinject queued or still-asserted level IRQs.
- The Linux quiet interval starts after PCI/virtio setup, where virtio INTx and
  cross-VM event IRQs are active and guest EOI/maintenance behavior matters.

The main virtio paths are registered IRQ paths, not the unregistered
direct-inject path:

- PCI INTx:
  - `projects/virtioso-camkes-vm/src/libsel4vm_glue.c`
  - `shared_irq_line_init()` registers `INTERRUPT_PCI_INTX_BASE + i`
  - `handle_pci_intx()` drives `shared_irq_line_change()`
  - `shared_irq_line_change()` calls `vm_set_irq_level()`
- Cross-VM doorbell:
  - `projects/sel4_projects_libs/libsel4vmmplatsupport/src/drivers/cross_vm_connection.c`
  - `register_consume_event()` calls `vm_register_irq()`
  - `consume_connection_event()` later calls `vm_inject_irq()` for that registered IRQ

That matters because registered IRQs depend on:

- guest enable state (`GICD_ISENABLER` / `GICR_ISENABLER0`),
- the vGIC IRQ queue,
- list-register shadow state,
- maintenance exits after EOI,
- level-triggered reinjection if the line remains asserted.

A bug or excessive cost in that path could explain the Linux-side pause between
virtio probing and later block/rootfs progress. It does not explain the
pre-Linux VMM-side VM1 kernel image load outliers, because those happen before
VM1 executes Linux and before guest GIC/vGIC interrupt delivery can dominate.

Current classification:

- Pre-kernel VM1 image load slowness: keep VM1 memory mapping/cache-maintenance
  path as the leading suspect.
- Post-kernel Linux stall around virtio PCI/console/block: promote GICv3 vGIC
  registered-IRQ/maintenance behavior to a first-class suspect alongside
  per-op QEMU/sel4 request classification.
- seL4 IPI slow path: currently low priority for this app because generated
  VM/VMM/vCPU affinity is CPU 0 only.

### VM1 RAM touch and cache-maintenance granularity: 2026-04-29

Source review refined the pre-kernel VM1 image-load suspect from generic
"allocator-pool RAM" to a concrete granularity effect.

The ARM image loader path is:

1. `VM_Arm/src/main.c`
   - `load_vm_images()`
   - `vm_load_guest_kernel(vm, ..., vm_config->ram.base, ...)`
2. `libsel4vmmplatsupport/src/arch/arm/guest_image.c`
   - `load_image()`
   - `vm_ram_mark_allocated(vm, load_addr, ROUND_UP(file_size, PAGE_SIZE_4K))`
   - `vm_ram_touch(vm, load_addr, file_size, guest_write_address, &fd)`
3. `libsel4vm/src/guest_ram.c`
   - `vm_ram_touch()` maps/accesses one reservation page at a time.
   - The per-iteration page size is `vm_reservation_page_size_bits(reservation)`.
   - The callback receives one chunk no larger than that page size.
4. `guest_write_address()`
   - `read(fd, vaddr, size)`
   - when `vm->mem.clean_cache` is true:
     `seL4_ARM_Page_CleanInvalidate_Data(cap, 0, PAGE_SIZE_4K)`

The generated Orin configuration gives VM0 and VM1 different RAM backing
granularity:

- VM0:
  - `vm0.vm_image_config.map_one_to_one = true`
  - `vm0.untyped_mmios` contains two 256 MiB RAM regions with `page_bits = 21`
  - generated `vm0/camkes.simple.c` records those fixed regions as
    `.size_bits = 28, .page_bits = 21`
  - runtime log: `Guest RAM mapped from untyped memory (unity stage-2 mapping)`
- VM1:
  - `vm1.vm_image_config.map_one_to_one = false`
  - no `vm1.untyped_mmios` RAM region
  - `VIRTIO_DRIVER_GUEST_RAM_CONFIGURATION_DEF(1)` expands to
    `vm1.simple_untyped24_pool = 12 + (0x10000000 >> 24)`
  - generated `vm1/camkes.simple.c` records allocator-pool untypeds with
    `.size_bits = 24, .page_bits = 12`
  - runtime log: `Guest RAM mapped from allocator pool (NO unity stage-2 mapping)`

This is a regression from the older RPi4 `vm_qemu_virtio` large-page path, not
an inherent `VMSWIOTLB` limitation. Commit `c47cda6` (`Improve large page
support`) added explicit component attributes:

- `guest_large_pages`
- `cross_connector_large_pages`

and set the RPi4 app to:

- `vm0.cross_connector_large_pages = true`
- `vm1.guest_large_pages = true`

That older path allowed VM1 guest RAM to use large pages even in the
VMSWIOTLB-era app shape. The current shared Virtioso macro only expands VM1's
allocator pool to enough size-24 untypeds:

- `vm1.simple_untyped24_pool = 12 + (VM1_RAM_SIZE >> 24)`

but the current CAmkES simple template records normal allocator-pool untypeds
with `.page_bits = 12`. In other words, the current VM1 path has large enough
untyped objects, but the generated metadata still tells `vm_ram_touch()` to
retype and touch 4 KiB frames. That is the concrete behavior to restore or
replace when fixing VM1 pre-kernel load performance.

Follow-up history check corrected the implementation detail. The older elegant
path was primarily in `projects/sel4_projects_libs/libsel4vm`, not in the
CAmkES simple template:

- `ad99b3a` (`Improve guest large page support`, 2022-01-07) added weak
  `guest_large_pages`, `ram_base()`, and `ram_size()` symbols to
  `libsel4vm/src/guest_ram.c`. When `guest_large_pages` was true and
  `vm_ram_touch()` accessed an address inside the configured RAM range, it used
  `seL4_LargePageBits` and 2 MiB alignment for the touch callback.
- `9d3ac10` (`Rework guest large page support`, 2023-03-16) replaced the old
  hard-coded large-page hack with externally provided CAmkES symbols:
  `guest_large_pages`, `ram_base`, and `ram_size`.
- The RPi4 app branch still shows this intended app-side contract:
  `vm1.guest_large_pages = true`, `vm0.cross_connector_large_pages = true`,
  and VM1 RAM omitted from `untyped_mmios`.
- `a579896` (`Use libsel4vm's automatic page size tracking`, 2023-09-01) later
  removed those app attributes from the then-current app, expecting libsel4vm
  to track page sizes automatically.
- The current working `projects/sel4_projects_libs` branch contains a newer
  replacement, `c89de50` (`guest_ram: Query page size from CAmkES
  untyped_mmios config`, 2026-01-09). That code queries
  `camkes_get_untyped_page_bits(addr)` in the RAM allocation iterators and
  falls back to 4 KiB when no configured page size is found.

Therefore the current Orin issue is more precise than "large pages missing":
the old `guest_large_pages` libsel4vm path is not present, and the newer
automatic page-size path only gives VM1 large frames when the CAmkES page-size
query can associate the guest RAM address with a 2 MiB-backed region. Current
Orin VM1 does not satisfy that condition, so it falls back to 4 KiB touching.
Do not fix this by globally changing CAmkES simple-pool metadata; restore the
guest-RAM large-page contract in libsel4vm/app configuration or make the newer
automatic page-size tracking cover VM1's VMSWIOTLB allocator-pool RAM in a
way that matches the old RPi4 semantics.

That difference predicts the measured kernel-load ratio:

- VM1 loads the 44,352,000 byte kernel through 4 KiB chunks:
  - about `ceil(44352000 / 4096) = 10829` touch/cache-maintenance iterations.
- VM0 loads the same kernel through 2 MiB chunks:
  - about `ceil(44352000 / 2097152) = 22` iterations.
- Ratio:
  - `10829 / 22 = 492x`.
- Measured ratio in `20260429-145048`:
  - VM0 kernel load `45.9 ms`
  - VM1 kernel load `21.7 s`
  - about `472x`.

This is close enough to explain the pre-kernel load outlier without invoking
QEMU, virtio, GICv3, IPI, or inter-VM RPC. It is also consistent with the
smaller generated-DTB timing gap: 320 KiB is about 80 4 KiB chunks versus one
2 MiB chunk, and the measured DTB load was about `159.6 ms` for VM1 versus
`1.7 ms` for VM0.

Important correctness caveat: the current cache-maintenance callback always
passes `PAGE_SIZE_4K` as the clean/invalidate range. For VM1's 4 KiB frames
that covers the whole touched frame. For VM0's 2 MiB frames it only covers the
first 4 KiB of each 2 MiB touched chunk, so VM0 is fast partly because it is not
performing equivalent cache maintenance over the whole loaded range. This makes
VM0 versus VM1 timing a useful performance clue, but not proof that the VM0
cache-maintenance behavior is the desired long-term behavior.

This correctness bug was fixed in `projects/sel4_projects_libs` commit
`7cf1a26` (`libsel4vmmplatsupport: clean loaded image ranges`). The ARM image
loader now asks the VMM vspace root to clean/invalidate the actual virtual
range written by the image-load callback instead of cleaning only offset
`0..4K` of the backing page cap. After that fix, VM0/VM1 timing comparisons
should be interpreted as the cost of the intended range operation, not the old
partial-clean artifact.

Current pre-kernel conclusion:

- VM1 is slow because the current RAM configuration makes the loader perform
  thousands of per-4 KiB map/access/unmap/cache-maintenance operations.
- VM0 is fast because its 2 MiB `untyped_mmios` RAM backing collapses the same
  load into tens of operations.
- A controlled VM1 A/B should therefore compare either:
  - VM1 2 MiB fixed RAM backing with `map_one_to_one=true`, or
  - a corrected/batched image-load cache-maintenance path,
  before spending more time on IPI/GIC/QEMU explanations for the pre-kernel
  `Loading Kernel` delay.

### RPi4 branch carry-over audit: 2026-04-29

Because the old RPi4 branch carried deliberate enhancements on top of upstream,
the `projects/sel4_projects_libs` history was compared against the current
`virtioso-next` branch with `git cherry` / `git log --cherry-pick` across:

- local `rpi4`
- `tiiuae/rpi4-next`
- dated `tiiuae/rpi4-*` snapshots
- `tiiuae/rpi4-so-next`

The audit result is not "everything was lost". Most recurring old topics are
either already in the current branch under different commit IDs, or are
platform-specific RPi4 support. The important exception is VM1 guest-RAM large
page semantics.

Carry-over status:

- **Missing / badly superseded: guest RAM large-page touch path.**
  - Old commits: `ad99b3a`, `9d3ac10`.
  - Old app contract: `vm1.guest_large_pages = true`.
  - Current replacement: `c89de50` queries `camkes_get_untyped_page_bits()`.
  - Problem: current Orin VM1 VMSWIOTLB allocator-pool RAM does not resolve to
    2 MiB through that query, so it falls back to 4 KiB touching.
  - Action: restore the old libsel4vm/app-level semantic or extend the current
    automatic page-size path to cover this RAM shape.

- **Present / superseded: dataport frame-array mapping and dataport large-page
  support.**
  - Old commits: `0884ff33`, `1a499098`, `bbeb0ca`.
  - Current code has `vm_map_reservation_frames()` and
    `cross_vm_connection.c` maps dataport frames using
    `dataport->frame_size_bits`.
  - This means the old dataport large-page concern was brought forward in the
    current frame-array mapping design, not forgotten in the same way as guest
    RAM touching.

- **Present / evolved: cross-VM PCI BAR size calculation.**
  - Old commit: `8318817`.
  - Current `get_pci_bar_size()` computes the largest data/control dataport
    resource size across connections and uses that stride when assigning event,
    data, and control BAR addresses.
  - This preserves the old Linux-remap avoidance idea, now extended for the
    split data/control BAR shape.

- **Present / evolved: reservation mapping hardening.**
  - Old commits include `088e9a7` and the `rpi4-so-next` reservation cleanup
    series (`9095ade`, `b9750e8`, `3d9c7d`, `3e23c22`, etc.).
  - Current `guest_memory.c` uses `bytes_left`, overflow checks, mixed-page-size
    detection, and proper map failure return values.

- **Present but still an Orin suspect: IRQ trigger/level handling.**
  - Old RPi4 commits: `cd1bb8e`, later `b412593` / `9821942`.
  - Current branch has GICv2 trigger/level support and separate custom GICv3
    support for Orin.
  - This is not an obvious missed RPi4 carry-over item, but GICv3 registered
    IRQ/maintenance behavior remains a first-class Orin-specific suspect for
    the post-kernel virtio stall.

- **Present / not currently central: vCPU thread naming and hard-coded page-size
  cleanup.**
  - Old topics such as `Use proper vCPU thread name`,
    `libsel4vm/trivial: Fix hardcoded page size`, and
    `libsel4vmmplatsupport/trivial: Fix hardcoded` have current equivalents or
    have been replaced by broader page-size plumbing.

- **Platform-specific / low relevance to current Orin: RPi4 platform bring-up
  and old zImage alignment.**
  - Old commits such as `rpi4: Add VM support` and `rpi4: Fix kernel alignment`
    are RPi4/platform-loader specific.
  - The current Orin path loads the kernel at the configured VM entry for
    `IMG_BIN`/LZ4, so the old RPi4 zImage 2 MiB load offset is not the leading
    explanation for the current Orin VM1 slowness.

Conclusion: the audit strengthens the current pre-kernel hypothesis. The main
forgotten RPi4 improvement relevant to Orin VM1 is not generic "large pages"
everywhere, but specifically the libsel4vm guest-RAM large-page touch contract
for VM1's VMSWIOTLB-style RAM. The next implementation should target that
contract narrowly and leave the already-carried dataport/reservation/vGIC work
alone unless a separate test proves a bug there.

### RPi4 guest large-page contract restored: 2026-04-29

The current `camkes_get_untyped_page_bits()` guest-RAM allocation logic was not
upstream seL4 code. It came from the local January 2026 branch work:

- `c89de50` / `22ee1fc`
  - `guest_ram: Query page size from CAmkES untyped_mmios config`
  - commit text includes `Generated with Claude Code`

Per the RPi4 branch policy for this investigation, that guest-RAM allocation
logic was removed from `libsel4vm/src/guest_ram.c` and the RPi4-style contract
was restored instead:

- `projects/sel4_projects_libs` commit `5b87dd5`
  - `libsel4vm: restore guest large page contract`
  - restores weak defaults for `guest_large_pages`, `ram_base`, and `ram_size`
  - RAM allocation iterators use `seL4_LargePageBits` when
    `guest_large_pages` is true and the address is inside
    `[ram_base, ram_base + ram_size)`
  - otherwise they keep the upstream/default `seL4_PageBits` behavior
- `projects/vm` commit `3acdbcc`
  - `VM_Arm: emit guest large page settings`
  - adds `guest_large_pages` as a VM attribute with default `false`
  - emits `ram_base` and `ram_size` from the VM config in
    `seL4VMParameters.template.c`
- `projects/vm` commit `fab38d1`
  - `VM_Arm: avoid duplicate large page symbol`
  - fixes the first clean-build failure after `3acdbcc`: CAmkES already emits
    the `guest_large_pages` attribute as a component global, so the VM
    parameter template must only emit the missing `ram_base` / `ram_size`
    symbols
- `projects/virtioso-camkes-vm` commit `dec6f0f`
  - `orinagx: enable VM1 guest large pages`
  - sets `vm1.guest_large_pages = true` for the Orin AGX
    `vm_qemu_virtio` app

This change intentionally does **not** globally modify CAmkES simple untyped
pool metadata. It also leaves the separate `guest_memory_util.c` use of
`camkes_get_untyped_page_bits()` for explicit `untyped_mmios` /
physical-host-bridge mapping alone; the replaced behavior is specifically the
guest-RAM allocation fallback that made VM1 allocator-pool RAM use 4 KiB pages.

Verification:

- `make mrproper`
- `make orinagx_defconfig`
- `make vm_qemu_virtio`

The first build attempt failed at the VM component link due to duplicate
`guest_large_pages` symbols. After `fab38d1`, the resumed clean build
completed and produced:

- `orinagx_vm_qemu_virtio/images/capdl-loader-image-arm-orinagx`
- size: `54624580` bytes
- timestamp: `2026-04-29 17:34`

### UARTA/header run: `20260429-105435`

Earlier clean Orin AGX rebuild and Autopilot run:

- Build sequence:
  - `make mrproper`
  - `make orinagx_defconfig`
  - `make vm_qemu_virtio`
- Built image:
  - `orinagx_vm_qemu_virtio/images/capdl-loader-image-arm-orinagx`
- Autopilot request:
  - `20260429-105435`
  - chain: `vm-qemu-virtio`
  - final status: `failed`
  - test verdict: `fail`
  - `tty0=/dev/ttyACM0`
  - `tty1=/dev/ttyACM1`

VM0 reached the driver-VM shell and `uservmctl start` reported VM1 running.
VM1 setup reached generated DTB handoff and produced the expected UARTA
bootargs:

- `console=ttyS0,115200n8`
- `earlycon=uart8250,mmio32,0x03100000,115200n8`
- `stdout-path=/bus@0/serial@3100000`

`tty1.ansi.log` was non-empty, but only contained firmware/UEFI shell output
and the EFI launch line. It had no VM1 Linux, earlycon, UART, or RAS matches.
`vm.log` was present but empty.

Important mapping correction: Autopilot `tty1=/dev/ttyACM1` is the host-side
capture name and, on the AGX Orin dev kit micro-USB/TOPO path, corresponds to
UARTI (`serial@31d0000`). The 40-pin expansion header pins 8/10 are a separate
physical path: header `UART1_TX/RX`, which NVIDIA's AGX Orin mapping identifies
as UARTA (`serial@3100000`). Do not infer the Tegra UART instance from the
Autopilot source name alone.

`tty0.ansi.log` shows VM1 faulting at first UARTA MMIO access:

- `ADDR = 0x8000000003100000`
- RAS uncorrectable error in IOB
- RAS uncorrectable error in ACI

Conclusion for this run: VM1 is configured to earlycon on header UARTA
(`serial@3100000`), while Autopilot `tty1` is likely capturing the micro-USB
UARTI path. That alone explains why VM1 UARTA output does not appear in
`tty1.ansi.log`. Separately, VM1 also faults on first UARTA MMIO access, so the
UARTA BCT/firmware access path remains relevant if the intended console is the
40-pin header pins 8/10.

## Earlier Status (2026-02-10)

### What changed since the initial IRQ mismatch finding

1. Cross-VM IRQ route was moved to an 8-bit-safe value:
   - Orin platform reserve now uses IRQ `236` for cross-VM connector IRQ allocation.
2. Runtime now shows matching inject path:
   - `consume_connection_event ... irq=236 inject_irq=1`
   - `vm_inject_irq irq=236 rc=0`
3. VM1 boot command line now includes:
   - `initcall_debug`
   - `keep_bootcon`

### Current boot symptom

VM1 still stalls in early driver init at:

- `calling  virtio_console_init+0x0/0x110 @ 1`

No matching return line appears:

- missing `initcall virtio_console_init... returned ...`

This indicates the stall is inside `virtio_console_init` / `virtcons_probe` flow, not after it.

### Evidence that IRQ delivery is active during the stall

At and after the `virtio_console_init` line, logs continue to show:

- VM1 -> VM0 notify (`rpcdbg: notify: sending to device VM`)
- VM0 `consume_callback`
- VM0 `consume_connection_event` badge match and event register increment
- `vm_inject_irq irq=236 rc=0`
- VM1 `rpc_run summary ... rc=0`
- VM1 handling `QEMU_OP_SET_IRQ` events (`event[0] op=16`, `handle_pci op=16`)

So the current issue is not a complete interrupt blackout.

### New hypothesis (primary)

Likely stall point is Linux virtio-console multiport/early-console synchronization in `virtcons_probe()`:

- File: `drivers/char/virtio_console.c` (guest kernel)
- `virtio_console_init()` registers the virtio console driver.
- probe path can block in:
  - `wait_for_completion(&early_console_added)` when:
    - `multiport == true`
    - early console path is active

This matches observed behavior:

- `console=hvc0` + early boot console path is active
- initcall enters `virtio_console_init` and does not return
- system continues RPC/IRQ activity around console device traffic

### Why this can still be true even if RPi4 worked before

RPi4 success does not disprove this root cause because behavior depends on:

- exact kernel version/config,
- early console timing,
- virtio-serial feature negotiation timing,
- host/backend launch details.

The same logic can be benign on one platform and stall on another.

### Alternative hypotheses still open

1. Virtio-console control queue protocol handling mismatch in backend emulation.
2. Incomplete port/control event progression despite active data/IRQ traffic.
3. Timing regression introduced by recent debug or bootarg changes (less likely, but testable).

### Next recommended validation steps

1. Add targeted logs in guest `drivers/char/virtio_console.c` around:
   - `multiport` detection,
   - `__send_control_msg(... DEVICE_READY ...)`,
   - `wait_for_completion(&early_console_added)`.
2. Run A/B with single-port virtio-serial host config (`max_ports=1`) as an experiment only.
3. If A/B confirms the block path, decide between:
   - enforcing single-port mode in this environment, or
   - fixing/aligning multiport control-queue behavior end-to-end.
