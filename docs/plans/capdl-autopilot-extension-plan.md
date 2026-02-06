# Plan: Extend Autopilot for capdl-loader Testing

## Overview

Extend the existing autopilot framework to support building and testing CAmkES applications (vm_minimal, vm_qemu_virtio, etc.) on Orin AGX hardware. The capdl-loader serves as the init process for these applications.

## Key Hardware Configuration

### Dual UART Setup
| Device | Target | Output |
|--------|--------|--------|
| `/dev/ttyACM0` | ttyTCU0 (main UART) | elfloader, seL4 kernel, capdl-loader, VMM |
| `/dev/ttyACM1` | UARTI (hypervisor UART) | Guest Linux VM console |

For CAmkES VM applications, we need to capture **both** UARTs simultaneously:
- ttyACM0: seL4/VMM-level output (component initialization, faults, etc.)
- ttyACM1: Guest VM output (Linux boot, application logs)

**Existing infrastructure**: The Linux kernel test flow already captures ttyACM1 using `BootHarness.log_port()` - see `orin_kernel_autopilot.py` lines 422-428, 537-543. We can reuse this exact pattern.

### Binary Cleanup Requirement
Before uploading ANY test binary (sel4test or capdl-loader):
```bash
ssh root@192.168.101.112 "rm -f /boot/efi/sel4test-* /boot/efi/capdl-*"
```
This prevents confusion with old binaries and frees EFI partition space.

## Current State

### Autopilot Architecture (Reusable)
- **Request queue**: `requests/pending/` → `processing/` → `completed/`
- **Result storage**: `results/<timestamp>/` with logs and metadata
- **Boot harnesses**: SeL4UploadHarness, SeL4RunHarness, ReadyBootHarness
- **MCP server**: `sel4_mcp_server.py` with tool dispatch
- **Multi-run support**: Already implemented for stress testing

### sel4test vs capdl-loader Differences

| Aspect | sel4test | capdl-loader (CAmkES) |
|--------|----------|----------------------|
| Build script | `build_sel4.sh` | `build_camkes.sh` |
| Build directory | `orinagx_sel4test/` | `orinagx_<app>/` (e.g., `orinagx_vm_minimal`) |
| Output binary | `sel4test-driver-image-arm-orinagx` | `capdl-loader-image-arm-orinagx` |
| Binary size | ~3.4 MB | ~50 MB (includes CPIO with Linux images) |
| Boot time | ~5-10 seconds | ~30-60+ seconds (VM boot) |
| Output format | Test pass/fail markers | CAmkES component logs + VM console |
| UART capture | ttyACM0 only | **Both ttyACM0 and ttyACM1** |
| Quiescence | 30 sec after "All tests passed" | Configurable (VM may keep running) |

## Implementation Plan

### Phase 1: Build Infrastructure

#### 1.1 Add `build_capdl_app()` function to MCP server

**File**: `/home/hlyytine/autopilot/sel4_mcp_server.py`

```python
async def build_capdl_app(app: str, mode: str = "el2") -> dict:
    """
    Build a CAmkES application for Orin AGX.

    Args:
        app: Application name (vm_minimal, vm_qemu_virtio, etc.)
        mode: Build mode (el2, el1, el2-ftrace)

    Returns:
        {success, binary_path, build_time, app_name}
    """
```

**Implementation steps**:
1. Map mode to defconfig (same as sel4test)
2. Run `make orinagx_{mode}_defconfig`
3. Run `make {app}` (e.g., `make vm_minimal`)
4. Return binary path: `orinagx_{app}/images/capdl-loader-image-arm-orinagx`

#### 1.2 Add build configurations for capdl apps

**File**: `/home/hlyytine/tii-sel4/virtioso-build/scripts/cmake_vars.map`

Ensure CAmkES-specific variables are mapped:
- `CAMKES_VM_APP` → passed to build script
- `CapDLLoaderMaxObjects` → object limit
- VM-specific options (VmPCISupport, VmVirtioNet, etc.)

