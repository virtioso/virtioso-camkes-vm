# VM1 Launch Automation Options

Date: 2026-04-23

## Summary

The current `vm_qemu_virtio` user-VM launch path is optimized for an interactive
operator session, not for automated testing. Today the driver VM image ships
`screenrc` convenience files, and `screenrc-drivervm` launches
`/usr/bin/qemu-rnd-helper` from a root shell inside `screen`. That works for
manual benchmarking, but it gives automation no stable launch API, no durable
state model, and no explicit readiness contract for VM1.

The recommended direction is:

1. introduce a first-class non-interactive launcher interface inside driver-vm
2. make readiness explicit and machine-detectable
3. keep `screenrc` only as an optional debug/operator frontend
4. add init-service integration only after the launcher contract exists

This preserves the current nested-QEMU architecture while removing the current
dependency on a human typing commands at the driver-vm prompt.

## Current State

### What exists today

- `vm-image-driver` includes `qemu-rnd-helper` and the benchmark package:
  [vm-image-driver.bb](/home/hlyytine/tii-sel4/vm-images/virtioso-yocto-layers/meta-virtioso/images/vm-image-driver.bb:1),
  [vm-image-features.bbclass](/home/hlyytine/tii-sel4/vm-images/virtioso-yocto-layers/meta-virtioso/classes/vm-image-features.bbclass:1)
- `tii-benchmark` installs two `screenrc` files into `/home/root`:
  [tii-benchmark_1.0.bb](/home/hlyytine/tii-sel4/vm-images/virtioso-yocto-layers/meta-virtioso/recipes-benchmark/tii-benchmark/tii-benchmark_1.0.bb:1)
- `screenrc-drivervm` starts a shell plus one window running
  `/usr/bin/qemu-rnd-helper`:
  [screenrc-drivervm](/home/hlyytine/tii-sel4/vm-images/virtioso-yocto-layers/meta-virtioso/recipes-benchmark/tii-benchmark/tii-benchmark/screenrc-drivervm:1)
- the helper launches nested QEMU using the `uservm=` bootarg contract,
  expects the user image at `/var/lib/virt/images/user-vm.qcow2`, and binds
  guest console to the helper's stdio channel:
  [qemu-rnd-helper](/home/hlyytine/tii-sel4/vm-images/virtioso-yocto-layers/meta-virtioso/recipes-devtools/qemu/qemu-rnd-helper/qemu-rnd-helper:1)
- the driver image already installs the user image into
  `/var/lib/virt/images/user-vm.qcow2` during rootfs construction:
  [vm-guest-images-install.bbclass](/home/hlyytine/tii-sel4/vm-images/virtioso-yocto-layers/meta-virtioso/classes/vm-guest-images-install.bbclass:1)

### Current automation bottlenecks

- Launch is tied to a logged-in shell in driver-vm.
- `screen` owns lifecycle and console multiplexing, but it does not provide a
  structured state contract.
- `qemu-rnd-helper` is foreground-oriented. It backgrounds QEMU internally and
  waits on it, but it still assumes an attached terminal and routes guest
  console over stdio.
- There is no explicit `start/status/wait/stop` interface for VM1.
- There is no explicit readiness marker beyond inferring success from console
  text.
- Existing Autopilot flow still reflects this interactive model:
  [autopilot-chain-diagrams.md](/home/hlyytine/tii-sel4/projects/virtioso-camkes-vm/docs/reference/autopilot-chain-diagrams.md:523)

### Constraint that should be preserved

Do not collapse the nested-QEMU design into a different topology just to make
testing easier. The current architecture is:

- seL4 boots driver-vm
- driver-vm launches nested QEMU
- nested QEMU boots user-vm from `user-vm.qcow2`

The automation work should improve control and observability around that path,
not replace the path.

## Alternatives

## Option A: Keep `screen`, automate around it

Example:

- Autopilot logs into driver-vm
- runs `screen -c /home/root/screenrc-drivervm`
- scrapes output until user-vm appears healthy

Pros:

- minimal image changes
- preserves current operator workflow exactly

Cons:

- still depends on terminal semantics and `screen` window behavior
- hard to expose structured state
- hard to distinguish launcher failure from console multiplexing failure
- poor basis for retries, teardown, and health reporting

Assessment:

Useful only as a stopgap. It keeps the same root problem.

## Option B: Add a first-class launcher wrapper in driver-vm

Introduce a small wrapper, for example `uservmctl`, that owns VM1 lifecycle:

- `uservmctl start`
- `uservmctl status`
- `uservmctl wait-ready`
- `uservmctl stop`
- `uservmctl logs`

The wrapper would:

- launch `qemu-rnd-helper` or the underlying `qemu-system-*` command detached
- write a pidfile in `/run/uservm/`
- redirect console output to a durable log file
- update a state file with phases such as `starting`, `running`, `ready`,
  `failed`, `exited`
- expose a bounded readiness wait instead of making callers parse terminal UI

Pros:

- does not depend on a specific init system
- compatible with current image and topology
- gives Autopilot a stable non-interactive API immediately
- keeps `screenrc` usable by having it call `uservmctl logs` or `uservmctl attach`

Cons:

- requires some scripting/package work in the driver image
- readiness still needs an explicit contract from user-vm

Assessment:

Best near-term architecture. It separates lifecycle control from human terminal
UX and creates the seam that both CI and operators can share.

## Option C: Add boot-managed service startup in driver-vm

After Option B exists, the launcher can be bound to the image's init system:

- service enabled for benchmark or CI images
- service disabled for normal interactive images
- boot-time policy controlled by image feature, kernel cmdline, or config file

Pros:

- removes the need for a login step before VM1 launch
- better fit for soak tests and unattended bring-up
- makes VM1 part of the driver-vm boot contract when desired

