# X86 `qemu_pc99` `vm_qemu_virtio` Port Notes

Date: 2026-04-16

## Scope

Track x86_64 bring-up work for `projects/virtioso-camkes-vm` on `qemu_pc99` / `pc99`,
with emphasis on getting a `vm_qemu_virtio` baseline boot path working and preserving
the existing zero-copy SWIOTLB design later in the port.

### 2026-04-19 q35 guest-stall fix: guest IRQ dispatch loop was still capped at 0..15

The current q35 guest stall after:

- `virtio_blk virtio1: [vda] ...`

turned out not to be an HPET root cause and not a Kvaser-side issue. The
regression was in the VMM async IRQ dispatch path after the q35 IRQ-routing
rework.

Root cause:

- q35 guest-owned physical PCI IRQs are now derived structurally as GSIs above
  15:
  - `00:01.0` virtio-net -> `21`
  - `00:02.0` virtio-blk -> `22`
- `bind_vm_irq_handler()` and generated IRQ caps already supported those values
- but `handle_async_event()` in `projects/vm/components/Init/src/main.c` still
  only iterated:
  - `for (int i = 0; i < 16; i++)`
- so guest IRQ badges `21` / `22` were minted and delivered to the
  notification, but silently ignored by the dispatch loop

Fix:

- extend the dispatch loop to iterate over the full `irq_badges[]` array
  instead of stopping at `15`

Committed in `projects/vm`:

- `525d24b` `x86: dispatch q35 guest irqs above 15`

Validated by autopilot request `20260419-200226`:

- the old stall point is gone
- after `virtio_blk ... [vda]`, the guest now receives and services:
  - `External IRQ badge irq=22`
  - `IRQ ack handler irq=22 cap=653`
- boot continues into userspace again:
  - `Run /init as init process`
  - `VIRTIOSO_INIT: Attempting QEMU virtio-blk boot`
  - `VIRTIOSO_INIT: QEMU virtio-blk boot successful`
  - `VIRTIOSO_INIT: Switching to rootfs via /sbin/init`

Interpretation:

- the guest-stall regression was introduced by the q35 IRQ-routing rework
- the missing dispatch of guest IRQs above `15` was the specific bug
- the separate native Kvaser IRQ work is not the cause of the guest stall

Execution-policy correction on 2026-04-17:

- `qemu_x86_64_defconfig` already has an autopilot-backed remote-QEMU path
- the correct test profile is `qemu_x86_64_defconfig`
- the direct `tools/qemu_runner.py` path remains useful for backend debugging,
  but it should not be treated as the preferred validation path when autopilot
  is available

## Additional 2026-04-17 QEMU Disk-Boot Direction

The current x86 QEMU target now reaches `/init` and executes the initramfs, but
the existing `bridge-initramfs-init` boot contract is still wrong for this use
case:

- it assumes network-first boot
- it unconditionally tries to create the `br0` / `tap0` / `tap1` topology
- it prefers NFS, then eMMC-loop-mounted ext4 images
- it does not have a proper QEMU `/dev/vda` primary-root path

The current dirty initramfs-tree experiment of just doing:

```sh
mount -o rw /dev/vda /newroot
```

proved the right direction, but it is only a local hack. The proper
implementation direction is now:

- keep `bridge-initramfs-init` as the bootstrap entrypoint for QEMU targets
- add a real QEMU `/dev/vda` primary-root path there
- preserve the existing non-QEMU boot behavior for Orin/RPi/other hardware
- keep eMMC image boot as a fallback path, not the default QEMU contract

### Yocto artifact split

The first assumption that `vm-guest-images-install.bbclass` would solve the
external QEMU disk artifact was wrong.

Verified behavior:

- `vm-image-driver.bb` inherits `vm-guest-images-install`
- that class copies guest images from deploy output into the
  `vm-image-driver` rootfs at `do_rootfs` time
- it does not by itself emit the external deploy artifact that the x86 QEMU
  runner should attach as `/dev/vda`

The minimal Yocto refactor direction is therefore:

- keep `vm-guest-images-install` for nested guest images inside
  `vm-image-driver`
- explicitly make `vm-image-driver` itself produce a deployable QCOW2 disk
  artifact for QEMU use

### Runner/bundle implication

The cleanest bundling approach is to copy the chosen host-side disk image into
the build's `images/` directory and point the generated `simulate` script at
that relative path. That way:

- local QEMU runs keep working
- remote bundle export keeps working
- `qemu_runner.py` does not need a second ad hoc artifact path, because it
  already copies `build/images`

### Implemented checkpoint

Implemented in progress on 2026-04-17:

- `vm-image-driver` now has an explicit Yocto-side path to produce a deployable
  QCOW2 artifact for QEMU use
- `bridge-initramfs-init` now has a proper QEMU virtio-blk primary-root path in
  the clean override tree
- the x86 `vm_qemu_virtio` app now:
  - copies the chosen host-side driver disk into `qemu_x86_64_vm_qemu_virtio/images/`
  - writes `images/qemu-extra-args`
  - uses relative `images/...` paths for the QEMU host disk
- `tools/qemu_runner.py` now merges build-defined QEMU args from
  `build/images/qemu-extra-args`

Validated locally:

- `make qemu_x86_64_defconfig`
- `make vm_qemu_virtio`
- generated build output now includes:
  - `images/vm-image-driver-qemux86-64.rootfs.ext4`
  - `images/qemu-extra-args`
- `prepare-remote-bundle` produces a bundle whose `runtime/run-bundle.sh`
  includes:
  - `-enable-kvm`
  - safe user-mode networking for VM0
  - `-drive id=disk0,file=images/vm-image-driver-qemux86-64.rootfs.ext4,...`
  - `-device virtio-blk-pci,drive=disk0`

Current limitation:

- the local deploy directory does not yet contain
  `vm-image-driver-qemux86-64.rootfs.ext4.qcow2`, so the x86 app currently
  falls back to the existing raw ext4 artifact
- once the Yocto image is rebuilt with the new recipe wiring, the same app path
  should automatically switch to the QCOW2 artifact without further runner
  changes

### 2026-04-17 Yocto rebuild checkpoint

The first full `make linux-image` after the Yocto changes failed during recipe
parsing:

```text
ERROR: No recipes in default available for:
  .../meta-virtioso-sel4/recipes-core/images/vm-image-driver_%.bbappend
```

Root cause:

- `vm-image-driver_%.bbappend` was not accepted in this layer setup
- there was already a valid `vm-image-driver.bbappend` in the same directory

Fix:

- fold the `inherit vm-guest-image` change into the existing
  `meta-virtioso-sel4/recipes-core/images/vm-image-driver.bbappend`
- delete the invalid wildcard append file

Result after rerunning `make linux-image`:

- the build completes past the previous parse error
- deploy output now contains:
  - `vm-image-driver-qemux86-64.rootfs.ext4.qcow2`
  - the stable symlink `vm-image-driver-qemux86-64.rootfs.ext4.qcow2`
- rebuilding `make vm_qemu_virtio` automatically switches the packaged host
  disk from:
  - `images/vm-image-driver-qemux86-64.rootfs.ext4`
  to:
  - `images/vm-image-driver-qemux86-64.rootfs.qcow2`
- `images/qemu-extra-args` now uses:
  - `format=qcow2`

### 2026-04-17 Remote `qemu_pc99` QCOW2 test

Remote runtime was exercised on the laptop with:

- bundled driver disk:
  - `images/vm-image-driver-qemux86-64.rootfs.qcow2`
- QEMU host args:
  - `-netdev user,id=net0`
  - `-device virtio-net-pci,...`
  - `-drive id=disk0,file=images/vm-image-driver-qemux86-64.rootfs.qcow2,if=none,format=qcow2`
  - `-device virtio-blk-pci,drive=disk0`

The first bounded rerun failed because earlier remote QEMU runs were still
holding the QCOW2 image open:

```text
Failed to get "write" lock
Is another process using the image [images/vm-image-driver-qemux86-64.rootfs.qcow2]?
```

After stopping stale remote `qemu-system-x86_64` processes, the bounded rerun
showed:

- Linux boots past the earlier x86 timer/TSC stalls
- the guest reaches:
  - `Run /init as init process`
- the initramfs is executing and loads at least:
  - `tun: Universal TUN/TAP device driver, 1.6`

So the `qemu_pc99` image is now validated on the QCOW2-backed path at least up
to initramfs userspace execution. The remaining open question is later
userspace/root-switch behavior, not QEMU disk attachment or early boot.

### 2026-04-17 Initramfs marker follow-up

Added explicit `VIRTIOSO_INIT:` markers in the Yocto override for:

- initramfs entry
- QEMU virtio-blk boot attempt
- `/dev/vda` mount
- successful virtio-blk boot
- root switch

Result after rebuilding `make linux-image`, rebuilding `make vm_qemu_virtio`,
and rerunning the bounded remote `qemu_pc99` test:

- Linux still reaches:
  - `Run /init as init process`
- the new `VIRTIOSO_INIT:` markers do **not** appear in the captured serial log

Local artifact verification confirms the rebuilt boot image does contain the
instrumented `/init` script:

- `vm-image-boot-qemux86-64.rootfs.cpio.gz` contains:
  - `VIRTIOSO_INIT:`
  - `Attempting QEMU virtio-blk boot`
  - `Switching to rootfs via ${INIT_PATH}`

So the current state is:

- the boot image definitely contains the instrumented initramfs script
- the guest definitely reaches `/init`
- but the expected userspace marker output still does not show on the captured
  console

That makes the next investigation target narrower:

- either `/init` output is not reaching the captured serial path as expected
- or another init/console redirection path is taking over before those marker
  prints become visible

### 2026-04-17 Rootfs-side marker follow-up

The current clean initramfs override did not actually retain the stronger
rootfs-side persistence path that the investigation expected. To remove console
visibility from the critical path, the override now also:

- appends all `VIRTIOSO_INIT:` events to `/run/virtioso-initramfs.log`
- redirects stdout/stderr to `/dev/console` after `/dev` is mounted
- writes persistent marker records to:
  - `/newroot/var/log/virtioso-initramfs.log`

Two markers are now emitted specifically for the QEMU `/dev/vda` path:

- `virtio-blk-mounted`
- `switch-root-<init>`

The next validation step is:

- rebuild `make linux-image`
- rebuild `make vm_qemu_virtio`
- rerun `qemu_pc99` via autopilot
- inspect whether `/var/log/virtioso-initramfs.log` appears inside the QCOW2
  rootfs after the run

### 2026-04-17 Late-init correction and next discriminator

The previous diagnosis that the newer x86/QEMU image had regressed back to an
early ACPI/e820 stall was wrong.

Rechecking the full autopilot logs with NUL bytes stripped shows:

- both the current override-backed run (`20260417-122800`) and the temporary
  fallback-to-original-init run (`20260417-133526`) print:
  - `Trying to unpack rootfs image as initramfs...`
- the current override-backed run definitely reaches:
  - `Freeing unused kernel image (initmem) memory: 3540K`
- the fallback-to-original-init run reaches the same late-init region

What is still absent in the cleaned log tails:

- `Run /init as init process`
- `Starting init`

### 2026-04-18 q35 DMA/IOMMU checkpoint

The persistent q35 stop after:

- `virtio_blk virtio1: [vda] ...`
- `External IRQ badge irq=11`

was narrowed to the physical DMA path, not PCI interrupt routing.

First structural DMA slice:

- enabled `CONFIG_CAMKES_VM_GUEST_DMA_IOMMU` again for the x86 app
- added structural q35 physical IOSpace cap generation in:
  - `projects/vm/components/Init/templates/seL4PhysicalPCIIospaces.template.c`
- attached those generated IOSpace caps to the guest in `Init`
- added outer QEMU:
  - `-device intel-iommu,intremap=off,caching-mode=on`

The first run with that slice failed early and explicitly:

```text
Failed to map page into iospace
Failed to map generated physical PCI host bridge region at 0xb0000000 size 0x10000000
X86PageMapIO: Invalid page size
```

Root cause:

- `libsel4vm/src/guest_vspace.c` mirrored **all** later guest mappings into all
  attached iospaces once any IOSpace existed
- that incorrectly included uncached MMIO/device mappings such as the structural
  q35 PCI host apertures

Fix:

- only mirror **cacheable** mappings into guest iospaces
- keep uncached MMIO/device mappings out of the IOMMU path

Committed in `projects/sel4_projects_libs`:

- `72bd019` `x86: skip mmio iospace mirroring for dma`

This removed the explicit IOSpace mapping failure, but the old QEMU-side virtio
corruption symptom still remained:

```text
qemu-system-x86_64: Guest says index 15934 is available
```

That error comes from `sources/qemu/hw/virtio/virtio.c` when the device reads an
invalid avail-ring head index, so the remaining issue was still queue-addressing
/ DMA related.

#### Missing QEMU virtio IOMMU contract

Reviewing QEMU source showed the next missing piece:

- `intel-iommu` on the machine is not enough by itself
- QEMU only presents `VIRTIO_F_IOMMU_PLATFORM` / `ACCESS_PLATFORM` to the guest
  virtio driver when the individual virtio device has:
  - `iommu_platform=on`

Relevant source:

- `sources/qemu/hw/virtio/virtio-bus.c`

So the outer q35 virtio devices were updated to:

- `virtio-net-pci,...,iommu_platform=on`
- `virtio-blk-pci,...,iommu_platform=on`

#### Validation result

Autopilot request:

- `20260418-201515`

Result:

- the previous QEMU error
  - `Guest says index ... is available`
  disappeared
- boot now proceeds through the old q35 `vda` checkpoint
- the guest reaches and logs:
  - `Run /init as init process`
  - `VIRTIOSO_INIT: seL4 VM Unified Init`
  - `VIRTIOSO_INIT: Attempting QEMU virtio-blk boot`
  - `VIRTIOSO_INIT: Mounting /dev/vda on /newroot`
  - `VIRTIOSO_INIT: QEMU virtio-blk boot successful`
  - `VIRTIOSO_INIT: Switching to rootfs via /sbin/init`
- the rootfs mounts cleanly:
  - `EXT4-fs (vda): mounted filesystem ...`
- userspace continues into the driver rootfs:
  - `INIT: version 3.14 booting`
  - `Starting udev`
  - `sel4_virt: loading out-of-tree module taints kernel.`
  - `Configuring network interfaces... done`
  - `Poky ... driver-vm /dev/ttyS0`
  - `driver-vm login:`

So this checkpoint is the first end-to-end proof that the q35 physical PCI path,
driver disk, and initramfs handoff now work together on x86.

Known minor issue on this successful path:

- the initramfs persistence helper still tries to append to
  `/newroot/var/log/virtioso-initramfs.log` before that directory exists:

```text
/init: line 43: can't create /newroot/var/log/virtioso-initramfs.log: nonexistent directory
```

That does not block boot and should be treated as a cleanup item, not a bring-up
blocker.

### 2026-04-18 Q35 serial-console checkpoint

The x86 `qemu_pc99` runtime now forces the outer QEMU process headless and
without a VGA adapter:

- `-nographic`
- `-vga none`

Validated with autopilot run:

- `20260418-132302`

Result:

- the earlier bochs/fbcon takeover is gone
- the log no longer contains:
  - `bochs-drm ...`
  - `Console: switching to colour frame buffer device ...`
- the guest still reaches:
  - `virtio_blk virtio1: [vda] ...`
  - `External IRQ badge irq=11`

So the serial-visibility problem has been narrowed further:

- framebuffer console takeover was real
- removing it was necessary
- but it was not the whole reason late output disappeared

At this checkpoint, the live serial log still goes quiet immediately after the
`virtio_blk` probe plus IRQ delivery, even though the console remains on
`ttyS0`.

### 2026-04-18 Q35 ACPI PCI IRQ-routing checkpoint

The x86 guest command line had been carrying:

- `pci=nomsi,noacpi`