### Phase 2: Upload Phase Enhancement

#### 2.1 Add cleanup step to upload harness

**File**: `/home/hlyytine/autopilot/seL4BootHarness.py`

Modify `SeL4UploadHarness` and `SeL4UploadOnlyHarness` to clean up old binaries:

```python
def cleanup_old_binaries(self):
    """Remove old sel4test and capdl binaries from EFI partition."""
    cleanup_cmd = "rm -f /boot/efi/sel4test-* /boot/efi/capdl-*"
    result = subprocess.run(
        ["ssh", "root@192.168.101.112", cleanup_cmd],
        capture_output=True, timeout=30
    )
    if result.returncode != 0:
        logger.warning(f"Cleanup warning: {result.stderr.decode()}")
```

**Call order in upload phase**:
1. Boot to stock Linux (if needed)
2. **Cleanup old binaries** ← NEW
3. SCP new binary to `/boot/efi/`
4. SSH reboot to trigger UEFI

#### 2.2 Update existing sel4test workflow

Apply the same cleanup to sel4test uploads for consistency:
- Prevents EFI partition from filling up
- Ensures only one test binary is present

### Phase 3: Dual UART Capture

**Key insight**: The Linux test flow already captures ttyACM1 using `BootHarness.log_port()`. We reuse this exact pattern for capdl-loader testing - no new classes needed.

#### 3.1 Reuse existing `log_port()` function

**File**: `/home/hlyytine/autopilot/BootHarness.py` (lines 48-55)

```python
def log_port(dev, fname, stop_event, baud=115200):
    """Background thread function to log a serial port to a file."""
    ser = serial.Serial(dev, baudrate=baud, timeout=0.1)
    with ser, open(fname, 'ab', buffering=0) as f:
        while not stop_event.is_set():
            data = ser.read(1024)
            if not data:
                continue
            f.write(data)
```

#### 3.2 Pattern for capdl test with dual UART (in orin_kernel_autopilot.py)

```python
# Start VM console capture (background thread)
vm_stop_evt = threading.Event()
vm_log_thread = threading.Thread(
    target=BootHarness.log_port,
    args=('/dev/ttyACM1', str(result_dir / 'vm-uart-raw.log'), vm_stop_evt),
    daemon=True
)
vm_log_thread.start()

# Run capdl-loader (uses ttyACM0 for seL4 output)
runner = seL4BootHarness.SeL4RunHarness(...)
runner.run()

# Stop VM console capture
vm_stop_evt.set()
vm_log_thread.join(timeout=1.0)
```

This is the exact same pattern used for Linux kernel tests - see lines 422-428, 537-543 in `orin_kernel_autopilot.py`.

#### 3.3 Result directory structure for capdl

```
results/<timestamp>/
├── uart-raw.log          # ttyACM0: seL4/capdl-loader raw output
├── vm-uart-raw.log       # ttyACM1: VM console raw output (NEW)
├── sel4.log              # Filtered seL4/capdl-loader output
├── vm.log                # Filtered VM console output (NEW)
├── upload.log            # Upload phase
├── recovery.log          # Recovery boot
├── config.json           # Build configuration
└── run_N/                # Multi-run (if applicable)
    ├── uart-raw.log
    ├── vm-uart-raw.log
    ├── sel4.log
    └── vm.log
```

### Phase 4: Log Filtering

#### 4.1 Create `filter_capdl_start.py`

**File**: `/home/hlyytine/autopilot/filter_capdl_start.py`

