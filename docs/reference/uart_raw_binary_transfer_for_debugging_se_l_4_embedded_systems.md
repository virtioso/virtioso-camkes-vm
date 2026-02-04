# UART Raw Binary Transfer for Debugging

This document summarizes practical, **low‑overhead methods for transferring large binary blobs (MB‑scale)** over UART for debugging purposes. The focus is on **raw binary streaming**, not production protocols, and the target environment is **embedded kernels / hypervisors (e.g. seL4)** where simplicity, determinism, and ease of removal matter.

The goal is to replace text encodings (Base64/hex) with framing approaches that:
- preserve binary data as‑is
- tolerate line noise and partial corruption
- allow resynchronization
- impose minimal size and CPU overhead

---

## Why Not Base64 (or Hex)

- **Base64** expands data by **~33%** and adds CPU overhead during encoding/decoding.
- **Hex** doubles the data size (**+100% overhead**).
- Both are only needed if the transport must be ASCII‑clean.

UART does **not** require ASCII. Binary framing is the right tool.

---

## Design Requirements for Debug UART Dumps

For large diagnostic blobs (crash dumps, page tables, logs, memory snapshots):

- Must survive **partial corruption** (drop bad frames, keep going)
- Must allow **resynchronization** without resetting the system
- Must tolerate **slow receivers / backpressure**
- Must be **easy to implement in kernel context**
- Preferably **stateless or near‑stateless** on the sender side

Retransmission is optional; robustness via resync is usually enough for debug.

---

## Recommended Framing Approaches

### 1. COBS (Consistent Overhead Byte Stuffing)

**Summary:** Binary‑safe framing with a fixed delimiter.

- Uses a reserved byte (commonly `0x00`) as frame delimiter
- Payload is encoded so that `0x00` never appears inside the frame
- Receiver resyncs by scanning for the next `0x00`

**Overhead:**
- Worst case: +1 byte per 254 bytes (~0.4%)
- Plus 1 byte per frame

**Why it’s ideal for UART debug:**
- Deterministic, tiny overhead
- Very fast encoder/decoder
- No data‑dependent worst cases
- Excellent resynchronization behavior

**Typical Frame Layout (before COBS):**

```
struct frame {
    u32 magic;        // constant marker
    u32 blob_id;      // identify dump instance
    u32 seq;          // chunk sequence number
    u16 payload_len;
    u16 flags;
    u8  payload[payload_len];
    u32 crc32;        // header + payload
}
```

After building the frame, COBS‑encode it and append `0x00`.

---

### 2. HDLC / PPP‑Style Byte Stuffing

**Summary:** Classic framing using flag bytes and escaping.

- Frame delimiter: `0x7E`
- Escape byte: `0x7D`
- Escaped bytes are XORed with `0x20`

**Pros:**
- Very well understood
- Easy to debug with logic analyzers
- Natural resync on `0x7E`

**Cons:**
- Overhead is **data‑dependent**
- Worst case can be larger than COBS

Still a very solid choice if you already have HDLC‑style code.

---

### 3. SLIP

**Summary:** Minimal framing with END/ESC bytes.

- END = `0xC0`
- ESC = `0xDB`

**Pros:**
- Extremely simple
- Easy to implement in kernel code

**Cons:**
- No inherent CRC
- Data‑dependent overhead
- Slightly weaker resync semantics than COBS

Usually combined with an explicit CRC field.

---

## Chunking Strategy (Important)

Never send multi‑megabyte blobs as a single frame.

**Recommended:**
- Payload size: **512–2048 bytes**
- Each frame independently checksummed
- Receiver writes chunks by `(blob_id, seq)`

Benefits:
- Partial dumps are still usable
- Errors are localized
- UART buffers are not overwhelmed

---

## Error Detection

Use a checksum per frame:

- **CRC32** (best robustness, more CPU)
- **CRC16‑CCITT** (lighter, often sufficient)

Avoid ad‑hoc XOR checksums; UART noise is real.

---

## Flow Control Considerations

UART TX can easily outrun the receiver.

Options:
- Hardware RTS/CTS (best, if available)
- Software ACK every N frames
- TX ring buffer + drop policy (debug‑only builds)

For one‑way crash dumps, many systems simply:
- transmit continuously
- rely on receiver resync
- accept partial data

This is often good enough.

---

## Comparison Summary

| Method | Overhead | Resync | Complexity | Notes |
|------|---------|--------|------------|------|
| Base64 | +33% | Yes | Medium | ASCII‑only environments |
| Hex | +100% | Yes | Low | Debug only, very slow |
| SLIP | Low | Good | Very low | Add CRC yourself |
| HDLC/PPP | Medium | Excellent | Medium | Classic, proven |
| **COBS** | **Very low** | **Excellent** | **Low** | **Recommended** |

---

## Practical Recommendation

For seL4 / kernel‑level debug on platforms like Jetson AGX Orin:

> **COBS‑framed binary chunks with CRC32**

This gives:
- minimal overhead
- fast resynchronization
- clean host‑side tooling
- small, removable code footprint

Host‑side decoding can be done in Python, C, or Rust in a few hundred lines.

---

## Final Note

These mechanisms are intentionally **not production protocols**. They are designed to:
- get data out of a dying system
- work even when parts of the stream are corrupted
- be removed once debugging is complete

In that role, simple binary framing beats historical file‑transfer protocols (e.g. ZMODEM) both in integration effort and debuggability.

