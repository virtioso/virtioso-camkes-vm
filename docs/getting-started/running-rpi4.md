# Running on Raspberry Pi 4

This document describes how to deploy and run the TII seL4 virtio platform on Raspberry Pi 4 hardware.

## Hardware Requirements

| Item | Specification |
|------|---------------|
| Raspberry Pi 4 | Model B, 4GB or 8GB RAM |
| MicroSD Card | 32GB+ Class 10 |
| Power Supply | Official 5V/3A USB-C |
| USB-UART Adapter | 3.3V TTL (e.g., FTDI) |
| MicroHDMI Cable | Optional, for display |

## Serial Console Setup

### UART Connection

Connect USB-UART adapter to RPi4 GPIO:

| UART Pin | RPi4 GPIO | Description |
|----------|-----------|-------------|
| TX | GPIO 15 (RXD) | UART Receive |
| RX | GPIO 14 (TXD) | UART Transmit |
| GND | GND | Ground |

**Warning**: Do not connect VCC. Power the RPi4 via USB-C.

### Terminal Setup

```bash
# Linux
screen /dev/ttyUSB0 115200

# Or minicom
minicom -D /dev/ttyUSB0 -b 115200

# macOS
screen /dev/tty.usbserial-* 115200
```

## SD Card Preparation

### Build Images

```bash
cd $WORKSPACE

# Configure for RPi4
make raspberrypi4-64_defconfig

# Build guest images
make linux-image

# Build CAmkES app
make vm_qemu_virtio
```

### Partition Layout

Create SD card with following layout:

| Partition | Type | Size | Contents |
|-----------|------|------|----------|
| 1 | FAT32 | 256MB | Boot files |
| 2 | ext4 | Rest | Root filesystem |

### Format SD Card

```bash
# Identify SD card device (e.g., /dev/sdX)
lsblk

# Create partitions
sudo fdisk /dev/sdX
# n, p, 1, default, +256M
# t, c (FAT32)
# n, p, 2, default, default
# w

# Format partitions
sudo mkfs.vfat -F 32 /dev/sdX1
sudo mkfs.ext4 /dev/sdX2
```

### Copy Boot Files

```bash
# Mount boot partition
sudo mount /dev/sdX1 /mnt

# Copy boot files
sudo cp $WORKSPACE/rpi4_vm_qemu_virtio/images/capdl-loader-image-arm-bcm2711 /mnt/kernel8.img

# Copy device tree (if needed)
sudo cp $WORKSPACE/hardware/rpi4/dts/*.dtb /mnt/

# Create config.txt
cat << 'EOF' | sudo tee /mnt/config.txt
arm_64bit=1
kernel=kernel8.img
enable_uart=1
uart_2ndstage=1
dtoverlay=disable-bt
EOF

# Unmount
sudo umount /mnt
```

### Copy Root Filesystem

```bash
# Mount root partition
sudo mount /dev/sdX2 /mnt

# Extract device VM image
sudo tar -xf $WORKSPACE/vm-images/build/tmp/deploy/images/vm-raspberrypi4-64/vm-image-driver-vm-raspberrypi4-64.tar.gz -C /mnt

# Unmount
sudo umount /mnt
```

## Booting

### First Boot

1. Insert SD card into RPi4
2. Connect serial console
3. Power on RPi4
4. Watch boot output on serial terminal

### Expected Boot Output

```
Raspberry Pi Bootloader
...
ELF-loader started on CPU: ARM Ltd. Cortex-A72 r0p3
  paddr=[...]
Bringing up 3 other CPU(s)
Switching to hypervisor mode
Bootstrapping kernel
seL4 microkernel ...

[vm0] Linux version 5.x.x ...
device-vm login:

[vm1] Linux version 5.x.x ...
driver-vm login:
```

## Network Boot (Optional)

### TFTP Setup

For faster development iteration, boot via network:

```bash
# On host machine
sudo apt install tftpd-hpa

# Copy boot image
sudo cp $WORKSPACE/rpi4_vm_qemu_virtio/images/capdl-loader-image-arm-bcm2711 /srv/tftp/kernel8.img
```

### RPi4 Configuration

Edit `config.txt` on SD card:

```
arm_64bit=1
enable_uart=1
uart_2ndstage=1

# Network boot
kernel=
boot_ramdisk=0
tftp_prefix=
```

### DHCP Configuration

Configure DHCP server to provide TFTP server address.

## Debugging

### Serial Console

Primary debugging interface. All kernel and VM output appears here.

### LED Indicators

| LED | Meaning |
|-----|---------|
| Red PWR | Power connected |
| Green ACT | SD card activity |

### Common Issues

**No Serial Output**
- Check UART connections
- Verify baud rate (115200)
- Ensure `enable_uart=1` in config.txt

**Boot Hangs**
- Check power supply (must be 5V/3A)
- Try different SD card
- Check boot files are correct

**VM Fails to Start**
- Check guest images are present
- Verify sufficient RAM allocated

## Performance Tuning

### CPU Governor

```bash
# In guest VM
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
```

### Memory Allocation

Edit CAmkES configuration for VM memory:

```camkes
vm0.vm_ram_size = "1024";  // MB
vm1.vm_ram_size = "512";
```

## Hardware Passthrough

### USB Controller

The RPi4's USB controller can be passed to a guest VM.

### PCIe Devices

External PCIe devices (via HAT) can be passed through.

See [PCI Passthrough](../components/pci-passthrough.md) for details.

## Troubleshooting

### Boot Diagnostic

Enable verbose boot:

```
# config.txt
uart_2ndstage=1
bootcode_delay=3
boot_delay=3
```

### Memory Issues

```bash
# Check available memory
vcgencmd get_mem arm
vcgencmd get_mem gpu

# Adjust GPU memory (config.txt)
gpu_mem=16
```

### Kernel Panic

If kernel panics occur:
1. Enable debug output in kernel config
2. Check serial console for stack trace
3. Verify DTB matches hardware

## Next Steps

- [QEMU Development](running-qemu.md) for faster iteration
- [Debugging Guide](../reference/debugging.md)
- [Deployment Scenarios](../deployment/deployment-scenarios.md)
