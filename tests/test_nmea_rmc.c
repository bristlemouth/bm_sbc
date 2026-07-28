/// @file test_nmea_rmc.c
/// @brief Unit tests for the NMEA RMC time parser.
///
/// The parser is fed the live pubsub receive buffer, so the important
/// properties are: it never writes to the caller's buffer, never reads past
/// the declared length, and rejects malformed sentences instead of walking
/// off the end of its field table.

#include "nmea_rmc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("  FAIL: %s\n", msg);                                             \
      g_fail++;                                                                \
    } else {                                                                   \
      g_pass++;                                                                \
    }                                                                          \
  } while (0)

/// Append "*hh" to @p body (which must start with '$') and return the length.
static size_t make_sentence(char *out, size_t out_cap, const char *body) {
  unsigned char cksum = 0;
  for (const char *c = body + 1; *c; c++) {
    cksum ^= (unsigned char)*c;
  }
  int n = snprintf(out, out_cap, "%s*%02X", body, cksum);
  return (n < 0) ? 0 : (size_t)n;
}

/// Parse @p sentence from a heap buffer sized to *exactly* len bytes, then
/// assert it came back unmodified.
///
/// The exact-size allocation is the point: it mirrors the real caller, where
/// the sentence is a slice of the live pubsub receive buffer with no slack
/// after it.  Under -fsanitize=address the redzone turns any read or write
/// past the payload into a hard failure — that is what catches a stray
/// terminator at buf[len] or a checksum scan that runs off the end.
static bool parse_checked(const char *sentence, size_t len,
                          struct timespec *out) {
  if (len == 0) {
    // malloc(0) may legitimately return NULL; nothing to guard.
    return nmea_rmc_parse("", 0, out);
  }

  char *buf = (char *)malloc(len);
  if (!buf) {
    printf("  FAIL: out of memory\n");
    g_fail++;
    return false;
  }
  memcpy(buf, sentence, len);

  bool ok = nmea_rmc_parse(buf, len, out);

  if (memcmp(buf, sentence, len) != 0) {
    printf("  FAIL: parser modified the caller's buffer\n");
    g_fail++;
  } else {
    g_pass++;
  }
  free(buf);
  return ok;
}

// ---------------------------------------------------------------------------

static void test_valid_rmc(void) {
  printf("test_valid_rmc\n");
  char s[128];
  size_t len = make_sentence(
      s, sizeof(s),
      "$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230324,003.1,W");

  struct timespec ts = {0, 0};
  CHECK(parse_checked(s, len, &ts), "valid RMC should parse");
  // 2024-03-23 12:35:19 UTC
  CHECK(ts.tv_sec == 1711197319, "epoch seconds");
  CHECK(ts.tv_nsec == 0, "nanoseconds");
}

static void test_centiseconds(void) {
  printf("test_centiseconds\n");
  char s[128];
  size_t len = make_sentence(
      s, sizeof(s),
      "$GPRMC,235959.25,A,4807.038,N,01131.000,E,022.4,084.4,311224,003.1,W");

  struct timespec ts = {0, 0};
  CHECK(parse_checked(s, len, &ts), "valid RMC should parse");
  // 2024-12-31 23:59:59 UTC
  CHECK(ts.tv_sec == 1735689599, "epoch seconds");
  CHECK(ts.tv_nsec == 250000000L, "centiseconds -> nanoseconds");
}

/// A checksum-valid sentence with far fewer than ten fields.  The old parser
/// read uninitialised entries of its field table and segfaulted here.
static void test_too_few_fields(void) {
  printf("test_too_few_fields\n");
  struct timespec ts = {0, 0};

  char s[128];
  size_t len = make_sentence(s, sizeof(s), "$X");
  CHECK(!parse_checked(s, len, &ts), "'$X' should be rejected");

  len = make_sentence(s, sizeof(s), "$GPRMC,123519.00,A,4807.038,N");
  CHECK(!parse_checked(s, len, &ts), "truncated RMC should be rejected");

  len = make_sentence(s, sizeof(s), "$GPRMC,,,,,,,,");
  CHECK(!parse_checked(s, len, &ts), "empty fields should be rejected");
}