That was preventing the guest from using the ACPI PCI interrupt-routing path on
q35, which in turn made the earlier IRQ 11 diagnosis ambiguous. The current
baseline now:

- removes `noacpi` from the guest cmdline
- adds a minimal q35 `PCI0._PRT` in guest ACPI for the passed-through virtio
  slots:
  - `00:01.0` -> GSI `10`
  - `00:02.0` -> GSI `11`

Validated with autopilot run:

- `20260418-175606`

Observed result:

- guest command line now shows:
  - `pci=nomsi`
- Linux now explicitly reports:
  - `PCI: Using ACPI for IRQ routing`

This is an architectural correction even though the run did not yet reach the
later `virtio_blk` / `External IRQ badge irq=11` checkpoint within the captured
log. The earlier state where q35 physical INTx routing was being debugged while
`pci=noacpi` was still on the guest command line is no longer valid.

Next checkpoint from this baseline:

- rerun with ACPI PCI routing enabled
- confirm whether guest IOAPIC entry 11 is now programmed to a real vector
  instead of remaining `masked=1 vector=0`
- only then revisit the later `virtio_blk` / IRQ-delivery behavior

### 2026-04-18 q35 `PCI0._CRS` first slice

This slice continued the physical-bus rework rather than the guest boot work.

What changed:

- `projects/vm/components/Init/src/main.c`
  - q35 host-bridge detection now exposes a structural MMIO32 aperture in the
    shared host-bridge model:
    - `MCFG`: `0xb0000000..0xbfffffff`
    - MMIO32: `0xc0000000..0xfebfffff`
- `projects/sel4_projects_libs/libsel4vmmplatsupport/src/arch/x86/acpi.c`
  - first minimal DSDT AML generator added for `\_SB.PCI0`
  - emits `_HID`, `_CID`, `_UID`, `_BBN`, `_SEG`, `_CRS`
  - `_CRS` is sourced from the shared host-bridge model, not app-local BAR
    mappings
- `projects/util_libs/.../mcfg.h`
  - fixed local `acpi_mcfg_desc_t` layout so q35 `MCFG` now encodes
    `bus_start` / `bus_end` in the expected order

Validation:

- `make vm_qemu_virtio` succeeded after each step
- autopilot `20260418-001120` showed the first DSDT-backed attempt was not yet
  clean:
  - `ACPI Error: AE_NO_ACPI_TABLES, While loading namespace from ACPI tables`
  - `PCI: root bus 00: using default resources`
  - `pci_bus 0000:00: No busn resource found for root bus, will use [bus 00-ff]`
- autopilot `20260418-001552` after the `MCFG` struct fix now shows:
  - `PCI: ECAM [mem 0xb0000000-0xbfffffff] (base 0xb0000000) for domain 0000 [bus 00-ff]`
  - `PCI: Using host bridge windows from ACPI; if necessary, use "pci=nocrs" and report a bug`

Current state of the physical-bus rework:

- q35 `MCFG` is now structurally correct
- a first shared-model-backed `PCI0` DSDT path exists
- ACPICA still logs `AE_NO_ACPI_TABLES` during namespace load
- Linux still falls back to default root-bus resources, so `_CRS` is not yet
  taking full effect

Next step:

- make the DSDT/namespace load cleanly
- then re-check whether Linux stops printing:
  - `PCI: root bus 00: using default resources`
  - `No busn resource found for root bus`

### 2026-04-17 Modern-only virtio checkpoint

The x86 `qemu_pc99` path was switched from legacy-capable outer QEMU virtio
devices to modern-only devices:

- `virtio-net-pci,disable-legacy=on,disable-modern=off`
- `virtio-blk-pci,disable-legacy=on,disable-modern=off`

The legacy I/O-port workaround in the x86 app was removed at the same time:

- dropped `vm0.vm_ioports = [0xc000..0xc100]`

This clarified the architecture substantially:

- the old generic-passhrough failure on:
  - `Failed to get io port from simple for range 0xc000 - 0xc07f`
  - `Failed to get io port from simple for range 0xc080 - 0xc09f`
  was a legacy virtio BAR problem
- with modern-only outer devices, those legacy I/O-port failures disappear

The generic x86 runtime passthrough path also needed two separate fixes before
it could even attempt modern-only registration:

1. Wrong platform macro:
   - the code was gated on `CONFIG_PLAT_QEMU_PC99`
   - the actual build defines `CONFIG_PLAT_PC99`
2. Runtime IRQ provisioning:
   - truly dynamic IRQ allocation is not available in this generated x86
     CAmkES environment
   - `simple_get_IRQ_handler()` returns `seL4_FailedLookup`
   - `arch_simple_get_ioapic()` is not implemented
   - the current workaround is to reuse a statically declared IRQ slot:
     - `vm0.vm_irqs = [{ "name":"QemuPciIrq11", "ioapic":0, "source":11, "level_trig":1, "active_low":1, "dest":11 }]`

After those fixes, the generic runtime path does register the outer modern
virtio devices:

- `Auto passthrough PCI device bdf=00:03.0 vid=1af4 did=1000 irq=11`
- `Auto passthrough PCI device bdf=00:04.0 vid=1af4 did=1001 irq=11`

Current blocker with modern-only passthrough:

- BAR MMIO mapping for the first modern virtio device still fails before Linux
  guest enumeration:

```text
ut_alloc_iterator: Failed to allocate page
Failed to get frame for reservation address 0x8100000
Failed to map PCI bar 0xfebd1000 size 4096
Failed to map BARs for auto passthrough device 00:03.0
```

Important conclusion:

- switching to modern-only virtio is the correct cleanup direction for x86
  passthrough
- it removes the legacy port-I/O dependency cleanly
- the next issue is now guest/VMM memory headroom for passthrough MMIO BAR
  reservations, not legacy virtio support

Additional experiment on the same day:

- raising `vm0.simple_untyped22_pool` from `1` to `2` did **not** change the
  BAR mapping failure, so that tweak was reverted

### 2026-04-17 Modern MMIO guest-mapping bridge

The modern-only virtio BAR failure turned out not to be a QEMU BAR-discovery
problem. Runtime PCI scan was already giving the authoritative resource layout.
The real missing piece was CapDL device-frame provisioning for those MMIO BAR
pages.

The x86 app now predeclares the modern virtio MMIO BAR pages via
`vm0.guest_mappings`:

- `0xfebd1000` size `0x1000`
- `0xfebd2000` size `0x1000`
- `0xfe000000` size `0x4000`
- `0xfe004000` size `0x4000`

Autopilot run:

- request id: `20260417-205932`

Important evidence from the log:

- CapDL loader now creates device objects for those BAR pages instead of
  failing frame allocation later:
  - `frame_4k_device_(0xfebd1000,0x1000)`
  - `frame_4k_device_(0xfebd2000,0x1000)`
  - `frame_4k_device_(0xfe000000,0x1000)` through
    `frame_4k_device_(0xfe003000,0x1000)`
  - `frame_4k_device_(0xfe004000,0x1000)` through
    `frame_4k_device_(0xfe007000,0x1000)`
- Runtime PCI scan now shows the expected modern-only virtio device IDs:
  - `00:03.0 vid=1af4 did=1041`
  - `00:04.0 vid=1af4 did=1042`
- Runtime BAR layout is now visible and matches the guest-mapping set:
  - `00:03.0`: BAR1 `0xfebd1000 size 0x1000`, BAR4 `0xfe000000 size 0x4000`
  - `00:04.0`: BAR1 `0xfebd2000 size 0x1000`, BAR4 `0xfe004000 size 0x4000`

That proves the modern-MMIO bridge is the correct direction:

- generic auto-passthrough of outer QEMU PCI devices works
- static IRQ reuse works
- predeclared guest mappings solve the earlier MMIO BAR frame-cap failure

The new failure after this change is a plain caller-side bug in `Init`:

```text
vm0: main_continued@main.c:967 [Cond failed: error]
Failed to get guest map at 0
```

### 2026-04-18 Template-generated q35 physical PCI IRQ caps

The remaining app-local PCI IRQ description on x86 was:

- `vm0.vm_irqs = [{ ... source=11 ... dest=11 ... }]`

That is the wrong architectural seam for the structural q35 physical PCI path.
The first attempt to remove it and bind IOAPIC handlers at runtime failed in
autopilot run `20260418-102100`:

```text
X86EPTPageMap: Need a page directory first.
Failed to allocate runtime IRQ ioapic=0 pin=11 source=11 dest=11
Failed to bind qemu pci irqs
```

That proved the issue was not “manual IRQ numbers are unavoidable”, but “IRQ
handler caps cannot be created late on this path”.

The fix now follows the normal repo pattern used by `seL4VMIRQs.template.c`:

- added `projects/vm/components/Init/templates/seL4PhysicalPCIIrqs.template.c`
- wired it into `DeclareCAmkESVM(...)`
- exported generated lookup hooks via `camkes_vm_interfaces.h`
- taught `Init/main.c` to consult that generated physical-PCI IRQ table before
  attempting any runtime IOAPIC allocation

Current q35 scope:

- generate the physical PCI INTx handler actually needed by the outer modern
  virtio devices:
  - `ioapic=0`
  - `source=11`
  - `level_trig=1`
  - `active_low=1`
  - `dest=11`

The x86 app now keeps:

```text
vm0.vm_irqs = [];
```

Validation:

- `make vm_qemu_virtio` succeeded
- autopilot run `20260418-102638` restored the previous working checkpoint with
  no manual PCI IRQ entry:
  - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks ...`
  - `External IRQ badge irq=11`

Conclusion:

- yes, PCI passthrough IRQ caps should be template-generated here
- that matches the existing `Init` generation model much better than late
  runtime allocation
- manual app-local PCI IRQ description is no longer needed for the current q35
  physical passthrough slice

### 2026-04-18 Raw config as the primary q35 config path

The structural q35 path had already removed most synthetic device shaping, but
PCI config access still treated raw host config as a fallback behind the
`vmm_pci_*` device table.

That was the wrong center of gravity for physical q35 passthrough. The next
small architectural cleanup was:

- add `raw_config_primary_enabled` to `vmm_pci_space_t`
- add `vmm_pci_set_raw_config_primary(...)`
- in the x86 PCI config handlers, try raw host config first when that mode is
  enabled, and only fall back to the synthetic device table if raw access does
  not handle the request
- enable this mode for the structural q35 path in `Init/main.c`

This does not remove `vmm_pci_space_t` entirely, because the current path still
uses it for config-address bookkeeping and the remaining optional synthetic
surface. But it does move the physical q35 path closer to the intended model:

- physical bus/config first
- synthetic table only as compatibility scaffolding

Validation:

- `make vm_qemu_virtio` succeeded
- autopilot run `20260418-103920` preserved the same runtime checkpoint:
  - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks ...`
  - `External IRQ badge irq=11`

So the raw-primary change did not regress the current physical-q35 passthrough
behavior.

### 2026-04-18 Remove dead configured-device passthrough loop on structural q35

After the previous slices, the structural q35 app now generates:

- `pci_devices_num_devices() == 0`
- `irqs_num_irqs() == 0`

That means the old configured-device passthrough loop in `Init/main.c` had
become dead code on the physical q35 path. It still belonged to the older
synthetic/configured passthrough model:

- walk `pci_devices`
- resolve named IRQs from `vm_irqs`
- register passthrough devices via that configured surface

The structural q35 path now skips that loop entirely and uses only:

- generated physical PCI host apertures
- raw host config as the primary config path
- generated physical PCI IRQ caps
- runtime discovery of the outer QEMU virtio devices

Validation:

- `make vm_qemu_virtio` succeeded
- autopilot run `20260418-104407` still reached:
  - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks ...`
  - `External IRQ badge irq=11`

So the configured-device passthrough loop is no longer part of the structural
q35 physical PCI path.

### 2026-04-18 Let raw config own q35 bus 0

The next remaining synthetic dependency was in the x86 PCI config handler
itself. Even after the previous cleanup, bus-0 config cycles still went
through this sequence:

- decode config address
- try raw host config
- potentially fall back into `find_device(...)`

That was still too synthetic for the structural q35 physical-bus path. The
physical bus on `00:*` should be owned by the raw outer QEMU config mirror,
with the VMM PCI table only kept for synthetic devices elsewhere.

Implemented:

- added `raw_config_owns_bus0` in `vmm_pci_space_t`
- added `vmm_pci_set_raw_config_owns_bus0(...)`
- updated x86 PCI config handlers so that, when enabled, all bus-0 config
  reads/writes go directly to the raw host-config backend before any device
  table lookup
- enabled that mode for the structural q35 path in `Init/main.c`

Validation:

- `make vm_qemu_virtio` succeeded
- autopilot run `20260418-111415` still reached:
  - `PCI: Using host bridge windows from ACPI`
  - `pci_bus 0000:00: root bus resource [bus 00-ff]`
  - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks ...`
  - `External IRQ badge irq=11`

The log still goes quiet immediately after the `virtio_blk` probe, so this
architectural cut did not move the later runtime stop. But it does mean the
structural q35 physical path now treats bus 0 as raw host config space rather
than a synthetic device table.

### 2026-04-18 Add q35 `PCI0._OSC`

After the bus-0 ownership cleanup, the remaining ACPI fidelity gap was:

- `acpi PNP0A08:00: _OSC: platform retains control of PCIe features (AE_NOT_FOUND)`

QEMU's q35 root bridge provides `_OSC`, and our generated `PCI0` should do the
same. The missing method was not a passthrough requirement in itself, but it
was a concrete sign that the generated q35 root bridge was still less faithful
than the QEMU model.

Implemented:

- added a minimal AML helper to emit a method that returns one of its args
- added `PCI0._OSC` to the generated q35 DSDT
- the current method simply returns `Arg3`, which is enough to remove the
  `AE_NOT_FOUND` hole and let Linux complete normal `_OSC` negotiation

Validation:

- `make vm_qemu_virtio` succeeded
- autopilot run `20260418-112117` now shows:
  - `acpi PNP0A08:00: _OSC: OS supports [ExtendedConfig ASPM ClockPM Segments HPX-Type3]`
  - `acpi PNP0A08:00: _OSC: not requesting OS control; OS requires [ExtendedConfig ASPM ClockPM MSI]`
  - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks ...`
  - `External IRQ badge irq=11`

So the q35 root bridge is closer to the QEMU ACPI model than before, and the
`AE_NOT_FOUND` warning is gone. The later post-`vda` stop remains unchanged.

### 2026-04-18 Dedicated raw q35 PCI config io-port handler

Even after bus-0 raw ownership, the structural q35 path still installed the
generic x86 PCI config io-port handler:

- `vmm_pci_io_port_in`
- `vmm_pci_io_port_out`

That handler still contains the synthetic `find_device(...)` fallback path,
which is useful for emulated/synthetic PCI devices but not for the structural
q35 physical bus.

Implemented:

- added dedicated raw handlers:
  - `vmm_pci_raw_bus0_io_port_in`
  - `vmm_pci_raw_bus0_io_port_out`
- these handlers keep only:
  - the emulated config-address register (`0xcf8`)
  - direct raw bus-0 config reads/writes via the outer QEMU config mirror
- structural q35 now installs those raw handlers instead of the generic
  `vmm_pci_io_port_*` path

Validation:

- `make vm_qemu_virtio` succeeded
- autopilot run `20260418-113748` still reached:
  - `PCI: Using host bridge windows from ACPI`
  - `acpi PNP0A08:00: _OSC: OS supports [...]`
  - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks ...`
  - `External IRQ badge irq=11`

So the structural q35 physical path no longer routes PCI config-space traffic
through the generic synthetic/emulated handler. The later post-`vda` stop
remains unchanged.

### 2026-04-18 Separate raw PCI config state from `vmm_pci_space_t`

The dedicated raw q35 io-port handler was still using `vmm_pci_space_t` as its
cookie, which meant the physical PCI path still depended on the synthetic PCI
space even for basic config-address register bookkeeping.

