#pragma once

/// @file nmea_rmc.h
/// @brief NMEA RMC sentence parser (time/date extraction only).

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Longest legal NMEA sentence, including "$" and the "*hh" checksum.
#define MAX_NMEA_RMC_LEN 82

/// Extract UTC time from an RMC sentence.
///
/// @p raw need not be NUL-terminated and is never written to or read past
/// @p len — it is typically the live pubsub receive buffer.
///
/// @param raw          Sentence bytes, starting at '$'.
/// @param len          Number of valid bytes at @p raw.
/// @param time_output  Populated with the UTC epoch time on success.
/// @return true if the sentence was well-formed and the time was extracted.
bool nmea_rmc_parse(const char *raw, size_t len, struct timespec *time_output);

#ifdef __cplusplus
}
#endif
