#include "critical_op.h"
#include "bm_log.h"
#include "bm_os.h"
#include "bm_service_request.h"
#include "device.h"
#include "util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CRITICAL_SERVICE_PATH_CAP 64
#define TIMER_TIMEOUT_MS 2000
#define SERVICE_REQUEST_TIMEOUT_S 1
#define TIMER_MAX_WAIT_MS 5
#define RETRY_SEND_MAX 3

struct CriticalServiceCtx {
  BmSemaphore mut;
  BmTimer timer;
  SbcCriticalOpCb cb;
  bool critical_status;
  uint8_t retry_count;
};

static struct CriticalServiceCtx ctx = {0};

static bool critical_service_request_cb(bool ack, uint32_t msg_id,
                                        size_t service_strlen,
                                        const char *service, size_t reply_len,
                                        uint8_t *reply_data) {
  (void)msg_id;
  (void)ack;
  (void)reply_data;
  (void)reply_len;

  bm_log_info("%s: received reply on service %.*s", __func__,
              (int)service_strlen, service);

  if (ack) {
    if (ctx.cb) {
      ctx.cb(true);
    }
    bm_timer_stop(ctx.timer, TIMER_MAX_WAIT_MS);
  }

  return true;
}

static void send_request(bool critical) {
  char service[CRITICAL_SERVICE_PATH_CAP] = {0};

  int n = snprintf(service, sizeof(service), "borealis/%016" PRIx64 "/critical",
                   node_id());
  if (n < 0 || (size_t)n >= sizeof(service)) {
    bm_log_error("%s: failed to format critical operation service path",
                 __func__);
    return;
  }

  uint8_t data = !critical;
  if (!bm_service_request(n, service, sizeof(data), &data,
                          critical_service_request_cb,
                          SERVICE_REQUEST_TIMEOUT_S)) {
    bm_log_error("%s: bm_service_request failed", __func__);
  }
}

static void critical_timer_cb(BmTimer timer) {
  (void)timer;
  bm_semaphore_take(ctx.mut, BM_MAX_DELAY_UINT32);
  if (ctx.retry_count >= RETRY_SEND_MAX) {
    if (ctx.cb) {
      ctx.cb(false);
    }
    bm_timer_stop(ctx.timer, 0);
    bm_semaphore_give(ctx.mut);
    return;
  }
  send_request(ctx.critical_status);
  ctx.retry_count++;
  bm_log_error("%s: retrying to send critical op status, count: %u", __func__,
               ctx.retry_count);
  bm_semaphore_give(ctx.mut);
}

/*!
 @brief Sets the device in a critical state

 @details This prevents the device from being powered off. The neighboring
          device must register a service under:
            borealis/{sbc_node_id}/critical
          This function will send a request on the aformentioned service,
          and attempt to send a request on that service every 2 seconds until
          the neighbor replies or a retry limit is reached.

 @param critical whether device is in a critical operation or not
 @param cb callback which is invoked upon reply to request or timeout
 */
void sbc_critical_op(bool critical, SbcCriticalOpCb cb) {
  if (!ctx.mut) {
    ctx.mut = bm_mutex_create();
    if (!ctx.mut) {
      bm_log_error("%s: could not create mutex...", __func__);
      return;
    }
  }

  if (!ctx.timer) {
    ctx.timer = bm_timer_create("critical", TIMER_TIMEOUT_MS, true, NULL,
                                critical_timer_cb);
    if (!ctx.timer) {
      bm_log_error("%s: could not create timer...", __func__);
      return;
    }
  }

  // Only enter critical mode if allowed
  if (ctx.critical_status) {
    if (critical) {
      bm_log_info("%s: already in critical mode...", __func__);
      return;
    }
  } else {
    if (!critical) {
      bm_log_info("%s: already not in critical mode...", __func__);
      return;
    }
  }

  bm_semaphore_take(ctx.mut, BM_MAX_DELAY_UINT32);
  bm_timer_stop(ctx.timer, TIMER_MAX_WAIT_MS);
  ctx.critical_status = critical;
  ctx.retry_count = 0;
  ctx.cb = cb;
  send_request(ctx.critical_status);
  bm_timer_start(ctx.timer, TIMER_MAX_WAIT_MS);
  bm_semaphore_give(ctx.mut);
}
