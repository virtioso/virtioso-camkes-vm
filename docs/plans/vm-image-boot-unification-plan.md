# Plan: Unify vm-image-boot init with minimal-init features

> **INSTRUCTIONS FOR CLAUDE CODE:**
> - When implementing steps, mark them done by changing `[ ]` to `[x]`
> - Do NOT remove any content from this file
> - Add notes below each step if needed to document what was done
> - This file is linked from the top-level CLAUDE.md

---

## Implementation Checklist

- [x] **Step 1**: Update bridge-initramfs-init init script
- [x] **Step 2**: Update bridge-initramfs-init_0.1.bb recipe
- [x] **Step 3**: Update vm-image-boot.bb image recipe
- [x] **Step 4**: Build and test on Orin AGX (2026-01-12: Platform detection, driver loading, TAP bridge, NFS+eMMC fallback all working)
- [ ] **Step 5**: Test on QEMU (if NFS server available)

---

## Goal
Use vm-image-boot as the single VM kernel initramfs by merging essential functionality from minimal-init into bridge-initramfs-init, with platform awareness.

## Requirements (from user)
- **Boot method**: NFS root primary, eMMC/loop mount fallback, then shell
- **TAP bridge**: Keep tap0/tap1 bridge setup (always)
- **Orin drivers**: Include Tegra-specific driver loading when on Orin AGX
- **Error handling**: Drop to shell only if both NFS and eMMC fail

## Files to Modify

### Primary: bridge-initramfs-init init script
**File**: `vm-images/virtioso-yocto-layers/meta-virtioso/recipes-core/bridge-initramfs-init/bridge-initramfs-init/init`

Current (58 lines): TAP bridge → DHCP → NFS mount → switch_root

New structure (~200 lines):
1. **Essential filesystem mount** (from minimal-init)
2. **Platform detection** (via /proc/device-tree/compatible)
3. **Platform-specific driver loading** (Orin: pmc-irq-domain, tegra_bpmp, nvethernet)
4. **TAP bridge setup** (keep existing)
5. **Try NFS boot** (DHCP + NFS mount)
6. **If NFS fails → Try eMMC/loop mount** (from minimal-init)
7. **If eMMC fails → Drop to shell**

### Secondary: bridge-initramfs-init recipe
**File**: `vm-images/virtioso-yocto-layers/meta-virtioso/recipes-core/bridge-initramfs-init/bridge-initramfs-init_0.1.bb`

Add runtime dependency for kernel modules (conditional on MACHINE).

### Tertiary: vm-image-boot image recipe
**File**: `vm-images/virtioso-yocto-layers/meta-virtioso/images/vm-image-boot.bb`

Add device node creation and platform-specific kernel modules.

## Implementation Details

### 1. Platform Detection Function
```sh
detect_platform() {
    if [ -f /proc/device-tree/compatible ]; then
        compat=$(cat /proc/device-tree/compatible | tr '\0' ' ')
        case "$compat" in
            *nvidia,jetson-agx-orin*) PLATFORM="orinagx" ;;
            *raspberrypi,4*)          PLATFORM="rpi4" ;;
            *)                        PLATFORM="qemu" ;;
        esac
    else
        PLATFORM="qemu"  # Default fallback
    fi
}
```

### 2. Platform-Specific Driver Loading

**IMPORTANT**: pmc-irq-domain is a **dummy PMC driver** that uses the same device tree
compatible string (`nvidia,tegra234-pmc`) as the real PMC driver. It MUST load FIRST
to "win" the device node before the real PMC driver. Do NOT use modalias scanning
before loading pmc-irq-domain, as it could trigger the real PMC driver.