Cons:

- depends on init-manager choice, which is not declared locally in the image
  recipes reviewed here
- harder to keep "manual debug" and "auto-start on boot" semantics separate
  unless paired with an explicit launch policy

Assessment:

Good second step, not the first step. The missing piece today is not "service
manager integration"; it is the lack of a launch contract.

## Option D: Move VM1 launch out of driver-vm

Examples:

- launch user-vm directly from seL4 side
- have Autopilot or host tooling spawn nested guest logic externally

Pros:

- could simplify some testing flows

Cons:

- changes the architecture under test
- invalidates the current driver-vm responsibility model
- no longer tests the nested-QEMU path that production and benchmark flows use

Assessment:

Reject for this problem. It solves automation by changing what is being tested.

## Readiness Verification Options

Launch automation is only half the problem. VM1 also needs a readiness signal
that is stronger than "some login prompt appeared."

## Readiness signal 1: Serial sentinel

Add a one-line marker from user-vm late in boot, for example:

`USERVM_READY version=1`

Pros:

- simplest to implement
- easy for Autopilot to wait on
- no extra transport required

Cons:

- still console-based
- weaker for functional validation than a service probe

Assessment:

Good baseline signal. This should exist even if stronger checks are added.

## Readiness signal 2: Shared-directory heartbeat

The user image already mounts a QEMU shared directory at `/mnt/shared`:
[vm-image-user.bb](/home/hlyytine/tii-sel4/vm-images/virtioso-yocto-layers/meta-virtioso/images/vm-image-user.bb:1)

User-vm can write a file such as:

- `/mnt/shared/uservm-ready.json`

Pros:

- machine-readable
- works without parsing console
- can carry richer metadata such as boot id, kernel version, test mode

Cons:

- depends on 9p being mounted and healthy
- slightly later signal than raw console

Assessment:

Best companion to a serial sentinel. Use this for structured verification.

## Readiness signal 3: Guest service probe

Examples:

- SSH reachable
- `iperf3` server reachable
- custom RPC endpoint reachable

Pros:

- proves the actual workload is alive, not just the OS boot

Cons:

- depends on more subsystems
- slower and more failure-prone as a first-line boot signal

Assessment:

Use as a functional test after basic VM1 readiness, not as the only readiness
definition.

## Recommended Architecture

## Recommendation

Implement Option B first, then optionally Option C.

Concrete target shape:

1. `qemu-rnd-helper` remains the low-level launcher.
2. Add a new wrapper package in the driver image, for example `uservmctl`.
3. `uservmctl start` launches VM1 detached and writes:
   - `/run/uservm/pid`
   - `/run/uservm/state`
   - `/run/uservm/console.log`
4. `uservmctl wait-ready` waits for:
   - serial sentinel in console log, and optionally
   - `/mnt/shared/uservm-ready.json`
5. `screenrc-drivervm` stops launching QEMU directly and instead opens:
   - a shell window
   - a `uservmctl logs` window
   - optional benchmark windows

This turns `screen` from the owner of VM1 lifecycle into a passive observer.

## Why this is the right split

- It preserves the current architecture and image layout.
- It removes shell interactivity as the control plane.
- It works even if the guest images do not standardize on `systemd`.
- It gives Autopilot a stable command surface:
  - send `uservmctl start`
  - wait for `uservmctl wait-ready`
  - run benchmark or health-check commands
- It keeps the existing benchmark and operator workflow available.

## Validation Path

## Phase 1: Launch contract

Implement and validate from driver-vm shell:

- `uservmctl start`
- `uservmctl status`
- `uservmctl wait-ready`
- `uservmctl stop`

Success criteria:

- no `screen` required for launch
- repeated starts fail cleanly or are idempotent
- failure states are visible in `/run/uservm/state`

## Phase 2: Readiness contract

Add user-vm readiness markers:

- console sentinel: `USERVM_READY version=1`
- shared-file marker: `/mnt/shared/uservm-ready.json`

Success criteria:

- Autopilot can wait on a stable marker without screen/window coupling
- readiness includes enough metadata to identify the boot instance

## Phase 3: Autopilot integration

Change the test flow from:

- login
- run `qemu-rnd-helper`
- map nested console
- scrape for prompt

to:

- login
- run `uservmctl start`
- wait for `uservmctl wait-ready`
- run explicit health checks

Suggested health checks:

- `uservmctl status` returns `ready`
- `/mnt/shared/uservm-ready.json` exists
- optional guest workload probe such as `iperf3` or a benchmark command

## Phase 4: Optional boot-managed mode

If unattended startup is still wanted after Phase 3:

- add init integration that invokes `uservmctl start`
- gate it behind an image feature or config file

This should be optional so manual bring-up images remain debuggable.

## Risks and Open Questions

- `qemu-rnd-helper` still defaults to stdio+monitor multiplexing, but it now
  has an explicit backend seam via `QEMU_CHARDEV_MODE`:
  - `stdio_mux` keeps the legacy monitor+console-on-stdio behavior
  - `stdio_console` routes only the guest virtconsole over stdio
  - `none_ringbuf` removes stdio console attachment entirely and keeps the
    console in a QEMU ring buffer
- that reduces the need for a one-off wrapper just to get out of mandatory
  monitor multiplexing, but it does not yet provide a full detached
  management/ready-state contract
- The current docs in the Autopilot chain diagram appear stale around VM1 login
  patterns and should be reviewed when the automation flow is updated.
- The best "ready" definition depends on what testing actually needs:
  OS boot, benchmark tooling ready, networking ready, or application ready.
- If the guest image stack later standardizes on a specific init manager, the
  same `uservmctl` contract can be reused under that manager without changing
  Autopilot again.