Implemented:

- introduced `vmm_pci_raw_io_space_t`
  - `conf_port_addr`
  - raw outer PCI config backend
- added `vmm_pci_raw_io_space_init(...)`
- updated the raw q35 io-port handler to use that standalone raw state object
  rather than `vmm_pci_space_t`
- structural q35 `Init/main.c` now allocates and uses that raw state directly

Validation:

- `make vm_qemu_virtio` succeeded
- autopilot run `20260418-114741` still reached:
  - `PCI: Using host bridge windows from ACPI`
  - `acpi PNP0A08:00: _OSC: OS supports [...]`
  - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks ...`
  - `External IRQ badge irq=11`

So the structural q35 physical path now has a genuinely separate raw PCI config
state object. `vmm_pci_space_t` remains only for synthetic/emulated PCI users
and any later optional emulated-device integrations, not for the physical bus
config path itself.

### 2026-04-18 Skip synthetic PCI space when unused

After splitting the raw config state, the current structural q35 app still
initialized `vmm_pci_space_t` even though the live configuration had:

- `init_cons_num_connections() == 0`
- no configured `pci_devices`
- no configured `vm_irqs`
- no active cross-VM initializer using the PCI space

That meant the synthetic PCI space had become dead state on the current
physical-bus path.

Implemented:

- added `physical_q35_needs_synthetic_pci_space(...)`
- structural q35 now skips `vmm_pci_init*()` entirely when there are no
  synthetic/emulated PCI consumers
- the current app therefore runs with:
  - no synthetic PCI space allocation
  - separate raw PCI config state only
  - structural host apertures
  - generated physical IRQ caps

Validation:

- `make vm_qemu_virtio` succeeded
- autopilot run `20260418-121426` still reached:
  - `PCI: Using host bridge windows from ACPI`
  - `acpi PNP0A08:00: _OSC: OS supports [...]`
  - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks ...`
  - `External IRQ badge irq=11`

So on the current x86 `vm_qemu_virtio` q35 path, the physical PCI bus no
longer depends on `vmm_pci_space_t` at all. That state is now fully optional
and only needed for later synthetic/emulated PCI users.

### 2026-04-18 Remove dead synthetic-helper branches

After the previous slices, the current structural q35 path no longer used:

- `physical_q35_pci_uses_raw_config_mirror(...)`
- the q35-specific raw-config branch inside `register_physical_pci_device(...)`

Those only existed to support an older hybrid model where structural q35 still
shared code paths with synthetic passthrough registration.

Implemented:

- removed `physical_q35_pci_uses_raw_config_mirror(...)`
- simplified `register_physical_pci_device(...)` back to its actual role:
  synthetic passthrough device registration only
- simplified the auto-passthrough logging to reflect that this path is now
  purely synthetic/non-structural

Validation:

- `make vm_qemu_virtio` succeeded
- autopilot run `20260418-125709` still reached:
  - `PCI: Using host bridge windows from ACPI`
  - `acpi PNP0A08:00: _OSC: OS supports [...]`
  - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks ...`
  - `External IRQ badge irq=11`

So the live structural q35 path no longer shares dead helper branches with the
older synthetic passthrough model.

### 2026-04-18 Bind physical PCI IRQs from generated metadata

The remaining structural q35 runtime mechanism was `auto_bind_qemu_pci_irqs()`,
which scanned the outer QEMU PCI bus just to rediscover INTx routing that the
generated physical-PCI IRQ template already knew.

Implemented:

- replaced `auto_bind_qemu_pci_irqs()` with `bind_generated_physical_pci_irqs()`
- structural q35 now binds IRQ handlers directly from:
  - `physical_pci_irqs_num_irqs()`
  - `physical_pci_irqs_get_irq(...)`
- the current q35 path no longer scans outer QEMU devices merely to learn which
  INTx line to bind

Validation:

- `make vm_qemu_virtio` succeeded
- autopilot run `20260418-130434` still reached:
  - `PCI: Using host bridge windows from ACPI`
  - `acpi PNP0A08:00: _OSC: OS supports [...]`
  - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks ...`
  - `External IRQ badge irq=11`

So the q35 physical IRQ binding is now declarative/generated rather than
runtime-scanned.

The generated `seL4GuestMaps.template.c` returns:

- `1` on success
- `0` on failure

but `main.c` was checking it backwards:

```c
error = guest_mappings_get_guest_map(i, &frame_paddr, &size);
ZF_LOGF_IF(error, "Failed to get guest map at %d\n", i);
```

So the next fix is simply to invert that failure check and rerun the same
modern-only x86 path.

### 2026-04-17 Guest-mapping caller fix result

The `guest_mappings_get_guest_map()` caller bug in `Init` was fixed by
inverting the success/failure check:

```c
error = guest_mappings_get_guest_map(i, &frame_paddr, &size);
ZF_LOGF_IF(!error, "Failed to get guest map at %d\n", i);
```

Autopilot rerun:

- request id: `20260417-210459`

What this proved:

- the earlier `Failed to get guest map at 0` failure is gone
- generic auto-passthrough still registers the outer modern virtio devices
- the guest reaches late init and definitely runs `/init`
- the initramfs instrumentation is now visible on autopilot, so the old
  uncertainty about missing tail lines no longer applies here

Visible markers in the latest log:

- `Run /init as init process`
- `VIRTIOSO_INIT: seL4 VM Unified Init`
- `VIRTIOSO_INIT: Mounting essential filesystems`
- `VIRTIOSO_INIT: Detected platform: qemu`
- `VIRTIOSO_INIT: Loading platform drivers for qemu`
- `VIRTIOSO_INIT: QEMU target: expecting virtio devices to enumerate automatically`

### 2026-04-17 Architecture plan: replace synthetic x86 PCI bus with physical-bus passthrough

This section is now the persistent architecture tracker for the x86 `qemu_pc99`
PCI rework. Update this section first when implementation status changes, so the
current plan and checkpoint history survive context loss.

#### Objective

Make x86 `qemu_pc99` expose the physical QEMU PCI root bus to the inner guest in
the same architectural sense that ARM `qemuarm64` exposes the physical
`/pcie@10000000` host bridge:

- preserve the physical host-bridge model
- preserve host BDFs and BAR numbering
- reserve and pass the structural QEMU PCI apertures
- keep synthetic/emulated PCI on a separate path

The current synthetic x86 PCI bus work was useful as a diagnostic bridge, but it
is not the intended end-state for physical-bus passthrough.

#### Verified current-state facts

ARM:

- `apps/Arm/vm_qemu_virtio/qemu-arm-virt/devices.camkes` reserves broad QEMU PCI
  windows rather than only per-device BARs
- it also keeps the original QEMU DT node:
  - `{"path": "/pcie@10000000"}`
- QEMU ARM `virt` machine constants in `sources/qemu/hw/arm/virt.c` are the
  authority for those windows:
  - `VIRT_PCIE_MMIO = { 0x10000000, 0x2eff0000 }`
  - `VIRT_PCIE_PIO = { 0x3eff0000, 0x00010000 }`
  - `VIRT_PCIE_ECAM = { 0x3f000000, 0x01000000 }`
- ARM `vpci` is a separate synthetic path for emulated PCI devices; it does not
  create `/pcie@10000000`

x86:

- QEMU x86 ACPI is the authority for the physical root-bridge description:
  - `sources/qemu/hw/i386/acpi-build.c`
  - `sources/qemu/hw/acpi/aml-build.c`
- current x86 passthrough is still routed through synthetic `vmm_pci_*`
  insertion and app-local guest mappings
- preserving original BAR numbering in
  `libsel4vmmplatsupport/src/arch/x86/drivers/vmm_pci_helper.c` was necessary
  for modern virtio capability fidelity and must be kept

#### Target architecture

1. Common physical PCI host-bridge model

- Create a shared/common representation of the physical QEMU PCI host bridge.
- It must live outside `x86/` and `arm/` directories so it can feed:
  - current ARM DT physical-bus rendering
  - x86 ACPI physical-bus rendering
  - future ARM ACPI if needed
- The model should carry at least:
  - bus range
  - config/ECAM aperture
  - PIO aperture
  - MMIO32 aperture
  - MMIO64 or prefetch aperture
  - interrupt-routing metadata

2. Explicit split: physical bus vs synthetic/emulated PCI

- Physical path:
  - preserve the QEMU root bus structurally
  - preserve host BDFs and BAR numbering
  - reserve and map the QEMU PCI apertures
  - render guest metadata from the common host-bridge model
- Synthetic path:
  - emulated devices only
  - keep separate from physical-bus exposure
- On ARM, this matches the existing distinction between:
  - physical `/pcie@10000000`
  - synthetic `pci` node created by `vpci`
- On x86, this means the current synthetic `vmm_pci_*` path must stop being the
  primary physical-bus mechanism

3. QEMU-derived authority, not hardcoded per-device guesses

- Use QEMU machine sources as the platform authority:
  - ARM: `sources/qemu/hw/arm/virt.c`
  - x86: `sources/qemu/hw/i386/acpi-build.c` and
    `sources/qemu/hw/acpi/aml-build.c`
- Use runtime PCI scan only for dynamic device assignment details inside those
  apertures, not for inventing a new guest topology

#### Planned migration slices

Slice A: freeze what is worth keeping

- Keep:
  - modern-only outer virtio QEMU args
  - BAR-number preservation and sparse-BAR handling fixes
  - unrelated x86 boot/timer/cpuid fixes
- Retire later:
  - synthetic guest-bus insertion as the primary physical path
  - app-local guest mappings as the long-term physical-bus mechanism

Slice B: build common host-bridge model

- Add a shared/common data model for physical PCI host bridges
- Populate it from:
  - ARM `virt.c`
  - x86 ACPI/QEMU root-bridge logic
- Keep the first implementation scoped to `qemuarm64` and `qemu_pc99`

Slice C: migrate ARM physical path onto the common model

- Keep behavior unchanged
- Replace duplicated ARM window constants with values emitted from the common
  model
- Leave ARM synthetic `vpci` untouched, but explicitly scoped to emulated
  devices only

Slice D: add x86 ACPI physical-bus renderer

- Emit the x86 physical root-bridge description from the common model
- Stop relying on generic/default ACPI root-bus resources for `qemu_pc99`
- Provide the structural PCI apertures, not just per-device BAR exceptions

Slice E: remove synthetic x86 PCI bus from the physical path

- Revert synthetic `vmm_pci_*` insertion for outer QEMU physical devices
- Keep synthetic PCI support only for emulated-device cases
- Remove x86 app-local physical guest mappings once structural aperture
  provisioning replaces them

#### Progress tracker

Status legend:

- `done`: implemented and validated enough to keep
- `in_progress`: active migration work
- `pending`: planned but not started
- `drop`: temporary scaffolding to remove once the structural path exists

| Item | Status | Notes |
| --- | --- | --- |
| Modern-only outer virtio devices | done | Keep as part of the end-state |
| Preserve original BAR numbering | done | Required for modern virtio capability fidelity |
| Sparse BAR handling | done | Keep with BAR-number preservation |
| Synthetic x86 fixed-BDF insertion | drop | Debugging bridge only; not the final physical-bus path |
| X86 app-local guest BAR mappings | drop | Temporary cap-provision bridge; replace with structural aperture provisioning |
| Common physical PCI host-bridge model | pending | New shared/common code, not under arch-specific dirs |
| ARM physical-bus renderer from common model | pending | Behavior-preserving refactor |
| X86 ACPI physical-bus renderer from common model | pending | `qemu_pc99` first |
| Synthetic PCI split from physical path | in_progress | Architecture agreed; code still mixed on x86 |
| Revert synthetic x86 PCI as primary path | pending | Depends on structural x86 physical-bus path landing |

#### Implementation notes for the next execution phase

- Do not delete the current synthetic x86 bridge immediately; keep it only long
  enough to compare logs and preserve rollback.
- The first new code should be common-model code, not more x86-local
  passthrough hacks.
- When execution resumes, append each checkpoint here with:
  - date
  - affected repos/files
  - commit SHAs
  - what moved from `pending` to `in_progress` or `done`

### 2026-04-17 First common-model implementation slice

The first architectural code slice is now in place.

Affected repos/files:

- `projects/sel4_projects_libs`
  - `libsel4vmmplatsupport/include/sel4vmmplatsupport/pci_host_bridge.h`
  - `libsel4vmmplatsupport/src/pci_host_bridge.c`
- `projects/virtioso-camkes-vm`
  - `src/mmio_reservations.c`

What landed:

- Added a new shared/common physical PCI host-bridge descriptor API in
  `libsel4vmmplatsupport`, not under an arch-specific directory
- The API currently provides:
  - a neutral host-bridge data model
  - a QEMU ARM `virt` physical-host-bridge initializer sourced from the known
    QEMU aperture layout
  - a generic QEMU PC/x86 initializer that can later be fed from dynamic
    `_CRS` or machine-derived aperture data
- `virtioso-camkes-vm/src/mmio_reservations.c` now recognizes structural PCI
  host apertures explicitly, rather than relying only on the older "skip huge
  MMIO windows" heuristic

Current scope and limitation:

- The first consumer is ARM-side only because those physical aperture values
  are already explicit and stable
- X86 still needs the dynamic physical root-bus builder fed from QEMU x86
  machine/ACPI authority; this slice intentionally stops short of hardcoding
  x86 aperture guesses
- The current synthetic x86 PCI bus remains in place for now

Validation:

- `make vm_qemu_virtio` succeeded for the current `qemu_pc99` build tree after
  landing the common-model API and the first consumer

Progress-table updates after this slice:

- `Common physical PCI host-bridge model`: `in_progress`
- `ARM physical-bus renderer from common model`: still `pending`, but now has
  the first shared data source
- `Synthetic PCI split from physical path`: still `in_progress`

### 2026-04-17 X86 machine-model correction and common-model extension

The current `qemu_pc99` path is not launching `q35`. The generated
`images/qemu-extra-args` only adds modern virtio devices and networking, and
the generated `simulate` script does not pass `-machine`, so the outer QEMU
machine is following the default PC/i440fx path.

Verified authority:

- `sources/qemu/hw/i386/acpi-build.c` already has separate root-bridge logic for:
  - i440fx `PNP0A03`
  - q35 `PNP0A08` with `PNP0A03` compatibility
- `sources/qemu/hw/pci-host/i440fx.c` exposes the same dynamic hole properties
  that `acpi-build.c` consumes for `_CRS`
- `sources/qemu/hw/pci-host/q35.c` does the same for the q35 path

Architectural consequence:

- the common host-bridge model must represent:
  - CAM config ports for x86 PC/i440fx and q35
  - optional MCFG/MMCONFIG separately
  - dynamic 32-bit and 64-bit PCI windows supplied by the machine model

Implemented extension:

- `libsel4vmmplatsupport` common host-bridge model now carries:
  - `config_region`
  - `mcfg_region`
  - `io_region`
  - `mem32_region`
  - `mem64_region`
- added dedicated initializers for:
  - `qemu_pc_i440fx`
  - `qemu_pc_q35`

This is still scaffolding, not the final x86 consumer. The next x86 slice is
to feed actual i440fx root-bus aperture values into ACPI or another structural
consumer instead of continuing to route physical devices through synthetic
`vmm_pci_*` insertion.

### 2026-04-17 Outer machine switched to q35

Decision:

- stop using the default PC/i440fx outer QEMU machine for the `qemu_pc99`
  runtime path
- force the generated x86 runtime args to use:
  - `-machine q35`

Reasoning:

- the long-term architecture target is a structural physical PCI root bus,
  closer to ARM `qemuarm64` PCIe than to legacy PCI/PIIX
- `q35` is the better fit for:
  - modern-only virtio devices
  - optional MCFG/MMCONFIG
  - a cleaner PCIe-root-bus model

Current expectation:

- this may invalidate some current x86 passthrough assumptions that were learned
  under the old default i440fx machine
