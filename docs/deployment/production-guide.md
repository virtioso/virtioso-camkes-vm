# Production Guide

This document provides guidance for deploying the TII seL4 virtio system in production environments.

## Overview

Production deployment requires attention to:

- Security hardening
- Performance optimization
- Monitoring and logging
- Maintenance procedures

## Security Hardening

### seL4 Security Model

seL4 provides formal verification guarantees:

```
┌─────────────────────────────────────────┐
│         Verified Kernel Core            │
│  - Memory isolation                     │
│  - Capability-based access control      │
│  - Information flow control             │
└─────────────────────────────────────────┘
```

### VM Isolation

**Principle of Least Privilege:**
- Each VM receives only necessary capabilities
- Dataports explicitly shared between specific VMs
- IRQs assigned per-VM

**CAmkES Configuration:**
```camkes
configuration {
    // Minimal permissions
    vm0._priority = 97;
    vm0._affinity = 0;

    // Explicit resource assignment
    vm0.simple_untyped24_pool = 16;
}
```

### Guest Linux Hardening

**Kernel Configuration:**
```kconfig
# Enable security features
CONFIG_SECURITY=y
CONFIG_SECCOMP=y
CONFIG_STRICT_KERNEL_RWX=y
CONFIG_HARDENED_USERCOPY=y

# Disable unnecessary features
CONFIG_MODULES=n  # If modules not needed
CONFIG_KEXEC=n
CONFIG_HIBERNATION=n
```

**Runtime Security:**
```bash
# Disable unnecessary services
systemctl disable bluetooth
systemctl disable avahi-daemon

# Set restrictive permissions
chmod 700 /root
chmod 600 /etc/shadow
```

### Network Security

**Device VM Firewall:**
```bash
# Default deny
iptables -P INPUT DROP
iptables -P FORWARD DROP
iptables -P OUTPUT DROP

# Allow established connections
iptables -A INPUT -m state --state ESTABLISHED,RELATED -j ACCEPT
iptables -A OUTPUT -m state --state ESTABLISHED,RELATED -j ACCEPT

# Allow specific services
iptables -A INPUT -p tcp --dport 22 -j ACCEPT  # SSH
```

**Driver VM Isolation:**
```bash
# Only allow communication with device VM
iptables -A OUTPUT -d 10.0.0.1 -j ACCEPT
iptables -A OUTPUT -j DROP
```

## Performance Optimization

### Large Page Mappings

Use 2MB pages for better TLB performance:

```camkes
configuration {
    // Enable large pages for guest RAM
    vm0.ram_large_pages = true;
}
```

### SWIOTLB Sizing

Balance between DMA buffer availability and memory usage:

| Use Case | SWIOTLB Size | Recommendation |
|----------|--------------|----------------|
| Minimal I/O | 32MB | Basic workloads |
| Standard | 64MB | Most applications |
| High I/O | 128MB+ | Database, storage |

```
# Kernel command line
swiotlb=32768   # 128MB (32768 * 4KB pages)
```

### CPU Pinning

Assign VMs to specific CPU cores:

```camkes
configuration {
    vm0._affinity = 0;  // Core 0
    vm1._affinity = 1;  // Core 1
    vm2._affinity = 2;  // Core 2
}
```

### IRQ Affinity

Balance interrupt handling:

```bash
# Pin virtio IRQ to specific CPU
echo 2 > /proc/irq/XX/smp_affinity
```

### Memory Tuning

**Guest Linux:**
```bash
# Reduce swappiness
echo 10 > /proc/sys/vm/swappiness

# Disable transparent huge pages (if causing issues)
echo never > /sys/kernel/mm/transparent_hugepage/enabled
```

### virtio Performance

**Ring Size:**
```bash
# Increase virtio ring size (if supported)
ethtool -G eth0 rx 1024 tx 1024
```

**Interrupt Coalescing:**
```bash
# Reduce interrupt rate
ethtool -C eth0 rx-usecs 100
```

## Monitoring

### System Metrics

**Device VM Monitoring:**
```bash
# CPU usage
top -b -n1 | head -20

# Memory usage
free -h

# Disk I/O
iostat -x 1

# Network
netstat -i
```

**QEMU Monitoring:**
```bash
# QEMU process stats
ps aux | grep qemu

# QEMU monitor (if enabled)
echo "info status" | nc localhost 4444
```

### seL4 Debugging

**Kernel Tracing (if enabled):**
```c
// Enable tracing in kernel build
CONFIG_DEBUG_BUILD=y
CONFIG_BENCHMARK_TRACK_KERNEL_ENTRIES=y
```

**VMM Logging:**
```c
// In VMM code
ZF_LOGI("VM %d: MMIO access at 0x%lx", vm_id, addr);
```

### Health Checks

**VM Status Script:**
```bash
#!/bin/bash
# Check all VMs are running

check_vm() {
    local vm_name=$1
    if pgrep -f "$vm_name" > /dev/null; then
        echo "$vm_name: OK"
    else
        echo "$vm_name: FAILED"
        return 1
    fi
}

check_vm "qemu-system-aarch64"
check_vm "user-vm"
```

### Alerting

**Log Monitoring:**
```bash
# Watch for errors
tail -f /var/log/messages | grep -E "(error|fail|panic)"
```

