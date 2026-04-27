"""Parser for event-agnostic VIO_TRACE_STREAM transfers."""

from __future__ import annotations

import base64
import binascii
from dataclasses import dataclass

TRANSFER_START = "=== BINARY TRANSFER START ==="
TRANSFER_END = "=== BINARY TRANSFER END ==="
TRANSFER_TYPE = "VIO_TRACE_STREAM"
TRANSFER_VERSION = 1
CHECKSUM_PREFIX = "crc32:"


class TraceStreamError(ValueError):
    """Raised when a VIO trace stream is malformed."""


@dataclass(frozen=True)
class TraceStream:
    stream_kind: str
    source_name: str
    source_id: int
    raw_size: int
    compressed_size: int
    checksum: str
    raw: bytes


def crc32_hex(data: bytes) -> str:
    return f"{binascii.crc32(data) & 0xFFFFFFFF:08x}"


def _decode_lz4_literal_only(payload: bytes, expected_size: int) -> bytes:
    if not payload:
        if expected_size == 0:
            return b""
        raise TraceStreamError("empty compressed payload")

    token = payload[0]
    literal_len = token >> 4
    pos = 1
    if literal_len == 15:
        while True:
            if pos >= len(payload):
                raise TraceStreamError("truncated LZ4 literal length")
            ext = payload[pos]
            pos += 1
            literal_len += ext
            if ext != 255:
                break

    raw = payload[pos:pos + literal_len]
    if len(raw) != literal_len:
        raise TraceStreamError("truncated LZ4 literal payload")
    if pos + literal_len != len(payload):
        raise TraceStreamError("unexpected LZ4 sequence data")
    if len(raw) != expected_size:
        raise TraceStreamError(
            f"raw size mismatch: decoded={len(raw)} expected={expected_size}"
        )
    return raw


def _parse_header_line(line: str) -> tuple[str, str]:
    if ":" not in line:
        raise TraceStreamError(f"malformed header line: {line}")
    key, value = line.split(":", 1)
    return key.strip(), value.strip()


def _build_stream(headers: dict[str, str], payload_lines: list[str]) -> TraceStream:
    if headers.get("TYPE") != TRANSFER_TYPE:
        raise TraceStreamError(f"unsupported stream type: {headers.get('TYPE')}")

    version = int(headers.get("FORMAT_VERSION", "0"), 10)
    if version != TRANSFER_VERSION:
        raise TraceStreamError(f"unsupported stream version: {version}")

    stream_kind = headers["STREAM_KIND"]
    source_name = headers["SOURCE_NAME"]
    source_id = int(headers["SOURCE_ID"], 10)
    raw_size = int(headers["RAW_SIZE"], 10)
    compressed_size = int(headers["COMPRESSED_SIZE"], 10)
    checksum = headers["CHECKSUM"]
    if not checksum.startswith(CHECKSUM_PREFIX):
        raise TraceStreamError(f"unsupported checksum: {checksum}")

    encoded = "".join(line.strip() for line in payload_lines if line.strip())
    try:
        compressed = base64.b64decode(encoded, validate=True)
    except binascii.Error as exc:
        raise TraceStreamError(f"invalid base64 payload: {exc}") from exc

    if len(compressed) != compressed_size:
        raise TraceStreamError(
            f"compressed size mismatch: decoded={len(compressed)} expected={compressed_size}"
        )

    raw = _decode_lz4_literal_only(compressed, raw_size)
    actual_crc = crc32_hex(raw)
    expected_crc = checksum[len(CHECKSUM_PREFIX):].lower()
    if actual_crc != expected_crc:
        raise TraceStreamError(f"crc32 mismatch: decoded={actual_crc} expected={expected_crc}")

    return TraceStream(
        stream_kind=stream_kind,
        source_name=source_name,
        source_id=source_id,
        raw_size=raw_size,
        compressed_size=compressed_size,
        checksum=checksum,
        raw=raw,
    )


def parse_streams(text: str) -> list[TraceStream]:
    streams: list[TraceStream] = []
    lines = text.splitlines()
    index = 0

    while index < len(lines):
        if lines[index].strip() != TRANSFER_START:
            index += 1
            continue

        index += 1
        headers: dict[str, str] = {}
        payload_lines: list[str] = []

        while index < len(lines):
            line = lines[index].strip()
            index += 1
            if line == TRANSFER_END:
                streams.append(_build_stream(headers, payload_lines))
                break
            if line.startswith(("TYPE:", "STREAM_KIND:", "FORMAT_VERSION:", "SOURCE_NAME:",
                                "SOURCE_ID:", "RAW_SIZE:", "COMPRESSED_SIZE:", "CHECKSUM:")):
                key, value = _parse_header_line(line)
                headers[key] = value
            else:
                payload_lines.append(line)
        else:
            raise TraceStreamError("unterminated VIO_TRACE_STREAM transfer")

    return streams
