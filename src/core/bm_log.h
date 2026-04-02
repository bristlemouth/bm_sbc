#ifndef __BM_LOG_H__
#define __BM_LOG_H__

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  BM_LOG_TRACE = 0,
  BM_LOG_DEBUG = 1,
  BM_LOG_INFO = 2,
  BM_LOG_WARN = 3,
  BM_LOG_ERROR = 4,
  BM_LOG_FATAL = 5,
} BmSbcLogLevel;

/// Initialize the logging subsystem.
///
/// Opens (or creates) a log file at <log_dir>/<app_name>_<node_id hex16>.log.
/// If the directory cannot be created or the file cannot be opened, logging
/// falls back to stdout-only and a warning is printed to stderr.
///
/// @param app_name  Short app identifier (e.g. "multinode").
/// @param node_id   64-bit Bristlemouth node ID for this process.
/// @param log_dir   Directory for log files (e.g. "/var/log/bm_sbc").
///                  NULL uses the default "/var/log/bm_sbc".
/// @param also_stdout  If true, tee all log output to stdout as well.
/// @return 0 on success, non-zero if the log file could not be opened
///         (logging still works via stdout fallback).
int bm_log_init(const char *app_name, uint64_t node_id, const char *log_dir,
                bool also_stdout);

/// Set the minimum severity level.  Messages below this level are discarded.
void bm_log_set_level(BmSbcLogLevel level);

/// Get the current minimum severity level.
BmSbcLogLevel bm_log_get_level(void);

/// Emit a log line.  Thread-safe.
///
/// If called before bm_log_init(), falls back to fprintf(stderr, ...) with
/// no structured prefix.
void bm_log(BmSbcLogLevel level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/// Close and reopen the log file (for log rotation).
/// Typically called from a SIGHUP handler via the reopen flag, but can
/// also be called directly.
void bm_log_reopen(void);

/// Flush and close the log file.
void bm_log_shutdown(void);

// ---------------------------------------------------------------------------
// Convenience macros
// ---------------------------------------------------------------------------

#ifndef BM_LOG_MIN_LEVEL
#define BM_LOG_MIN_LEVEL BM_LOG_TRACE
#endif

#define BM_LOG_(level, fmt, ...)                                               \
  do {                                                                         \
    if ((level) >= BM_LOG_MIN_LEVEL) {                                         \
      bm_log((level), fmt, ##__VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

#define bm_log_trace(fmt, ...) BM_LOG_(BM_LOG_TRACE, fmt, ##__VA_ARGS__)
#define bm_log_debug(fmt, ...) BM_LOG_(BM_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define bm_log_info(fmt, ...) BM_LOG_(BM_LOG_INFO, fmt, ##__VA_ARGS__)
#define bm_log_warn(fmt, ...) BM_LOG_(BM_LOG_WARN, fmt, ##__VA_ARGS__)
#define bm_log_error(fmt, ...) BM_LOG_(BM_LOG_ERROR, fmt, ##__VA_ARGS__)
#define bm_log_fatal(fmt, ...) BM_LOG_(BM_LOG_FATAL, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // __BM_LOG_H__