- in particular, BAR placements, BDFs, and current guest-mapping bridge values
  may change

The next checkpoint must therefore rebuild and rerun the x86 image under the
new outer machine model before any more x86 structural assumptions are added.

Validation result:

- rebuilt `make vm_qemu_virtio`
- confirmed generated `images/qemu-extra-args` now starts with:
  - `-machine q35`
- autopilot request: `20260417-232313`

Observed runtime effects from the current log:

- the new `q35` outer machine launches successfully
- Linux guest still reaches late init
- ACPI is active in the guest
- `virtio_pci_driver_init` probes:
  - `0000:00:02.0`
  - `0000:00:03.0`
- `virtio_blk` still binds and exposes:
  - `vda`

So the outer-machine switch itself is viable. The remaining x86 work now needs
to treat `q35` as the active structural target instead of the earlier i440fx
compatibility baseline.

### 2026-04-17 Failed cleanup probe: removing hardcoded host BAR frame mappings

Experiment:

- temporarily removed the x86 app-local `vm0.guest_mappings` entries for:
  - `0xfebd1000`
  - `0xfebd2000`
  - `0xfe000000`
  - `0xfe004000`
- rebuilt `make vm_qemu_virtio`
- reran under the current `q35` outer-machine baseline

Autopilot request:

- `20260417-232805`

Result:

- the run immediately regressed in the old host-side MMIO frame-cap path:
  - `Failed to map PCI bar 0xfebd1000 size 4096`

Meaning:

- switching the outer machine to `q35` changes the guest-visible BAR placement
  and overall PCI topology, but it does **not** remove the need for host-side
  frame caps backing the outer virtio BAR pages
- the current app-local `guest_mappings` are still acting as a host-MMIO
  frame-cap bridge, even though the guest later sees BARs assigned at
  `0x0810....`

Action taken:

- restored the previous `guest_mappings` immediately to keep the build tree in a
  working configuration

Architectural consequence:

- the next real cleanup target is **not** "delete guest_mappings"
- it is "replace app-local host BAR frame declarations with structural MMIO-cap
  provisioning for discovered passthrough devices"
- `VIRTIOSO_INIT: Loading TUN module`
- `VIRTIOSO_INIT: Attempting QEMU virtio-blk boot`

Current blocker after this fix:

- the log now stops exactly after:
  - `VIRTIOSO_INIT: Attempting QEMU virtio-blk boot`
- no later marker appears for:
  - `QEMU virtio-blk boot successful`
  - `Switching to rootfs`
- no guest-visible block-device lines appear for:
  - `virtio_blk`
  - `vda`

So the modern MMIO passthrough path is now good enough to boot into the
initramfs, but the remaining issue is guest-side block-device visibility or the
exact wait/mount path around `/dev/vda`, not PCI BAR mapping anymore.

### 2026-04-17 ARM comparison and BAR-numbering correction

Rechecking the known-good ARM `qemu_arm64_defconfig` path confirmed the user's
objection about "exact PCI space" was materially right.

Verified ARM behavior:

- `projects/vm/components/VM_Arm/src/modules/pci.c` installs a full virtual PCI
  host bridge with `vm_install_vpci(...)`
- `projects/virtioso-camkes-vm/apps/Arm/vm_qemu_virtio/qemu-arm-virt/user-vm.dts`
  gives the guest the whole PCI host-bridge shape:
  - ECAM config region
  - PCI IO range
  - PCI MMIO ranges
  - interrupt map
  - MSI parent
- `projects/virtioso-camkes-vm/apps/Arm/vm_qemu_virtio/qemu-arm-virt/devices.camkes`
  reserves the broad QEMU PCI windows, not just per-device BAR pages

So the important x86 lesson is:

- the guest must see a faithful PCI device/configuration model
- modern virtio is especially sensitive because its vendor capabilities point to
  specific BAR numbers

The x86 passthrough bug turned out to be exactly there:

- `vmm_pci_helper_map_bars()` in
  `libsel4vmmplatsupport/src/arch/x86/drivers/vmm_pci_helper.c`
  was compacting host BARs down to guest BAR0/BAR1/...
- for modern virtio this is wrong, because the capability structures still
  refer to the original host BAR numbers (for example BAR1 and BAR4)
- that explains the earlier Linux behavior:
  - devices enumerated
  - `virtio-pci ... leaving for legacy driver`
  - no `/dev/vda`

The x86 helper was corrected to preserve the original BAR numbering, and then a
second sparse-BAR fix was needed so empty intermediate BAR slots no longer hit
an assertion in `pci_make_bar()`.

Autopilot runs:

- `20260417-213800`
  - first BAR-numbering attempt
  - moved the failure forward immediately
  - guest no longer printed `leaving for legacy driver`
  - but `pci_make_bar()` asserted because sparse BAR slots were now possible
- `20260417-214142`
  - after sparse-BAR handling fix
  - guest now binds modern virtio-blk successfully:
    - `virtio_blk virtio1: 1/0/0 default/read/poll queues`
    - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks ...`

Important conclusion:

- the original "modern virtio only" direction was correct
- the user's concern about exact PCI exposure was also correct
- the decisive x86 bug was BAR renumbering in the passthrough helper, not the
  absence of one more ad hoc guest-mapping kludge

Open point after `20260417-214142`:

- autopilot still returned overall `fail`
- the captured log clearly shows `vda` enumeration, but not yet the later
  `/init` markers in that specific run
- so the next step is no longer PCI capability fidelity; it is checking why the
  run terminated before or without the later initramfs-visible phase
- `No working init found`
- `Kernel panic`

### 2026-04-18 HPET and post-`vda` checkpoint

The q35 physical-PCI path now has working ACPI PCI IRQ routing for the
passed-through virtio-blk device.

Validated on autopilot run `20260418-184103`:

- guest uses ACPI PCI IRQ routing
- `virtio_blk` binds and exposes `vda`
- guest IOAPIC redirection entry 11 is programmed and serviced correctly
- the full external IRQ path completes:
  - `External IRQ badge irq=11`
  - `ioapic inject irq=11 ... masked=0 vector=33 level=1`
  - `ioapic service irq=11 ...`
  - `ioapic notify eoi irq=11 ...`
  - `IRQ ack handler irq=11 ...`

So the earlier `irq=11` stop was not an EOI/ack problem.

#### HPET fidelity fix

Comparing local HPET emulation with `sources/qemu/hw/timer/hpet.c` showed that
`projects/vm/components/Init/src/hpet.c` was still missing QEMU's widened
comparator state:

- QEMU maintains `cmp64` and arms from that widened counter-width comparator
- local code re-armed directly from partially written `cmp`
- that can produce pathological near-immediate expiries and the earlier
  `CE: hpet ...` failure mode

Implemented local fix in `projects/vm/components/Init/src/hpet.c`:

- add `cmp64` and `last` to `HPETTimer`
- add `hpet_get_ns()`
- add `hpet_calculate_cmp64()`
- add `hpet_next_wrap()`
- add `hpet_arm()`
- make `hpet_set_timer()` and `hpet_timer()` operate on widened comparator
  state instead of directly on transient `cmp` writes

Autopilot run `20260418-190856` after that fix showed:

- boot still reaches the `virtio_blk` / `vda` checkpoint
- the previous immediate HPET failure mode is reduced substantially
- the first visible `CE: hpet increased min_delta_ns ...` complaint occurs much
  later than before
- the earlier immediate storm of
  `CE: Reprogramming failure. Giving up`
  is no longer the first post-`vda` symptom

#### `hpet=disable` control

Temporary control experiment:

- add `hpet=disable` to the guest cmdline
- autopilot run `20260418-192954`

Control result:

- the visible `CE: hpet ...` complaint disappears
- guest reaches the same practical checkpoint much faster:
  - `virtio_blk virtio1: [vda] ...`
  - first external IRQ/EOI/ack on irq 11
- but the guest still stalls after that same practical point and does not reach
  `/init`

Current interpretation:

- HPET was a real problem, but it is not the only remaining blocker
- disabling guest HPET alone does not get the boot past the first block-device
  interrupt path
- the remaining stall is now more likely in code immediately after the first
  virtio-blk interrupt completion, not in the old PCI IRQ-routing path itself

So the current problem is not an early-boot regression. The remaining unknown
is the last boundary between late kernel init and visible userspace handoff.

The next discriminator is a control run that bypasses the current initramfs
script logic entirely:

- keep the same initramfs image
- change the x86 guest command line to:
  - `keep_bootcon`
  - `rdinit=/bin/sh`

If that still fails to show a userspace shell on the serial console, the
remaining issue is not the current `/init` script. If it succeeds, the problem
is inside the initramfs script path or its console handling.

### 2026-04-17 `rdinit=/bin/sh` control result

A control build was made by changing only the x86 guest command line to:

- `keep_bootcon`
- `rdinit=/bin/sh`

while keeping the same initramfs image and QEMU/rootfs path.

Autopilot run:

- request id: `20260417-135917`

Observed in the cleaned log:

- command line contains `keep_bootcon` and `rdinit=/bin/sh`
- the guest still prints:
  - `Trying to unpack rootfs image as initramfs...`
- no visible shell banner or prompt appears
- no visible:
  - `Run /init as init process`
  - `Starting init`
  - `No working init found`
  - `Kernel panic`

The control run did continue much longer than the fixed-window checks suggested:

- the cleaned tail reaches roughly `27s-28s` of late init activity

Current interpretation:

- this is not obviously a bug inside the current initramfs `/init` script
- even a direct `rdinit=/bin/sh` control does not produce visible userspace on
  the captured serial console
- the next unknown is now the boundary between late kernel init and visible
  userspace/console handoff, not the script logic itself

Important logging note from manual and autopilot runs:

- with `keep_bootcon` enabled, kernel lines are expected to appear duplicated
  for a long stretch because both the boot console and `ttyS0` stay active
- this duplication is therefore not, by itself, evidence that autopilot is
  duplicating lines
- autopilot may still miss some trailing bytes around timeout/teardown, but the
  absence of a shell prompt is not explained by the duplicated log lines alone

## Implemented Repo Changes

Committed in `projects/virtioso-camkes-vm`:

- `dc9f3bd` `x86: add qemu pc99 vm_qemu_virtio build path`
- `9263478` `x86: tune single-vm image footprint`

Main changes:

- Added x86 app path:
  - `apps/x86/vm_qemu_virtio/app_settings.cmake`
  - `apps/x86/vm_qemu_virtio/CMakeLists.txt`
  - `apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes`
- Added x86 routing in:
  - `settings.cmake`
  - `Findvirtioso-camkes-vm.cmake`
  - `CMakeLists.txt`

Current x86 app is intentionally a single-VM bring-up path. The full virtioso
two-VM topology and zero-copy SWIOTLB path are not ported yet.

## Current `vm_qemu_virtio` X86 Runtime State

Build:

- `make qemu_x86_64_defconfig`
- `make vm_qemu_virtio`

Result:

- Builds successfully.
- Produces:
  - `qemu_x86_64_vm_qemu_virtio/images/capdl-loader-image-x86_64-pc99`

Runtime command used:

```bash
python3 projects/virtioso-camkes-vm/tools/qemu_runner.py run-remote \
  --target qemu_x86_64_defconfig \
  --binary /home/hlyytine/tii-sel4/qemu_x86_64_vm_qemu_virtio/images/capdl-loader-image-x86_64-pc99
```

Observed runtime:

- seL4 boots
- CapDL Loader completes
- failure occurs later in `vm0` bootstrap:

```text
vm0: vka_alloc_object_at_maybe_dev@object.h:57 Failed to allocate object of size 4096, error 1
vm0: alloc_and_map@bootstrap.c:102 Failed to allocate bootstrap frame, error: 1
```

This is the current blocker for the x86 port.

## Additional 2026-04-16 Late Progress

Two more early-x86 firmware gaps were isolated and fixed after adding EPT-side
address logging.

### Fix 4: back the BIOS Data Area page

With EPT diagnostics enabled, the first post-entry guest fault was:

```text
EPT fault: gpa=0x413 ... read=1 size=2
```

That address is in the BIOS Data Area conventional-memory word. Linux reached
real kernel entry and immediately consulted low firmware state that the VMM had
reported as reserved in e820 but had not actually backed with guest memory.

Fix implemented in `projects/sel4_projects_libs`:

- map a synthetic guest page at `0x0`
- populate BDA fields:
  - EBDA segment at `0x40e`
  - conventional-memory size word at `0x413`

### Fix 5: back the EBDA page

After the BDA fix, the next fault was:

```text
EPT fault: gpa=0x9fc00 ... instruction dump: 81 38 5f 4d 50 5f
```

This is Linux probing the EBDA for the `_MP_` floating-pointer structure. The
guest was still faulting because the reserved EBDA page was not backed.

Fix implemented in `projects/sel4_projects_libs`:

- map a zeroed synthetic guest page at `0x9f000`
- point the BDA EBDA segment at `0x9fc0`

### Runtime state after BDA + EBDA fixes

`qemu_pc99` `vm_qemu_virtio` now progresses substantially further:

- Linux kernel banner prints
- command line is consumed correctly
- BIOS e820 is parsed
- ACPI tables are discovered from the synthetic low BIOS region
- LAPIC / HPET MMIO faults are handled and guest boot continues
- memory zones, percpu, slab, RCU, IRQ, APIC, and console init all start

Latest observed progress reaches:

```text
[    0.004000] tsc: Unable to calibrate against PIT
[    0.004000] tsc: HPET/PMTIMER calibration failed
[    0.004000] tsc: Marking TSC unstable due to could not calculate TSC khz
```

At that point the guest no longer hard-crashes; the current question is whether
it is merely slow/stalled in later early boot or needs another platform/x86
firmware fix.

### Current local diagnostic state

There is still an uncommitted diagnostic patch in:

- `projects/sel4_projects_libs/libsel4vm/src/arch/x86/ept.c`

Purpose:

- print the exact guest physical address, access type, decoded operand size,
  and RIP for each EPT violation

This diagnostic patch was essential for identifying the BDA and EBDA faults and
may be kept temporarily until the next runtime blocker is isolated.

## Additional 2026-04-16 Timer / Calibration Progress

The original suspicion after the early Linux bring-up was that PIT or HPET
emulation might simply not be running. That is no longer the case.

### Finding 1: PIT timer callbacks are firing continuously

With temporary `ZF_LOGE` diagnostics in `projects/vm`, the x86 VMM now shows:

- `init_timer_completed()` returns `BIT(TIMER_PIT)` repeatedly
- `pit_timer_interrupt()` fires
- `pit_irq_timer_update()` toggles IRQ0 level continuously

This rules out "dead PIT timer scheduling" as the reason Linux stalls during
`tsc` calibration.

### Finding 2: HPET main counter / timer path is active

Additional HPET diagnostics show:

- Linux probes and enables HPET through the expected MMIO registers
- HPET timer 0 is programmed in legacy mode
- HPET callbacks fire periodically

An HPET emulation bug was found during this stage:

- guest writes the main counter to `0`
- the old implementation used an unsigned offset against host uptime
- enabling HPET therefore made the guest-visible counter jump to "host uptime"
  instead of starting from the guest-programmed value
- that collapsed comparator setup toward immediate expiry

Fix applied in `projects/vm`:

- make `hpet_offset` signed
- preserve the guest-programmed counter base when HPET is enabled

After this fix, HPET timer setup no longer collapses to `diff=1`; timer 0 now
arms with a sensible delta close to the guest-programmed period.

### Finding 3: PIT channel-2 speaker port was missing

Linux uses PIT channel 2 via port `0x61` during TSC calibration.

This repo previously had:

- PIT channels at `0x40..0x43`
- no emulation for the PC speaker / PIT channel-2 gate port at `0x61`

Fix applied in `projects/vm`:

- add port `0x61` emulation
- reflect PIT channel-2 OUT on bit 5
- drive PIT channel-2 gate from bit 0

Verified runtime behavior after this fix:

- Linux reads `0x61`
- Linux writes `0x21`
- channel 2 is programmed in mode 0
- guest polls bit 5 and sees it transition from `0` to `1`

This rules out "missing speaker gate / channel-2 OUT status" as the remaining
cause of calibration failure.

### Finding 4: PIC interrupt delivery is functioning

Additional diagnostics in `projects/sel4_projects_libs` show:

- `vm_set_irq_level(..., irq=0, ...)` is called continuously
- `vm_check_external_interrupt()` reports `pending=1 accept=1`
- `i8259_get_interrupt()` returns vector `48` and drains the pending PIC state

This rules out "timer IRQs are generated but never delivered to the guest" as
the remaining blocker.

### Current blocker after the timer/IRQ fixes

Even with:

- working PIT scheduling
- working channel-2 `0x61` calibration path
- working HPET enable / compare / callback path
- working PIC external interrupt consumption

the guest still reaches:

```text
[    0.037000] tsc: Unable to calibrate against PIT
[    0.037000] tsc: HPET/PMTIMER calibration failed
[    0.037000] tsc: Marking TSC unstable due to could not calculate TSC khz
```

So the next investigation target is narrower:

- x86 TSC semantics as seen by the guest
- or a remaining mismatch in HPET counter/read semantics rather than basic
  timer scheduling or IRQ routing

## Additional 2026-04-16 CPUID / APIC Findings

The LAPIC instrumentation made the next x86 failure mode explicit.

### Finding 5: Linux switches out of PIC virtual-wire mode

During early boot Linux prints:

```text
APIC: Switch to symmetric I/O mode setup
```

Immediately around that transition the guest performs LAPIC MMIO writes that
change:

- `SPIV` from `0x100` to `0x0`, then later to `0x1ff`
- `LVT0` remains `0x10700` (`EXTINT` delivery mode but masked)

The VMM then reports:

```text
lapic accept pic vcpu=0 accept=0 lvt0=0x10700 mode=7 masked=1 spiv=0x1ff sw_enabled=256
```

So after Linux switches modes, the bootstrap vCPU no longer accepts PIC
interrupts through `LVT0`. This is a real mode transition, not a logging
artifact.

### Finding 6: generated guest MADT is incomplete

The guest's synthetic ACPI `APIC` table currently advertises LAPIC and HPET,
but the source generator in:

- `projects/sel4_projects_libs/libsel4vmmplatsupport/src/arch/x86/acpi.c`

still has the IOAPIC entry code compiled out with `#if 0`.

