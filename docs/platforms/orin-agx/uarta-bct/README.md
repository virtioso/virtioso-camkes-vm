# Orin AGX UARTA BCT Override

This directory contains the BSP flashing files needed to make the AGX Orin
40-pin header UART1 pins usable as a direct VM UARTA console.

Local evidence:

- Header UART1 TX/RX maps to Tegra UARTA, not UARTI.
- UARTA is `serial@3100000`, MMIO `0x03100000`, SPI `112`.
- The stock L4T firewall BCT locks UARTA differently from UARTI. VM1 then
  raises CBB/ACI RAS on the first UARTA register access.
- Earlycon access happens before a Linux UART driver can enable clocks or
  reset through BPMP. The UARTA VM console therefore depends on firmware/BCT
  leaving UARTA accessible and usable before VM1 Linux starts.

## Files

- `tegra234-mb2-bct-scr-p3701-0000-uarta-vm.dts`
  - MB2 SCR/firewall BCT that keeps the stock Tegra234 chip include and
    overrides only UARTA and UARTA_CAR firewall entries.
  - The UARTA policy is copied from the stock UARTI policy.
- `jetson-agx-orin-devkit-uarta-vm.conf`
  - Flash target wrapper that sources NVIDIA's stock
    `jetson-agx-orin-devkit.conf` and selects the UARTA SCR BCT file.

## Install Into Linux_for_Tegra

From this workspace:

```bash
L4T=/home/hlyytine/tii-sel4/vm-images/build/tmp/work-shared/L4T-native-36.4.3-r0/Linux_for_Tegra
cp docs/platforms/orin-agx/uarta-bct/tegra234-mb2-bct-scr-p3701-0000-uarta-vm.dts "$L4T/bootloader/generic/BCT/"
cp docs/platforms/orin-agx/uarta-bct/jetson-agx-orin-devkit-uarta-vm.conf "$L4T/"
```

## Flash Only The BCT Partitions

Put the AGX Orin in recovery mode, connect USB recovery, then run from the
Linux_for_Tegra directory. This updates the boot config table partitions only;
it does not flash the Linux kernel or rootfs.

```bash
cd /home/hlyytine/pkvm/Linux_for_Tegra
sudo ./flash.sh --no-systemimg -k A_MB1_BCT jetson-agx-orin-devkit-uarta-vm mmcblk0p1
sudo ./flash.sh --no-systemimg -k B_MB1_BCT jetson-agx-orin-devkit-uarta-vm mmcblk0p1
```

For a dry generation/signing pass without flashing:

```bash
cd /home/hlyytine/pkvm/Linux_for_Tegra
sudo ./flash.sh --no-flash --no-systemimg -k A_MB1_BCT jetson-agx-orin-devkit-uarta-vm mmcblk0p1
```

After flashing, boot the seL4 `vm_qemu_virtio` image that passes UARTA to VM1.
VM1 should be allowed to touch `0x03100000` without the previous CBB/ACI RAS.

If RAS still reports `ADDR = 0x8000000003100000`, the guest has reached UARTA
and the remaining problem is platform-side UARTA access, clock, or reset state,
not Linux Image alignment. The VM DT uses a self-contained 8250-compatible
earlycon node, so the first UARTA access happens before BPMP-mediated driver
probe.
