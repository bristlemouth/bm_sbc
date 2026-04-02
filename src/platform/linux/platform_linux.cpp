#include "platform_linux.h"
#include "bm_config.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// bm_configs_generic.h, bm_rtc.h, bm_dfu_generic.h have no extern "C" guards.
extern "C" {
#include "bm_configs_generic.h"
#include "bm_rtc.h"
#include "bm_dfu_generic.h"
}

int platform_linux_init(void) { return 0; }

// ---------------------------------------------------------------------------
// Config partition — file-backed persistence
// ---------------------------------------------------------------------------

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
  // Create the directory if it doesn't exist (one level only).
  mkdir(dir, 0755);
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

bool bm_config_write(BmConfigPartition partition, uint32_t offset,
                     uint8_t *buffer, size_t length, uint32_t timeout_ms) {
  (void)timeout_ms;
  if (!s_cfg_dir_set || partition >= BM_CFG_PARTITION_COUNT) {
    return true;
  }
  if (!buffer || !length) {
    return true;
  }
  FILE *f = fopen(s_cfg_paths[partition], "r+b");
  if (!f) {
    // File doesn't exist — create it.
    f = fopen(s_cfg_paths[partition], "wb");
  }
  if (!f) {
    return false;
  }
  if (fseek(f, (long)offset, SEEK_SET) != 0) {
    fclose(f);
    return false;
  }
  size_t n = fwrite(buffer, 1, length, f);
  fflush(f);
  fclose(f);
  return n == length;
}

void bm_config_reset(void) {}

// ---------------------------------------------------------------------------
// RTC — backed by CLOCK_REALTIME
// ---------------------------------------------------------------------------

BmErr bm_rtc_set(const RtcTimeAndDate *time_and_date) {
  // Updating the system clock requires elevated privileges; accept the call
  // and return success so the protocol stack does not stall.
  (void)time_and_date;
  return BmOK;
}

BmErr bm_rtc_get(RtcTimeAndDate *time_and_date) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return BmEIO;
  }
  struct tm t;
  gmtime_r(&ts.tv_sec, &t);
  time_and_date->year   = (uint16_t)(t.tm_year + 1900);
  time_and_date->month  = (uint8_t)(t.tm_mon + 1);
  time_and_date->day    = (uint8_t)t.tm_mday;
  time_and_date->hour   = (uint8_t)t.tm_hour;
  time_and_date->minute = (uint8_t)t.tm_min;
  time_and_date->second = (uint8_t)t.tm_sec;
  time_and_date->ms     = (uint16_t)(ts.tv_nsec / 1000000UL);
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
    time_and_date->year   = (uint16_t)(t.tm_year + 1900);
    time_and_date->month  = (uint8_t)(t.tm_mon + 1);
    time_and_date->day    = (uint8_t)t.tm_mday;
    time_and_date->hour   = (uint8_t)t.tm_hour;
    time_and_date->minute = (uint8_t)t.tm_min;
    time_and_date->second = (uint8_t)t.tm_sec;
    time_and_date->ms     = (uint16_t)(ts.tv_nsec / 1000000UL);
  }
  return (uint64_t)ts.tv_sec * 1000000ULL +
         (uint64_t)(ts.tv_nsec / 1000UL);
}

// ---------------------------------------------------------------------------
// DFU — not supported on Linux; all operations are no-ops / permission errors
// ---------------------------------------------------------------------------

BmErr bm_dfu_client_set_confirmed(void)          { return BmOK; }
BmErr bm_dfu_client_set_pending_and_reset(void)  { return BmOK; }
BmErr bm_dfu_client_fail_update_and_reset(void)  { return BmOK; }

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
  (void)flash_area; (void)off; (void)src; (void)len;
  return BmEPERM;
}
BmErr bm_dfu_client_flash_area_erase(const void *flash_area, uint32_t off,
                                     uint32_t len) {
  (void)flash_area; (void)off; (void)len;
  return BmEPERM;
}
uint32_t bm_dfu_client_flash_area_get_size(const void *flash_area) {
  (void)flash_area;
  return 0;
}
BmErr bm_dfu_host_get_chunk(uint32_t offset, uint8_t *buffer, size_t len,
                            uint32_t timeouts) {
  (void)offset; (void)buffer; (void)len; (void)timeouts;
  return BmEPERM;
}
void bm_dfu_core_lpm_peripheral_active(void)   {}
void bm_dfu_core_lpm_peripheral_inactive(void) {}

