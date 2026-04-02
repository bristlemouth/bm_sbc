/// @file bm_log.c
/// @brief Structured logging subsystem for bm_sbc.
///
/// Writes OTEL-compatible semi-structured log lines to a per-process file
/// in /var/log/bm_sbc/ (configurable).  Thread-safe via pthread mutex.
/// Supports SIGHUP-based log rotation and optional stdout tee.

#include "bm_log.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static FILE *s_log_fp = NULL;
static char s_log_path[512] = {0};
static char s_prefix[128] = {0}; // "[app_name node=0x<hex16>] "
static BmSbcLogLevel s_min_level = BM_LOG_INFO;
static bool s_also_stdout = false;
static bool s_initialized = false;
static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t s_reopen_flag = 0;

static const char *const k_level_names[] = {"TRACE", "DEBUG", "INFO ",
                                            "WARN ", "ERROR", "FATAL"};

#define BM_LOG_DEFAULT_DIR "/var/log/bm_sbc"
#define BM_LOG_BUF_SIZE 1024

// ---------------------------------------------------------------------------
// SIGHUP handler
// ---------------------------------------------------------------------------

static void sighup_handler(int sig) {
  (void)sig;
  s_reopen_flag = 1;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Format the current UTC time with microsecond precision into buf.
/// Returns the number of chars written (excluding NUL), or 0 on failure.
static int format_timestamp(char *buf, size_t buflen) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm;
  gmtime_r(&ts.tv_sec, &tm);
  int n = (int)strftime(buf, buflen, "%Y-%m-%dT%H:%M:%S", &tm);
  if (n <= 0) {
    return 0;
  }
  // Append .microseconds and Z
  int extra = snprintf(buf + n, buflen - (size_t)n, ".%06ldZ",
                       (long)(ts.tv_nsec / 1000));
  return n + extra;
}

/// Write a fully formatted line to a FILE*, flushing immediately.
static void emit_line(FILE *fp, const char *line, size_t len) {
  fwrite(line, 1, len, fp);
  fflush(fp);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int bm_log_init(const char *app_name, uint64_t node_id, const char *log_dir,
                bool also_stdout) {
  pthread_mutex_lock(&s_log_mutex);

  if (!log_dir) {
    log_dir = BM_LOG_DEFAULT_DIR;
  }

  // Build the structured prefix: "[app_name node=0x<hex16>] "
  snprintf(s_prefix, sizeof(s_prefix), "[%s node=0x%016" PRIx64 "] ",
           app_name ? app_name : "bm_sbc", node_id);

  s_also_stdout = also_stdout;

  // Build log file path.
  snprintf(s_log_path, sizeof(s_log_path), "%s/%s_%016" PRIx64 ".log", log_dir,
           app_name ? app_name : "bm_sbc", node_id);

  // Attempt to create the log directory (may fail if not root).
  (void)mkdir(log_dir, 0755);

  // Open log file for appending.
  s_log_fp = fopen(s_log_path, "a");
  if (!s_log_fp) {
    fprintf(stderr,
            "bm_log: cannot open %s (%s); logging to stdout only\n",
            s_log_path, strerror(errno));
    s_also_stdout = true;
  }

  // Install SIGHUP handler for log rotation.
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sighup_handler;
  sa.sa_flags = SA_RESTART;
  sigaction(SIGHUP, &sa, NULL);

  s_initialized = true;
  pthread_mutex_unlock(&s_log_mutex);

  return s_log_fp ? 0 : 1;
}

void bm_log_set_level(BmSbcLogLevel level) {
  s_min_level = level;
}

BmSbcLogLevel bm_log_get_level(void) {
  return s_min_level;
}

void bm_log(BmSbcLogLevel level, const char *fmt, ...) {
  if (level < s_min_level) {
    return;
  }

  // Pre-init fallback: write unformatted to stderr.
  if (!s_initialized) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    return;
  }

  pthread_mutex_lock(&s_log_mutex);

  // Check SIGHUP reopen flag.
  if (s_reopen_flag) {
    s_reopen_flag = 0;
    if (s_log_fp) {
      fclose(s_log_fp);
      s_log_fp = fopen(s_log_path, "a");
      if (!s_log_fp) {
        fprintf(stderr, "bm_log: reopen failed for %s (%s)\n", s_log_path,
                strerror(errno));
      }
    }
  }

  // Format the user message into a temporary buffer.
  char msgbuf[BM_LOG_BUF_SIZE];
  va_list ap;
  va_start(ap, fmt);
  int msg_len = vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
  va_end(ap);
  if (msg_len < 0) {
    msg_len = 0;
  }
  if ((size_t)msg_len >= sizeof(msgbuf)) {
    msg_len = (int)sizeof(msgbuf) - 1;
  }

  // Strip trailing newline(s) from the user message — we add our own.
  while (msg_len > 0 && msgbuf[msg_len - 1] == '\n') {
    msg_len--;
  }
  msgbuf[msg_len] = '\0';

  // Build the full log line: timestamp + level + prefix + message + newline.
  char line[BM_LOG_BUF_SIZE + 256];
  int ts_len = format_timestamp(line, sizeof(line));
  int level_idx = (level >= BM_LOG_TRACE && level <= BM_LOG_FATAL) ? level : BM_LOG_INFO;
  int total = ts_len + snprintf(line + ts_len, sizeof(line) - (size_t)ts_len,
                                " %s %s%s\n", k_level_names[level_idx],
                                s_prefix, msgbuf);
  if ((size_t)total >= sizeof(line)) {
    total = (int)sizeof(line) - 1;
    line[total - 1] = '\n'; // ensure newline at end
  }

  if (s_log_fp) {
    emit_line(s_log_fp, line, (size_t)total);
  }
  if (s_also_stdout) {
    emit_line(stdout, line, (size_t)total);
  }

  pthread_mutex_unlock(&s_log_mutex);
}

void bm_log_reopen(void) {
  pthread_mutex_lock(&s_log_mutex);
  if (s_log_fp) {
    fclose(s_log_fp);
    s_log_fp = fopen(s_log_path, "a");
    if (!s_log_fp) {
      fprintf(stderr, "bm_log: reopen failed for %s (%s)\n", s_log_path,
              strerror(errno));
    }
  }
  pthread_mutex_unlock(&s_log_mutex);
}

void bm_log_shutdown(void) {
  pthread_mutex_lock(&s_log_mutex);
  if (s_log_fp) {
    fflush(s_log_fp);
    fclose(s_log_fp);
    s_log_fp = NULL;
  }
  s_initialized = false;
  pthread_mutex_unlock(&s_log_mutex);
}
