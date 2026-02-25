#include "platform_linux.h"
#include "bm_config.h"
#include <string.h>
#include <time.h>

// bm_configs_generic.h, bm_rtc.h, bm_dfu_generic.h have no extern "C" guards.
extern "C" {
#include "bm_configs_generic.h"
#include "bm_rtc.h"
#include "bm_dfu_generic.h"
}

int platform_linux_init(void) { return 0; }

// ---------------------------------------------------------------------------
// Config partition — in-memory no-ops (read zeros, writes accepted silently)
// ---------------------------------------------------------------------------

bool bm_config_read(BmConfigPartition partition, uint32_t offset,
                    uint8_t *buffer, size_t length, uint32_t timeout_ms) {
  (void)partition;
  (void)offset;
  (void)timeout_ms;
  if (buffer && length) {
    memset(buffer, 0, length);
  }
  return true;
}

bool bm_config_write(BmConfigPartition partition, uint32_t offset,
                     uint8_t *buffer, size_t length, uint32_t timeout_ms) {
  (void)partition;
  (void)offset;
  (void)buffer;
  (void)length;
  (void)timeout_ms;
  return true;
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

