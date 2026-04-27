#!/usr/bin/env python3
"""CLI for framework-level VIO trace stream extraction."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from vio_trace.stream import TraceStream, parse_streams


def _safe_name(stream: TraceStream, index: int) -> str:
    base = f"{index:03d}-{stream.stream_kind}-{stream.source_name}"
    base = re.sub(r"[^A-Za-z0-9_.-]+", "_", base).strip("._")
    return base or f"{index:03d}-trace"


def _cmd_extract(args: argparse.Namespace) -> int:
    text = args.input.read_text(errors="replace")
    streams = parse_streams(text)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    manifest = []
    for index, stream in enumerate(streams):
        name = _safe_name(stream, index)
        raw_path = args.out_dir / f"{name}.bin"
        raw_path.write_bytes(stream.raw)
        manifest.append(
            {
                "index": index,
                "stream_kind": stream.stream_kind,
                "source_name": stream.source_name,
                "source_id": stream.source_id,
                "raw_size": stream.raw_size,
                "compressed_size": stream.compressed_size,
                "checksum": stream.checksum,
                "raw_path": raw_path.name,
            }
        )

    manifest_path = args.out_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"streams={len(streams)} out={args.out_dir}")
    return 0


def _cmd_summary(args: argparse.Namespace) -> int:
    text = args.input.read_text(errors="replace")
    streams = parse_streams(text)
    for stream in streams:
        print(
            f"{stream.stream_kind} source={stream.source_name} "
            f"source_id={stream.source_id} raw_size={stream.raw_size} "
            f"checksum={stream.checksum}"
        )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="vio-trace")
    subparsers = parser.add_subparsers(dest="cmd", required=True)

    extract = subparsers.add_parser("extract", help="Extract raw VIO_TRACE_STREAM buffers")
    extract.add_argument("--input", type=Path, required=True)
    extract.add_argument("--out-dir", type=Path, required=True)
    extract.set_defaults(func=_cmd_extract)

    summary = subparsers.add_parser("summary", help="List VIO_TRACE_STREAM buffers")
    summary.add_argument("--input", type=Path, required=True)
    summary.set_defaults(func=_cmd_summary)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