That explains why earlier guest logs said:

```text
ACPI: No IOAPIC entries present
```

while the seL4-side ACPI parser logs from the boot image still showed MADT
IOAPIC / ISO entries from the host firmware view.

This is one real bug in the x86 guest firmware model, although it is not yet
enough by itself to complete guest Linux boot.

### Finding 7: CPUID frequency leaves were hidden by the max-leaf cap

The CPUID emulator was updated to pass through leaves `0x15` and `0x16`
(architectural TSC / frequency enumeration), but that initially had no effect
because leaf `0x0` still clamped the advertised maximum basic leaf to `0xb`.

Fix applied locally in `projects/sel4_projects_libs`:

- raise the advertised max basic leaf from `0xb` to `0x16`

This exposed the next concrete guest requirement.

### Finding 8: next runtime blocker became explicit CPUID leaf coverage

Once leaves `0x15/0x16` became visible, the guest no longer just "silently
stalled" at the old point. The next verified failure was:

```text
vm_cpuid_virt@cpuid.c:185 CPUID unimplemented function 0xd
VM_FATAL_ERROR ::: vmexit handler return error
```

This means the guest is now probing farther into the CPUID surface, and the
active blocker moved from "TSC calibration appears stuck" to "missing CPUID
leaf emulation".

### Current local x86 CPUID work in progress

Local uncommitted work in:

- `projects/sel4_projects_libs/libsel4vm/src/arch/x86/processor/cpuid.c`

currently does all of the following:

- passes through leaves `0x15` and `0x16`
- advertises max basic leaf `0x16`
- returns zeroes for leaf `0xd` and nearby unsupported XSAVE-era leaves

This is the current front of the investigation. The latest remote run after the
`0xd` zero-fill change did not stream back enough log output to claim a new
verified post-`0xd` blocker yet, so that part still needs re-validation.

## Additional 2026-04-16 Progress

Three generic x86 issues were isolated after the initial x86 build path landed.

### Fix 1: bootstrap vspace starvation

Applying:

- `vm0.simple_untyped22_pool = 1`

to the x86 app clears the earlier failure:

```text
Failed to allocate bootstrap frame
```

This change was committed in `projects/virtioso-camkes-vm`:

- `54e6368` `x86: add small untyped reserve for vm bootstrap`

### Fix 2: debug-build x86 VCPU naming crash

The next x86 halt was:

```text
SysDebugNameThread: cap is not a TCB
```

Root cause:

- generic `libsel4vm` `vm_create_vcpu()` names `vm_get_vcpu_tcb(vcpu)`
- x86 binds the VCPU to the init thread TCB but does not populate
  `vcpu->tcb.tcb`
- in debug builds, the name call therefore receives a null / invalid cap

Fix committed in `projects/sel4_projects_libs`:

- `6025be1` `libsel4vm: skip naming x86 vcpu without tcb`

### Fix 3: unaligned low-memory RAM reservations

After the VCPU naming fix, x86 failed while mapping low guest RAM:

```text
Mapping frame of size 4096 to 0x7f500 overflows reservation 523008 bytes at 0x500
Failed to alloc guest ram at 0x500
```

Root cause:

- shared x86 Init code reserved low guest RAM ranges that were not 4 KiB aligned:
  - `0x500 .. 0x80000`
  - `0x80000 .. 0x9fc00`

Fix committed in `projects/vm`:

- `30fece0` `x86: align low guest ram reservations`

The aligned ranges now use:

- `0x1000 .. 0x80000`
- `0x80000 .. 0x9f000`

### Current remaining blocker after those fixes

With the three fixes above applied, x86 now progresses through:

- seL4 boot
- CapDL Loader
- guest address-space root creation
- low-memory guest RAM setup

The current failure is now bulk guest-RAM backing allocation.

Observed with:

- `vm0.simple_untyped22_pool = 1`
- `vm0.guest_ram_mb = 64`

runtime:

```text
Failed to allocate 67108864 bytes of guest ram. Already allocated 0.
```

Observed with:

- `vm0.simple_untyped22_pool = 1`
- `vm0.guest_ram_mb = 32`

runtime:

```text
Failed to allocate 33554432 bytes of guest ram. Already allocated 0.
```

This indicates that the single `22-bit` pool is enough to fix bootstrap metadata
allocation, but not enough to back the guest RAM itself.

## Additional 2026-04-16 IOAPIC / MADT Checkpoint

The next correct x86 fix was to stop pretending the guest platform was still
PIC-only after Linux switched into symmetric I/O mode.

### Fix 9: add a guest IOAPIC model to `libsel4vm`

Local implementation added in `projects/sel4_projects_libs`:

- `libsel4vm/src/arch/x86/ioapic.c`
- `libsel4vm/src/arch/x86/ioapic.h`

Main points:

- reserve guest MMIO at `0xfec00000`
- expose IOREGSEL/IOWIN accesses
- implement ID / VER / ARB plus the redirection table
- route IRQ delivery through LAPIC delivery helpers
- notify the IOAPIC on LAPIC EOI so level-triggered entries can clear remote
  IRR and ack callbacks

The default x86 guest IRQ controller now creates:

- PIC
- IOAPIC
- LAPIC

instead of only PIC plus LAPIC.

### Fix 10: publish the IOAPIC and ISA overrides in guest MADT

The synthetic guest MADT in:

- `projects/sel4_projects_libs/libsel4vmmplatsupport/src/arch/x86/acpi.c`

now includes:

- one `MADT_IOAPIC` entry for `0xfec00000`
- ISA interrupt source overrides:
  - `0 -> GSI 2`
  - `5 -> GSI 5`
  - `9 -> GSI 9`
  - `10 -> GSI 10`
  - `11 -> GSI 11`

This aligns the synthetic guest ACPI view much more closely with the host-style
pc99 topology Linux expects.

### Rerun result after IOAPIC + MADT work

Remote rerun command:

```bash
python3 projects/virtioso-camkes-vm/tools/qemu_runner.py run-remote \
  --target qemu_x86_64_defconfig \
  --binary /home/hlyytine/tii-sel4/qemu_x86_64_vm_qemu_virtio/images/capdl-loader-image-x86_64-pc99
```

Confirmed runtime facts:

- seL4-side ACPI parsing now shows:
  - `MADT_IOAPIC ioapic_addr=0xfec00000`
  - the expected ISA interrupt source overrides
- Linux still reaches:

```text
APIC: Switch to symmetric I/O mode setup
tsc: Unable to calibrate against PIT
tsc: HPET/PMTIMER calibration failed
tsc: Marking TSC unstable due to could not calculate TSC khz
```

- there are still no guest IOAPIC MMIO accesses at `0xfec00000`
- the guest continues to access:
  - LAPIC MMIO at `0xfee...`
  - HPET MMIO at `0xfed...`
  - PIT channel-2 speaker port `0x61`

### Current conclusion after the rerun

The IOAPIC / MADT work was still the correct architectural fix and is worth
keeping, but it is not the active blocker anymore.

The current live blocker is now narrower:

- Linux sees the improved ACPI topology
- Linux still does not program the guest IOAPIC before stalling
- the stall remains in the x86 time / TSC calibration path

The next investigation seam is therefore:

- CPUID frequency / TSC leaves and related feature bits
- guest-visible MSR behaviour on x86 timer / capability probes
- not further PIC-only plumbing

## Additional 2026-04-17 CPUID / MSR Timer Model Progress

The next real fix was not more PIT or HPET work. It was to give Linux a
coherent x86 time model so it could stop relying on the failing slow-path
calibration heuristics.

### Fix 11: provide a stable TSC frequency model through CPUID

`projects/sel4_projects_libs/libsel4vm/src/arch/x86/processor/cpuid.c`
now does all of the following:

- advertises max basic leaf `0x16`
- keeps exposing `0x15` / `0x16`
- if the host leaves are empty, synthesizes:
  - `CPUID 0x15`: ratio `96 / 1`, crystal `24 MHz`
  - `CPUID 0x16`: base / max frequency `2304 MHz`, bus `100 MHz`
- sets `CPUID 0x80000007.edx[8]` so the guest sees invariant TSC

Visible runtime effect from the next `qemu_pc99` rerun:

```text
cpuid leaf=0x16 ... eax=0x900 ebx=0x900 ecx=0x64
cpuid leaf=0x15 ... eax=0x1 ebx=0x60 ecx=0x16e3600
tsc: Detected 2304.000 MHz processor
clocksource: tsc-early: ...
Calibrating delay loop (skipped), value calculated using timer frequency..
```

This eliminates the old failure signature:

```text
tsc: Unable to calibrate against PIT
tsc: HPET/PMTIMER calibration failed
tsc: Marking TSC unstable due to could not calculate TSC khz
```

### Fix 12: make mitigation-control MSRs benign

`projects/sel4_projects_libs/libsel4vm/src/arch/x86/processor/msr.c`
now treats these as benign reads / writes instead of injecting `#GP`:

- `0x10a` `IA32_ARCH_CAPABILITIES`
- `0x48` `IA32_SPEC_CTRL`
- `0x49` `IA32_PRED_CMD`

Visible runtime effect:

- the earlier unchecked `RDMSR/WRMSR 0x48` warnings disappear
- Linux proceeds through Spectre / mitigation setup normally

### Current runtime state after the CPUID / MSR fixes

The guest now advances well past the original x86 timer stall:

- TSC frequency is detected from CPUID
- HPET is accepted as a clocksource
- SMP bring-up completes for the single configured CPU
- mitigation and CPU-finalization code runs without the earlier MSR traps
- kernel reaches normal init well beyond:
  - `PCI: Using configuration type 1 for base access`
  - `kprobes: kprobe jump-optimization is enabled`

### Current next blocker

The run no longer fails at the original TSC calibration point. The current
question is the next later-stage stall after this progress:

- no userspace prompt or `/init` handoff has been observed yet
- after the newer kernel-init progress, the serial log becomes quiet

So the active blocker has moved again. It is no longer:

- missing guest IOAPIC wiring
- missing PIT / HPET operation
- missing TSC frequency enumeration
- or mitigation-control MSR traps

The next investigation should focus on the later kernel-init / userspace
handoff path on x86.

### Current uncommitted sizing experiment

There is one active uncommitted experiment in:

- `apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes`

Current local values:

- `vm0.simple_untyped22_pool = 8`
- `vm0.guest_ram_mb = 32`

Result:

- reintroduces CapDL pressure:

```text
Untyped Retype: Insufficient memory (1 * 2097152 bytes needed, 0 bytes available)
```

- and still fails to allocate guest RAM:

```text
Failed to allocate 33554432 bytes of guest ram. Already allocated 0.
```

Conclusion:

- simply increasing `simple_untyped22_pool` on the current x86 app is not yet
  sufficient
- there is still a sizing/layout issue between CapDL object pressure and the
  amount/type of untyped memory available for guest RAM backing

## `vm_qemu_virtio` X86 Variants Already Tried

### Variant A: larger initial image / guest RAM

Earlier x86 attempts failed before the current point with CapDL pressure and later
`vm0:control` death. That path was improved by reducing image size and guest RAM.

### Variant B: current committed footprint reduction

Committed in `9263478`:

- switched x86 app rootfs to:
  - the legacy minimal initramfs image
- reduced x86 guest footprint

Effect:

- removed earlier CapDL loader large-untyped failures
- moved failure forward to later `alloc_and_map()` bootstrap frame allocation

### Variant C: uncommitted `guest_ram_mb = 64`

File:

- `apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes`

Change:

- `vm0.guest_ram_mb = 128 -> 64`

Result:

- no change in failure mode
- still fails after CapDL Loader with:
  - `Failed to allocate bootstrap frame`

Conclusion:

- reducing guest RAM from 128 MiB to 64 MiB alone does not fix the remaining x86
  bootstrap allocation failure

## Control Experiment: `vm-examples` `minimal_64`

Goal:

- determine whether the current `qemu_pc99` x86 environment is known-good for a
  baseline x86 VM example

Build commands used:

```bash
make mrproper
make qemu_x86_64_defconfig
make minimal_64
```

Produced:

- `qemu_x86_64_minimal_64/images/capdl-loader-image-x86_64-pc99`

Runtime command used:

```bash
python3 projects/virtioso-camkes-vm/tools/qemu_runner.py run-remote \
  --target qemu_x86_64_defconfig \
  --binary /home/hlyytine/tii-sel4/qemu_x86_64_minimal_64/images/capdl-loader-image-x86_64-pc99
```

### Control Result 1: stock local `minimal_64`

Local state in `projects/vm-examples/apps/x86/minimal_64/minimal.camkes` already had:

- `vm0.guest_ram_mb = 64`
- `vm0.simple_untyped23_pool = 20`

Observed runtime:

- CapDL Loader starts
- then large-untyped allocation failures appear:

```text
Untyped Retype: Insufficient memory (1 * 8388608 bytes needed, 0 bytes available)
Untyped Retype: Insufficient memory (1 * 2097152 bytes needed, 0 bytes available)
```

- then:

```text
Guest address space root allocated at 0x10000000. Creating 1-1 entries
SysDebugNameThread: cap is not a TCB, halting
```

Conclusion:

- stock local `minimal_64` is not currently a clean known-good control on this
  `qemu_pc99` path

### Control Result 2: remove `simple_untyped23_pool`

Temporary local experiment in:

- `projects/vm-examples/apps/x86/minimal_64/minimal.camkes`

Edit:

- removed:
  - `vm0.simple_untyped23_pool = 20;`

Runtime result:

