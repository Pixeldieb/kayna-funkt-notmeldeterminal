#pragma once
#include <stddef.h>
#include <stdint.h>

// Consistent Overhead Byte Stuffing (COBS) -- encodes a buffer so the byte
// 0x00 never appears in the output except as a frame delimiter the caller
// appends separately. Used for kayna-funkt's ESP32-S3 <-> RP2040 serial
// link on the SenseCAP board (see src/sensecap/sd_mirror.cpp and
// src/sensecap_rp2040/main.cpp) -- the same framing family Seeed's own
// stock RP2040 firmware uses on this exact UART (see Seeed's "ESP32-S3 and
// RP2040 Communication" wiki page), just our own hand-rolled implementation
// instead of depending on an external library's exact Stream/callback API
// (see platformio.ini's sensecap_rp2040 env comment for why).
//
// Reference: Cheshire & Baker, "Consistent Overhead Byte Stuffing" (2004),
// https://www.stuartcheshire.org/papers/COBSforToN.pdf -- this is the
// textbook algorithm, not a novel implementation.
//
// Usage: encode a packet, write the encoded bytes followed by a single
// 0x00 byte to the UART. On the receiving end, accumulate bytes until a
// 0x00 is seen, then decode everything received before it.

// dst must be at least cobsMaxEncodedLength(srcLen) bytes.
constexpr size_t cobsMaxEncodedLength(size_t srcLen) { return srcLen + srcLen / 254 + 1; }

inline size_t cobsEncode(const uint8_t *src, size_t srcLen, uint8_t *dst) {
  size_t readIdx = 0, writeIdx = 1, codeIdx = 0;
  uint8_t code = 1;
  while (readIdx < srcLen) {
    if (src[readIdx] == 0) {
      dst[codeIdx] = code;
      code = 1;
      codeIdx = writeIdx++;
      readIdx++;
    } else {
      dst[writeIdx++] = src[readIdx++];
      code++;
      if (code == 0xFF) {
        dst[codeIdx] = code;
        code = 1;
        codeIdx = writeIdx++;
      }
    }
  }
  dst[codeIdx] = code;
  return writeIdx;
}

// Returns the decoded length, or 0 on malformed input. dst must be at
// least srcLen bytes (decoding never grows the data). Does NOT expect the
// trailing 0x00 delimiter to be part of src -- strip that first.
inline size_t cobsDecode(const uint8_t *src, size_t srcLen, uint8_t *dst) {
  size_t readIdx = 0, writeIdx = 0;
  while (readIdx < srcLen) {
    uint8_t code = src[readIdx];
    if (code == 0 || (readIdx + code > srcLen && code != 1)) return 0; // malformed
    readIdx++;
    for (uint8_t i = 1; i < code; i++) {
      dst[writeIdx++] = src[readIdx++];
    }
    if (code != 0xFF && readIdx < srcLen) dst[writeIdx++] = 0;
  }
  return writeIdx;
}
