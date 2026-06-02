#include "platform_linux.h"
#include "bm_config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// bm_configs_generic.h, bm_rtc.h, bm_dfu_generic.h have no extern "C" guards.
extern "C" {
#include "bm_configs_generic.h"
#include "bm_dfu_generic.h"
#include "bm_rtc.h"
}

int platform_linux_init(void) { return 0; }

// ---------------------------------------------------------------------------
// Config partition — file-backed persistence
// ---------------------------------------------------------------------------

static char s_cfg_dir[512] = {0};
static char s_cfg_paths[BM_CFG_PARTITION_COUNT][512];
static bool s_cfg_dir_set = false;

static const char *k_partition_filenames[BM_CFG_PARTITION_COUNT] = {
    "config.user.bin",
    "config.sys.bin",
    "config.hw.bin",
};

void platform_linux_set_cfg_dir(const char *dir) {
  if (!dir) {
    return;
  }
  // The cfg dir must exist before bm_sbc starts.
  // Fail loudly rather than silently disabling persistence on a typo.
  struct stat st;
  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    bm_log_error("platform_linux_set_cfg_dir: %s is not an existing directory "
                 "(%s) — config persistence disabled!",
                 dir, strerror(errno));
    return;
  }
  snprintf(s_cfg_dir, sizeof(s_cfg_dir), "%s", dir);
  for (int i = 0; i < BM_CFG_PARTITION_COUNT; i++) {
    snprintf(s_cfg_paths[i], sizeof(s_cfg_paths[i]), "%s/%s", dir,
             k_partition_filenames[i]);
  }
  s_cfg_dir_set = true;
}

bool bm_config_read(BmConfigPartition partition, uint32_t offset,
                    uint8_t *buffer, size_t length, uint32_t timeout_ms) {
  (void)timeout_ms;
  if (!buffer || !length) {
    return true;
  }
  // No cfg-dir configured — return zeros (fresh partition).
  if (!s_cfg_dir_set || partition >= BM_CFG_PARTITION_COUNT) {
    memset(buffer, 0, length);
    return true;
  }
  FILE *f = fopen(s_cfg_paths[partition], "rb");
  if (!f) {
    // File doesn't exist yet — return zeros so config_init() creates a fresh
    // partition, which will be written back via bm_config_write on first save.
    memset(buffer, 0, length);
    return true;
  }
  if (fseek(f, (long)offset, SEEK_SET) != 0) {
    fclose(f);
    memset(buffer, 0, length);
    return true;
  }
  size_t n = fread(buffer, 1, length, f);
  fclose(f);
  // Zero-fill any remainder (file may be shorter than requested length).
  if (n < length) {
    memset(buffer + n, 0, length - n);
  }
  return true;
}

// Atomic write: stage the new contents in a sibling tempfile,
// fsync, then rename over the target.
// On any failure the target is left untouched.
bool bm_config_write(BmConfigPartition partition, uint32_t offset,
                     uint8_t *buffer, size_t length, uint32_t timeout_ms) {
  (void)timeout_ms;
  if (partition >= BM_CFG_PARTITION_COUNT) {
    return false;
  }
  if (!buffer || !length) {
    return false;
  }
  if (!s_cfg_dir_set) {
    bm_log_error(
        "bm_config_write: no cfg-dir set; unable to persist partition %u",
        (unsigned)partition);
    return false;
  }

  const char *target = s_cfg_paths[partition];

  // Stage final contents in a tempfile we'll rename into place.
  size_t final_size = (size_t)offset + length;
  uint8_t *staging = (uint8_t *)calloc(1, final_size);
  if (!staging) {
    return false;
  }
  FILE *existing = fopen(target, "rb");
  if (existing) {
    // Read up to final_size bytes of existing content so we don't clobber
    // unmodified regions outside [offset, offset+length).
    fread(staging, 1, final_size, existing);
    // If existing file is longer than final_size, fold its tail in too.
    if (fseek(existing, 0, SEEK_END) == 0) {
      long actual = ftell(existing);
      if (actual > (long)final_size) {
        size_t extra = (size_t)actual - final_size;
        uint8_t *grown = (uint8_t *)realloc(staging, (size_t)actual);
        if (!grown) {
          free(staging);
          fclose(existing);
          return false;
        }
        staging = grown;
        if (fseek(existing, (long)final_size, SEEK_SET) == 0) {
          fread(staging + final_size, 1, extra, existing);
        }
        final_size = (size_t)actual;
      }
    }
    fclose(existing);
  }
  memcpy(staging + offset, buffer, length);

  char tmp_path[512 + 16];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", target);
  int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    free(staging);
    return false;
  }
  ssize_t w = write(fd, staging, final_size);
  free(staging);
  if (w != (ssize_t)final_size) {
    close(fd);
    unlink(tmp_path);
    return false;
  }
  if (fsync(fd) != 0) {
    close(fd);
    unlink(tmp_path);
    return false;
  }
  if (close(fd) != 0) {
    unlink(tmp_path);
    return false;
  }
  if (rename(tmp_path, target) != 0) {
    unlink(tmp_path);
    return false;
  }
  // Best-effort directory fsync so the rename is durable across power loss.
  int dfd = open(s_cfg_dir, O_RDONLY | O_DIRECTORY);
  if (dfd >= 0) {
    fsync(dfd);
    close(dfd);
  }
  return true;
}

