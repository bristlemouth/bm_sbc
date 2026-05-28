#include "gateway_ipc.h"

#include "bm_log.h"
#include "bm_os.h"
#include "bm_service_request.h"
#include "cbor.h"
#include "messages/config.h"
#include "pubsub.h"
#include "spotter.h"
extern "C" {
#include "device.h"
}

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr size_t IPC_RECV_BUF_BYTES = 4096;
constexpr size_t MAX_TOPIC_LEN = 255;
constexpr size_t MAX_FILE_NAME_LEN = 63;
constexpr size_t MAX_LOG_LINE_LEN = 1024;

// Published for sensor_data messages: "sensor/<node_id hex16>/<topic_suffix>".
// The suffix from the client must not begin with a slash.
constexpr const char *SENSOR_TOPIC_PREFIX_FMT = "sensor/%016" PRIx64 "/";
// "sensor/" + 16 hex chars + "/".
constexpr size_t SENSOR_TOPIC_PREFIX_LEN = 7 + 16 + 1;

// Poweroff service request (bm_service_request API is seconds-granular, so
// 100 ms is not expressible — use the smallest allowed value and cap total
// attempts instead).
constexpr uint32_t POWEROFF_TIMEOUT_S = 1;

int g_ipc_fd = -1;
uint64_t mote_node_id = 0;

static struct {
  BmTaskHandle handle = NULL;
  struct {
    BmQueue handle = NULL;
    const size_t count = 1;
  } queue;
  const char *name = "Power Off";
  const uint32_t stack_size = 248;
  const uint32_t priority = 1;
  const size_t request_retry_max = 30;
  size_t request_retry = 0;
} power_off_task;

bool cbor_get_text(const CborValue *map, const char *key, char *out,
                   size_t out_cap, size_t *out_len) {
  CborValue v;
  if (cbor_value_map_find_value(map, key, &v) != CborNoError) {
    return false;
  }
  if (!cbor_value_is_text_string(&v)) {
    return false;
  }
  size_t cap = out_cap - 1; // leave room for NUL
  if (cbor_value_copy_text_string(&v, out, &cap, nullptr) != CborNoError) {
    return false;
  }
  out[cap] = '\0';
  if (out_len) {
    *out_len = cap;
  }
  return true;
}

bool cbor_get_bool(const CborValue *map, const char *key, bool *out) {
  CborValue v;
  if (cbor_value_map_find_value(map, key, &v) != CborNoError) {
    return false;
  }
  if (!cbor_value_is_boolean(&v)) {
    return false;
  }
  return cbor_value_get_boolean(&v, out) == CborNoError;
}

// Returns a pointer+length into the source buffer for a byte string at key.
// No copy required because the CBOR source buffer is the recvfrom() buffer,
// which stays valid for the lifetime of the handler.
bool cbor_get_bytes_view(const CborValue *map, const char *key,
                         const uint8_t **out_ptr, size_t *out_len) {
  CborValue v;
  if (cbor_value_map_find_value(map, key, &v) != CborNoError) {
    return false;
  }
  if (!cbor_value_is_byte_string(&v)) {
    return false;
  }
  // Walk the chunk iterator to extract the (single) contiguous chunk pointer.
  CborValue it = v;
  if (cbor_value_begin_string_iteration(&it) != CborNoError) {
    return false;
  }
  const uint8_t *chunk = nullptr;
  size_t chunk_len = 0;
  CborValue next;
  if (cbor_value_get_byte_string_chunk(&it, &chunk, &chunk_len, &next) !=
      CborNoError) {
    return false;
  }
  *out_ptr = chunk;
  *out_len = chunk_len;
  return true;
}

bool poweroff_reply_cb(bool ack, uint32_t msg_id, size_t, const char *, size_t,
                       uint8_t *) {
  size_t ack_received = static_cast<size_t>(ack);

  if (ack) {
    bm_log_info("replay_caught_up: poweroff reply received (msg_id=%u), "
                "running systemctl poweroff",
                msg_id);
  } else {
    bm_log_info("replay_caught_up: poweroff reply timeout (msg_id=%u)", msg_id);
  }

  bm_queue_send(power_off_task.queue.handle, &ack_received, 0);

  return true;
}

static inline void cleanup_power_off_task(void) {
  bm_queue_delete(power_off_task.queue.handle);
  bm_task_delete(power_off_task.handle);
  power_off_task.queue.handle = NULL;
  power_off_task.handle = NULL;
}

