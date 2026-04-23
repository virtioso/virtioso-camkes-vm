#!/usr/bin/env python3
"""Manual QEMU runner for local and remote seL4 simulation targets.

This tool is the source of truth for QEMU-backed execution outside autopilot.
Autopilot should invoke this script instead of hardcoding QEMU launch logic.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SCRIPT_DIR = Path(__file__).resolve().parent
WORKSPACE_ROOT = SCRIPT_DIR.parent.parent.parent
VM_IMAGES_DIR = WORKSPACE_ROOT / "vm-images"
DEFAULT_REMOTE_CONFIG = Path.home() / ".virtioso-qemu-runners.json"
DEFAULT_RUNTIME_DEPLOY_DIR = VM_IMAGES_DIR / "build" / "tmp" / "deploy" / "virtioso-qemu-runtime"


@dataclass(frozen=True)
class TargetSpec:
    defconfig: str
    qemu_binary: str
    requires_remote: bool
    fallback_qemu_args: tuple[str, ...] = ()
    default_extra_qemu_args: tuple[str, ...] = ()


@dataclass(frozen=True)
class QemuRuntime:
    binary: Path
    support_usr: Path
    bios_dir: Path | None = None
    interpreter: Path | None = None


TARGETS: dict[str, TargetSpec] = {
    "qemu_arm64_defconfig": TargetSpec(
        defconfig="qemu_arm64_defconfig",
        qemu_binary="qemu-system-aarch64",
        requires_remote=False,
        fallback_qemu_args=(
            "-machine",
            "virt,virtualization=on,highmem=off,secure=off",
            "-cpu",
            "cortex-a57",
            "-m",
            "2048",
            "-nographic",
            "-serial",
            "mon:stdio",
        ),
    ),
    "qemu_x86_64_defconfig": TargetSpec(
        defconfig="qemu_x86_64_defconfig",
        qemu_binary="qemu-system-x86_64",
        requires_remote=True,
        default_extra_qemu_args=("-enable-kvm",),
    ),
}


class RunnerError(RuntimeError):
    pass


def _shell_join(argv: Iterable[str]) -> str:
    return " ".join(shlex.quote(part) for part in argv)


def _resolve_target(name: str) -> TargetSpec:
    try:
        return TARGETS[name]
    except KeyError as exc:
        known = ", ".join(sorted(TARGETS))
        raise RunnerError(f"unknown target {name!r}; expected one of: {known}") from exc


def _resolve_binary(path: str) -> Path:
    binary = Path(path).expanduser().resolve()
    if not binary.exists():
        raise RunnerError(f"binary not found: {binary}")
    return binary


def _build_dir_for_binary(binary: Path) -> Path:
    build_dir = binary.parent.parent
    if not build_dir.exists():
        raise RunnerError(f"unable to determine build dir for {binary}")
    return build_dir


def _simulate_script(build_dir: Path) -> Path:
    return build_dir / "simulate"


def _qemu_runtime(spec: TargetSpec) -> QemuRuntime:
    work_root = VM_IMAGES_DIR / "build" / "tmp" / "work" / "x86_64-linux" / "qemu-system-native"
    candidates: list[QemuRuntime] = []
    for recipe_dir in sorted(work_root.glob("*")):
        support_usr = recipe_dir / "recipe-sysroot-native" / "usr"
        installed_binary = support_usr / "bin" / spec.qemu_binary
        if installed_binary.exists():
            candidates.append(
                QemuRuntime(
                    binary=installed_binary,
                    support_usr=support_usr,
                    interpreter=_program_interpreter(installed_binary),
                )
            )
            continue
        build_binary = recipe_dir / "build" / spec.qemu_binary
        if build_binary.exists():
            bios_dir = recipe_dir / "build" / "pc-bios"
            candidates.append(
                QemuRuntime(
                    binary=build_binary,
                    support_usr=support_usr,
                    bios_dir=bios_dir if bios_dir.exists() else None,
                    interpreter=_program_interpreter(build_binary),
                )
            )
    if not candidates:
        raise RunnerError(
            f"Yocto-built {spec.qemu_binary} not found under vm-images/build/tmp/work/x86_64-linux/qemu-system-native"
        )
    return candidates[-1]


def _runtime_deploy_tar(spec: TargetSpec) -> Path | None:
    if spec.defconfig != "qemu_x86_64_defconfig":
        return None
    candidates = [
        DEFAULT_RUNTIME_DEPLOY_DIR / "latest-x86_64.tar.zst",
        DEFAULT_RUNTIME_DEPLOY_DIR / "virtioso-qemu-runtime-x86_64.tar.zst",
        DEFAULT_RUNTIME_DEPLOY_DIR / "latest-x86_64.tar.gz",
        DEFAULT_RUNTIME_DEPLOY_DIR / "virtioso-qemu-runtime-x86_64.tar.gz",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def _runtime_interpreter_from_dir(runtime_root: Path) -> Path | None:
    uninative_root = runtime_root / "uninative"
    if not uninative_root.exists():
        return None
    direct_candidates = [
        uninative_root / "x86_64-linux" / "lib" / "ld-linux-x86-64.so.2",
        uninative_root / "lib" / "ld-linux-x86-64.so.2",
    ]
    for candidate in direct_candidates:
        if candidate.exists():
            return candidate
    for pattern in ("ld-linux*", "ld64.so.*", "ld-musl-*", "ld-linux-*.so.*"):
        matches = [
            path for path in sorted(uninative_root.rglob(pattern))
            if ".debug" not in path.parts
        ]
        if matches:
            return matches[0]
    return None


def _runtime_from_dir(spec: TargetSpec, runtime_dir: Path) -> QemuRuntime:
    runtime_root = runtime_dir.expanduser().resolve()
    binary = runtime_root / "usr" / "bin" / spec.qemu_binary
    if not binary.exists():
        raise RunnerError(f"runtime binary not found in runtime dir: {binary}")
    support_usr = runtime_root / "usr"
    bios_dir = runtime_root / "pc-bios"
    if not bios_dir.exists():
        share_qemu = support_usr / "share" / "qemu"
        if (share_qemu / "bios-256k.bin").exists():
            bios_dir = share_qemu
    return QemuRuntime(
        binary=binary,
        support_usr=support_usr,
        bios_dir=bios_dir if bios_dir.exists() else None,
        interpreter=_runtime_interpreter_from_dir(runtime_root),
    )


def _extract_runtime_tar(runtime_tar: Path, extract_root: Path) -> Path:
    extract_root.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["tar", "--zstd", "-xf", str(runtime_tar), "-C", str(extract_root)],
        check=True,
    )
    roots = sorted(extract_root.iterdir())
    for root in roots:
        if root.is_dir():
            return root
    raise RunnerError(f"unable to determine runtime root after extracting {runtime_tar}")


def _resolve_runtime(
    spec: TargetSpec,
    runtime_dir: str,
    runtime_tar: str,
) -> tuple[QemuRuntime, Path | None]:
    if runtime_dir.strip():
        return _runtime_from_dir(spec, Path(runtime_dir)), None
    tar_candidate = Path(runtime_tar).expanduser().resolve() if runtime_tar.strip() else _runtime_deploy_tar(spec)
    if tar_candidate is not None:
        extract_root = Path(tempfile.mkdtemp(prefix="virtioso-qemu-runtime-"))
        runtime_root = _extract_runtime_tar(tar_candidate, extract_root)
        return _runtime_from_dir(spec, runtime_root), extract_root
    return _qemu_runtime(spec), None


def _ld_library_path(usr_dir: Path) -> str:
    parts: list[str] = []
    for rel in ("lib", "lib64", "libexec"):
        path = usr_dir / rel
        if path.exists():
            parts.append(str(path))
    existing = os.environ.get("LD_LIBRARY_PATH", "").strip()
    if existing:
        parts.append(existing)
    return ":".join(parts)


def _base_env(usr_dir: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = _ld_library_path(usr_dir)
    env.setdefault("QEMU_AUDIO_DRV", "none")
    qemu_data_dir = usr_dir / "share" / "qemu"
    if qemu_data_dir.exists():
        env["QEMU_DATA_DIR"] = str(qemu_data_dir)
    return env


def _run_subprocess(argv: list[str], *, cwd: Path, env: dict[str, str], dry_run: bool) -> int:
    print(f"QEMU_RUNNER_INFO: cwd={cwd}", flush=True)
    print(f"QEMU_RUNNER_INFO: command={_shell_join(argv)}", flush=True)
    if dry_run:
        return 0
    proc = subprocess.run(argv, cwd=str(cwd), env=env, check=False)
    return proc.returncode


def _write_console_manifest(path: Path, target: str, binary: Path, spec: TargetSpec) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(_console_manifest(target, binary, spec), indent=2) + "\n")
    return path


def _router_command(
    manifest_path: Path,
    runtime_dir: Path,
    wrapped_command: list[str],
) -> list[str]:
    return [
        "python3",
        str(SCRIPT_DIR / "console_router.py"),
        "run-command",
        "--manifest",
        str(manifest_path),
        "--runtime-dir",
        str(runtime_dir),
        "--",
        *wrapped_command,
    ]


def _build_extra_qemu_args(build_dir: Path) -> str:
    args_file = build_dir / "images" / "qemu-extra-args"
    if not args_file.exists():
        return ""
    return args_file.read_text().strip()


def _merge_extra_qemu_args(
    spec: TargetSpec,
    build_extra_qemu_args: str,
    extra_qemu_args: str,
    runtime: QemuRuntime,
    *,
    include_local_bios: bool,
) -> str:
    merged: list[str] = []
    merged.extend(spec.default_extra_qemu_args)
    if include_local_bios and runtime.bios_dir is not None:
        merged.extend(["-L", str(runtime.bios_dir)])
    if build_extra_qemu_args.strip():
        merged.extend(shlex.split(build_extra_qemu_args))
    if extra_qemu_args.strip():
        merged.extend(shlex.split(extra_qemu_args))
    return _shell_join(merged)


def _simulate_command(build_dir: Path, binary: Path, qemu_binary: Path, extra_qemu_args: str) -> list[str]:
    simulate = _simulate_script(build_dir)
    if not simulate.exists():
        raise RunnerError(f"simulate script not found: {simulate}")
    argv = [str(simulate), "-b", str(qemu_binary)]
    serial_opt = _simulate_serial_opt(binary)
    if serial_opt:
        argv.extend(["--serial", serial_opt])
    if extra_qemu_args.strip():
        argv.extend(["--extra-qemu-args", extra_qemu_args.strip()])
    return argv


def _fallback_local_command(binary: Path, spec: TargetSpec, qemu_binary: Path, extra_qemu_args: str) -> list[str]:
    if spec.defconfig != "qemu_arm64_defconfig":
        raise RunnerError(f"{spec.defconfig} requires a generated simulate script")
    argv = [str(qemu_binary), *spec.fallback_qemu_args, "-kernel", str(binary)]
    if extra_qemu_args.strip():
        argv.extend(shlex.split(extra_qemu_args))
    return argv


def run_local(
    target: str,
    binary_path: str,
    extra_qemu_args: str,
    dry_run: bool,
    runtime_dir: str,
    runtime_tar: str,
    console_runtime_dir: str,
) -> int:
    spec = _resolve_target(target)
    if spec.requires_remote:
        raise RunnerError(f"{target} must be run via the remote workflow")
    binary = _resolve_binary(binary_path)
    build_dir = _build_dir_for_binary(binary)
    runtime, temp_runtime_root = _resolve_runtime(spec, runtime_dir, runtime_tar)
    env = _base_env(runtime.support_usr)
    build_extra_qemu_args = _build_extra_qemu_args(build_dir)
    merged_extra_qemu_args = _merge_extra_qemu_args(
        spec,
        build_extra_qemu_args,
        extra_qemu_args,
        runtime,
        include_local_bios=True,
    )
    simulate = _simulate_script(build_dir)
    if simulate.exists():
        wrapped_argv = _simulate_command(build_dir, binary, runtime.binary, merged_extra_qemu_args)
    else:
        wrapped_argv = _fallback_local_command(binary, spec, runtime.binary, merged_extra_qemu_args)
    preserve_console_root = bool(console_runtime_dir.strip())
    console_root = (
        Path(console_runtime_dir).expanduser().resolve()
        if preserve_console_root
        else Path(tempfile.mkdtemp(prefix="virtioso-console-local-"))
    )
    manifest_path = _write_console_manifest(console_root / "console-manifest.json", target, binary, spec)
    router_runtime_dir = console_root / "console-runtime"
    argv = _router_command(manifest_path, router_runtime_dir, wrapped_argv)
    try:
        return _run_subprocess(argv, cwd=build_dir, env=env, dry_run=dry_run)
    finally:
        if not preserve_console_root:
            _remove_path(console_root)
        if temp_runtime_root is not None:
            _remove_path(temp_runtime_root)


def _copy_tree(src: Path, dst: Path) -> None:
    if src.is_dir():
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def _copy_file_preserve_rel(src: Path, root: Path, dst_root: Path) -> None:
    rel = src.relative_to(root)
    _copy_tree(src, dst_root / rel)


def _collect_runtime_tree(build_dir: Path, bundle_runtime: Path) -> Path:
    simulate = _simulate_script(build_dir)
    if not simulate.exists():
        raise RunnerError(f"remote workflow requires generated simulate script: {simulate}")
    images_dir = build_dir / "images"
    if not images_dir.exists():
        raise RunnerError(f"images directory not found: {images_dir}")
    runtime_build = bundle_runtime / "build"
    runtime_build.mkdir(parents=True, exist_ok=True)
    _copy_tree(simulate, runtime_build / "simulate")
    _copy_tree(images_dir, runtime_build / "images")
    return runtime_build


def _runtime_lib_files(binary: Path, support_usr: Path) -> set[Path]:
    proc = subprocess.run(["ldd", str(binary)], check=True, capture_output=True, text=True)
    libs: set[Path] = set()
    pattern = re.compile(r"=>\s+(\S+)")
    for line in proc.stdout.splitlines():
        match = pattern.search(line)
        if match:
            lib_path = Path(match.group(1))
            if lib_path.exists() and support_usr in lib_path.parents:
                libs.add(lib_path.resolve())
            continue
        stripped = line.strip()
        if "=>" in stripped or not stripped.startswith("/"):
            continue
        lib_path = Path(stripped.split()[0])
        if lib_path.exists() and support_usr in lib_path.parents:
            libs.add(lib_path.resolve())
    return libs


def _find_qemu_bios_blob(runtime: QemuRuntime, bios_name: str) -> Path | None:
    direct = runtime.support_usr / "share" / "qemu" / bios_name
    if direct.exists():
        return direct
    recipe_root = runtime.binary.parents[1]
    for candidate in recipe_root.glob(f"qemu-*/pc-bios/{bios_name}"):
        if candidate.exists():
            return candidate
    for candidate in recipe_root.glob(f"qemu-*/pc-bios/optionrom/{bios_name}"):
        if candidate.exists():
            return candidate
    return None


def _program_interpreter(binary: Path) -> Path | None:
    proc = subprocess.run(["readelf", "-l", str(binary)], check=True, capture_output=True, text=True)
    for line in proc.stdout.splitlines():
        marker = "Requesting program interpreter:"
        if marker in line:
            interp = line.split(marker, 1)[1].strip().rstrip("]").strip()
            path = Path(interp)
            return path if path.exists() else None
    return None


def _copy_qemu_runtime(bundle_dir: Path, runtime: QemuRuntime) -> Path:
    toolchain_usr = bundle_dir / "toolchain" / "usr"
    binary_dst = toolchain_usr / "bin" / runtime.binary.name
    binary_dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(runtime.binary, binary_dst)
    for lib_path in sorted(_runtime_lib_files(runtime.binary, runtime.support_usr)):
        _copy_file_preserve_rel(lib_path, runtime.support_usr, toolchain_usr)
    qemu_share_dirs = [
        runtime.support_usr / "share" / "qemu",
        runtime.support_usr / "share" / "qemu-firmware",
    ]
    for share_dir in qemu_share_dirs:
        if share_dir.exists():
            _copy_file_preserve_rel(share_dir, runtime.support_usr, toolchain_usr)
    if runtime.bios_dir is not None:
        pc_bios_dst = bundle_dir / "toolchain" / "pc-bios"
        _copy_tree(runtime.bios_dir, pc_bios_dst)
        recipe_root = runtime.binary.parents[1]
        for qemu_pc_bios in recipe_root.glob("qemu-*/pc-bios"):
            for pattern in ("*.bin", "*.rom", "optionrom/*.bin", "optionrom/*.rom"):
                for asset in qemu_pc_bios.glob(pattern):
                    _copy_tree(asset, pc_bios_dst / asset.name)
        _sanitize_pc_bios_tree(pc_bios_dst)
    return toolchain_usr


def _copy_uninative_interpreter(bundle_dir: Path, interpreter: Path | None) -> Path | None:
    if interpreter is None:
        return None
    candidates = [
        VM_IMAGES_DIR / "build" / "tmp" / "sysroots-uninative",
    ]
    if "uninative" in interpreter.parts:
        idx = interpreter.parts.index("uninative")
        candidates.append(Path(*interpreter.parts[: idx + 1]))
    rel = None
    for root in candidates:
        try:
            rel = interpreter.relative_to(root)
            break
        except ValueError:
            continue
    if rel is None:
        return None
    src_dir = interpreter.parent
    dst_dir = bundle_dir / "uninative" / rel.parent
    _copy_tree(src_dir, dst_dir)
    return Path("uninative") / rel


def _sanitize_pc_bios_tree(pc_bios_dir: Path) -> None:
    if not pc_bios_dir.exists():
        return
    _remove_path(pc_bios_dir / "descriptors")
    for path in pc_bios_dir.rglob("*"):
        if not path.is_file():
            continue
        if path.name == "Makefile":
            path.unlink()
            continue
        if path.suffix in {".d", ".o"}:
            path.unlink()
            continue
        if path.suffix == ".mak" and path.name.startswith("config"):
            path.unlink()


def _bundle_metadata(target: str, binary: Path, spec: TargetSpec) -> dict[str, str]:
    return {
        "target": target,
        "bundle_entrypoint": "runtime/run-bundle.sh",
        "bundle_qemu_wrapper": "runtime/qemu-wrapper.sh",
        "bundle_images_dir": "runtime/build/images",
        "bundle_console_manifest": "console-manifest.json",
        "binary_name": binary.name,
        "qemu_binary": spec.qemu_binary,
    }


def _logical_vm_qemu_virtio_channels() -> list[dict]:
    return [
        {
            "id": 1,
            "name": "driver_vm_console",
            "kind": "guest_console",
            "interactive": True,
            "pty": True,
            "legacy_aliases": ["tty0"],
        },
        {
            "id": 2,
            "name": "driver_vm_control",
            "kind": "control",
            "interactive": True,
            "pty": True,
        },
        {
            "id": 3,
            "name": "vmm_mux_control",
            "kind": "control",
            "interactive": False,
            "pty": False,
        },
        {
            "id": 4,
            "name": "nested_qemu_control",
            "kind": "control",
            "interactive": False,
            "pty": False,
        },
        {
            "id": 5,
            "name": "user_vm_console",
            "kind": "guest_console",
            "interactive": True,
            "pty": True,
        },
        {
            "id": 6,
            "name": "trace_control",
            "kind": "trace",
            "interactive": False,
            "pty": False,
        },
    ]


def _console_profile(binary: Path) -> str:
    build_dir = _build_dir_for_binary(binary)
    if build_dir.name.endswith("vm_qemu_virtio"):
        return "vm_qemu_virtio"
    return "default"


def _env_flag(name: str) -> bool:
    value = os.environ.get(name, "").strip().lower()
    return value in {"1", "true", "yes", "on"}


def _env_text(name: str) -> str:
    return os.environ.get(name, "").strip()


def _simulate_serial_opt(binary: Path) -> str:
    explicit = _env_text("VIRTIOSO_QEMU_SIM_SERIAL_OPT")
    if explicit:
        return explicit
    if _console_profile(binary) == "vm_qemu_virtio" and _env_flag("VIRTIOSO_QEMU_SPLIT_MONITOR"):
        return "-serial stdio -monitor none"
    return ""


def _console_manifest(target: str, binary: Path, spec: TargetSpec) -> dict:
    run_id = datetime.now(timezone.utc).isoformat(timespec="seconds")
    profile = _console_profile(binary)
    framed_opt_in = _env_flag("VIRTIOSO_CONSOLE_ROUTER_USE_JSONL_FRAMES")
    prefix_demux_opt_in = _env_flag("VIRTIOSO_CONSOLE_ROUTER_USE_VM_PREFIX_DEMUX")
    if profile == "vm_qemu_virtio" and framed_opt_in:
        return {
            "version": 1,
            "run_id": run_id,
            "target": target,
            "binary_name": binary.name,
            "transport": {
                "type": "jsonl_frames",
                "owner": "qemu_runner",
                "qemu_binary": spec.qemu_binary,
                "default_input_channel": "driver_vm_console",
                "framing_mode": "producer_tagged",
            },
            "channels": _logical_vm_qemu_virtio_channels(),
        }
    if profile == "vm_qemu_virtio" and prefix_demux_opt_in:
        return {
            "version": 1,
            "run_id": run_id,
            "target": target,
            "binary_name": binary.name,
            "transport": {
                "type": "line_prefixes",
                "owner": "qemu_runner",
                "qemu_binary": spec.qemu_binary,
                "default_input_channel": "driver_vm_console",
                "prefix_map": {
                    "[vm0] ": "driver_vm_console",
                    "[vm1] ": "user_vm_console",
                },
                "fallback_channel": "vmm_mux_control",
                "classification_mode": "ansi_stripped_vmm_mux_line_prefix",
                "producer_kind": "legacy_vmm_serial_mux",
                "note": "Prefix-demux classifies legacy VMM/mux annotations, not guest-native source tags.",
            },
            "channels": _logical_vm_qemu_virtio_channels(),
        }
    # The current runner still collapses QEMU-backed console output onto a single
    # runner-owned stream. Represent that topology honestly first; later slices
    # can split this into source-specific channels without changing manifest
    # ownership or format.
    merged_channel = {
        "id": 1,
        "name": "merged_console",
        "kind": "console",
        "interactive": True,
        "pty": True,
        "legacy_aliases": ["tty0"],
        "note": "Current QEMU runner topology merges runner and guest-visible console traffic.",
    }
    if profile == "vm_qemu_virtio":
        merged_channel["future_channel_set"] = "vm_qemu_virtio"
        merged_channel["declared_successor_channels"] = [
            channel["name"] for channel in _logical_vm_qemu_virtio_channels()
        ]
    return {
        "version": 1,
        "run_id": run_id,
        "target": target,
        "binary_name": binary.name,
        "transport": {
            "type": "process_stdio",
            "owner": "qemu_runner",
            "qemu_binary": spec.qemu_binary,
        },
        "channels": [merged_channel],
    }


def _copy_console_router(bundle_dir: Path) -> Path:
    router_src = SCRIPT_DIR / "console_router.py"
    router_dst = bundle_dir / "runtime" / "console_router.py"
    router_dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(router_src, router_dst)
    router_dst.chmod(0o755)
    return router_dst


def _write_remote_wrapper(
    runtime_build: Path,
    binary: Path,
    toolchain_usr: Path,
    spec: TargetSpec,
    extra_qemu_args: str,
    bios_dir: Path | None,
    bundled_interpreter: Path | None,
) -> Path:
    wrapper = runtime_build.parent / "run-bundle.sh"
    qemu_wrapper = runtime_build.parent / "qemu-wrapper.sh"
    qemu_data_dir = toolchain_usr / "share" / "qemu"
    qemu_extra_parts: list[str] = []
    if bios_dir is not None:
        qemu_extra_parts.extend(["-L", "../../toolchain/pc-bios"])
    if extra_qemu_args.strip():
        qemu_extra_parts.extend(shlex.split(extra_qemu_args))
    qemu_extra = _shell_join(qemu_extra_parts)
    qemu_wrapper_lines = [
        "#!/usr/bin/env bash",
        "set -euo pipefail",
        'SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"',
        'TOOLCHAIN_USR="${SCRIPT_DIR}/../toolchain/usr"',
    ]
    if bundled_interpreter is not None:
        qemu_wrapper_lines.append(
            'exec "${SCRIPT_DIR}/../'
            + bundled_interpreter.as_posix()
            + '" --library-path "${SCRIPT_DIR}/../'
            + bundled_interpreter.parent.as_posix()
            + ':${TOOLCHAIN_USR}/lib:${TOOLCHAIN_USR}/lib64:${TOOLCHAIN_USR}/libexec:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:/lib64:/usr/lib64" "../../toolchain/usr/bin/'
            + spec.qemu_binary
            + '" "$@"'
        )
    else:
        qemu_wrapper_lines.append(f'exec "../../toolchain/usr/bin/{spec.qemu_binary}" "$@"')
    qemu_wrapper.write_text("\n".join(qemu_wrapper_lines) + "\n")
    qemu_wrapper.chmod(0o755)
    simulate_serial_opt = _simulate_serial_opt(binary)
    lines = [
        "#!/usr/bin/env bash",
        "set -euo pipefail",
        'SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"',
        'TOOLCHAIN_USR="${SCRIPT_DIR}/../toolchain/usr"',
        'export LD_LIBRARY_PATH="${TOOLCHAIN_USR}/lib:${TOOLCHAIN_USR}/lib64:${TOOLCHAIN_USR}/libexec${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"',
        'export QEMU_AUDIO_DRV="${QEMU_AUDIO_DRV:-none}"',
        (f'export QEMU_DATA_DIR="${{TOOLCHAIN_USR}}/share/qemu"' if qemu_data_dir.exists() else "true"),
        'cd "${SCRIPT_DIR}/build"',
        'log_path="${SCRIPT_DIR}/qemu-run.log"',
        'console_runtime_dir="${SCRIPT_DIR}/console-runtime"',
        'console_manifest="${SCRIPT_DIR}/../console-manifest.json"',
        "set +e",
        'python3 "${SCRIPT_DIR}/console_router.py" run-command'
        + ' --manifest "${console_manifest}"'
        + ' --runtime-dir "${console_runtime_dir}"'
        + ' -- ./simulate -b ../qemu-wrapper.sh'
        + (f" --serial {shlex.quote(simulate_serial_opt)}" if simulate_serial_opt else "")
        + (f" --extra-qemu-args {shlex.quote(qemu_extra)}" if qemu_extra else "")
        + ' 2>&1 | tee "${log_path}"',
        'sim_rc=${PIPESTATUS[0]}',
        "set -e",
        'if [[ "${sim_rc}" -eq 0 ]] && grep -qE "Segmentation fault \\(core dumped\\)|QEMU failed;" "${log_path}"; then',
        "  sim_rc=139",
        "fi",
        'exit "${sim_rc}"',
    ]
    wrapper.write_text("\n".join(lines) + "\n")
    wrapper.chmod(0o755)
    return wrapper


def _load_remote_config(path: Path, target: str) -> dict:
    if not path.exists():
        raise RunnerError(f"remote config file not found: {path}")
    data = json.loads(path.read_text())
    runners = data.get("runners", {})
    runner = runners.get(target)
    if not isinstance(runner, dict):
        raise RunnerError(f"missing runners.{target} in {path}")
    for key in ("ssh_host", "ssh_user", "remote_dir"):
        value = str(runner.get(key, "")).strip()
        if not value:
            raise RunnerError(f"missing runners.{target}.{key} in {path}")
    return runner


def _bundle_name(binary: Path, target: str) -> str:
    return f"{target}-{binary.stem}"


def _remote_home_relative(path: str) -> str:
    text = str(path).strip()
    if text.startswith("~/"):
        return text[2:]
    if text == "~":
        return "."
    return text


def _remote_shell_dir(path: str) -> str:
    rel = _remote_home_relative(path)
    if rel in ("", "."):
        return "$HOME"
    return f"$HOME/{shlex.quote(rel)}"


def prepare_remote_bundle(
    target: str,
    binary_path: str,
    output: str | None,
    extra_qemu_args: str,
    runtime_dir: str,
    runtime_tar: str,
) -> Path:
    spec = _resolve_target(target)
    if not spec.requires_remote:
        raise RunnerError(f"{target} is a local target; use run-local")
    binary = _resolve_binary(binary_path)
    build_dir = _build_dir_for_binary(binary)
    runtime, temp_runtime_root = _resolve_runtime(spec, runtime_dir, runtime_tar)
    build_extra_qemu_args = _build_extra_qemu_args(build_dir)
    merged_extra_qemu_args = _merge_extra_qemu_args(
        spec,
        build_extra_qemu_args,
        extra_qemu_args,
        runtime,
        include_local_bios=False,
    )
    bundle_root = Path(output).expanduser().resolve() if output else Path(tempfile.mkdtemp(prefix="virtioso-qemu-bundle-"))
    try:
        bundle_dir = bundle_root / _bundle_name(binary, target)
        runtime_build = _collect_runtime_tree(build_dir, bundle_dir / "runtime")
        toolchain_usr = _copy_qemu_runtime(bundle_dir, runtime)
        bundled_interpreter = _copy_uninative_interpreter(bundle_dir, runtime.interpreter)
        _copy_console_router(bundle_dir)
        _write_remote_wrapper(
            runtime_build,
            binary,
            toolchain_usr,
            spec,
            merged_extra_qemu_args,
            runtime.bios_dir,
            bundled_interpreter,
        )
        (bundle_dir / "bundle.json").write_text(json.dumps(_bundle_metadata(target, binary, spec), indent=2))
        (bundle_dir / "console-manifest.json").write_text(json.dumps(_console_manifest(target, binary, spec), indent=2))
        return bundle_dir
    finally:
        if temp_runtime_root is not None:
            _remove_path(temp_runtime_root)


def _tar_bundle(bundle_dir: Path) -> Path:
    tar_path = bundle_dir.with_suffix(".tar.gz")
    with tarfile.open(tar_path, "w:gz") as tf:
        tf.add(bundle_dir, arcname=bundle_dir.name)
    return tar_path


def _remove_path(path: Path) -> None:
    if not path.exists():
        return
    if path.is_dir():
        shutil.rmtree(path, ignore_errors=True)
        return
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def _remote_cleanup_command(remote_dir: str, bundle_name: str, tar_name: str, diag_name: str) -> str:
    return (
        f"cd {remote_dir} && "
        f"rm -rf {shlex.quote(bundle_name)} {shlex.quote(tar_name)} {shlex.quote(diag_name)} {shlex.quote(bundle_name + '.diagnostics')}"
    )


def _remote_diag_name(bundle_name: str) -> str:
    return f"{bundle_name}.diagnostics.tar.gz"


def _remote_run_command(remote_dir: str, bundle_name: str, tar_name: str, diag_name: str, qemu_binary: str) -> str:
    quoted_bundle = shlex.quote(bundle_name)
    quoted_tar = shlex.quote(tar_name)
    quoted_diag = shlex.quote(diag_name)
    quoted_qemu = shlex.quote(qemu_binary)
    return "\n".join(
        [
            "set -euo pipefail",
            f"cd {remote_dir}",
            f"bundle_dir={quoted_bundle}",
            f"tar_name={quoted_tar}",
            f"diag_name={quoted_diag}",
            "runner_pid=''",
            "runner_pgid=''",
            "run_started_at=$(date --iso-8601=seconds)",
            "collect_diagnostics() {",
            "  local diag_dir=\"${bundle_dir}.diagnostics\"",
            "  mkdir -p \"${diag_dir}\"",
            "  env > \"${diag_dir}/env.txt\" || true",
            "  if command -v coredumpctl >/dev/null 2>&1; then",
            f"    coredumpctl --no-pager --since \"${{run_started_at}}\" list {quoted_qemu} > \"${{diag_dir}}/coredumpctl-list.txt\" 2>&1 || true",
            f"    coredumpctl --no-pager --since \"${{run_started_at}}\" info {quoted_qemu} > \"${{diag_dir}}/coredumpctl-info.txt\" 2>&1 || true",
            f"    coredumpctl --no-pager --since \"${{run_started_at}}\" dump {quoted_qemu} --output \"${{diag_dir}}/{qemu_binary}.core\" > /dev/null 2>&1 || true",
            "  fi",
            "  if compgen -G \"core\" > /dev/null; then mv core \"${diag_dir}/\"; fi",
            "  if compgen -G \"core.*\" > /dev/null; then mkdir -p \"${diag_dir}/cores\" && mv core.* \"${diag_dir}/cores/\"; fi",
            "  if [[ -d \"${diag_dir}\" ]]; then",
            "    tar -czf \"../${diag_name}\" \"${diag_dir}\"",
            "  fi",
            "}",
            "cleanup() {",
            "  local rc=$?",
            '  if [[ -n "${runner_pgid}" ]]; then',
            '    kill -TERM -- "-${runner_pgid}" 2>/dev/null || true',
            "    sleep 1",
            '    kill -KILL -- "-${runner_pgid}" 2>/dev/null || true',
            "  elif [[ -n \"${runner_pid}\" ]]; then",
            '    kill -TERM "${runner_pid}" 2>/dev/null || true',
            "    sleep 1",
            '    kill -KILL "${runner_pid}" 2>/dev/null || true',
            "  fi",
            '  rm -rf "${bundle_dir}" "${tar_name}"',
            "  exit ${rc}",
            "}",
            "trap cleanup EXIT HUP INT TERM",
            "tar -xzf \"${tar_name}\"",
            "cd \"${bundle_dir}\"",
            "ulimit -c unlimited || true",
            "setsid ./runtime/run-bundle.sh &",
            "runner_pid=$!",
            "runner_pgid=$(ps -o pgid= \"${runner_pid}\" | tr -d '[:space:]')",
            "set +e",
            "wait \"${runner_pid}\"",
            "runner_rc=$?",
            "set -e",
            'if [[ "${runner_rc}" -ne 0 ]]; then',
            "  collect_diagnostics",
            "fi",
            "exit \"${runner_rc}\"",
        ]
    )


def _remote_fetch_command(remote_dir: str, name: str) -> str:
    return (
        f"cd {remote_dir} && "
        f"test -f {shlex.quote(name)}"
    )


def _persist_remote_diagnostics(
    remote: str,
    ssh_opts: list[str],
    remote_scp_dir: str,
    remote_shell_dir: str,
    diag_name: str,
) -> Path | None:
    check_cmd = ["ssh", *ssh_opts, remote, "bash", "-lc", _remote_fetch_command(remote_shell_dir, diag_name)]
    check = subprocess.run(check_cmd, check=False)
    if check.returncode != 0:
        return None

    local_root = Path(tempfile.mkdtemp(prefix="virtioso-qemu-diagnostics-"))
    local_tar = local_root / diag_name
    remote_src = f"{remote}:{remote_scp_dir}/{diag_name}" if remote_scp_dir not in ("", ".") else f"{remote}:{diag_name}"
    scp_cmd = ["scp", *ssh_opts, remote_src, str(local_tar)]
    subprocess.run(scp_cmd, check=True)
    extract_dir = local_root / "extracted"
    extract_dir.mkdir(parents=True, exist_ok=True)
    with tarfile.open(local_tar, "r:gz") as tf:
        tf.extractall(extract_dir)
    return extract_dir


def run_remote(
    target: str,
    binary_path: str,
    config_path: Path,
    extra_qemu_args: str,
    dry_run: bool,
    runtime_dir: str,
    runtime_tar: str,
    console_runtime_dir: str,
) -> int:
    spec = _resolve_target(target)
    if not spec.requires_remote:
        raise RunnerError(f"{target} is a local target; use run-local")
    binary = _resolve_binary(binary_path)
    runner = _load_remote_config(config_path, target)
    bundle_dir = prepare_remote_bundle(target, binary_path, None, extra_qemu_args, runtime_dir, runtime_tar)
    tar_path = _tar_bundle(bundle_dir)
    temp_root = bundle_dir.parent
    preserve_console_root = bool(console_runtime_dir.strip())
    console_root = (
        Path(console_runtime_dir).expanduser().resolve()
        if preserve_console_root
        else Path(tempfile.mkdtemp(prefix="virtioso-console-remote-"))
    )
    manifest_path = _write_console_manifest(console_root / "console-manifest.json", target, binary, spec)
    router_runtime_dir = console_root / "console-runtime"
    remote = f"{runner['ssh_user']}@{runner['ssh_host']}"
    remote_dir = runner["remote_dir"]
    remote_scp_dir = _remote_home_relative(remote_dir).rstrip("/")
    remote_shell_dir = _remote_shell_dir(remote_dir)
    diag_name = _remote_diag_name(bundle_dir.name)
    ssh_opts = [str(opt) for opt in runner.get("ssh_options", [])]
    mkdir_cmd = [
        "ssh",
        *ssh_opts,
        remote,
        f"mkdir -p {remote_shell_dir}",
    ]
    scp_dest = f"{remote}:{remote_scp_dir}/" if remote_scp_dir not in ("", ".") else f"{remote}:"
    scp_cmd = ["scp", *ssh_opts, str(tar_path), scp_dest]
    remote_cleanup = _remote_cleanup_command(remote_shell_dir, bundle_dir.name, tar_path.name, diag_name)
    run_script = _remote_run_command(remote_shell_dir, bundle_dir.name, tar_path.name, diag_name, spec.qemu_binary)
    ssh_run_cmd = ["ssh", *ssh_opts, remote, f"bash -lc {shlex.quote(run_script)}"]
    run_cmd = _router_command(manifest_path, router_runtime_dir, ssh_run_cmd)
    cleanup_cmd = ["ssh", *ssh_opts, remote, f"bash -lc {shlex.quote(remote_cleanup)}"]
    print(f"QEMU_RUNNER_INFO: remote={remote}", flush=True)
    print(f"QEMU_RUNNER_INFO: bundle={bundle_dir}", flush=True)
    print(f"QEMU_RUNNER_INFO: ssh-mkdir={_shell_join(mkdir_cmd)}", flush=True)
    print(f"QEMU_RUNNER_INFO: scp={_shell_join(scp_cmd)}", flush=True)
    print(f"QEMU_RUNNER_INFO: ssh-run={_shell_join(run_cmd)}", flush=True)
    print(f"QEMU_RUNNER_INFO: ssh-cleanup={_shell_join(cleanup_cmd)}", flush=True)
    if dry_run:
        return 0

    cleanup_requested = False

    def _handle_signal(signum: int, _frame) -> None:
        nonlocal cleanup_requested
        cleanup_requested = True
        raise KeyboardInterrupt

    old_handlers = {
        sig: signal.getsignal(sig)
        for sig in (signal.SIGINT, signal.SIGTERM)
    }
    for sig in old_handlers:
        signal.signal(sig, _handle_signal)

    try:
        subprocess.run(mkdir_cmd, check=True)
        cleanup_requested = True
        subprocess.run(scp_cmd, check=True)
        proc = subprocess.run(run_cmd, check=False)
        if proc.returncode != 0:
            diagnostics_dir = _persist_remote_diagnostics(remote, ssh_opts, remote_scp_dir, remote_shell_dir, diag_name)
            if diagnostics_dir is not None:
                print(f"QEMU_RUNNER_INFO: diagnostics={diagnostics_dir}", flush=True)
        return proc.returncode
    except KeyboardInterrupt:
        cleanup_requested = True
        return 130
    finally:
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)
        if cleanup_requested:
            try:
                subprocess.run(cleanup_cmd, check=False)
            except Exception:
                pass
        _remove_path(tar_path)
        _remove_path(temp_root)
        if not preserve_console_root:
            _remove_path(console_root)


def _config_template() -> str:
    template = {
        "runners": {
            "qemu_x86_64_defconfig": {
                "ssh_host": "intel-host.example",
                "ssh_user": "your-user",
                "remote_dir": "~/virtioso-qemu-runs/qemu_x86_64_defconfig",
                "ssh_options": [
                    "-o",
                    "StrictHostKeyChecking=no",
                ],
            }
        }
    }
    return json.dumps(template, indent=2)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--target", required=True, choices=sorted(TARGETS))
    common.add_argument("--binary", required=True, help="Path to the built seL4 image used to locate the build dir")
    common.add_argument("--extra-qemu-args", default="", help="Arguments forwarded to the generated simulate script")
    common.add_argument("--runtime-dir", default="", help="Path to an extracted QEMU runtime artifact root")
    common.add_argument("--console-runtime-dir", default="", help="Directory where console router runtime artifacts are written")
    common.add_argument("--runtime-tar", default="", help="Path to a QEMU runtime artifact tarball")
    common.add_argument("--dry-run", action="store_true")

    run_local_parser = sub.add_parser("run-local", parents=[common], help="Run a local QEMU target")
    run_local_parser.set_defaults(handler="run_local")

    prepare_remote_parser = sub.add_parser("prepare-remote-bundle", parents=[common], help="Prepare a remote QEMU bundle")
    prepare_remote_parser.add_argument("--output-dir", default="", help="Directory where the bundle directory is created")
    prepare_remote_parser.set_defaults(handler="prepare_remote_bundle")

    run_remote_parser = sub.add_parser("run-remote", parents=[common], help="Copy and run a remote QEMU bundle over SSH")
    run_remote_parser.add_argument("--config", default=str(DEFAULT_REMOTE_CONFIG), help="Path to ~/.virtioso-qemu-runners.json")
    run_remote_parser.set_defaults(handler="run_remote")

    config_parser = sub.add_parser("print-config-template", help="Print the expected remote runner config file")
    config_parser.set_defaults(handler="print_config_template")

    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.handler == "run_local":
            return run_local(
                args.target,
                args.binary,
                args.extra_qemu_args,
                args.dry_run,
                args.runtime_dir,
                args.runtime_tar,
                args.console_runtime_dir,
            )
        if args.handler == "prepare_remote_bundle":
            bundle_dir = prepare_remote_bundle(
                args.target,
                args.binary,
                args.output_dir or None,
                args.extra_qemu_args,
                args.runtime_dir,
                args.runtime_tar,
            )
            print(bundle_dir, flush=True)
            return 0
        if args.handler == "run_remote":
            return run_remote(
                args.target,
                args.binary,
                Path(args.config).expanduser(),
                args.extra_qemu_args,
                args.dry_run,
                args.runtime_dir,
                args.runtime_tar,
                args.console_runtime_dir,
            )
        if args.handler == "print_config_template":
            print(_config_template(), flush=True)
            return 0
        raise RunnerError(f"unhandled command: {args.handler}")
    except RunnerError as exc:
        print(f"QEMU_RUNNER_ERROR: {exc}", file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    sys.exit(main())
