/// @file test_frame_codec.c
/// @brief Unit tests for COBS, CRC-32C, and frame_codec.

#include "cobs.h"
#include "crc32c.h"
#include "frame_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_EQ(a, b, msg)                                                   \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      printf("  FAIL: %s (got %zu, expected %zu)\n", msg,                      \
             (size_t)(a), (size_t)(b));                                        \
      g_fail++;                                                                \
    } else {                                                                   \
      g_pass++;                                                                \
    }                                                                          \
  } while (0)

#define ASSERT_MEM_EQ(a, b, len, msg)                                          \
  do {                                                                         \
    if (memcmp((a), (b), (len)) != 0) {                                        \
      printf("  FAIL: %s (memory mismatch)\n", msg);                           \
      g_fail++;                                                                \
    } else {                                                                   \
      g_pass++;                                                                \
    }                                                                          \
  } while (0)

// ---- COBS tests -----------------------------------------------------------

static void test_cobs_empty(void) {
  uint8_t enc[8];
  size_t n = cobs_encode(enc, sizeof(enc), (const uint8_t *)"", 0);
  // Empty input encodes to a single code byte (0x01).
  ASSERT_EQ(n, 1, "cobs_encode empty length");
  ASSERT_EQ(enc[0], 0x01, "cobs_encode empty code byte");

  uint8_t dec[8];
  size_t d = cobs_decode(dec, sizeof(dec), enc, n);
  ASSERT_EQ(d, 0, "cobs_decode empty roundtrip");
}

static void test_cobs_no_zeros(void) {
  const uint8_t src[] = {0x01, 0x02, 0x03};
  uint8_t enc[16], dec[16];
  size_t n = cobs_encode(enc, sizeof(enc), src, 3);
  ASSERT_EQ(enc[0], 0x04, "cobs no-zeros code byte");
  size_t d = cobs_decode(dec, sizeof(dec), enc, n);
  ASSERT_EQ(d, 3, "cobs no-zeros roundtrip length");
  ASSERT_MEM_EQ(dec, src, 3, "cobs no-zeros roundtrip data");
}

static void test_cobs_with_zeros(void) {
  const uint8_t src[] = {0x00, 0x00, 0x00};
  uint8_t enc[16], dec[16];
  size_t n = cobs_encode(enc, sizeof(enc), src, 3);
  size_t d = cobs_decode(dec, sizeof(dec), enc, n);
  ASSERT_EQ(d, 3, "cobs all-zeros roundtrip length");
  ASSERT_MEM_EQ(dec, src, 3, "cobs all-zeros roundtrip data");
}

static void test_cobs_mixed(void) {
  const uint8_t src[] = {0x11, 0x22, 0x00, 0x33};
  uint8_t enc[16], dec[16];
  size_t n = cobs_encode(enc, sizeof(enc), src, 4);
  // Encoded data must contain no zero bytes.
  for (size_t i = 0; i < n; i++) {
    if (enc[i] == 0x00) {
      printf("  FAIL: cobs mixed: zero at offset %zu\n", i);
      g_fail++;
      return;
    }
  }
  g_pass++;
  size_t d = cobs_decode(dec, sizeof(dec), enc, n);
  ASSERT_EQ(d, 4, "cobs mixed roundtrip length");
  ASSERT_MEM_EQ(dec, src, 4, "cobs mixed roundtrip data");
}

static void test_cobs_254_run(void) {
  // 254 non-zero bytes — exactly one COBS block.
  uint8_t src[254], enc[512], dec[512];
  memset(src, 0xAA, 254);
  size_t n = cobs_encode(enc, sizeof(enc), src, 254);
  size_t d = cobs_decode(dec, sizeof(dec), enc, n);
  ASSERT_EQ(d, 254, "cobs 254-run roundtrip length");
  ASSERT_MEM_EQ(dec, src, 254, "cobs 254-run roundtrip data");
}

