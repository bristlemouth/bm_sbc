#pragma once

/// @file cobs.h
/// @brief Minimal Consistent Overhead Byte Stuffing (COBS) encoder/decoder.
///
/// COBS encodes arbitrary byte strings so that zero bytes never appear in
/// the output.  A trailing 0x00 delimiter can then unambiguously mark the
/// end of a frame on a serial link.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Maximum encoded length for a given source length.
/// COBS adds at most ceil(len/254) overhead bytes, plus one leading code byte.
#define COBS_ENCODE_MAX(src_len) ((src_len) + ((src_len) / 254) + 1)

/// Encode @p src_len bytes from @p src into @p dst using COBS.
/// @param dst       Output buffer (must be at least COBS_ENCODE_MAX(src_len)
///                  bytes).
/// @param dst_len   Size of the output buffer.
/// @param src       Input data to encode.
/// @param src_len   Length of input data.
/// @return Number of bytes written to @p dst, or 0 on error (e.g. buffer
///         overflow).
size_t cobs_encode(uint8_t *dst, size_t dst_len, const uint8_t *src,
                   size_t src_len);

/// Decode a COBS-encoded block.  The input must NOT include the trailing
/// 0x00 delimiter (strip it before calling).
/// @param dst       Output buffer (at most src_len - 1 bytes will be written).
/// @param dst_len   Size of the output buffer.
/// @param src       COBS-encoded input (without trailing delimiter).
/// @param src_len   Length of encoded input.
/// @return Number of decoded bytes written to @p dst, or 0 on error.
size_t cobs_decode(uint8_t *dst, size_t dst_len, const uint8_t *src,
                   size_t src_len);

#ifdef __cplusplus
}
#endif