- the CapDL `8 MiB` and `2 MiB` untyped failures disappear
- failure moves to the same later bootstrap point seen in the virtioso x86 app:

```text
vm0: vka_alloc_object_at_maybe_dev@object.h:57 Failed to allocate object of size 4096, error 1
vm0: alloc_and_map@bootstrap.c:102 Failed to allocate bootstrap frame, error: 1
```

Important conclusion:

- `simple_untyped23_pool = 20` explains the earlier `minimal_64` CapDL large-untyped
  failures
- after removing that pool request, `minimal_64` and the current virtioso x86 app
  converge on the same later bootstrap frame allocation failure

This strongly suggests the remaining blocker is a broader x86 `qemu_pc99`
bootstrap/object allocation issue in the current workspace configuration, not
something unique to the virtioso app shape.

### Control Result 3: use `simple_untyped22_pool = 1`

Temporary local experiment in:

- `projects/vm-examples/apps/x86/minimal_64/minimal.camkes`

Edit:

- removed:
  - `vm0.simple_untyped23_pool = 20;`
- added:
  - `vm0.simple_untyped22_pool = 1;`
- kept:
  - `vm0.guest_ram_mb = 64;`

Runtime result:

- CapDL Loader completes
- `alloc_and_map()` bootstrap frame failure disappears
- runtime advances to:

```text
Guest address space root allocated at 0x10000000. Creating 1-1 entries
```

- then fails later with:

```text
SysDebugNameThread: cap is not a TCB, halting
```

Important conclusion:

- a small `22-bit` untyped reserve is sufficient to fix the earlier x86
  bootstrap-frame allocation failure

## Additional 2026-04-16 Progress After Bootstrap Fixes

The remaining x86 bring-up failures were not one issue. They were a chain of
independent x86 bugs and mismatches that had to be peeled back one by one.

### Fix 4: guest RAM pool sizing and image contract

The x86 app now uses:

- `vm0.simple_untyped22_pool = 1`
- `vm0.simple_untyped23_pool = 10`
- `vm0.guest_ram_mb = 64`

This was enough to get beyond the bulk guest-RAM allocation failure without the
CapDL pressure caused by many `22-bit` pools.

Separately, the x86 app CMake was corrected to use the standard x86 Linux
helper flow:

- `find_package(camkes-vm-linux REQUIRED)`
- `DecompressLinuxKernel(...)`
- serve the extracted kernel under `"bzimage"`

Without that, the x86 guest loader tried to parse the raw compressed `bzImage`
as ELF and failed immediately.

### Fix 5: x86_64 kernel relocation bug

The x86 guest image relocation helper in:

- `projects/sel4_projects_libs/libsel4vmmplatsupport/src/arch/x86/guest_image.c`

was broken for x86_64 kernels.

Root cause:

- Linux `relocs` entries are handled as `uint32_t`
- the code subtracted them from the full canonical `link_vaddr`
- on x86_64 this underflowed and produced bogus guest physical addresses

Fix:

- perform relocation arithmetic in the low 32-bit address space that the x86
  relocs file actually encodes
- fail fast if a relocation target cannot be touched

Effect:

- x86 guest kernel relocation now completes
- runtime advanced past kernel load/relocation to boot-info construction

### Fix 6: ACPI lower BIOS reservation off-by-one

The x86 ACPI code reserved:

- `LOWER_BIOS_START = 0xE0000`
- `LOWER_BIOS_SIZE = 0xFFFF`

but the actual range is one full `64 KiB` window:

- `0xE0000 .. 0xEFFFF`

That caused the last 4 KiB frame at `0xEF000` to overflow the reservation by
one byte.

Fixes in `libsel4vmmplatsupport`:

- `LOWER_BIOS_SIZE = 0x10000`
- use an exclusive end check in the BIOS frame iterator
- fix the adjacent `memset()` argument order bug in the BIOS allocation path

Effect:

- x86 boot-info and ACPI table construction now complete

### Fix 7: x86 decoder was using RAM-only access for hidden reservations

After ACPI boot-info setup, x86 still failed when the guest started running.

Root cause:

- x86 page-walk / decode code used `vm_ram_touch()`
- but guest page tables and some boot-time helper pages are reserved guest
  memory, not guest RAM
- those reservations are intentionally not advertised in the e820 RAM map

Fix in:

- `projects/sel4_projects_libs/libsel4vm/src/arch/x86/processor/decode.c`

Added a local helper that:

- finds arbitrary guest memory reservations
- maps them if needed
- accesses them through `vspace_access_page_with_callback()`

and switched the x86 decode / page-walk accessors to use it instead of
`vm_ram_touch()`.

Effect:

- x86 fetch/decode can now read hidden guest page-table reservations without
  falsely requiring them to be registered RAM

### Fix 8: x86 guest RAM layout was fundamentally wrong for Linux

The generic x86 Init code originally reserved anonymous guest RAM only from:

- `0x10003000 .. 0xA0000000`

This left a huge fake reserved hole between:

- `0x00100000 .. 0x10003000`

Linux immediately tried to use low RAM in that range for its own page tables,
which led to CR3/page-walk accesses into addresses that the VMM had not made
RAM.

Fix in `projects/vm/components/Init/src/main.c`:

- split anonymous guest memory into:
  - `0x00100000 .. 0x10000000`
  - `0x10003000 .. 0xA0000000`

This preserves the hidden x86 page-table window at:

- `0x10000000 .. 0x10002FFF`

while restoring normal low-memory guest RAM below it.

Effect:

- e820 now reports a sane x86 memory map:
  - `0x1000 .. 0x9F000` RAM
  - `0x00100000 .. 0x04100000` RAM for the current 64 MiB guest

### Fix 9: x86 kernel load address should not default to 4 MiB

With the low-memory RAM region restored, `vm_ram_find_largest_free_region()`
returned a low region starting at `0x00100000`.

Because the x86 loader rounded that to `4 MiB`, the decompressed kernel was
loaded at:

- `0x00400000`

instead of its linked physical start:

- `0x01000000`

That forced a large negative relocation and the guest then triple-faulted
immediately after entering the kernel.

Fix in `projects/vm/components/Init/src/main.c`:

- pin x86 kernel load address to at least `0x01000000` before calling
  `vm_load_guest_kernel()`

### Current runtime state after all fixes above

The latest validated progression reached:

- seL4 boot
- CapDL Loader
- guest address-space root construction
- guest kernel relocation
- guest cmdline construction
- guest boot-info / ACPI generation
- sane x86 e820 map
- guest entry transition

Most recent successful visible log before the latest rerun infrastructure issue:

## 2026-04-18 q35 `RSDT` plus rev-3 `FADT` checkpoint

Committed as:

- `projects/sel4_projects_libs` `444c938` `x86: add facs to guest acpi tables`

Follow-up uncommitted/then-tested slice:

- add a real `RSDT` alongside `XSDT`
- stop abusing `RSDP.rsdt_address` as an `XSDT` pointer
- switch guest `FADT` header revision from `1` to `3`
- populate timer/GAS fields more like q35

Validation run:

- autopilot `20260418-005802`

What changed in the guest-visible ACPI chain:

- `ACPI: RSDP 0x... (v02 NICTA)`
- `ACPI: XSDT ...`
- `ACPI: FACP ... (v03 ...)`
- `ACPI: DSDT ...`
- `ACPI: FACS ...`

What did **not** change yet:

- Linux still reports:
  - `ACPI Error: AE_NO_ACPI_TABLES, While loading namespace from ACPI tables`
  - `PCI: root bus 00: using default resources`
  - `No busn resource found for root bus`

New concrete clue from the same run:

- Linux now also says:
  - `PCI: [Firmware Info]: ECAM [mem 0xb0000000-0xbfffffff] not reserved in ACPI motherboard resources`

Interpretation:

- the `RSDT`/`FADT` cleanup was the correct structural fix, but it was not
  sufficient to make ACPICA consume the generated namespace
- the next physical-bus slice should reserve the q35 ECAM aperture in ACPI
  motherboard resources and likely align guest ACPI table placement more like
  QEMU instead of leaving tables packed at odd byte offsets

## 2026-04-18 q35 DSDT-structure follow-up: still no namespace load

Follow-up implementation after the `RSDT` checkpoint:

- reserve q35 `MCFG` / ECAM as a motherboard-resource device in the generated
  DSDT
- change ACPI table placement to aligned addresses instead of byte-packed
  placement
- make `PCI0._CRS` more QEMU-like:
  - keep the config-port fixed I/O range
  - add the low `0x0000..0x0CF7` root I/O window
  - add the VGA legacy memory window
  - mark memory resource descriptors read/write instead of read-only

Validation runs:

- autopilot `20260418-010433`
- autopilot `20260418-011955`

Observed effects:

- table layout is now more sane/aligned:
  - `XSDT 0x...1038`
  - `MCFG 0x...11B0`
  - `DSDT 0x...11F0`
  - `FACP 0x...1300`
- DSDT size increased from `0xE4` to `0x10E`, reflecting the fuller AML

What did **not** change:

- Linux still reports:
  - `ACPI Error: AE_NO_ACPI_TABLES, While loading namespace from ACPI tables`
  - `PCI: [Firmware Info]: ECAM [mem 0xb0000000-0xbfffffff] not reserved in ACPI motherboard resources`
  - `PCI: root bus 00: using default resources`
  - `No busn resource found for root bus`

Interpretation:

- we have now ruled out the most obvious “minimal AML” issues:
  - bogus `RSDT`
  - rev-1 `FADT` on q35
  - missing `FACS`
  - unaligned ACPI table placement
  - obviously incomplete `PCI0._CRS`
- the remaining issue is more likely a deeper mismatch between the generated
  AML structure and what ACPICA expects to successfully install the DSDT into
  the namespace
- next step should be to mirror QEMU's q35 DSDT object structure more directly
  instead of continuing to hand-approximate it

```text
Final e820 map is:
    0x0 - 0x1000: Type 2
    0x1000 - 0x9f000: Type 1
    0x9f000 - 0x100000: Type 2
    0x100000 - 0x4100000: Type 1
Initializing guest to start running at 0x400000
```

After pinning the x86 kernel load address to `0x01000000`, the next remote
runtime attempt was interrupted by an SSH timeout to the remote QEMU host before
full boot logs were retrieved, so that last adjustment still needs runtime
confirmation.
- the large `23-bit` reserve is not required for that specific allocator issue
- the next blocker is later than `sel4utils_bootstrap_vspace()`

This is the first x86 control configuration that gets past the generic
`alloc_and_map()` failure.

## Working Diagnosis

At this point there are two distinct x86 problems:

1. Early CapDL large-untyped pressure
   - caused by requesting `simple_untyped23_pool = 20` in `minimal_64`
   - already avoided in the current virtioso x86 app path

2. Later `vm0` bootstrap frame allocation failure
   - reproducible in both:
     - `virtioso-camkes-vm` x86 `vm_qemu_virtio`
     - `vm-examples` `minimal_64` after removing `simple_untyped23_pool`
   - likely in the generic x86 VMM/bootstrap/object allocation path for this
     workspace/config/runtime combination

3. Follow-on x86 control halt after fixing bootstrap allocation
   - with `simple_untyped22_pool = 1`, `minimal_64` progresses past
     `sel4utils_bootstrap_vspace()`
   - the next observed failure is:
     - `SysDebugNameThread: cap is not a TCB, halting`
   - this likely represents the next generic x86 bring-up issue after allocator
     starvation is addressed

## Next Steps

1. Apply `vm0.simple_untyped22_pool = 1` to the current virtioso x86 app and test
   whether it also clears the bootstrap-frame allocation failure.
2. Inspect the later x86 halt around:
   - `Guest address space root allocated ...`
   - `SysDebugNameThread: cap is not a TCB`
3. Compare the current `qemu_pc99` configuration and generated object layout
   against an x86 path known to boot in this workspace, if any.
4. Once the generic x86 bootstrap issue is resolved, resume the virtioso-specific
   port:
   - restore proper x86 app shape
   - add virtioso-specific x86 plumbing
   - preserve zero-copy SWIOTLB semantics

## Notes

- The temporary `minimal_64` control edit is diagnostic only and is not part of
  the virtioso implementation.
- The zero-copy SWIOTLB path has not been ported on x86 yet.

## Additional 2026-04-17 Memory / E820 Progress

The x86 guest-memory problem was not QEMU machine RAM. The remote runner already
starts QEMU with:

- `-m size=512M`

The actual constraint was the seL4-backed x86 guest memory budget exposed by
the VMM. A temporary local experiment changed the x86 app to:

- `vm0.simple_untyped23_pool = 21`
- `vm0.simple_untyped22_pool = 1`
- `vm0.heap_size = 0x10000`
- `vm0.guest_ram_mb = 128`

This matches the pool sizing pattern used by several working x86 examples under
`projects/vm-examples/apps/x86/*`.

### Result: guest RAM allocation succeeds at 128 MiB

Earlier experiments showed:

- with `guest_ram_mb = 128` and `simple_untyped23_pool = 10`, guest RAM mapping
  failed later in the reservation at about `0x4e38000`
- with `guest_ram_mb = 128` and no `simple_untyped23_pool`, failure happened
  much earlier at about `0x3ac000`

That established that the `23-bit` pool is part of the usable guest RAM budget
on this x86 path, not just allocator overhead.

With `simple_untyped23_pool = 21`, the remote `qemu_pc99` boot now gets past
the guest RAM allocation stage entirely and prints:

```text
Final e820 map is:
    0x0 - 0x1000: Type 2
    0x1000 - 0x9f000: Type 1
    0x9f000 - 0x100000: Type 2
    0x100000 - 0x8100000: Type 1
```

Linux later reports:

```text
Memory: 83032K/131704K available ... 45244K reserved
```

### Current conclusion

- The guest-visible RAM/e820 side is now behaving correctly for a `128 MiB`
  x86 guest.
- The old `64 MiB` ceiling was not an e820 generation bug by itself; it was the
  consequence of the x86 app's pool sizing and guest RAM configuration.
- The active blocker has moved back to later x86 kernel/userspace bring-up,
  after memory discovery and early init succeed.

## Additional 2026-04-17 Boot Image / Init Progress

Two more issues were identified after the memory/e820 checkpoint.

### Fix 1: use the correct x86 boot initramfs

The x86 app had been serving:

- the legacy minimal initramfs image

Inspection of that image showed its `/init` was the Orin/eMMC bootstrap script,
which is the wrong contract for `qemu_pc99`.

The x86 app now serves:

- `vm-image-boot-qemux86-64.rootfs.cpio.gz`

from:

- `projects/virtioso-camkes-vm/apps/x86/vm_qemu_virtio/CMakeLists.txt`

The x86 guest cmdline was also made more verbose during bring-up:

- `loglevel=8`
- `ignore_loglevel`
- `initcall_debug`

### Fix 2: leaf-7 CPUID advertised features were inconsistent

The virtual CPU model already hid:

- `XSAVE`
- `OSXSAVE`
- `AVX`

in CPUID leaf `1`, but leaf `7` still advertised:

- `AVX2`
- `HLE`
- `RTM`
- `BMI1`
- `BMI2`

That is not a coherent x86 CPU surface. The x86 CPUID virtualization in:

- `projects/sel4_projects_libs/libsel4vm/src/arch/x86/processor/cpuid.c`

was tightened so leaf `7.0.ebx` now only exposes:

- `FSGSBASE`
- `SMEP`
- `ERMS`

### Workaround: skip the current blocking RAID6 initcall

The latest visible late-kernel stall clustered around:

- `raid6_select_algo`

For bring-up only, the x86 guest cmdline now temporarily adds:

- `initcall_blacklist=raid6_select_algo`

This is not the final fix. It is a targeted workaround to unblock the rest of
the x86 boot path and prove whether the guest can reach userspace.

## Additional 2026-04-18 Physical PCI Host-Bridge Template Progress

The first Jinja/CAmkES-template-driven physical host-bridge slice is now
working at build time.