static void request_power_off(void *arg) {
  constexpr size_t POWEROFF_SERVICE_PATH_CAP = 64;
  char poweroff_service[POWEROFF_SERVICE_PATH_CAP] = {0};
  size_t poweroff_service_len = 0;

  int n = snprintf(poweroff_service, sizeof(poweroff_service),
                   "borealis/%016" PRIx64 "/poweroff", node_id());
  if (n < 0 || static_cast<size_t>(n) >= sizeof(poweroff_service)) {
    bm_log_error("replay_caught_up: failed to format poweroff service path");
    cleanup_power_off_task();
    return;
  }
  poweroff_service_len = static_cast<size_t>(n);

  bm_log_info("replay_caught_up: requesting %s (timeout=%us)", poweroff_service,
              POWEROFF_TIMEOUT_S);

  while (power_off_task.request_retry <= power_off_task.request_retry_max) {
    if (power_off_task.request_retry) {
      bm_log_warn("replay_caught_up: poweroff timeout, retry %zu/%zu",
                  power_off_task.request_retry,
                  power_off_task.request_retry_max);
    }

    if (!bm_service_request(poweroff_service_len, poweroff_service, 0, nullptr,
                            poweroff_reply_cb, POWEROFF_TIMEOUT_S)) {
      bm_log_error("replay_caught_up: bm_service_request failed");
      break;
    }

    size_t acknowledged = 0;
    size_t timeout_ms = (POWEROFF_TIMEOUT_S + POWEROFF_TIMEOUT_S) * 1000;
    if (bm_queue_receive(power_off_task.queue.handle, &acknowledged,
                         timeout_ms) != BmOK) {
      bm_log_error("replay_caught_up: bm_queue_receive failed");
      break;
    }

    if (acknowledged) {
      int rc = system("systemctl poweroff");
      if (rc != 0) {
        bm_log_error("systemctl poweroff returned %d", rc);
      } else {
        break;
      }
    }

    power_off_task.request_retry++;
  }

  if (power_off_task.request_retry > power_off_task.request_retry_max) {

    bm_log_error(
        "replay_caught_up: poweroff request gave up after %zu attempts",
        power_off_task.request_retry_max);
  }

  cleanup_power_off_task();
}

void handle_replay_caught_up(const CborValue *) {
  bm_log_info("IPC RX replay_caught_up");

  // A replay event is already being handled
  if (power_off_task.queue.handle || power_off_task.handle) {
    return;
  }

  // Reset retries and create task to handle sending service request,
  // must offload to a task as retries cannot occur from within the
  // reply callback
  power_off_task.request_retry = 0;
  power_off_task.queue.handle =
      bm_queue_create(power_off_task.queue.count, sizeof(size_t));
  bm_task_create(request_power_off, power_off_task.name,
                 power_off_task.stack_size, NULL, power_off_task.priority,
                 power_off_task.handle);
}

void handle_spotter_log(const CborValue *map) {
  char data[MAX_LOG_LINE_LEN + 1] = {0};
  size_t data_len = 0;
  if (!cbor_get_text(map, "data", data, sizeof(data), &data_len) ||
      data_len == 0) {
    bm_log_warn("IPC spotter_log: missing/empty data");
    return;
  }

  char file_name[MAX_FILE_NAME_LEN + 1] = {0};
  bool have_file_name =
      cbor_get_text(map, "file_name", file_name, sizeof(file_name), nullptr);

  bool print_timestamp = false;
  cbor_get_bool(map, "print_timestamp", &print_timestamp);

  bm_log_info("IPC RX spotter_log data_len=%zu file_name='%s' "
              "print_timestamp=%d",
              data_len, have_file_name ? file_name : "", print_timestamp);

  BmErr err =
      spotter_log(0, have_file_name ? file_name : nullptr,
                  print_timestamp ? USE_TIMESTAMP : NO_TIMESTAMP, "%s", data);
  if (err != BmOK) {
    bm_log_warn("IPC spotter_log: spotter_log failed, err=%d", err);
  }
}

void handle_spotter_tx(const CborValue *map) {
  const uint8_t *data = nullptr;
  size_t data_len = 0;
  if (!cbor_get_bytes_view(map, "data", &data, &data_len) || data_len == 0) {
    bm_log_warn("IPC spotter_tx: missing/empty data");
    return;
  }

  bool iridium_fallback = false;
  cbor_get_bool(map, "iridium_fallback", &iridium_fallback);
  BmSerialNetworkType net_type = iridium_fallback
                                     ? BmNetworkTypeCellularIriFallback
                                     : BmNetworkTypeCellularOnly;

  bm_log_info("IPC RX spotter_tx data_len=%zu iridium_fallback=%d", data_len,
              iridium_fallback);

  BmErr err = spotter_tx_data(data, static_cast<uint16_t>(data_len), net_type);
  if (err != BmOK) {
    bm_log_warn("IPC spotter_tx: spotter_tx_data failed, err=%d", err);
  }
}