```sh
load_platform_drivers() {
    case "$PLATFORM" in
        orinagx)
            # CRITICAL: pmc-irq-domain MUST load first!
            # It's a dummy driver that competes with real PMC for the same DT node.
            # GPIO driver defers probe until PMC IRQ domain exists.
            echo "Loading pmc-irq-domain (MUST be first - beats real PMC to DT node)..."
            modprobe pmc-irq-domain || echo "modprobe pmc-irq-domain failed (may be OK if no PMC node)"

            # BPMP driver - other drivers depend on it for clocks/resets
            echo "Loading tegra_bpmp..."
            modprobe tegra_bpmp || echo "modprobe tegra_bpmp failed"

            # Ethernet stack
            echo "Loading Ethernet drivers..."
            modprobe phy-tegra194-p2u || echo "modprobe phy-tegra194-p2u failed"
            modprobe nvpps || echo "modprobe nvpps failed"
            modprobe nvethernet || echo "modprobe nvethernet failed"
            ;;
        rpi4)
            # RPi4-specific drivers if needed
            ;;
        qemu)
            # QEMU virtio drivers auto-load
            ;;
    esac

    # Auto-load any remaining drivers via modalias (AFTER critical drivers)
    echo "Auto-loading remaining drivers via modalias..."
    find /sys -name modalias -print0 2>/dev/null | while IFS= read -r -d '' m; do
        modalias=$(cat "$m")
        modprobe "$modalias" || echo "modprobe $modalias failed"
    done

    # Common: TUN module for TAP devices
    echo "Loading TUN module..."
    modprobe tun || echo "modprobe tun failed"
}
```

**Note on driver loading order**:
1. Critical drivers (pmc-irq-domain, tegra_bpmp) MUST load first explicitly
2. pmc-irq-domain is a dummy driver competing with real PMC for the same DT node
3. Modalias scanning runs AFTER to catch any remaining drivers
4. TUN module loaded last (needed for TAP bridge, no modalias entry)

### 3. Error Handling
```sh
drop_to_shell() {
    echo "Boot failed. Dropping to shell for debugging..."
    echo "Run 'ip addr' to check network, 'mount' to check filesystems"
    exec /bin/sh
}
```

### 4. Boot Sequence with Fallback
```sh
# Try NFS first
try_nfs_boot() {
    echo "Attempting NFS boot..."
    udhcpc -i br0 -O rootpath -n -q  # -n = non-blocking, -q = quit after lease

    if [ ! -f /udhcpc.env ]; then
        echo "DHCP failed, no rootpath"
        return 1
    fi

    eval "$(grep rootpath= /udhcpc.env)"
    if [ -z "$rootpath" ]; then
        echo "No NFS rootpath from DHCP"
        return 1
    fi

    rootflags=$(echo "${rootpath}" | cut -d, -f2-)
    root=$(echo "${rootpath}" | cut -d, -f1)

    if ! mount -t nfs -o"$rootflags" "$root" /newroot; then
        echo "NFS mount failed: $root"
        return 1
    fi

    return 0
}

# eMMC fallback (from minimal-init)
try_emmc_boot() {
    echo "Attempting eMMC fallback..."

    # Wait for eMMC
    count=0
    while [ ! -b /dev/mmcblk0 ] && [ $count -lt 50 ]; do
        sleep 0.1
        count=$((count + 1))
    done

    if [ ! -b "$EMMC_PART" ]; then
        echo "eMMC partition not found: $EMMC_PART"
        return 1
    fi

    # Mount eMMC partition
    mkdir -p /mnt/emmc
    if ! mount -o rw "$EMMC_PART" /mnt/emmc; then
        echo "Failed to mount eMMC"
        return 1
    fi

    # Find and loop-mount rootfs image
    ROOTFS_PATH="/mnt/emmc/${ROOTFS_IMAGE}"
    if [ ! -f "$ROOTFS_PATH" ]; then
        echo "Rootfs image not found: $ROOTFS_PATH"
        return 1
    fi

    LOOP_DEV=$(losetup -f)
    if ! losetup "$LOOP_DEV" "$ROOTFS_PATH"; then
        echo "Loop device setup failed"
        return 1
    fi

    if ! mount -o rw "$LOOP_DEV" /newroot; then
        echo "Loop mount failed"
        losetup -d "$LOOP_DEV"
        return 1
    fi

    return 0
}

# Main boot logic
if try_nfs_boot; then
    echo "NFS boot successful"
elif try_emmc_boot; then
    echo "eMMC boot successful"
else
    echo "All boot methods failed"
    drop_to_shell
fi

exec switch_root /newroot /sbin/init
```

### 5. Recipe Changes