### What changed

Instead of continuing to hand-maintain q35 root-bus aperture chunks directly in
the x86 app's `vm0.untyped_mmios`, the VM `Init` path now has a dedicated
generated helper:

- `projects/vm/components/Init/templates/seL4PhysicalPCIHostBridge.template.c`

That template is wired through the existing `Init` generation path in:

- `projects/vm/camkes_vm_helpers.cmake`

and exports compiled helpers through:

- `projects/vm/components/Init/src/camkes_vm_interfaces.h`

The runtime consumer in:

- `projects/vm/components/Init/src/main.c`

now prefers generated physical-host-bridge regions and generated frame-cap
lookups before falling back to the older ad hoc x86 detection path.

The x86 app now declares a per-instance selector:

- `vm0.physical_pci_host_bridge = "qemu_pc_q35"`

in:

- `projects/virtioso-camkes-vm/apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes`

### Important correction

The initial empty render was not a CAmkES limitation. It was self-inflicted:

- the template stripped quotes from the attribute value
- but then compared it against still-quoted strings

Once that was fixed, the generated file for `vm0` started emitting real q35
regions and frame caps.

### Verified generated output

After `make vm_qemu_virtio`, the generated file:

- `qemu_x86_64_vm_qemu_virtio/vm0/seL4PhysicalPCIHostBridge.template.c`

now contains:

- `physical_pci_host_bridge_num_regions() == 2`
- region `0xb0000000, size 0x200000, page_bits 21`
- region `0xc0000000, size 0x200000, page_bits 21`
- frame-cap lookups for those region bases

This proves the intended template seam is viable:

- Jinja runs at build time
- emits compiled C into the VMM/Init build
- and carries the physical-host-bridge aperture model as generated code, not as
  a host-only config rewrite

### Current state

- The q35 `MCFG` / low MMIO aperture seed moved out of the app-local manual
  list and into generated `Init` code.
- The smaller device-specific BAR mappings are still app-local for now.
- The remaining architectural work is to grow this generated host-bridge path
  into the primary common model seam, then keep shrinking the synthetic x86
  per-device PCI path around it.

## Additional 2026-04-18 Full q35 host-window generation

The generated host-bridge path was then extended from "seed chunks" to the full
q35 low physical PCI windows used by the current build:

- `MCFG`: `0xb0000000 .. 0xbfffffff` (`0x10000000`)
- `MMIO32`: `0xc0000000 .. 0xfebfffff` (`0x3ec00000`)

### What changed

The generated helper in:

- `projects/vm/components/Init/templates/seL4PhysicalPCIHostBridge.template.c`

now allocates and emits:

- the two logical q35 host-bridge regions above
- the full backing frame-cap table for those regions, chunked into x86-valid
  `2 MiB` large pages
- a `physical_pci_host_bridge_get_mem_frame()` helper that resolves any guest
  paddr inside those windows to the backing large-page cap

With that in place, the x86 app no longer needs explicit q35 window fragments
in:

- `projects/virtioso-camkes-vm/apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes`

The app now uses:

- `vm0.untyped_mmios = []`

for the q35 physical PCI host bridge, relying entirely on the generated `Init`
path for structural q35 aperture provisioning.

### Verified build result

After `make vm_qemu_virtio`:

- the build succeeds with no x86 app-local q35 `untyped_mmios`
- the generated file:
  - `qemu_x86_64_vm_qemu_virtio/vm0/seL4PhysicalPCIHostBridge.template.c`
  - now spans thousands of lines, which reflects the full `2 MiB` chunk table
    rather than the earlier two-cap seed version
- `physical_pci_host_bridge_num_regions()` still reports `2`, because the
  logical model is still "MCFG region + MMIO32 region"; only the backing cap
  table expanded

### Current conclusion

This is the first buildable x86 checkpoint where the q35 physical host-bridge
windows are provisioned structurally from generated VMM code rather than from
hand-written app-local MMIO declarations.

## Additional 2026-04-18 Per-device q35 BAR reservation removal

After the full q35 host-window generation landed, the remaining x86 q35
physical-device path still had one redundant fallback:

- `projects/vm/components/Init/src/main.c`
  - `reserve_structural_physical_pci_bars(...)`

That helper reserved each physical device BAR individually even though the full
q35 `MMIO32` host window was already being provisioned structurally by the new
generated host-bridge path.

That per-device q35 BAR reservation path is now removed from the structural q35
physical-device registration flow.

### Verified result

After another `make vm_qemu_virtio`:

- the build still succeeds
- the q35 physical path no longer depends on:
  - app-local q35 `untyped_mmios`
  - per-device BAR reservation in `register_physical_pci_device(...)`

### Current conclusion

The remaining x86 physical-q35 dependency on the synthetic PCI layer is now
narrower again:

- thin config-space/device-table mirroring still exists
- but q35 host-window provisioning is now structural, generated, and shared via
  compiled `Init` code

## Additional 2026-04-18 Raw host-config fallback for q35

The next reduction step was to stop requiring explicit synthetic PCI-table
entries for the q35 host bridge and auto-scanned physical devices just to serve
guest config-space probes.

### What changed

`libsel4vmmplatsupport` now supports an optional raw config fallback in:

- `include/sel4vmmplatsupport/drivers/pci.h`
- `src/drivers/pci.c`

and the x86 CAM helper in:

- `src/arch/x86/drivers/vmm_pci_helper.c`

now does this on config-space access:

- if a normal `vmm_pci_entry_t` exists, use it
- otherwise, for bus `0`, forward the access directly to the real host config
  backend

The q35 structural path in:

- `projects/vm/components/Init/src/main.c`

now:

- initializes an empty PCI space
- enables that raw config fallback with `make_camkes_pci_config()`
- no longer mirrors `00:00.0` as a synthetic/raw entry
- no longer auto-registers the scanned outer q35 physical devices in the PCI
  table

### Intended architectural effect

This moves x86 q35 closer to the ARM physical-bus model:

- structural host apertures are still provided explicitly
- config-space access for the physical bus no longer depends on per-device
  synthetic registration
- the remaining synthetic PCI layer is reduced further toward "only what is
  still genuinely needed for emulated devices"

### Validation status

Build status:

- `make vm_qemu_virtio` succeeds after the raw-fallback change

Runtime validation:

- autopilot request `20260418-100621` was submitted for the first q35 runtime
  check of this mode
- that run failed before later guest bring-up, but the failure was not the raw
  config fallback itself

Relevant failure:

- `vm0: map_page@mapping.c:81 Failed to map page at address 0xb0001000 with cap 19, error: 2`

Interpretation:

- the remaining blocker is now the interaction between the structural q35
  host-bridge windows and the x86 reserved-memory touch/mapping path
- the current generated host-bridge code is using `2 MiB` large-page caps
- later access into subpages of that region still causes a conflicting remap in
  the x86 touch path
- so the raw-config fallback reduction and the large-page host-window mapping
  problem need to be treated as separate concerns

### Follow-up fix

That conflict was narrowed to page-size selection in:

- `projects/sel4_projects_libs/libsel4vmmplatsupport/src/guest_memory_util.c`

`ut_alloc_iterator()` still defaulted to `4 KiB` for generated q35 host-bridge
addresses because `camkes_get_untyped_page_bits()` no longer knew about the
generated host windows once the app-local `untyped_mmios` entries were removed.

The generated host-bridge template now also emits:

- `physical_pci_host_bridge_get_page_bits(uintptr_t paddr)`

and `guest_memory_util.c` now falls back to that helper when the old CAmkES
page-bits lookup returns zero.

### Result

Autopilot rerun:

- request `20260418-101227`

The earlier q35 host-window remap failure at:

- `0xb0001000`

is gone.

The run now progresses back to the later physical-device checkpoint:

- `PCI: Using host bridge windows from ACPI`
- `pci_bus 0000:00: root bus resource [mem 0xc0000000-0xfebfffff window]`
- `virtio_blk virtio1: [vda] ...`

and then still goes quiet at the already-known later point after `vda`
enumeration.

### Updated conclusion

This means the raw q35 config fallback plus structural host-window generation is
now at least coherent enough to reach the same later `virtio_blk` checkpoint as
the earlier hybrid path, without reintroducing the old `0xb0001000` mapping
failure.

### Latest runtime result

With:

- `vm-image-boot-qemux86-64.rootfs.cpio.gz`
- verbose kernel logging
- the CPUID leaf-7 cleanup
- temporary `initcall_blacklist=raid6_select_algo`

the guest now progresses through late init, frees init memory, and reaches:

```text
Run /init as init process
  with arguments:
    /init
```

The same run also shows post-handoff userspace-triggered module activity:

```text
calling tun_init+0x0/0xff0 [tun]
tun: Universal TUN/TAP device driver, 1.6
```

That is enough to conclude:

- the x86 guest kernel boot path is now alive through `/init`
- the corrected x86 boot image contract matters
- the remaining work has moved into initramfs/userspace behavior and removal of
  the temporary `raid6_select_algo` blacklist
- 2026-04-17: Autopilot/manual tail mismatch root cause found. The current
  `qemu_x86_64_defconfig` chain used a bare `panic` regex in the fail clause,
  which falsely matched normal Linux symbols like `kernel_panic_sysctls_init`
  and stopped the remote runner around 28.25s. Manual `run-bundle.sh` reached
  about 33.72s and showed `Run /bin/sh as init process`, so the missing tail
  was primarily an autopilot false-positive verdict, not just incomplete log
  capture. Fix applied in `autopilot/chains/qemu_x86_64_defconfig.json` by
  removing the bare `panic` token.
- 2026-04-17: Validation rerun `20260417-185644` confirmed the fix. Autopilot
  no longer stopped at the bogus `panic` match, timed out only after the real
  late-boot idle period, and captured `Run /bin/sh as init process` in
  `console/tty0.raw`. Byte counts are now materially aligned with the manual
  remote run: autopilot `278806` bytes vs manual `279469` bytes. The remaining
  delta is the manual `QEMU: Terminated` tail from operator interruption, not a
  missing userspace handoff marker.
- 2026-04-17: Explicit x86 PCI passthrough for outer QEMU `00:04.0` was wired
  into `apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes` using:
  - `vm0.vm_ioports = [{"start":0xc000, "end":0xc080, "pci_device":0x4, "name":"QemuVirtioBlk"}]`
  - `vm0.pci_devices = [{"name":"QemuVirtioBlk", "bus":0, "dev":0x04, "fun":0, "irq":"QemuVirtioBlk", "memory":[{"paddr":0xfebd2000, "size":0x1000, "page_bits":12}, {"paddr":0xfe004000, "size":0x4000, "page_bits":12}]}]`
  - `vm0.vm_irqs = [{"name":"QemuVirtioBlk", "ioapic":0, "source":11, "level_trig":1, "active_low":1, "dest":11}]`
- 2026-04-17: The first passthrough attempt with `vm0.pci_devices_iospace = 1`
  regressed before guest Linux boot. Autopilot run `20260417-193124` showed:
  - CapDL created the BAR frames at `0xfe004000..0xfe007000` and `0xfebd2000`
  - then rootserver aborted with `CNode Copy/Mint/Move/Mutate: Source slot invalid or empty`
  Source-code diagnosis: `qemu_pc99` runtime reports `ACPI: 0 IOMMUs detected`,
  so allocating a per-device IOSpace cap for `00:04.0` is not valid on this
  platform.
- 2026-04-17: Removing the explicit IOSpace object fixed the early CapDL abort
  but exposed the next wrong assumption. Autopilot run `20260417-193719`
  reached `vm_create_vcpu()` and then failed with:
  - `Failed to map page into iospace`
  - `Failed to map address 0x10000000 into guest vm vspace`
  Source-code diagnosis:
  - `projects/vm/CMakeLists.txt` defaults `CAmkESVMGuestDMAIommu` to `ON`
  - `projects/vm/camkes_vm_settings.cmake` then forces `KernelIOMMU=ON`
  - `libsel4vm/src/guest_vspace.c` mirrors every guest page into IOSpace under `CONFIG_IOMMU`
  That path is incorrect for `qemu_pc99`, which has no actual IOMMU at runtime.
- 2026-04-17: QEMU-specific x86 fix committed in
  `ec78ddc` (`x86: disable qemu pc99 iommu passthrough path`):
  - removed `vm0.pci_devices_iospace = 1`
  - set `CAmkESVMGuestDMAIommu OFF` in `apps/x86/vm_qemu_virtio/app_settings.cmake`
- 2026-04-17: After the qemu-specific IOMMU disable, autopilot run
  `20260417-194208` confirmed the main passthrough goal:
  - guest boot reached late Linux init again
  - passed-through outer PCI function `00:04.0` enumerated in the guest as a
    real virtio block device
  Key lines from `tty0.raw`:
  - `virtio_blk virtio0: 1/0/0 default/read/poll queues`
  - `virtio_blk virtio0: [vda] 1465652 512-byte logical blocks (750 MB/716 MiB)`
  This proves the inner guest now sees the outer QEMU virtio-blk PCI function
  directly. The remaining work has moved from PCI passthrough plumbing to
  initramfs root-switch / userspace verification.
- 2026-04-17: Implemented a first generic x86 `qemu_pc99` runtime auto-expose
  path instead of hand-writing `vm0.pci_devices` / `vm0.vm_irqs` /
  `vm0.vm_ioports` in the app config.
  Source changes:
  - `projects/vm/components/Init/src/main.c`
    - dynamic IOAPIC IRQ allocation with `arch_simple_get_ioapic(...)`
    - runtime BAR mapping via existing `vmm_pci_helper_map_bars(...)`
    - qemu policy hook `qemu_auto_passthrough_candidate(...)`
    - first policy scope: outer virtio PCI devices (`vendor_id == 0x1af4`)
  - `projects/virtioso-camkes-vm/apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes`
    - removed the explicit `QemuVirtioBlk` PCI/IRQ/ioport entries
- 2026-04-17: Autopilot validation run `20260417-195524` proved the generic
  runtime path works:
  - no explicit x86 PCI passthrough entries remained in the app config
  - guest still enumerated the auto-exposed outer virtio block device:
    - `virtio_blk virtio0: [vda] 1465652 512-byte logical blocks (750 MB/716 MiB)`
  - guest reached `/init`
  - the remaining failure stayed the same:
    - repeated `/dev/vda: Can't lookup blockdev`
  Conclusion: the generic x86 PCI exposure change did not reintroduce a PCI
  plumbing regression; the next blocker is still initramfs/userspace handling
  of the now-present `vda` device.
- 2026-04-17: Verified that the x86 `guest_mappings` BAR-page hack is not the
  correct long-term mechanism, and replaced it with `untyped_mmios`.
  Source-code proof:
  - `libsel4vmmplatsupport/src/guest_memory_util.c:ut_alloc_iterator()`
    already prefers:
    - `simple_get_frame_cap(vm->simple, alloc_addr, ...)`
    - then `vka_utspace_alloc_at(..., alloc_addr, ...)`
  - `camkes/templates/component.simple.c` turns `untyped_mmios` into fixed-paddr
    device untyped capabilities, which is the structural CAmkES path for this
    use-case.
- 2026-04-17: Converted `apps/x86/vm_qemu_virtio/vm_qemu_virtio.camkes` from:
  - `vm0.guest_mappings = [...]`
  to:
  - `vm0.untyped_mmios = [`
    - `"0xfebd1000:12"`
    - `"0xfebd2000:12"`
    - `"0xfe000000:14"`
    - `"0xfe004000:14"`
    - `]`
- 2026-04-17: Autopilot run `20260417-233642` validated that this replacement
  works for the q35 physical PCI path:
  - CapDL now reports these addresses as device frame/untyped objects:
    - `paddr = 0xfe000000, size = 14 bits`
    - `paddr = 0xfe004000, size = 14 bits`
    - `paddr = 0xfebd1000, size = 12 bits`
    - `paddr = 0xfebd2000, size = 12 bits`
  - outer virtio devices are still auto-passed through:
    - `Auto passthrough PCI device bdf=00:02.0 vid=1af4 did=1041 irq=11`
    - `Auto passthrough PCI device bdf=00:03.0 vid=1af4 did=1042 irq=11`
  - no `Failed to map PCI bar ...` regression occurred