void bm_config_reset(void) {}

// ---------------------------------------------------------------------------
// RTC — backed by CLOCK_REALTIME
// ---------------------------------------------------------------------------

BmErr bm_rtc_set(const RtcTimeAndDate *time_and_date) {
  struct tm t = {};
  t.tm_year = time_and_date->year - 1900;
  t.tm_mon = time_and_date->month - 1;
  t.tm_mday = time_and_date->day;
  t.tm_hour = time_and_date->hour;
  t.tm_min = time_and_date->minute;
  t.tm_sec = time_and_date->second;

  struct timespec ts;
  ts.tv_sec = timegm(&t);
  ts.tv_nsec = (long)time_and_date->ms * 1000000L;

  if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
    if (errno == EPERM || errno == EACCES) {
      bm_log_error("bm_rtc_set: insufficient privileges to set system clock");
      return BmEPERM;
    }
    return BmEIO;
  }
  return BmOK;
}

BmErr bm_rtc_get(RtcTimeAndDate *time_and_date) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return BmEIO;
  }
  struct tm t;
  gmtime_r(&ts.tv_sec, &t);
  time_and_date->year = (uint16_t)(t.tm_year + 1900);
  time_and_date->month = (uint8_t)(t.tm_mon + 1);
  time_and_date->day = (uint8_t)t.tm_mday;
  time_and_date->hour = (uint8_t)t.tm_hour;
  time_and_date->minute = (uint8_t)t.tm_min;
  time_and_date->second = (uint8_t)t.tm_sec;
  time_and_date->ms = (uint16_t)(ts.tv_nsec / 1000000UL);
  return BmOK;
}

uint64_t bm_rtc_get_micro_seconds(RtcTimeAndDate *time_and_date) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return 0;
  }
  if (time_and_date) {
    struct tm t;
    gmtime_r(&ts.tv_sec, &t);
    time_and_date->year = (uint16_t)(t.tm_year + 1900);
    time_and_date->month = (uint8_t)(t.tm_mon + 1);
    time_and_date->day = (uint8_t)t.tm_mday;
    time_and_date->hour = (uint8_t)t.tm_hour;
    time_and_date->minute = (uint8_t)t.tm_min;
    time_and_date->second = (uint8_t)t.tm_sec;
    time_and_date->ms = (uint16_t)(ts.tv_nsec / 1000000UL);
  }
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000UL);
}

// ---------------------------------------------------------------------------
// DFU — not supported on Linux; all operations are no-ops / permission errors
// ---------------------------------------------------------------------------

BmErr bm_dfu_client_set_confirmed(void) { return BmOK; }
BmErr bm_dfu_client_set_pending_and_reset(void) { return BmOK; }
BmErr bm_dfu_client_fail_update_and_reset(void) { return BmOK; }

BmErr bm_dfu_client_flash_area_open(const void **flash_area) {
  (void)flash_area;
  return BmEPERM;
}
BmErr bm_dfu_client_flash_area_close(const void *flash_area) {
  (void)flash_area;
  return BmOK;
}
BmErr bm_dfu_client_flash_area_write(const void *flash_area, uint32_t off,
                                     const void *src, uint32_t len) {
  (void)flash_area;
  (void)off;
  (void)src;
  (void)len;
  return BmEPERM;
}
BmErr bm_dfu_client_flash_area_erase(const void *flash_area, uint32_t off,
                                     uint32_t len) {
  (void)flash_area;
  (void)off;
  (void)len;
  return BmEPERM;
}
uint32_t bm_dfu_client_flash_area_get_size(const void *flash_area) {
  (void)flash_area;
  return 0;
}
BmErr bm_dfu_host_get_chunk(uint32_t offset, uint8_t *buffer, size_t len,
                            uint32_t timeouts) {
  (void)offset;
  (void)buffer;
  (void)len;
  (void)timeouts;
  return BmEPERM;
}
void bm_dfu_core_lpm_peripheral_active(void) {}
void bm_dfu_core_lpm_peripheral_inactive(void) {}