void handle_sensor_data(const CborValue *map) {
  char topic_suffix[MAX_TOPIC_LEN + 1] = {0};
  size_t suffix_len = 0;
  if (!cbor_get_text(map, "topic_suffix", topic_suffix, sizeof(topic_suffix),
                     &suffix_len) ||
      suffix_len == 0) {
    bm_log_warn("IPC sensor_data: missing/empty topic_suffix");
    return;
  }
  if (topic_suffix[0] == '/') {
    bm_log_warn("IPC sensor_data: topic_suffix must not begin with '/'");
    return;
  }

  if (SENSOR_TOPIC_PREFIX_LEN + suffix_len > MAX_TOPIC_LEN) {
    bm_log_warn("IPC sensor_data: topic too long (%zu)",
                SENSOR_TOPIC_PREFIX_LEN + suffix_len);
    return;
  }
  char topic[MAX_TOPIC_LEN + 1] = {0};
  int n = snprintf(topic, sizeof(topic), SENSOR_TOPIC_PREFIX_FMT, node_id());
  if (n < 0 || static_cast<size_t>(n) != SENSOR_TOPIC_PREFIX_LEN) {
    bm_log_warn("IPC sensor_data: failed to format topic prefix");
    return;
  }
  memcpy(topic + SENSOR_TOPIC_PREFIX_LEN, topic_suffix, suffix_len);
  topic[SENSOR_TOPIC_PREFIX_LEN + suffix_len] = '\0';

  const uint8_t *data = nullptr;
  size_t data_len = 0;
  if (!cbor_get_bytes_view(map, "data", &data, &data_len)) {
    bm_log_warn("IPC sensor_data: missing data");
    return;
  }

  bm_log_info("IPC RX sensor_data topic='%s' data_len=%zu", topic, data_len);

  BmErr err = bm_pub(topic, data, static_cast<uint16_t>(data_len), 0,
                     BM_COMMON_PUB_SUB_VERSION);
  if (err != BmOK) {
    bm_log_warn("IPC sensor_data: bm_pub(%s) failed, err=%d", topic, err);
  }
}

bool apply_config_string(const char *key, size_t key_len, const CborValue *v) {
  char value[MAX_STR_LEN_BYTES + 1] = {0};
  size_t cap = sizeof(value) - 1;
  if (cbor_value_copy_text_string(v, value, &cap, nullptr) != CborNoError) {
    bm_log_warn("IPC config_set: failed to read string value");
    return false;
  }
  value[cap] = '\0';
  bm_log_info("IPC config_set key='%s' value(str)='%s'", key, value);
  return set_config_string(BM_CFG_PARTITION_SYSTEM, key, key_len, value, cap);
}

bool apply_config_uint(const char *key, size_t key_len, const CborValue *v) {
  uint64_t u = 0;
  cbor_value_get_uint64(v, &u);
  if (u > UINT32_MAX) {
    bm_log_warn("IPC config_set: uint value %" PRIu64 " out of range", u);
    return false;
  }
  bm_log_info("IPC config_set key='%s' value(uint)=%" PRIu64, key, u);
  return set_config_uint(BM_CFG_PARTITION_SYSTEM, key, key_len,
                         static_cast<uint32_t>(u));
}

bool apply_config_int(const char *key, size_t key_len, const CborValue *v) {
  int64_t i = 0;
  cbor_value_get_int64(v, &i);
  if (i < INT32_MIN || i > INT32_MAX) {
    bm_log_warn("IPC config_set: int value %" PRId64 " out of range", i);
    return false;
  }
  bm_log_info("IPC config_set key='%s' value(int)=%" PRId64, key, i);
  return set_config_int(BM_CFG_PARTITION_SYSTEM, key, key_len,
                        static_cast<int32_t>(i));
}

bool apply_config_float(const char *key, size_t key_len, const CborValue *v) {
  double d = 0.0;
  if (cbor_value_is_double(v)) {
    cbor_value_get_double(v, &d);
  } else {
    float f = 0.0f;
    cbor_value_get_float(v, &f);
    d = f;
  }
  bm_log_info("IPC config_set key='%s' value(float)=%f", key, d);
  return set_config_float(BM_CFG_PARTITION_SYSTEM, key, key_len,
                          static_cast<float>(d));
}

