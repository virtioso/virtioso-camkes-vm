import base64
import binascii
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS = REPO_ROOT / "tools"
sys.path.insert(0, str(TOOLS))

STREAM_SPEC = importlib.util.spec_from_file_location(
    "vio_trace.stream", TOOLS / "vio_trace" / "stream.py"
)
assert STREAM_SPEC is not None and STREAM_SPEC.loader is not None
STREAM = importlib.util.module_from_spec(STREAM_SPEC)
sys.modules[STREAM_SPEC.name] = STREAM
STREAM_SPEC.loader.exec_module(STREAM)

CLI_SPEC = importlib.util.spec_from_file_location(
    "vio_trace.cli", TOOLS / "vio_trace" / "cli.py"
)
assert CLI_SPEC is not None and CLI_SPEC.loader is not None
CLI = importlib.util.module_from_spec(CLI_SPEC)
sys.modules[CLI_SPEC.name] = CLI
CLI_SPEC.loader.exec_module(CLI)


def literal_lz4(data: bytes) -> bytes:
    if len(data) < 15:
        return bytes([len(data) << 4]) + data
    rem = len(data) - 15
    ext = bytearray()
    while rem >= 255:
        ext.append(255)
        rem -= 255
    ext.append(rem)
    return bytes([15 << 4]) + bytes(ext) + data


def transfer_text(data: bytes) -> str:
    compressed = literal_lz4(data)
    encoded = base64.b64encode(compressed).decode("ascii")
    crc = binascii.crc32(data) & 0xFFFFFFFF
    return "\n".join(
        [
            "noise before",
            "=== BINARY TRANSFER START ===",
            "TYPE: VIO_TRACE_STREAM",
            "STREAM_KIND: GUEST_TRACE",
            "FORMAT_VERSION: 1",
            "SOURCE_NAME: vm0_el1",
            "SOURCE_ID: 4",
            f"RAW_SIZE: {len(data)}",
            f"COMPRESSED_SIZE: {len(compressed)}",
            f"CHECKSUM: crc32:{crc:08x}",
            encoded,
            "=== BINARY TRANSFER END ===",
            "noise after",
            "",
        ]
    )


class VioTraceStreamTests(unittest.TestCase):
    def test_parse_stream_recovers_raw_payload(self) -> None:
        streams = STREAM.parse_streams(transfer_text(b"abcdef0123456789"))
        self.assertEqual(len(streams), 1)
        self.assertEqual(streams[0].stream_kind, "GUEST_TRACE")
        self.assertEqual(streams[0].source_name, "vm0_el1")
        self.assertEqual(streams[0].source_id, 4)
        self.assertEqual(streams[0].raw, b"abcdef0123456789")

    def test_cli_extract_writes_manifest_and_raw_payload(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            input_path = root / "tty0.raw"
            out_dir = root / "out"
            input_path.write_text(transfer_text(b"raw-buffer"))

            rc = CLI.main(["extract", "--input", str(input_path), "--out-dir", str(out_dir)])

            self.assertEqual(rc, 0)
            self.assertTrue((out_dir / "manifest.json").exists())
            raw_files = sorted(out_dir.glob("*.bin"))
            self.assertEqual(len(raw_files), 1)
            self.assertEqual(raw_files[0].read_bytes(), b"raw-buffer")


if __name__ == "__main__":
    unittest.main()