static void test_cobs_255_run(void) {
  // 255 non-zero bytes — forces a block split at 254.
  uint8_t src[255], enc[512], dec[512];
  memset(src, 0xBB, 255);
  size_t n = cobs_encode(enc, sizeof(enc), src, 255);
  size_t d = cobs_decode(dec, sizeof(dec), enc, n);
  ASSERT_EQ(d, 255, "cobs 255-run roundtrip length");
  ASSERT_MEM_EQ(dec, src, 255, "cobs 255-run roundtrip data");
}

static void test_cobs_null_ptrs(void) {
  uint8_t buf[8];
  ASSERT_EQ(cobs_encode(NULL, 8, buf, 1), 0, "cobs_encode null dst");
  ASSERT_EQ(cobs_encode(buf, 8, NULL, 1), 0, "cobs_encode null src");
  ASSERT_EQ(cobs_decode(NULL, 8, buf, 1), 0, "cobs_decode null dst");
  ASSERT_EQ(cobs_decode(buf, 8, NULL, 1), 0, "cobs_decode null src");
}

static void test_cobs_buffer_too_small(void) {
  const uint8_t src[] = {0x01, 0x02, 0x03};
  uint8_t enc[1]; // way too small
  ASSERT_EQ(cobs_encode(enc, sizeof(enc), src, 3), 0,
            "cobs_encode buffer overflow");
}

// ---- CRC-32C tests --------------------------------------------------------

static void test_crc32c_known_value(void) {
  // CRC-32C of "123456789" is 0xE3069283.
  const uint8_t data[] = "123456789";
  uint32_t crc = crc32c(data, 9);
  if (crc != 0xE3069283U) {
    printf("  FAIL: crc32c known value (got 0x%08X, expected 0xE3069283)\n",
           crc);
    g_fail++;
  } else {
    g_pass++;
  }
}

static void test_crc32c_incremental(void) {
  const uint8_t data[] = "123456789";
  uint32_t crc = 0xFFFFFFFF;
  crc = crc32c_update(crc, data, 5);
  crc = crc32c_update(crc, data + 5, 4);
  crc = crc32c_finalize(crc);
  if (crc != 0xE3069283U) {
    printf("  FAIL: crc32c incremental (got 0x%08X)\n", crc);
    g_fail++;
  } else {
    g_pass++;
  }
}

static void test_crc32c_empty(void) {
  uint32_t crc = crc32c((const uint8_t *)"", 0);
  if (crc != 0x00000000U) {
    printf("  FAIL: crc32c empty (got 0x%08X, expected 0x00000000)\n", crc);
    g_fail++;
  } else {
    g_pass++;
  }
}

// ---- Frame codec tests ----------------------------------------------------

static void test_frame_roundtrip(void) {
  const uint8_t l2[] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, // dst MAC
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, // src MAC
    0x08, 0x00,                          // ethertype
    0x48, 0x65, 0x6C, 0x6C, 0x6F,       // payload "Hello"
  };
  uint8_t wire[FRAME_CODEC_MAX_WIRE_SIZE];
  size_t wn = frame_encode(wire, sizeof(wire), l2, sizeof(l2));
  ASSERT_EQ(wn > 0, 1, "frame_encode success");

  // Last byte must be 0x00 delimiter.
  ASSERT_EQ(wire[wn - 1], 0x00, "frame_encode trailing delimiter");

  // No zeros in the encoded body (before delimiter).
  int has_zero = 0;
  for (size_t i = 0; i < wn - 1; i++) {
    if (wire[i] == 0x00) { has_zero = 1; break; }
  }
  ASSERT_EQ(has_zero, 0, "frame_encode no zeros in body");

  // Decode (strip the trailing 0x00 before calling frame_decode).
  uint8_t out[1600];
  size_t dn = frame_decode(out, sizeof(out), wire, wn - 1);
  ASSERT_EQ(dn, sizeof(l2), "frame_decode roundtrip length");
  ASSERT_MEM_EQ(out, l2, sizeof(l2), "frame_decode roundtrip data");
}