void handle_config_set(const CborValue *map) {
  bm_log_info("IPC RX config_set");

  char key[MAX_KEY_LEN_BYTES + 1] = {0};
  size_t key_len = 0;
  if (!cbor_get_text(map, "config_key", key, sizeof(key), &key_len) ||
      key_len == 0) {
    bm_log_warn("IPC config_set: missing/empty config_key");
    return;
  }

  CborValue v;
  if (cbor_value_map_find_value(map, "config_value", &v) != CborNoError ||
      cbor_value_get_type(&v) == CborInvalidType) {
    bm_log_warn("IPC config_set: missing config_value");
    return;
  }

  bool ok;
  if (cbor_value_is_text_string(&v)) {
    ok = apply_config_string(key, key_len, &v);
  } else if (cbor_value_is_unsigned_integer(&v)) {
    ok = apply_config_uint(key, key_len, &v);
  } else if (cbor_value_is_integer(&v)) {
    ok = apply_config_int(key, key_len, &v);
  } else if (cbor_value_is_double(&v) || cbor_value_is_float(&v)) {
    ok = apply_config_float(key, key_len, &v);
  } else {
    bm_log_warn("IPC config_set: unsupported config_value CBOR type");
    return;
  }

  if (!ok) {
    bm_log_warn("IPC config_set: write failed for key='%s'", key);
  }
}

void dispatch(const uint8_t *buf, size_t len) {
  CborParser parser;
  CborValue root;
  if (cbor_parser_init(buf, len, 0, &parser, &root) != CborNoError ||
      !cbor_value_is_map(&root)) {
    bm_log_warn("IPC: malformed datagram (%zu bytes)", len);
    return;
  }

  CborValue v_field;
  if (cbor_value_map_find_value(&root, "v", &v_field) != CborNoError ||
      !cbor_value_is_integer(&v_field)) {
    bm_log_warn("IPC: missing schema version");
    return;
  }
  int schema_version = 0;
  cbor_value_get_int(&v_field, &schema_version);
  if (schema_version != 1) {
    bm_log_warn("IPC: unsupported schema version %d", schema_version);
    return;
  }

  char type[32] = {0};
  size_t type_len = 0;
  if (!cbor_get_text(&root, "type", type, sizeof(type), &type_len) ||
      type_len == 0) {
    bm_log_warn("IPC: missing type field");
    return;
  }

  if (strcmp(type, "replay_caught_up") == 0) {
    handle_replay_caught_up(&root);
  } else if (strcmp(type, "spotter_log") == 0) {
    handle_spotter_log(&root);
  } else if (strcmp(type, "spotter_tx") == 0) {
    handle_spotter_tx(&root);
  } else if (strcmp(type, "sensor_data") == 0) {
    handle_sensor_data(&root);
  } else if (strcmp(type, "config_set") == 0) {
    handle_config_set(&root);
  } else {
    bm_log_warn("IPC: unknown type '%s'", type);
  }
}

} // namespace

int gateway_ipc_init(uint64_t mote_node_id_arg) {
  mote_node_id = mote_node_id_arg;

  if (g_ipc_fd >= 0) {
    return 0;
  }

  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    bm_log_error("IPC: socket() failed: %s", strerror(errno));
    return -1;
  }

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    bm_log_error("IPC: fcntl(O_NONBLOCK) failed: %s", strerror(errno));
    close(fd);
    return -1;
  }
  int fdflags = fcntl(fd, F_GETFD, 0);
  if (fdflags >= 0) {
    fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
  }

  // $BM_SBC_GATEWAY_IPC overrides the default path (used by tests).
  const char *env_path = getenv("BM_SBC_GATEWAY_IPC");
  const char *path =
      (env_path && env_path[0]) ? env_path : GATEWAY_IPC_SOCKET_PATH;

  // Remove any stale socket from a prior run.
  unlink(path);

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    bm_log_error("IPC: bind(%s) failed: %s", path, strerror(errno));
    close(fd);
    return -1;
  }

  // World-writable: clients run as arbitrary users (Hydrotwin, tools).
  // Access control is expected at the filesystem/directory level
  // (/run/bm_sbc owned by a shared group).
  if (chmod(path, 0666) < 0) {
    bm_log_warn("IPC: chmod(%s) failed: %s", path, strerror(errno));
  }

  g_ipc_fd = fd;
  bm_log_info("IPC: listening on %s", path);
  return 0;
}

void gateway_ipc_poll(void) {
  if (g_ipc_fd < 0) {
    return;
  }

  uint8_t buf[IPC_RECV_BUF_BYTES];
  for (;;) {
    ssize_t n = recvfrom(g_ipc_fd, buf, sizeof(buf), 0, nullptr, nullptr);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      bm_log_warn("IPC: recvfrom failed: %s", strerror(errno));
      return;
    }
    if (n == 0) {
      continue;
    }
    dispatch(buf, static_cast<size_t>(n));
  }
}