- 2026-04-17: Conclusion from `20260417-233642`:
  - `untyped_mmios` is the right structural replacement for x86 q35 host BAR
    pages
  - `guest_mappings` is no longer needed for this BAR-cap provisioning role
  - the remaining failure is later in guest boot; the PCI/BAR-cap seam has
    moved closer to the ARM structural model
- 2026-04-17: Follow-up cleanup committed in `projects/vm` as
  `a897cf7` (`x86: fall back to camkes frame cap lookup`):
  - `components/Init/src/main.c:simple_frame_cap_wrapper()` now falls back to
    the original CAmkES `simple->frame_cap` hook after checking explicit
    `pci_devices` / `guest_mappings`
  - this preserves the existing explicit paths, but also lets the built-in
    CAmkES `untyped_mmios` mechanism participate in fixed-paddr frame lookup
    directly
- 2026-04-17: Autopilot validation run `20260417-234241` after `a897cf7`
  showed:
  - still no `Failed to map PCI bar ...`
  - still no `Failed to allocate page`
  - auto passthrough of the two outer virtio devices remained intact
  - `virtio_blk` still bound later in the log
  Conclusion: the wrapper fallback does not regress the q35 physical PCI path;
  it is a safe structural cleanup on top of the `untyped_mmios` conversion.
- 2026-04-17: First x86 ACPI consumer of the shared host-bridge model landed.
  Implementation split:
  - `projects/vm/components/Init/src/main.c`
    - added `vmm_guest_detect_physical_pci_host_bridge(...)`
    - runtime-detects the outer host bridge at `00:00.0`
    - currently recognizes:
      - q35 MCH: `8086:29c0`
      - i440fx host: `8086:1237`
  - `projects/sel4_projects_libs/libsel4vmmplatsupport/src/arch/x86/acpi.c`
    - added weak default hook
    - emits `MCFG` when the detected host bridge reports a valid `mcfg_region`
- 2026-04-17: Autopilot validation run `20260417-235357` confirmed that the
  new x86 ACPI path is live:
  - Linux now sees and reserves the generated MCFG table:
    - `ACPI: MCFG ...`
    - `ACPI: Reserving MCFG table memory ...`
  - but the guest still says:
    - `PCI: root bus 00: using default resources`
    - `pci_bus 0000:00: No busn resource found for root bus, will use [bus 00-ff]`
  Conclusion:
  - `MCFG` is now sourced from the shared host-bridge model for q35
  - the next missing physical-bus metadata piece is still PCI root-bridge
    `_CRS` / bus-resource description, not MCFG itself
- 2026-04-18: The q35 physical-bus rework finally crossed the ACPI acceptance
  threshold.
  Source changes in `projects/sel4_projects_libs/libsel4vmmplatsupport/src/arch/x86/acpi.c`:
  - keep `DSDT` out of `RSDT` / `XSDT`
  - reference `DSDT` only through `FADT`
  - keep `FACS` explicit
  Result in autopilot run `20260418-015152`:
  - `ACPI: 1 ACPI AML tables successfully acquired and loaded`
  - `PCI: ECAM [mem 0xb0000000-0xbfffffff] reserved as ACPI motherboard resource`
  - `ACPI: PCI Root Bridge [PCI0] (domain 0000 [bus 00-ff])`
  - `pci_bus 0000:00: root bus resource [bus 00-ff]`
  This retired the earlier `AE_NO_ACPI_TABLES` failure class. The next blocker
  immediately moved to structural aperture mapping:
  - `EPT fault: gpa=0xb0000100`
- 2026-04-18: First q35 structural aperture reservation slice landed in
  `projects/vm/components/Init/src/main.c`.
  Implementation:
  - detect physical q35 host bridge from the shared host-bridge model
  - reserve and map the first `MCFG` chunk structurally via
    `vm_reserve_memory_at(...)` + `map_ut_alloc_reservation(...)`
  - add matching `vm0.untyped_mmios` entry for:
    - `0xb0000000`
  Validation:
  - `20260418-020157` retired the old `0xb0000100` fault
  - the next failure moved into the first q35 MMIO32 device window:
    - `Guest-Physical address 0xc0000014`
  Conclusion:
  - ECAM / config-aperture passthrough is now structurally working
  - next missing piece is the q35 MMIO32 host aperture
- 2026-04-18: Second structural aperture slice reserved the first q35 MMIO32
  chunk:
  - `vm0.untyped_mmios` added `0xc0000000`
  - `reserve_physical_pci_host_apertures()` now reserves both:
    - `bridge.mcfg_region`
    - `bridge.mem32_region`
  Initial validation with `20260418-020557` showed the failure move from:
  - `0xc0000014`
  to:
  - `0xc0004014`
  That was the key clue that only the first 4 KiB inside the structural 2 MiB
  chunk was being mapped.
- 2026-04-18: Root cause of the partial q35 aperture mapping was in CAmkES
  `untyped_mmios` syntax and the guest-memory iterator, not in the host-bridge
  model.
  Verified from generated `vm0/camkes.simple.c`:
  - `"0xc0000000:21"` produced:
    - `.size_bits = 21`
    - `.page_bits = 12`
  Meaning:
  - we had created a 2 MiB untyped object, but were still consuming it in 4 KiB
    fragments
  Fixes:
  - use explicit syntax:
    - `"0xb0000000:21:21"`
    - `"0xc0000000:21:21"`
  - update `libsel4vmmplatsupport/src/guest_memory_util.c:ut_alloc_iterator()`
    to honor `camkes_get_untyped_page_bits(addr)` instead of hardcoding
    `seL4_PageBits`
  Generated proof after rebuild:
  - `vm0/camkes.simple.c` now emits:
    - `.paddr = 0xb0000000, .size_bits = 21, .page_bits = 21`
    - `.paddr = 0xc0000000, .size_bits = 21, .page_bits = 21`
- 2026-04-18: Autopilot run `20260418-021721` confirmed the structural q35 host
  apertures are now functioning as intended.
  Important log transitions:
  - `virtio_pci_driver_init` now completes:
    - `probe of 0000:00:02.0 returned 0`
    - `probe of 0000:00:03.0 returned 0`
  - the guest moves past the earlier EPT/aperture failures into late boot
  - kernel reaches userspace handoff again:
    - `Run /init as init process`
  - initramfs QEMU path starts:
    - `VIRTIOSO_INIT: Attempting QEMU virtio-blk boot`
  The active failure is no longer physical PCI-bus aperture setup. It has moved
  forward into the modern virtio feature contract for the passed-through block
  device:
  - `virtio_blk virtio1: virtio: device uses modern interface but does not have VIRTIO_F_VERSION_1`
  - `probe with driver virtio_blk failed with error -22`
  Current conclusion:
  - q35 physical PCI host apertures are now structurally passed through far
    enough for the guest to enumerate both outer modern virtio devices and boot
    into `/init`
  - remaining work is now at the virtio feature/config fidelity layer, not the
    q35 physical host-bridge aperture layer
- 2026-04-18: Architectural separation checkpoint for x86 physical q35 devices.
  Verified remaining hybrid hook from source:
  - `projects/vm/components/Init/src/main.c:auto_register_qemu_pci_passthrough()`
    was still auto-inserting physical q35 virtio devices into `vmm_pci_*`
  - ARM comparison remains:
    - physical `/pcie@10000000` is kept structurally
    - synthetic `vpci` is separate and not the physical bus path
  Implemented next coexistence slice:
  - add `physical_q35_pci_uses_structural_host_bridge(vm)`
  - make `auto_register_qemu_pci_passthrough()` return early on q35 when
    structural `MCFG` + `mem32` host apertures are present
  Validation in autopilot run `20260418-022802`:
  - `virtio_pci_driver_init` still completes even without synthetic insertion
  - guest still reaches:
    - `Run /init as init process`
    - `VIRTIOSO_INIT: Attempting QEMU virtio-blk boot`
  Important architectural conclusion:
  - on the current q35 path, physical device enumeration is no longer relying
    on `auto_register_qemu_pci_passthrough()`
  - that makes the structural host-bridge path the real primary mechanism for
    q35 physical devices
  Remaining caveat:
  - `vmm_pci_init()` still exists in the x86 path, so the full physical-vs-
    synthetic separation is not finished yet
- but the first major dependency has been removed: physical q35 devices no
    longer need synthetic device insertion for the observed boot path
- 2026-04-18: Refined the q35 physical-device path after checking the actual
  `20260418-022802` autopilot log more carefully.
  Correction:
  - that run did *not* show bound virtio devices
  - it only showed the drivers initializing and `/init` starting
  Root cause from source:
  - x86 PCI config accesses still go through `find_device(self, addr)` in
    `libsel4vmmplatsupport/src/arch/x86/drivers/vmm_pci_helper.c`
  - without per-device entries, the structural root bridge alone is not enough
    for enumeration on the current x86 path
  Implemented the next architectural slice in
  `projects/vm/components/Init/src/main.c`:
  - keep q35 physical devices in the VMM PCI space only as raw config-space
    mirrors at their original BDFs
  - remove BAR rewriting, IRQ-line rewriting, and MSI-cap stripping from the
    q35 physical-device path
  - for q35 raw-config mode, reserve/map each physical BAR at its original host
    address instead of creating synthetic guest BAR addresses
  Validation:
  - autopilot run `20260418-023540` proved the raw-config mirrors were active
    but failed at the first real BAR access with:
    - `EPT fault: gpa=0xfe000014`
  - autopilot run `20260418-024138` after backing the physical BAR pages showed:
    - `Auto passthrough PCI device ... mode=raw-config`
    - `virtio_blk virtio1: [vda] 1465652 512-byte logical blocks`
  Architectural conclusion:
  - q35 physical devices on x86 still currently need a thin config-space mirror
    for enumeration
  - but they no longer need synthetic BAR emulation, IRQ emulation, or MSI-cap
    filtering in the observed passthrough path
- 2026-04-18: Removed the last unconditional synthetic `00:00.0` seed from the
  x86 q35 path.
  Implemented:
  - `vmm_pci_init_empty()` in `libsel4vmmplatsupport` so x86 can initialise a
    PCI space without installing the fake host bridge device
  - `projects/vm/components/Init/src/main.c` now uses `vmm_pci_init_empty()`
    for structural q35 mode and raw-mirrors the real q35 host bridge at
    `00:00.0`
  Validation in autopilot run `20260418-024733`:
  - physical q35 virtio devices still enumerate through the raw-config mirror
  - `virtio_blk virtio1: [vda] ...` still appears
  - the guest still reports:
    - `PCI: root bus 00: using default resources`
  - kernel log still goes quiet immediately after:
    - `virtio_blk virtio1: [vda] ...`
    - `qemu-system-x86_64: Guest says index 506 is available`
    - `External IRQ badge irq=11`
  Conclusion:
  - the fake synthetic `00:00.0` host bridge was not the cause of the current
    post-`vda` stop
  - x86 q35 now has a thinner physical-bus path:
    - real host bridge at `00:00.0`
    - raw config-space mirrors for physical devices
    - structural BAR backing at host BAR addresses
- 2026-04-18: Tried removing the remaining CAM-port indirection by wiring
  q35 structural mode to the raw host PCI config ports (`0xcf8/0xcfc`) instead
  of `vmm_pci_io_port_in/out`.
  Validation in autopilot run `20260418-025315`:
  - guest still sees the q35 root bridge from ACPI and keeps the physical BDFs
  - guest still enumerates the raw host devices:
    - `00:02.0 [1af4:1041]`
    - `00:03.0 [1af4:1042]`
  - but no later virtio binding appears:
    - no `virtio_blk`
    - no `vda`
  Conclusion:
  - raw CAM-port access alone is not yet enough to replace the thin per-device
    config mirror on x86 q35
  - the remaining synthetic dependency is now clearly in config-space response
    semantics, not in host-bridge structure or BAR backing
- 2026-04-18: Tried the next structural step by backing the q35 host apertures
  more broadly instead of keeping only the first `2 MiB` chunks plus a few BAR
  pages.
  Attempted aperture decomposition:
  - full q35 `MMIO32` window represented as large power-of-two untyped objects
  - larger `MCFG` object at `0xb0000000`
  Validation in autopilot run `20260418-032537`:
  - CapDL created the requested device objects
  - but `Init` failed before guest boot with:
    - `Unknown object type -1`
    - `Failed to allocate page`
    - `Failed to map PCI config aperture at 0xb0000000 size 0x10000000`
  Source confirmation:
  - x86 VKA only supports page objects at:
    - `4 KiB`
    - `2 MiB`
    - `1 GiB`
  Architectural conclusion:
  - exact q35 `MMIO32` structural passthrough on x86 cannot be expressed with a
    handful of arbitrary `256 MiB` / `64 MiB` / `8 MiB` objects
  - the correct next implementation slice is to generate the q35 aperture as
    exact x86-supported chunks, which in practice means many `2 MiB` mappings
    (and optionally `1 GiB` only where it fits exactly)
  - the failed broad-aperture experiment was reverted immediately after the
    diagnosis, so the tree is back at the last known-good q35 checkpoint
- 2026-04-18: Ran a no-outer-virtio-blk control to separate late boot from
  block-device bring-up.
  Validation in autopilot run `20260418-194754`:
  - guest still reaches late init and userspace:
    - `Run /init as init process`
    - `VIRTIOSO_INIT: seL4 VM Unified Init`
  - `/dev/vda` is correctly absent in this control:
    - `VIRTIOSO_INIT: QEMU virtio-blk device missing: /dev/vda`
  - the dominant delay is instead the default q35 AHCI controller:
    - `ahci 0000:00:1f.2: can't derive routing for PCI INT A`
    - `ahci 0000:00:1f.2: PCI INT A: no GSI`
    - repeated `ata3.00: qc timeout ... IDENTIFY`
  - the first userspace handoff is delayed until after those AHCI timeouts:
    - `Freeing unused kernel image (initmem) memory`
    - `Run /init as init process`
  Source confirmation from local QEMU tree:
  - `sources/qemu/hw/i386/pc.c` defaults `pcms->sata_enabled = true`
  - `sources/qemu/hw/i386/pc_q35.c` instantiates the built-in `ich9-ahci`
    controller only when `pcms->sata_enabled` is enabled
  Conclusion:
  - the long "hang before /init" was not a generic late-boot stall
  - the default q35 AHCI controller is a major source of delay and noise on
    this path, independent of outer `virtio-blk`
  - the next machine-level fix is to run q35 with `sata=off` while restoring
    the intended outer `virtio-blk-pci` device
- 2026-04-18: Switched the outer machine to `q35,sata=off` and restored the
  intended modern `virtio-blk-pci` device.
  Validation in autopilot run `20260418-195455`:
  - the bogus q35 AHCI path is gone:
    - no `ahci 0000:00:1f.2`
    - no `ata3.00: qc timeout`
  - early boot is much faster and cleaner
  - the physical q35 `virtio-blk` path is unchanged and still functional:
    - `virtio_blk virtio1: [vda] 1465652 ...`
    - `External IRQ badge irq=11`
    - `ioapic inject irq=11 ...`
    - `IRQ ack handler irq=11 ...`
  - the run still stops before userspace handoff:
    - no `Run /init as init process`
    - no `VIRTIOSO_INIT`
  - the final visible complaint remains timer-side:
    - `CE: hpet increased min_delta_ns to 432478 nsec`
  Conclusion:
  - `q35,sata=off` is the correct machine baseline for this target
  - the previous 60-second delay was mostly q35 AHCI noise, now removed
  - the remaining post-`vda` stop is still consistent with the HPET /
    clockevent path, not with missing block-device enumeration
