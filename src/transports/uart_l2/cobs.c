#include "cobs.h"

/// Encode @p src_len bytes from @p src into @p dst using COBS.
///
/// Algorithm: walk through the source data, grouping runs of non-zero bytes
/// up to 254 in length.  Each group is preceded by a code byte whose value
/// is (run_length + 1).  A zero byte in the source ends the current group
/// and starts a new one.
size_t cobs_encode(uint8_t *dst, size_t dst_len, const uint8_t *src,
                   size_t src_len) {
  if (!dst || !src) {
    return 0;
  }

  size_t dst_idx = 0;
  size_t code_idx = dst_idx; // position of the current code byte
  uint8_t code = 1;          // distance to next zero (or end-of-block)

  // Reserve space for the first code byte.
  if (dst_idx >= dst_len) {
    return 0;
  }
  dst_idx++;

  for (size_t i = 0; i < src_len; i++) {
    if (src[i] == 0) {
      // End of a non-zero run — write the code byte.
      dst[code_idx] = code;
      code_idx = dst_idx;
      code = 1;
      if (dst_idx >= dst_len) {
        return 0;
      }
      dst_idx++;
    } else {
      if (dst_idx >= dst_len) {
        return 0;
      }
      dst[dst_idx++] = src[i];
      code++;
      if (code == 0xFF) {
        // Maximum non-zero run length (254 data bytes + 1 code byte).
        dst[code_idx] = code;
        code_idx = dst_idx;
        code = 1;
        if (i + 1 < src_len) {
          if (dst_idx >= dst_len) {
            return 0;
          }
          dst_idx++;
        }
      }
    }
  }

  // Write the final code byte.
  dst[code_idx] = code;
  return dst_idx;
}

/// Decode a COBS-encoded block (without trailing 0x00 delimiter).
size_t cobs_decode(uint8_t *dst, size_t dst_len, const uint8_t *src,
                   size_t src_len) {
  if (!dst || !src || src_len == 0) {
    return 0;
  }

  size_t dst_idx = 0;
  size_t src_idx = 0;

  while (src_idx < src_len) {
    uint8_t code = src[src_idx++];
    if (code == 0) {
      // Zero byte in COBS-encoded data is invalid.
      return 0;
    }

    uint8_t run = code - 1;
    if (src_idx + run > src_len) {
      // Not enough data for the declared run.
      return 0;
    }
    if (dst_idx + run > dst_len) {
      // Output buffer overflow.
      return 0;
    }

    for (uint8_t j = 0; j < run; j++) {
      if (src[src_idx] == 0) {
        // Unexpected zero in encoded data.
        return 0;
      }
      dst[dst_idx++] = src[src_idx++];
    }

    // If code < 0xFF and we're not at the end, emit a zero (implicit
    // delimiter).
    if (code < 0xFF && src_idx < src_len) {
      if (dst_idx >= dst_len) {
        return 0;
      }
      dst[dst_idx++] = 0;
    }
  }

  return dst_idx;
}
