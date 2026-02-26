#pragma once

/// @file frame_codec.h
/// @brief UART L2 frame codec — encodes/decodes L2 Ethernet frames for
///        transport over a serial link.
///
/// Wire format:
///   [COBS-encoded payload] [0x00 delimiter]
///
/// Payload (before COBS encoding):
///   [len_hi] [len_lo] [L2 frame bytes...] [crc32 (4 bytes, big-endian)]
///
/// - Length is a 2-byte big-endian value equal to the L2 frame size.
/// - CRC-32C (Castagnoli) is computed over the length + L2 frame bytes.
/// - COBS encoding ensures no 0x00 bytes appear in the encoded payload,
///   so 0x00 can serve as an unambiguous frame delimiter.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Overhead: 2 bytes length + 4 bytes CRC-32C.
#define FRAME_CODEC_OVERHEAD 6

/// Maximum L2 frame size we support (standard Ethernet MTU + header).
#define FRAME_CODEC_MAX_L2_SIZE 1522

/// Maximum wire size: COBS overhead + payload + delimiter.
#define FRAME_CODEC_MAX_WIRE_SIZE                                              \
  (COBS_ENCODE_MAX(FRAME_CODEC_MAX_L2_SIZE + FRAME_CODEC_OVERHEAD) + 1)

/// Encode an L2 frame into wire format (COBS-encoded, 0x00-terminated).
///
/// @param wire      Output buffer for the encoded frame.
/// @param wire_len  Size of the output buffer.
/// @param l2_frame  The raw L2 Ethernet frame to encode.
/// @param l2_len    Length of the L2 frame in bytes.
/// @return Total number of bytes written to @p wire (including the 0x00
///         delimiter), or 0 on error.
size_t frame_encode(uint8_t *wire, size_t wire_len, const uint8_t *l2_frame,
                    size_t l2_len);

/// Decode a wire frame back into the original L2 frame.
///
/// @param l2_frame  Output buffer for the decoded L2 frame.
/// @param l2_len    Size of the output buffer.
/// @param wire      The wire-format data (COBS-encoded, WITHOUT the trailing
///                  0x00 delimiter — caller strips it before calling).
/// @param wire_len  Length of the wire data.
/// @return Length of the decoded L2 frame, or 0 on error (CRC mismatch,
///         length mismatch, decode failure).
size_t frame_decode(uint8_t *l2_frame, size_t l2_len, const uint8_t *wire,
                    size_t wire_len);

#ifdef __cplusplus
}
#endif