```python
#!/usr/bin/env python3
"""Filter capdl-loader output, removing bootloader/UEFI noise."""

import sys
import re

# Start markers for capdl-loader output
CAPDL_START_MARKERS = [
    "ELF-loader started",
    "Bootstrapping kernel",
    "Booting all finished",
]

def filter_capdl_log(input_file, output_file):
    """
    Filter rules:
    1. Skip everything before start markers
    2. Keep all CAmkES component output
    3. Strip ANSI escape sequences
    """
    started = False
    with open(input_file, 'rb') as f_in:
        with open(output_file, 'wb') as f_out:
            for line in f_in:
                # Check for start markers
                for marker in CAPDL_START_MARKERS:
                    if marker.encode() in line:
                        started = True
                        break

                if started:
                    # Strip ANSI escapes
                    clean_line = re.sub(rb'\x1b\[[0-9;]*[mK]', b'', line)
                    f_out.write(clean_line)

if __name__ == "__main__":
    filter_capdl_log(sys.argv[1], sys.argv[2])
```

#### 4.2 Create `filter_vm_console.py` for VM UART

**File**: `/home/hlyytine/autopilot/filter_vm_console.py`

```python
#!/usr/bin/env python3
"""Filter VM console output from ttyACM1."""

import sys
import re

# VM console start markers
VM_START_MARKERS = [
    "Linux version",
    "Booting Linux",
    "[    0.000000]",  # First kernel log
]

def filter_vm_log(input_file, output_file):
    """Filter VM console, keeping from Linux boot onwards."""
    started = False
    with open(input_file, 'rb') as f_in:
        with open(output_file, 'wb') as f_out:
            for line in f_in:
                for marker in VM_START_MARKERS:
                    if marker.encode() in line:
                        started = True
                        break
                if started:
                    clean_line = re.sub(rb'\x1b\[[0-9;]*[mK]', b'', line)
                    f_out.write(clean_line)

if __name__ == "__main__":
    filter_vm_log(sys.argv[1], sys.argv[2])
```

#### 4.3 Quiescence detection (no success criteria yet)

For now, we just capture logs until ttyACM1 is silent for 5 seconds:

1. Start capturing both UARTs
2. Monitor ttyACM1 for activity
3. When 5 seconds pass with no output → stop capture
4. Save logs, mark test as complete (no pass/fail yet)

Success criteria will be added later. For now, just capture the logs.

### Phase 5: Request Handling

#### 5.1 Add `capdl` request type

**File**: `/home/hlyytine/autopilot/orin_kernel_autopilot.py`

Add new branch in request processing:

```python
def process_request(request, result_dir):
    req_type = request.get("type")

    if req_type == "sel4":
        process_sel4_request(request, result_dir)
    elif req_type == "capdl":
        process_capdl_request(request, result_dir)
    elif req_type == "linux":
        process_linux_request(request, result_dir)
    else:
        raise ValueError(f"Unknown request type: {req_type}")
```

#### 5.2 Implement `process_capdl_request()`

```python
def process_capdl_request(request, result_dir):
    """Process a capdl-loader test request."""
    binary_path = request["binary_path"]
    binary_name = request["binary_name"]
    app_name = request.get("app_name", "unknown")
    timeout = request.get("timeout", 120)
    app_config = get_app_config(app_name)

    # Phase 1: Upload (with cleanup)
    cleanup_old_binaries()  # NEW: Remove sel4test-* and capdl-*
    upload_harness = SeL4UploadOnlyHarness(binary_path, binary_name)
    upload_harness.run(f"{result_dir}/upload.log")

    # Phase 2: Run with dual UART capture
    run_harness = CapDLRunHarness(binary_name, timeout, app_config)
    sel4_raw, vm_raw = run_harness.run(result_dir)

    # Phase 3: Filter logs (parallel)
    filter_capdl_log(sel4_raw, f"{result_dir}/sel4.log")
    if os.path.exists(vm_raw):
        filter_vm_log(vm_raw, f"{result_dir}/vm.log")

    # Phase 4: Detect result
    result, marker, source = detect_capdl_result(
        f"{result_dir}/sel4.log",
        f"{result_dir}/vm.log",
        app_config
    )

    # Phase 5: Recovery
    recovery_harness = ReadyBootHarness()
    recovery_harness.run(f"{result_dir}/recovery.log")

    # Phase 6: Write metadata
    write_config_json(result_dir, request, result, marker, source)

    return result
```