**Automated Alerts:**
```bash
# Simple alerting script
if ! pgrep -f "qemu" > /dev/null; then
    echo "QEMU process died!" | mail -s "Alert" admin@example.com
fi
```

## Logging

### Centralized Logging

**Device VM as Log Server:**
```bash
# rsyslog configuration
# /etc/rsyslog.conf
$ModLoad imudp
$UDPServerRun 514

# Log to file
*.* /var/log/all-vms.log
```

**Driver VM Client:**
```bash
# Forward logs to device VM
*.* @10.0.0.1:514
```

### Log Rotation

```bash
# /etc/logrotate.d/vm-logs
/var/log/all-vms.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
}
```

### Audit Logging

```bash
# Enable audit daemon
auditctl -e 1

# Log all execve calls
auditctl -a exit,always -S execve

# Log file access
auditctl -w /etc/passwd -p wa -k passwd_changes
```

## Maintenance

### Updates

**Guest Image Updates:**
1. Build new Yocto image
2. Test in staging environment
3. Schedule maintenance window
4. Replace image and reboot

```bash
# Backup existing image
cp /var/lib/virt/images/user-vm.qcow2 /backup/

# Replace with new image
cp /new-images/user-vm.qcow2 /var/lib/virt/images/

# Restart VM
systemctl restart qemu-virtio
```

### Backup

**VM Image Backup:**
```bash
# Stop VM gracefully
systemctl stop qemu-virtio

# Backup image
tar -czvf backup-$(date +%Y%m%d).tar.gz \
    /var/lib/virt/images/

# Restart VM
systemctl start qemu-virtio
```

**Configuration Backup:**
```bash
# Backup seL4 configuration
tar -czvf config-backup.tar.gz \
    /boot/sel4/ \
    /etc/qemu/
```

### Recovery

**Boot Failure Recovery:**
1. Boot from rescue image
2. Mount root filesystem
3. Check/repair filesystem
4. Restore from backup if needed

**VM Crash Recovery:**
```bash
# Check QEMU logs
journalctl -u qemu-virtio

# Restart with debugging
qemu-system-aarch64 -d guest_errors ...
```

## High Availability

### Watchdog

**Hardware Watchdog:**
```bash
# Enable watchdog
modprobe bcm2835_wdt

# Configure watchdog daemon
# /etc/watchdog.conf
watchdog-device = /dev/watchdog
watchdog-timeout = 60
```

### Automatic Restart

**Systemd Service:**
```ini
# /etc/systemd/system/qemu-virtio.service
[Unit]
Description=QEMU virtio backend
After=network.target

[Service]
Type=simple
ExecStart=/usr/bin/qemu-system-aarch64 ...
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

### Health Monitoring

```bash
#!/bin/bash
# Health check script

# Check QEMU process
if ! pgrep -f "qemu-system-aarch64" > /dev/null; then
    systemctl restart qemu-virtio
    logger "QEMU restarted by health check"
fi

# Check virtio devices responding
if ! timeout 5 cat /dev/vda > /dev/null 2>&1; then
    logger "virtio-blk not responding"
fi
```

## Troubleshooting

### Common Issues

| Symptom | Possible Cause | Solution |
|---------|----------------|----------|
| VM won't boot | Image corruption | Restore from backup |
| Slow I/O | SWIOTLB too small | Increase swiotlb size |
| Network failure | Bridge misconfigured | Check bridge settings |
| Guest crash | Memory issue | Check dmesg, increase RAM |

### Debug Information Collection

```bash
#!/bin/bash
# Collect debug info
mkdir -p /tmp/debug

# System info
uname -a > /tmp/debug/uname.txt
cat /proc/cpuinfo > /tmp/debug/cpuinfo.txt
free -h > /tmp/debug/memory.txt

# Logs
dmesg > /tmp/debug/dmesg.txt
journalctl -b > /tmp/debug/journal.txt

# Process info
ps aux > /tmp/debug/processes.txt

# Create archive
tar -czvf /tmp/debug-$(date +%Y%m%d-%H%M%S).tar.gz /tmp/debug/
```

### Performance Profiling

```bash
# CPU profiling
perf record -g -p $(pgrep qemu) sleep 30
perf report

# I/O profiling
iotop -b -n 10

# Memory profiling
vmstat 1 10
```

## Checklist

### Pre-Production

- [ ] Security hardening applied
- [ ] Performance tuning complete
- [ ] Monitoring configured
- [ ] Logging enabled
- [ ] Backup procedures tested
- [ ] Recovery procedures tested
- [ ] Documentation complete

### Go-Live

- [ ] Final security audit
- [ ] Performance baseline captured
- [ ] Alerting verified
- [ ] On-call procedures defined
- [ ] Rollback plan ready

### Post-Production

- [ ] Monitor for issues
- [ ] Review logs regularly
- [ ] Update procedures as needed
- [ ] Regular backup verification
- [ ] Periodic security reviews

## Source Files

| File | Description |
|------|-------------|
| System configs | `/etc/` configurations |
| VM images | `/var/lib/virt/images/` |
| Logs | `/var/log/` |

## Related Documentation

- [Deployment Scenarios](deployment-scenarios.md)
- [Building](../getting-started/building.md)
- [Running on RPi4](../getting-started/running-rpi4.md)
