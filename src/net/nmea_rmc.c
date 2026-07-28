#include "nmea_rmc.h"

#include "bm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 0=$GPRMC, 1=HHMMSS.ss, 2=A/V, 3=lat, 4=N/S, 5=lon, 6=E/W,
// 7=speed, 8=course, 9=DDMMYY, ...
// We are only interested in fields 1 and 9.  Details for the rest:
// https://gpsd.gitlab.io/gpsd/NMEA.html#_rmc_recommended_minimum_navigation_information
#define NMEA_FIELD_TIME 1
#define NMEA_FIELD_DATE 9
#define MAX_NMEA_FIELDS 14

/// XOR checksum of everything between '$' and '*', compared against the two
/// hex digits following the '*'.
static bool nmea_checksum_valid(const char *line, size_t len) {
  uint8_t checksum = 0;
  size_t idx = 1;

  while ((idx < len) && ('*' != line[idx])) {
    checksum ^= (uint8_t)line[idx];
    idx++;
  }

  // Need the '*' plus two hex digits still inside the buffer.  This also
  // rejects a sentence with no '*' at all (idx == len), which would
  // otherwise read past the end of the caller's buffer.
  if (idx + 2 >= len) {
    return false;
  }

  const char hex[3] = {line[idx + 1], line[idx + 2], '\0'};
  char *end = NULL;
  uint8_t line_checksum = (uint8_t)strtoul(hex, &end, 16);
  if (end != &hex[2]) {
    return false; // not two hex digits
  }

  return checksum == line_checksum;
}

bool nmea_rmc_parse(const char *raw, size_t len, struct timespec *time_output) {
  if (!raw || !time_output || len == 0 || len > MAX_NMEA_RMC_LEN) {
    bm_log_error("Invalid NMEA RMC string");
    return false;
  }

  if (!nmea_checksum_valid(raw, len)) {
    bm_log_error("Invalid checksum");
    return false;
  }

  // Split on a NUL-terminated copy.  The caller's buffer is the live pubsub
  // receive buffer: writing the terminator there would run one byte past the
  // payload, and the same bytes are forwarded downstream afterwards.
  char line[MAX_NMEA_RMC_LEN + 1];
  memcpy(line, raw, len);
  line[len] = '\0';

  char *fields[MAX_NMEA_FIELDS] = {0};
  int field_count = 0;
  fields[field_count++] = line;
  for (size_t i = 0; i < len && field_count < MAX_NMEA_FIELDS; i++) {
    if (line[i] == ',' || line[i] == '*') {
      line[i] = '\0';
      // Point the field at the first char after the ',' or '*'.
      fields[field_count++] = &line[i + 1];
    }
  }

  if (field_count <= NMEA_FIELD_DATE) {
    bm_log_error("NMEA RMC has too few fields (%d)", field_count);
    return false;
  }

  int hour, min, sec, centisec = 0;
  if (sscanf(fields[NMEA_FIELD_TIME], "%2d%2d%2d.%2d", &hour, &min, &sec,
             &centisec) != 4) {
    bm_log_error("Failed to get HHMMSS.ss");
    return false;
  }

  int day, mon, year = 0;
  if (sscanf(fields[NMEA_FIELD_DATE], "%2d%2d%2d", &day, &mon, &year) != 3) {
    bm_log_error("Failed to get DDMMYY");
    return false;
  }

  struct tm datetime = {
      .tm_sec = sec,
      .tm_min = min,
      .tm_hour = hour,
      .tm_mday = day,
      .tm_mon = mon - 1,
      // GPS year is 2 digits. tm_year is (year - 1900).
      // So we do gps_year + 2000 - 1900 -> + 100
      .tm_year = year + 100,
  };

  time_t epoch = timegm(&datetime); // Get UTC Epoch
  if (epoch == (time_t)-1) {
    bm_log_error("Failed to convert date time to epoch");
    return false;
  }

  time_output->tv_sec = epoch;
  time_output->tv_nsec = (long)centisec * 10000000L;
  return true;
}