/// No '*' in the sentence: the old parser ran strtoul() past the end of the
/// buffer looking for the checksum digits.
static void test_missing_checksum(void) {
  printf("test_missing_checksum\n");
  struct timespec ts = {0, 0};

  const char *no_star =
      "$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230324,003.1,W";
  CHECK(!parse_checked(no_star, strlen(no_star), &ts),
        "sentence with no '*' should be rejected");

  // '*' present but the two hex digits are cut off.
  const char *short_star = "$GPRMC,123519.00,A,230324*";
  CHECK(!parse_checked(short_star, strlen(short_star), &ts),
        "'*' with no digits should be rejected");

  const char *one_digit = "$GPRMC,123519.00,A,230324*4";
  CHECK(!parse_checked(one_digit, strlen(one_digit), &ts),
        "'*' with one digit should be rejected");

  const char *not_hex = "$GPRMC,123519.00,A,230324*zz";
  CHECK(!parse_checked(not_hex, strlen(not_hex), &ts),
        "non-hex checksum should be rejected");
}

static void test_bad_checksum(void) {
  printf("test_bad_checksum\n");
  char s[128];
  size_t len = make_sentence(
      s, sizeof(s),
      "$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230324,003.1,W");
  s[len - 1] ^= 0x01; // corrupt the low checksum digit

  struct timespec ts = {0, 0};
  CHECK(!parse_checked(s, len, &ts), "bad checksum should be rejected");
}

static void test_degenerate_lengths(void) {
  printf("test_degenerate_lengths\n");
  struct timespec ts = {0, 0};

  CHECK(!nmea_rmc_parse(NULL, 10, &ts), "NULL input should be rejected");
  CHECK(!parse_checked("", 0, &ts), "zero length should be rejected");
  CHECK(!parse_checked("$", 1, &ts), "'$' alone should be rejected");
  CHECK(!parse_checked("*", 1, &ts), "'*' alone should be rejected");

  // Longer than the longest legal NMEA sentence.
  char oversized[MAX_NMEA_RMC_LEN + 8];
  memset(oversized, 'A', sizeof(oversized));
  oversized[0] = '$';
  CHECK(!parse_checked(oversized, sizeof(oversized), &ts),
        "oversized sentence should be rejected");

  // Exactly the maximum length: must be handled without overrunning.
  char maxlen[MAX_NMEA_RMC_LEN];
  memset(maxlen, 'A', sizeof(maxlen));
  maxlen[0] = '$';
  (void)parse_checked(maxlen, sizeof(maxlen), &ts);
}

/// Sentences that pass the checksum but hold junk in the time/date fields.
static void test_unparseable_time(void) {
  printf("test_unparseable_time\n");
  struct timespec ts = {0, 0};

  char s[128];
  size_t len = make_sentence(
      s, sizeof(s), "$GPRMC,xxxxxx.xx,A,4807.038,N,01131.000,E,0,0,230324,0,W");
  CHECK(!parse_checked(s, len, &ts), "non-numeric time should be rejected");

  len = make_sentence(
      s, sizeof(s), "$GPRMC,123519.00,A,4807.038,N,01131.000,E,0,0,xxxxxx,0,W");
  CHECK(!parse_checked(s, len, &ts), "non-numeric date should be rejected");

  // Time field present but with no fractional part.
  len = make_sentence(
      s, sizeof(s), "$GPRMC,123519,A,4807.038,N,01131.000,E,0,0,230324,0,W");
  (void)parse_checked(s, len, &ts);
}

int main(void) {
  test_valid_rmc();
  test_centiseconds();
  test_too_few_fields();
  test_missing_checksum();
  test_bad_checksum();
  test_degenerate_lengths();
  test_unparseable_time();

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