#### 5.3 Request JSON format

```json
{
    "type": "capdl",
    "binary_path": "/home/hlyytine/tii-sel4/orinagx_vm_minimal/images/capdl-loader-image-arm-orinagx",
    "binary_name": "capdl-vm_minimal-20260102-143000.efi",
    "app_name": "vm_minimal",
    "description": "vm_minimal basic boot test",
    "timeout": 120,
    "submitted_at": "20260102-143000",
    "build_config": {
        "arm_hyp": true,
        "platform": "orinagx",
        "app": "vm_minimal"
    }
}
```

### Phase 6: MCP Server Tools

#### 6.1 Add CAmkES-specific MCP tools

**File**: `/home/hlyytine/autopilot/sel4_mcp_server.py`

```python
# VM boot detection settings
VM_QUIESCENCE_TIMEOUT = 5  # seconds without output = test complete

@tool
async def build_vm_minimal(mode: str = "el2") -> dict:
    """
    Build vm_minimal CAmkES application for Orin AGX.

    Args:
        mode: Build mode (el2, el1)

    Returns:
        {success, binary_path, build_time}
    """
    # Similar to build_sel4test but calls:
    # make orinagx_{mode}_defconfig && make vm_minimal
    pass

@tool
async def test_vm_minimal(
    binary_path: str,
    description: str = ""
) -> dict:
    """
    Test vm_minimal on Orin AGX hardware.

    Captures both seL4 UART (ttyACM0) and VM console (ttyACM1).
    Waits until 5 seconds of no output on ttyACM1 (VM console),
    then stops capture.

    No success criteria yet - just captures logs.

    Args:
        binary_path: Path to capdl-loader-image-arm-orinagx
        description: Optional test description

    Returns:
        {request_id, sel4_log_path, vm_log_path}
    """
    pass

@tool
async def get_vm_logs(request_id: str) -> dict:
    """
    Get logs from a vm_minimal test.

    Args:
        request_id: Test timestamp

    Returns:
        {sel4_log: str, vm_log: str}
    """
    pass
```

### Phase 7: Client Library

#### 7.1 Extend `sel4_client.py`

**File**: `/home/hlyytine/autopilot/sel4_client.py`

```python
def submit_capdl_test(
    binary_path: str,
    app_name: str,
    binary_name: str = None,
    description: str = "",
    timeout: int = None,
    build_config: dict = None
) -> str:
    """
    Submit a capdl-loader test to the autopilot queue.

    Args:
        binary_path: Path to capdl-loader EFI binary
        app_name: CAmkES application name
        binary_name: Name to use on target (auto-generated if None)
        description: Test description
        timeout: Override default timeout
        build_config: Build configuration metadata

    Returns:
        Request timestamp (use with get_status, wait_for_result)
    """
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")

    request = {
        "type": "capdl",
        "binary_path": binary_path,
        "binary_name": binary_name or f"capdl-{app_name}-{timestamp}.efi",
        "app_name": app_name,
        "description": description,
        "timeout": timeout,
        "submitted_at": timestamp,
        "build_config": build_config or {
            "arm_hyp": True,
            "platform": "orinagx",
            "app": app_name
        }
    }

    # Copy binary to staging
    staging_path = f"{BINARIES_DIR}/{request['binary_name']}"
    shutil.copy(binary_path, staging_path)
    request["binary_path"] = staging_path

    # Write request
    request_file = f"{PENDING_DIR}/{timestamp}.request"
    with open(request_file, 'w') as f:
        json.dump(request, f, indent=2)

    return timestamp


def get_capdl_logs(timestamp: str) -> dict:
    """
    Get both seL4 and VM logs from a capdl test.

    Returns:
        {"sel4_log": str, "vm_log": str or None}
    """
    result_dir = f"{RESULTS_DIR}/{timestamp}"
    logs = {}

    sel4_log = f"{result_dir}/sel4.log"
    if os.path.exists(sel4_log):
        with open(sel4_log, 'r') as f:
            logs["sel4_log"] = f.read()

    vm_log = f"{result_dir}/vm.log"
    if os.path.exists(vm_log):
        with open(vm_log, 'r') as f:
            logs["vm_log"] = f.read()
    else:
        logs["vm_log"] = None

    return logs
```

