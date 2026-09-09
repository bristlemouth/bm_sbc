#include "hostname_service.h"
#include "bm_log.h"
#include "bm_service.h"
#include "device.h"
#include <limits.h>
#include <string.h>
#include <unistd.h>

#define SBC_STR "sbc"
#define HOSTNAME_STR "hostname"
#define HOSTNAME_SERVICE_STR SBC_STR "/XXXXXXXXXXXXXXXX/" HOSTNAME_STR
#define HOSTNAME_MAX_LEN 255

static bool hostname_service_request_cb(size_t service_strlen,
                                        const char *service, size_t req_len,
                                        uint8_t *req_data, size_t *reply_len,
                                        uint8_t *reply_data) {
  (void)req_len;
  (void)req_data;

  bm_log_info("Received hostname service request on topic: %.*s",
              (int)service_strlen, service);

  char *name = (char *)reply_data;
  if (gethostname(name, HOSTNAME_MAX_LEN) == 0) {
    bm_log_info("Hostname is %s", name);
  } else {
    bm_log_error("Failed to get hostname!");
  }

  *reply_len = strlen(name);

  return true;
}

BmErr hostname_service_register(void) {
  static char hostname_service_topic[sizeof(HOSTNAME_SERVICE_STR)] = "";

  snprintf(hostname_service_topic, sizeof(hostname_service_topic),
           SBC_STR "/%016" PRIx64 "/" HOSTNAME_STR, node_id());
  size_t hostname_service_topic_len = strlen(hostname_service_topic);

  bool registered =
      bm_service_register(hostname_service_topic_len, hostname_service_topic,
                          hostname_service_request_cb);

  if (!registered) {
    return BmECANCELED;
  }

  return BmOK;
}