**bridge-initramfs-init_0.1.bb** - add directory creation and modules:
```bitbake
do_install:append() {
    install -m 0755 ${UNPACKDIR}/init ${D}/init

    # Create mount point directories (from minimal-init)
    install -d ${D}/proc
    install -d ${D}/sys
    install -d ${D}/dev
    install -d ${D}/tmp
    install -d ${D}/run
    install -d ${D}/newroot
    install -d ${D}/mnt/emmc
    install -d ${D}/etc/udhcpc.d
}

FILES:${PN} = "/init /proc /sys /dev /tmp /run /newroot /mnt /etc"

# Platform-specific kernel modules for Orin AGX
# Core drivers (currently used)
RDEPENDS:${PN}:append:vm-jetson-agx-orin = " \
    kernel-module-pmc-irq-domain \
    nv-kernel-module-tegra-bpmp \
    kernel-module-phy-tegra194-p2u \
    nv-kernel-module-nvpps \
    nv-kernel-module-nvethernet \
"

# Additional Tegra modules (from tegra-minimal-initramfs)
# Not used yet, but available for future device passthrough
RDEPENDS:${PN}:append:vm-jetson-agx-orin = " \
    tegra-firmware-xusb \
    kernel-module-nvme \
    kernel-module-pcie-tegra194 \
    kernel-module-tegra-xudc \
    kernel-module-ucsi-ccg \
"
```

**vm-image-boot.bb** - add device node creation from the legacy minimal initramfs:
```bitbake
# Device nodes for console - kernel needs /dev/console before running init
create_console_devices() {
    mkdir -p ${IMAGE_ROOTFS}/dev
    mknod -m 600 ${IMAGE_ROOTFS}/dev/console c 5 1
    mknod -m 666 ${IMAGE_ROOTFS}/dev/null c 1 3
    mknod -m 666 ${IMAGE_ROOTFS}/dev/tty c 5 0
    mknod -m 666 ${IMAGE_ROOTFS}/dev/ttyAMA0 c 204 64
}
ROOTFS_POSTPROCESS_COMMAND:append = " create_console_devices;"
```

## What's NOT Being Migrated
- devmem2 tool (can add to vm-image-boot recipe separately if needed)

## New Init Script Structure
```
1. mount_essential_filesystems()     # proc, sys, dev, tmp, devpts, loop devices
2. parse_cmdline()                   # rootfs_image, emmc_part, shell
3. detect_platform()                 # orinagx/rpi4/qemu
4. load_platform_drivers()           # platform-specific + TUN
5. wait_for_ethernet()               # wait for eth0 with timeout
6. setup_tap_bridge()                # br0, tap0, tap1
7. try_nfs_boot()                    # DHCP + NFS mount (primary)
8. try_emmc_boot()                   # eMMC + loop mount (fallback)
9. switch_root or drop_to_shell()    # based on boot success
```

## Verification

1. **Build vm-image-boot**:
   ```bash
   make shell
   cd vm-images && source setup.sh
   bitbake vm-image-boot
   ```
   Output: `vm-images/build/tmp/deploy/images/${MACHINE}/vm-image-boot-${MACHINE}.rootfs.cpio.gz`

2. **Copy to CAmkES images** (for Orin AGX):
   ```bash
   cp ~/tii-sel4/vm-images/build/tmp/deploy/images/vm-jetson-agx-orin/vm-image-boot-vm-jetson-agx-orin.rootfs.cpio.gz \
      ~/tii-sel4/projects/camkes-vm-images/orinagx/rootfs.cpio.gz
   cp ~/tii-sel4/vm-images/build/tmp/deploy/images/vm-jetson-agx-orin/Image \
      ~/tii-sel4/projects/camkes-vm-images/orinagx/linux
   ```

3. **Build and test vm_minimal on Orin AGX**:
   - Build with `make mrproper && make orinagx_defconfig && make vm_minimal`
   - Use `mcp__sel4-autopilot__test_sel4_efi`
   - Verify: driver loading messages, TAP bridge setup, NFS mount attempt (or shell if no NFS server)

4. **Test on QEMU** (if NFS server available):
   ```bash
   make simulate_vm_qemu_virtio
   ```

## Rollback Plan
Keep minimal-init recipe intact. If issues arise, can switch vm-image back to use minimal-init.