### Phase 8: Update sel4test Workflow

Apply cleanup to existing sel4test workflow for consistency:

**File**: `/home/hlyytine/autopilot/seL4BootHarness.py`

```python
def cleanup_old_binaries():
    """Remove old sel4test and capdl binaries from EFI partition."""
    try:
        result = subprocess.run(
            ["ssh", "root@192.168.101.112",
             "rm -f /boot/efi/sel4test-* /boot/efi/capdl-*"],
            capture_output=True,
            timeout=30
        )
        logger.info("Cleaned up old binaries from /boot/efi")
    except subprocess.TimeoutExpired:
        logger.warning("Cleanup timed out, continuing anyway")
```

Call this in both `SeL4UploadHarness.run()` and `SeL4UploadOnlyHarness.run()`.

## File Changes Summary

| File | Change Type | Description |
|------|-------------|-------------|
| `sel4_mcp_server.py` | Modify | Add `build_vm_minimal`, `test_vm_minimal`, `get_vm_logs` |
| `orin_kernel_autopilot.py` | Modify | Add `vm_minimal` request type with dual UART + 5s quiescence on ttyACM1 |
| `sel4_client.py` | Modify | Add `submit_vm_minimal_test`, `get_vm_logs` |
| `seL4BootHarness.py` | Modify | Add `cleanup_old_binaries()` call in upload harnesses |
| `filter_capdl_start.py` | **NEW** | Filter capdl-loader output (ttyACM0) |
| `filter_vm_console.py` | **NEW** | Filter VM console output (ttyACM1) |

## Reused Infrastructure (No Changes Needed)

- `BootHarness.log_port()` - background UART logging (for ttyACM1)
- Request queue system (requests/pending/, processing/, completed/, failed/)
- Result directory structure (extended, not replaced)
- Recovery mechanism (ReadyBootHarness)
- MCP server framework
- Multi-run support (works with any request type)
- UEFI navigation sequence (SeL4RunHarness)
- SeL4UploadHarness / SeL4UploadOnlyHarness

## Testing Plan

### Smoke Tests
1. **Cleanup verification**: Confirm old binaries removed before upload (both sel4test and capdl)
2. **Single UART (sel4test)**: Verify existing workflow still works with cleanup
3. **Dual UART (capdl)**: Verify both logs captured
4. **VM boot detection**: Verify success marker detection

### Integration Tests
1. Build and test `vm_minimal`
2. Build and test `vm_qemu_virtio` (multi-VM)
3. Multi-run stress test
4. Recovery after failed test

## Implementation Priority

**Phase 0 (Quick Win)**: Add cleanup to existing sel4test workflow
- Modify `SeL4UploadHarness.run()` and `SeL4UploadOnlyHarness.run()`
- Add SSH cleanup command before SCP upload
- Benefits both sel4test and future capdl testing

**Phase 1-8**: As described above

## Resolved Questions

1. **VM UART baud rate**: Yes, ttyACM1 is also 115200 baud ✓
2. **Scope**: Focus on `vm_minimal` only (not vm_qemu_virtio) ✓
3. **Quiescence detection**: 5 seconds without output on ttyACM1 → stop capture
4. **Result determination**:
   - No success criteria yet (will be added later)
   - Always mark as "failed" for now - just captures the logs
   - Linux is expected to boot; we're just waiting for console to go quiet