static void test_frame_min_frame(void) {
  // Smallest valid frame: 1 byte.
  const uint8_t l2[] = {0x42};
  uint8_t wire[FRAME_CODEC_MAX_WIRE_SIZE];
  size_t wn = frame_encode(wire, sizeof(wire), l2, 1);
  ASSERT_EQ(wn > 0, 1, "frame_encode 1-byte");
  uint8_t out[16];
  size_t dn = frame_decode(out, sizeof(out), wire, wn - 1);
  ASSERT_EQ(dn, 1, "frame_decode 1-byte length");
  ASSERT_EQ(out[0], 0x42, "frame_decode 1-byte value");
}

static void test_frame_max_frame(void) {
  // Max L2 frame size.
  uint8_t l2[FRAME_CODEC_MAX_L2_SIZE];
  memset(l2, 0x55, sizeof(l2));
  uint8_t wire[FRAME_CODEC_MAX_WIRE_SIZE];
  size_t wn = frame_encode(wire, sizeof(wire), l2, sizeof(l2));
  ASSERT_EQ(wn > 0, 1, "frame_encode max size");
  uint8_t out[FRAME_CODEC_MAX_L2_SIZE];
  size_t dn = frame_decode(out, sizeof(out), wire, wn - 1);
  ASSERT_EQ(dn, sizeof(l2), "frame_decode max size roundtrip");
  ASSERT_MEM_EQ(out, l2, sizeof(l2), "frame_decode max size data");
}

static void test_frame_corrupt_crc(void) {
  const uint8_t l2[] = {0x01, 0x02, 0x03, 0x04};
  uint8_t wire[FRAME_CODEC_MAX_WIRE_SIZE];
  size_t wn = frame_encode(wire, sizeof(wire), l2, sizeof(l2));
  // Flip a bit in the encoded body to corrupt the CRC.
  wire[1] ^= 0x01;
  uint8_t out[16];
  size_t dn = frame_decode(out, sizeof(out), wire, wn - 1);
  ASSERT_EQ(dn, 0, "frame_decode rejects corrupted frame");
}

static void test_frame_null_ptrs(void) {
  uint8_t buf[64];
  ASSERT_EQ(frame_encode(NULL, 64, buf, 4), 0, "frame_encode null wire");
  ASSERT_EQ(frame_encode(buf, 64, NULL, 4), 0, "frame_encode null l2");
  ASSERT_EQ(frame_encode(buf, 64, buf, 0), 0, "frame_encode zero len");
  ASSERT_EQ(frame_decode(NULL, 64, buf, 4), 0, "frame_decode null l2");
  ASSERT_EQ(frame_decode(buf, 64, NULL, 4), 0, "frame_decode null wire");
  ASSERT_EQ(frame_decode(buf, 64, buf, 0), 0, "frame_decode zero len");
}

static void test_frame_oversized(void) {
  uint8_t l2[FRAME_CODEC_MAX_L2_SIZE + 1];
  memset(l2, 0xAA, sizeof(l2));
  uint8_t wire[FRAME_CODEC_MAX_WIRE_SIZE + 64];
  ASSERT_EQ(frame_encode(wire, sizeof(wire), l2, sizeof(l2)), 0,
            "frame_encode rejects oversized");
}

// ---- Main -----------------------------------------------------------------

int main(void) {
  printf("=== COBS ===\n");
  test_cobs_empty();
  test_cobs_no_zeros();
  test_cobs_with_zeros();
  test_cobs_mixed();
  test_cobs_254_run();
  test_cobs_255_run();
  test_cobs_null_ptrs();
  test_cobs_buffer_too_small();

  printf("=== CRC-32C ===\n");
  test_crc32c_known_value();
  test_crc32c_incremental();
  test_crc32c_empty();

  printf("=== Frame Codec ===\n");
  test_frame_roundtrip();
  test_frame_min_frame();
  test_frame_max_frame();
  test_frame_corrupt_crc();
  test_frame_null_ptrs();
  test_frame_oversized();

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
