#!/usr/bin/env python3
"""Explicit binary frame wrapper for a single logical console channel.

This helper sits between console_router.py and a legacy stdio-producing command.
It does not classify plaintext. Instead, it assigns all child stdout/stderr to a
single configured logical channel and relays router tx frames back to child
stdin for that same channel.
"""

from __future__ import annotations

import argparse
import errno
import json
import os
import selectors
import signal
import struct
import subprocess
import sys
from pathlib import Path
from typing import Any


_BINARY_FRAME_MAGIC = b"CF"
_BINARY_FRAME_VERSION = 1
_BINARY_FRAME_HEADER = struct.Struct(">2sBBBBI")
_BINARY_FRAME_DIR_TO_CODE = {"rx": 0x01, "tx": 0x02}
_BINARY_FRAME_CODE_TO_DIR = {value: key for key, value in _BINARY_FRAME_DIR_TO_CODE.items()}
_BINARY_FRAME_MAX_PAYLOAD = 16 * 1024 * 1024


class FrameStreamError(RuntimeError):
    pass


def _set_nonblocking(fd: int) -> None:
    import fcntl

    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)


def _load_manifest(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text())
    except FileNotFoundError as exc:
        raise FrameStreamError(f"manifest not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise FrameStreamError(f"invalid JSON in manifest {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise FrameStreamError(f"manifest root must be an object: {path}")
    return data


def _resolve_channel_id(manifest: dict[str, Any], channel_name: str) -> int:
    channels = manifest.get("channels")
    if not isinstance(channels, list):
        raise FrameStreamError("manifest field 'channels' must be a list")
    for channel in channels:
        if not isinstance(channel, dict):
            continue
        if channel.get("name") == channel_name:
            channel_id = channel.get("id")
            if not isinstance(channel_id, int) or channel_id < 0 or channel_id > 0xFF:
                raise FrameStreamError(f"invalid channel id for {channel_name!r}: {channel_id!r}")
            return channel_id
    raise FrameStreamError(f"channel not found in manifest: {channel_name!r}")


def _encode_binary_frame(channel_id: int, direction: str, data: bytes) -> bytes:
    if direction not in _BINARY_FRAME_DIR_TO_CODE:
        raise FrameStreamError(f"invalid binary frame direction: {direction!r}")
    if len(data) > _BINARY_FRAME_MAX_PAYLOAD:
        raise FrameStreamError(
            f"binary frame payload too large: {len(data)} > {_BINARY_FRAME_MAX_PAYLOAD}"
        )
    header = _BINARY_FRAME_HEADER.pack(
        _BINARY_FRAME_MAGIC,
        _BINARY_FRAME_VERSION,
        channel_id,
        _BINARY_FRAME_DIR_TO_CODE[direction],
        0,
        len(data),
    )
    return header + data


def _decode_binary_frame(buffer: bytearray) -> tuple[int, str, bytes] | None:
    if len(buffer) < _BINARY_FRAME_HEADER.size:
        return None
    magic, version, channel_id, direction_code, _flags, payload_len = _BINARY_FRAME_HEADER.unpack(
        bytes(buffer[:_BINARY_FRAME_HEADER.size])
    )
    if magic != _BINARY_FRAME_MAGIC:
        raise FrameStreamError(f"invalid binary frame magic: {magic!r}")
    if version != _BINARY_FRAME_VERSION:
        raise FrameStreamError(f"unsupported binary frame version: {version}")
    direction = _BINARY_FRAME_CODE_TO_DIR.get(direction_code)
    if direction is None:
        raise FrameStreamError(f"invalid binary frame direction code: {direction_code}")
    if payload_len > _BINARY_FRAME_MAX_PAYLOAD:
        raise FrameStreamError(
            f"binary frame payload too large: {payload_len} > {_BINARY_FRAME_MAX_PAYLOAD}"
        )
    frame_len = _BINARY_FRAME_HEADER.size + payload_len
    if len(buffer) < frame_len:
        return None
    payload = bytes(buffer[_BINARY_FRAME_HEADER.size:frame_len])
    del buffer[:frame_len]
    return channel_id, direction, payload


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, help="Path to console-manifest.json")
    parser.add_argument("--channel", required=True, help="Logical channel name to assign child stdio")
    parser.add_argument("cmd", nargs=argparse.REMAINDER, help="Command to run after '--'")
    args = parser.parse_args()
    if not args.cmd or args.cmd[0] != "--" or len(args.cmd) == 1:
        raise FrameStreamError("command requires a non-empty argv after '--'")
    args.cmd = args.cmd[1:]
    return args


def main() -> int:
    args = parse_args()
    manifest = _load_manifest(Path(args.manifest).expanduser().resolve())
    channel_id = _resolve_channel_id(manifest, str(args.channel))

    proc = subprocess.Popen(
        args.cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
        start_new_session=True,
    )
    if proc.stdout is None:
        raise FrameStreamError("child stdout unavailable")
    stdout_fd = proc.stdout.fileno()
    stdin_fd = proc.stdin.fileno() if proc.stdin is not None else None
    _set_nonblocking(stdout_fd)
    if stdin_fd is not None:
        _set_nonblocking(stdin_fd)
    try:
        _set_nonblocking(sys.stdin.fileno())
    except OSError:
        pass

    in_buffer = bytearray()
    selector = selectors.DefaultSelector()
    selector.register(stdout_fd, selectors.EVENT_READ, ("child_out", None))
    try:
        selector.register(sys.stdin.fileno(), selectors.EVENT_READ, ("router_in", None))
    except Exception:
        pass

    return_code = 0
    try:
        while True:
            events = selector.select(timeout=0.1)
            for key, _mask in events:
                kind, _payload = key.data
                if kind == "child_out":
                    try:
                        data = os.read(stdout_fd, 4096)
                    except OSError as exc:
                        if exc.errno == errno.EIO:
                            data = b""
                        else:
                            raise
                    if data:
                        os.write(sys.stdout.fileno(), _encode_binary_frame(channel_id, "rx", data))
                    else:
                        try:
                            selector.unregister(stdout_fd)
                        except Exception:
                            pass
                elif kind == "router_in":
                    try:
                        data = os.read(sys.stdin.fileno(), 4096)
                    except OSError as exc:
                        if exc.errno in {errno.EIO, errno.EBADF}:
                            data = b""
                        else:
                            raise
                    if data:
                        in_buffer.extend(data)
                        while True:
                            decoded = _decode_binary_frame(in_buffer)
                            if decoded is None:
                                break
                            frame_channel_id, direction, payload = decoded
                            if frame_channel_id != channel_id:
                                raise FrameStreamError(
                                    f"received frame for unexpected channel id {frame_channel_id}; expected {channel_id}"
                                )
                            if direction != "tx":
                                raise FrameStreamError(
                                    f"received unexpected frame direction {direction!r}; expected 'tx'"
                                )
                            if stdin_fd is not None and payload:
                                os.write(stdin_fd, payload)
                    else:
                        try:
                            selector.unregister(sys.stdin.fileno())
                        except Exception:
                            pass

            polled = proc.poll()
            if polled is not None:
                return_code = polled
                try:
                    trailing = os.read(stdout_fd, 4096)
                except OSError:
                    trailing = b""
                if trailing:
                    os.write(sys.stdout.fileno(), _encode_binary_frame(channel_id, "rx", trailing))
                if in_buffer:
                    raise FrameStreamError("binary frame transport ended with an incomplete input frame")
                break
    finally:
        selector.close()
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=5)

    return return_code


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FrameStreamError as exc:
        print(f"FRAME_STREAM_ERROR: {exc}", file=sys.stderr, flush=True)
        raise SystemExit(2)
