#include "bm_log.h"
extern "C" {
  #include "bm_common_pub_sub.h"
  #include "bm_os.h"
  #include "config_cbor_map_service.h"
  #include "config_cbor_map_srv_reply_msg.h"
  #include "pubsub.h"
  #include "sys_info_service.h"
  #include "sys_info_svc_reply_msg.h"
  #include "messages/config.h"
  #include "messages/neighbors.h"
}
#include "cbor.h"
#include "gateway_device.h"
#include "gateway_ipc.h"
#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SBC_COMMAND_KEY "sbc_command"
#define SBC_COMMAND_KEY_LEN (sizeof(SBC_COMMAND_KEY) - 1)

#define WIFI_ENABLED_KEY "wifi_enabled"
#define WIFI_ENABLED_KEY_LEN (sizeof(WIFI_ENABLED_KEY) - 1)

#define INIT_LOG_PATH "/var/run/bristlemouth_init_log.txt"
#define INIT_LOG_TMP_PATH INIT_LOG_PATH ".tmp"
#define CONFIG_MAP_REQUEST_TIMEOUT_S 2

static struct sockaddr_in GPS_DEST = {
    .sin_family = AF_INET,
    .sin_port = htons(5000),
    .sin_addr = {.s_addr = inet_addr("127.0.0.1")}};

static struct {
  int gps_udp_socket_fd = -1;
  uint64_t mote_node_id = 0;
  bool mote_neighbor_found = false;
  std::atomic<bool> mote_app_name_received = false;
  char mote_app_name[MAX_STR_LEN_BYTES + 1] = "borealis2";
  std::atomic<bool> sbc_command_received = false;
  char sbc_command[MAX_STR_LEN_BYTES] = "";
  std::atomic<bool> config_map_received = false;
  std::atomic<bool> wifi_command_received = false;
  std::atomic<bool> system_time_synced = false;
  time_t last_rmc_time = 0;
  uint32_t wifi_enabled = 1;
} CONTEXT;

static void neighbor_cb(BcmpNeighbor *neighbor) {
  bcmp_print_neighbor_info(neighbor);
  if (neighbor->port == GATEWAY_UART_PORT) {
    CONTEXT.mote_node_id = neighbor->node_id;
    CONTEXT.mote_neighbor_found = true;
  }
}

static void await_uart_neighbor(void) {
  uint8_t num_neighbors = 0;
  while (!CONTEXT.mote_neighbor_found) {
    BcmpNeighbor *neighbor = bcmp_get_neighbors(&num_neighbors);
    if (num_neighbors == 0) {
      bm_log_warn("No neighbors, will delay and retry");
      bm_delay(1000);
    } else {
      bm_log_info("Found %u neighbor(s)", num_neighbors);
      bcmp_neighbor_foreach(neighbor_cb);
      if (!CONTEXT.mote_neighbor_found) {
        bm_log_warn("None of the neighbors are the mote, will delay and retry");
        bm_delay(1000);
      }
    }
  }
}

/**************** SBC command ****************/

static BmErr sbc_command_reply_cb(uint8_t *payload) {
  BmErr err = BmENODATA;
  if (payload) {
    BmConfigValue *msg = reinterpret_cast<BmConfigValue *>(payload);
    static char sbc_command[1024];
    size_t sbc_command_len = sizeof(sbc_command);
    memset(sbc_command, 0, sbc_command_len);
    err = bcmp_config_decode_value(STR, msg->data, msg->data_length,
                                   sbc_command, &sbc_command_len);
    if (err == BmOK) {
      if (sbc_command_len > 0) {
        bm_log_info("Received sbc command: %.*s", (int)sbc_command_len,
                    sbc_command);
        CONTEXT.sbc_command_received = true;
        memcpy(CONTEXT.sbc_command, sbc_command, sbc_command_len);
      }
    } else {
      bm_log_error("Failed to decode sbc command bcmp value, err=%d", err);
    }
  }

  return err;
}

static void send_sbc_command_request(void) {
  BmErr err = BmOK;
  bool sent = bcmp_config_get(CONTEXT.mote_node_id, BM_CFG_PARTITION_SYSTEM,
                              SBC_COMMAND_KEY_LEN, SBC_COMMAND_KEY, &err,
                              sbc_command_reply_cb);
  if (!sent) {
    bm_log_warn("Failed to send bcmp config get for sbc_command, err=%d", err);
  }
}

static void wait_for_config_reply(std::atomic<bool> *received) {
  uint32_t total_awaited_ms = 0;
  const uint32_t timeout_ms = 500;
  while (!received->load() && total_awaited_ms < timeout_ms) {
    const uint32_t delay_poll_ms = 20;
    bm_delay(delay_poll_ms);
    total_awaited_ms += delay_poll_ms;
  }
}

static void get_sbc_command(void) {
  int8_t retries_remaining = 3;
  while (!CONTEXT.sbc_command_received && retries_remaining > 0) {
    send_sbc_command_request();
    wait_for_config_reply(&CONTEXT.sbc_command_received);
    retries_remaining--;
  }
}

/**************** CBOR config map ****************/

static bool write_cbor_value(FILE *fp, CborValue *value) {
  switch (cbor_value_get_type(value)) {
  case CborIntegerType: {
    if (cbor_value_is_unsigned_integer(value)) {
      uint64_t u = 0;
      if (cbor_value_get_uint64(value, &u) != CborNoError) {
        return false;
      }
      fprintf(fp, "%llu", (unsigned long long)u);
    } else {
      int64_t i = 0;
      if (cbor_value_get_int64(value, &i) != CborNoError) {
        return false;
      }
      fprintf(fp, "%lld", (long long)i);
    }
    return true;
  }
  case CborFloatType: {
    float f = 0.0f;
    if (cbor_value_get_float(value, &f) != CborNoError) {
      return false;
    }
    if (f == (float)(long long)f) {
      fprintf(fp, "%.1f", f);
    } else {
      fprintf(fp, "%g", f);
    }
    return true;
  }
  case CborDoubleType: {
    double d = 0.0;
    if (cbor_value_get_double(value, &d) != CborNoError) {
      return false;
    }
    if (d == (double)(long long)d) {
      fprintf(fp, "%.1f", d);
    } else {
      fprintf(fp, "%g", d);
    }
    return true;
  }
  case CborTextStringType: {
    char buf[MAX_STR_LEN_BYTES + 1];
    size_t len = sizeof(buf) - 1;
    if (cbor_value_copy_text_string(value, buf, &len, NULL) != CborNoError) {
      return false;
    }
    buf[len] = '\0';
    fputs(buf, fp);
    return true;
  }
  case CborByteStringType: {
    uint8_t buf[MAX_STR_LEN_BYTES];
    size_t len = sizeof(buf);
    if (cbor_value_copy_byte_string(value, buf, &len, NULL) != CborNoError) {
      return false;
    }
    for (size_t i = 0; i < len; i++) {
      fprintf(fp, "%02x", buf[i]);
    }
    return true;
  }
  default:
    return false;
  }
}

static bool write_init_log_file(const uint8_t *cbor_data, size_t cbor_len) {
  CborParser parser;
  CborValue map;
  if (cbor_parser_init(cbor_data, cbor_len, 0, &parser, &map) != CborNoError ||
      !cbor_value_is_map(&map)) {
    bm_log_error("Config map reply is not a CBOR map");
    return false;
  }

  FILE *fp = fopen(INIT_LOG_TMP_PATH, "w");
  if (!fp) {
    bm_log_error("Failed to open %s for writing", INIT_LOG_TMP_PATH);
    return false;
  }

  bool ok = true;
  fprintf(fp, "app: %s\r\n", CONTEXT.mote_app_name);

  CborValue it;
  if (cbor_value_enter_container(&map, &it) != CborNoError) {
    ok = false;
  }

  while (ok && !cbor_value_at_end(&it)) {
    if (!cbor_value_is_text_string(&it)) {
      bm_log_warn("Config map key is not a text string, skipping entry");
      ok = false;
      break;
    }
    char key[MAX_KEY_LEN_BYTES + 1];
    size_t key_len = sizeof(key) - 1;
    if (cbor_value_copy_text_string(&it, key, &key_len, NULL) != CborNoError) {
      ok = false;
      break;
    }
    key[key_len] = '\0';
    if (cbor_value_advance(&it) != CborNoError) {
      ok = false;
      break;
    }

    fprintf(fp, "%s: ", key);
    if (!write_cbor_value(fp, &it)) {
      bm_log_warn("Skipping config key '%s' with unsupported value type", key);
      // Best effort: emit the line anyway so the key is at least visible.
      fputs("(unsupported)", fp);
    }
    fputs("\r\n", fp);

    if (cbor_value_advance(&it) != CborNoError) {
      ok = false;
      break;
    }
  }

  if (fflush(fp) != 0 || fclose(fp) != 0) {
    ok = false;
  }

  if (!ok) {
    unlink(INIT_LOG_TMP_PATH);
    return false;
  }

  if (rename(INIT_LOG_TMP_PATH, INIT_LOG_PATH) != 0) {
    bm_log_error("Failed to rename %s to %s", INIT_LOG_TMP_PATH, INIT_LOG_PATH);
    unlink(INIT_LOG_TMP_PATH);
    return false;
  }

  bm_log_info("Wrote mote system configs to %s", INIT_LOG_PATH);
  return true;
}

static bool config_map_reply_cb(bool ack, uint32_t msg_id,
                                size_t service_strlen, const char *service,
                                size_t reply_len, uint8_t *reply_data) {
  (void)msg_id;
  (void)service_strlen;
  (void)service;

  if (!ack || !reply_data) {
    bm_log_warn("Config map request not acknowledged");
    return false;
  }

  ConfigCborMapReplyData reply = {0, 0, false, 0, NULL};
  CborError err = config_cbor_map_reply_decode(&reply, reply_data, reply_len);
  if (err != CborNoError) {
    bm_log_error("Failed to decode config map reply, err=%d", err);
    if (reply.cbor_data) {
      bm_free(reply.cbor_data);
    }
    return false;
  }

  if (!reply.success || !reply.cbor_data || reply.cbor_encoded_map_len == 0) {
    bm_log_warn("Config map reply unsuccessful or empty");
    if (reply.cbor_data) {
      bm_free(reply.cbor_data);
    }
    return false;
  }

  bool wrote = write_init_log_file(reply.cbor_data, reply.cbor_encoded_map_len);
  bm_free(reply.cbor_data);

  if (wrote) {
    CONTEXT.config_map_received = true;
  }
  return wrote;
}

static void send_config_map_request(void) {
  bool sent = config_cbor_map_service_request(
      CONTEXT.mote_node_id, CONFIG_CBOR_MAP_PARTITION_ID_SYS,
      config_map_reply_cb, CONFIG_MAP_REQUEST_TIMEOUT_S);
  if (!sent) {
    bm_log_warn("Failed to send config map request");
  }
}

static void wait_for_config_map_reply(void) {
  uint32_t total_awaited_ms = 0;
  const uint32_t extra_padding_ms = 500;
  const uint32_t timeout_ms =
      CONFIG_MAP_REQUEST_TIMEOUT_S * 1000 + extra_padding_ms;
  while (!CONTEXT.config_map_received && total_awaited_ms < timeout_ms) {
    const uint32_t delay_poll_ms = 20;
    bm_delay(delay_poll_ms);
    total_awaited_ms += delay_poll_ms;
  }
}

static void get_mote_system_configs(void) {
  int8_t retries_remaining = 3;
  while (!CONTEXT.config_map_received && retries_remaining > 0) {
    send_config_map_request();
    wait_for_config_map_reply();
    retries_remaining--;
  }
  if (!CONTEXT.config_map_received) {
    bm_log_warn("Could not retrieve mote system configs; "
                "leaving %s untouched",
                INIT_LOG_PATH);
  }
}

/**************** mote app name ****************/

static bool sys_info_reply_cb(bool ack, uint32_t msg_id, size_t service_strlen,
                              const char *service, size_t reply_len,
                              uint8_t *reply_data) {
  (void)msg_id;
  (void)service_strlen;
  (void)service;

  if (!ack || !reply_data) {
    bm_log_warn("Sys info request not acknowledged");
    return false;
  }

  SysInfoReplyData reply = {0, 0, 0, 0, NULL};
  CborError err = sys_info_reply_decode(&reply, reply_data, reply_len);
  if (err != CborNoError) {
    bm_log_error("Failed to decode sys info reply, err=%d", err);
    if (reply.app_name) {
      bm_free(reply.app_name);
    }
    return false;
  }

  if (reply.app_name && reply.app_name_strlen > 0 &&
      reply.app_name_strlen <= MAX_STR_LEN_BYTES) {
    memcpy(CONTEXT.mote_app_name, reply.app_name, reply.app_name_strlen);
    CONTEXT.mote_app_name[reply.app_name_strlen] = '\0';
    CONTEXT.mote_app_name_received = true;
    bm_log_info("Mote app name: %s", CONTEXT.mote_app_name);
  } else {
    bm_log_warn("Sys info reply missing or oversized app_name");
  }

  if (reply.app_name) {
    bm_free(reply.app_name);
  }
  return CONTEXT.mote_app_name_received;
}

static void send_sys_info_request(void) {
  bool sent = sys_info_service_request(CONTEXT.mote_node_id, sys_info_reply_cb,
                                       CONFIG_MAP_REQUEST_TIMEOUT_S);
  if (!sent) {
    bm_log_warn("Failed to send sys info request");
  }
}

static void wait_for_sys_info_reply(void) {
  uint32_t total_awaited_ms = 0;
  const uint32_t extra_padding_ms = 500;
  const uint32_t timeout_ms =
      CONFIG_MAP_REQUEST_TIMEOUT_S * 1000 + extra_padding_ms;
  while (!CONTEXT.mote_app_name_received && total_awaited_ms < timeout_ms) {
    const uint32_t delay_poll_ms = 20;
    bm_delay(delay_poll_ms);
    total_awaited_ms += delay_poll_ms;
  }
}

static void get_mote_app_name(void) {
  int8_t retries_remaining = 3;
  while (!CONTEXT.mote_app_name_received && retries_remaining > 0) {
    send_sys_info_request();
    wait_for_sys_info_reply();
    retries_remaining--;
  }
  if (!CONTEXT.mote_app_name_received) {
    bm_log_warn("Could not retrieve mote app name; defaulting to '%s'",
                CONTEXT.mote_app_name);
  }
}

static BmErr wifi_enabled_reply_cb(uint8_t *payload) {
  bm_log_debug("Ticks in " WIFI_ENABLED_KEY " reply cb: %u",
               bm_get_tick_count());

  BmErr err = BmENODATA;
  if (payload) {
    BmConfigValue *msg = reinterpret_cast<BmConfigValue *>(payload);
    size_t wifi_enabled_len = sizeof(CONTEXT.wifi_enabled);
    err = bcmp_config_decode_value(UINT32, msg->data, msg->data_length,
                                   &CONTEXT.wifi_enabled, &wifi_enabled_len);
    if (err == BmOK) {
      if (wifi_enabled_len > 0) {
        bm_log_info("Received " WIFI_ENABLED_KEY ": %u",
                    (uint)CONTEXT.wifi_enabled);
        CONTEXT.wifi_command_received = true;
      }
    } else {
      bm_log_error("Failed to decode " WIFI_ENABLED_KEY
                   " bcmp value, enabling Wi-Fi, err=%d",
                   err);
    }
  }

  return err;
}

static void send_wifi_enable_request(void) {
  bm_log_debug("Ticks before bcmp config get: %u", bm_get_tick_count());
  BmErr err = BmOK;
  bool sent = bcmp_config_get(CONTEXT.mote_node_id, BM_CFG_PARTITION_SYSTEM,
                              WIFI_ENABLED_KEY_LEN, WIFI_ENABLED_KEY, &err,
                              wifi_enabled_reply_cb);
  bm_log_debug("Ticks after bcmp config get: %u", bm_get_tick_count());
  if (!sent) {
    bm_log_warn(
        "Failed to send bcmp config get for " WIFI_ENABLED_KEY ", err=%d", err);
  }
}

static void get_wifi_enable(void) {
  int8_t retries_remaining = 3;
  while (!CONTEXT.wifi_command_received && retries_remaining > 0) {
    send_wifi_enable_request();
    wait_for_config_reply(&CONTEXT.wifi_command_received);
    retries_remaining--;
  }

  std::string network_manager_service_action = "enable";
  std::string wifi_enable_action = "unblock";
  std::string wifi_driver_action = "";
  std::string wifi_driver_dependency_command = "";

  if (!CONTEXT.wifi_enabled) {
    network_manager_service_action = "disable";
    wifi_enable_action = "block";
    wifi_driver_action = "-r ";
    wifi_driver_dependency_command = " modprobe -r brcmutil";
  }

  // Configure Wi-Fi In Software
  std::string wifi_command = "rfkill " + wifi_enable_action + " wifi";
  bm_log_info("Invoking command for Wi-Fi radio: %s", wifi_command.c_str());
  system(wifi_command.c_str());

  // Configure NetworkManager.service
  std::string systemctl_action =
      "systemctl " + network_manager_service_action + " --now NetworkManager";
  bm_log_info("Invoking systemctl command: %s", systemctl_action.c_str());
  system(systemctl_action.c_str());

  // Configure Wi-Fi Hardware
  std::string wifi_driver_command =
      "modprobe " + wifi_driver_action + "brcmfmac";
  bm_log_info("Invoking driver command: %s", wifi_driver_command.c_str());
  system(wifi_driver_command.c_str());
  if (wifi_driver_dependency_command.length()) {
    bm_log_info("Invoking driver dependency command: %s",
                wifi_driver_dependency_command.c_str());
    system(wifi_driver_dependency_command.c_str());
  }
}

#define MAX_NMEA_RMC_LEN 82
#define MAX_NMEA_FIELDS 14

static bool nmea_checksum_valid(char *line, size_t len) {
  uint8_t checksum = 0;
  uint32_t idx = 1;

  while((idx < len) && ('*' != line[idx])) {
    checksum ^= (uint8_t)line[idx];
    idx++;
  }
  uint8_t line_checksum = strtoul(&line[idx + 1], NULL, 16);

  if (checksum != line_checksum) {
    return false;
  }
  return true;
}

static bool parse_nmea_rmc(char *line, size_t len, struct timespec *time_output) {
  bool success = false;

  do {

    if (!line || len > MAX_NMEA_RMC_LEN) {
      bm_log_error("Invalid NMEA RMC string");
      break;
    }

    if (!nmea_checksum_valid(line, len)) {
      bm_log_error("Invalid checksum");
      break;
    }

    // Make sure the line is NULL terminated
    line[len] = '\0';
    // Here we will break the NMEA RMC into fields, splitting on the ',' or '*'.
    // The fields will be:
    //   0=$GPRMC, 1=HHMMSS.ss, 2=A/V, 3=lat, 4=N/S, 5=lon, 6=E/W,
    //   7=speed, 8=course, 9=DDMMYY, ...
    // We are only interested in fields 1 and 9.
    // Details for the rest can be found here: https://gpsd.gitlab.io/gpsd/NMEA.html#_rmc_recommended_minimum_navigation_information
    char *fields[MAX_NMEA_FIELDS];
    int field_idx = 0;
    fields[field_idx++] = line;
    for (size_t i = 0; i < len && field_idx < 12; i++) {
      if (line[i] == ',' || line[i] == '*') {
        line[i] = '\0';
        // Point the field at the first char after the ',' or '*'.
        fields[field_idx++] = &line[i + 1];
      }
    }

    int hour, min, sec, centisec = 0;
    if (sscanf(fields[1], "%2d%2d%2d.%2d", &hour, &min, &sec, &centisec) != 4) {
      bm_log_error("Failed to get HHMMSS.ss");
      break;
    }

    int day, mon, year = 0;
    if (sscanf(fields[9], "%2d%2d%2d", &day, &mon, &year) != 3) {
      bm_log_error("Failed to get DDMMYY");
      break;
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
      break;
    }

    time_output->tv_sec = epoch;
    time_output->tv_nsec = (long)centisec * 10000000L;

    success = true;
  } while (0);

  return success;
}

static bool sync_time(struct timespec *time) {
  bm_log_info("Syncing to unixtime %ld : %ld", time->tv_sec, time->tv_nsec);
  if (clock_settime(CLOCK_REALTIME, time) == 0) {
    bm_log_info("System time synced");
    return true;
  }
  bm_log_error("Failed to sync system time: %s", strerror(errno));
  return false;
}

static void run_sbc_command(void) {
  static bool sbc_command_ran = false;

  if (!sbc_command_ran && CONTEXT.sbc_command_received) {
    bm_log_info("Running sbc_command: %s", CONTEXT.sbc_command);
    const int status = system(CONTEXT.sbc_command);
    sbc_command_ran = true;
    if (status == -1) {
      bm_log_error("Failed to run sbc_command: %s", strerror(errno));
    } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
      bm_log_error("sbc_command exited %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      bm_log_error("sbc_command killed by signal %d", WTERMSIG(status));
    }
  }
}

static void gprmc_callback(uint64_t node_id, const char *topic,
                           uint16_t topic_len, const uint8_t *data,
                           uint16_t data_len, uint8_t type, uint8_t version) {

  if (!CONTEXT.system_time_synced.load()) {

    struct timespec unixtime = {
      .tv_sec = 0,
      .tv_nsec = 0,
    };

    if (!parse_nmea_rmc((char *)data, data_len, &unixtime)) {
      bm_log_error("Failed to parse NMEA RMC string");
      return;
    }

    if (sync_time(&unixtime)) {
      CONTEXT.system_time_synced = true;
      bm_log_info("System time synced to GPS");
    } else {
      // Do not continue with the reset of this function until we can snap
      // the realtime clock to the latest gps rmc message. This prevents
      // the sbc_command from being run until the realtime clock is synced.
      return;
    }
  }

  // Runs the sbc command if it hasn't
  // been run yet, otherwise does nothing.
  run_sbc_command();
  bm_log_debug("sendto: %.*s", data_len, (char*)data);
  ssize_t bytes_sent = sendto(CONTEXT.gps_udp_socket_fd, data, data_len, 0,
                              (struct sockaddr *)&GPS_DEST, sizeof(GPS_DEST));
  if (bytes_sent == -1) {
    bm_log_error("GPS RMC sendto failed: %s", strerror(errno));
  }

  CONTEXT.last_rmc_time = time(NULL);
}

#ifndef USEC_PER_SEC
#define USEC_PER_SEC 1000000L
#endif

static unsigned char hex_byte(unsigned char c) {
    return (c >= 10) ? ('A' + (c - 10)) : ('0' + c);
}

static void utc_callback(uint64_t node_id, const char *topic,
                         uint16_t topic_len, const uint8_t *data,
                         uint16_t data_len, uint8_t type, uint8_t version) {
  (void)node_id;
  (void)topic;
  (void)topic_len;

  if (type != 1 || version != 1 ||
      data_len < sizeof(bm_common_pub_sub_utc_t)) {
    return;
  }

  const bm_common_pub_sub_utc_t *utc = (const bm_common_pub_sub_utc_t *)data;

  if (!CONTEXT.system_time_synced.load()) {

    struct timespec unixtime = {
      .tv_sec = 0,
      .tv_nsec = 0,
    };

    // parse the spotter/utc-time message and set unix time to it
    // will need to separate the seconds portion from the uSec portion
    // since tv_nsec is the number of nano seconds in the currecnt second
    // not the number of nanoseconds since 1970 or whatever.
    unixtime.tv_sec = utc->utc_us / USEC_PER_SEC;
    unixtime.tv_nsec = (utc->utc_us % USEC_PER_SEC) * 1000L;

    if (sync_time(&unixtime)) {
      CONTEXT.system_time_synced = true;
      bm_log_info("System time synced to UTC");
    } else {
      // Do not continue with the reset of this function until we can snap
      // the realtime clock to the latest gps rmc message. This prevents
      // the sbc_command from being run until the realtime clock is synced.
      return;
    }
  }

  // Runs the sbc command if it hasn't
  // been run yet, otherwise does nothing.
  run_sbc_command();

  UtcDateTime rtc = {0};
  date_time_from_utc(utc->utc_us, &rtc);

  char fake_gpzda[] = "$GPZDA,hhmmss.ff,dd,mm,yyyy,00,00*xx\r\n";

  const unsigned hundredths = rtc.usec / 10000U;

  fake_gpzda[7] = rtc.hour / 10U + '0';
  fake_gpzda[8] = rtc.hour % 10U + '0';

  fake_gpzda[9] = rtc.min / 10U + '0';
  fake_gpzda[10] = rtc.min % 10U + '0';

  fake_gpzda[11] = rtc.sec / 10U + '0';
  fake_gpzda[12] = rtc.sec % 10U + '0';

  fake_gpzda[14] = hundredths / 10U + '0';
  fake_gpzda[15] = hundredths % 10U + '0';

  fake_gpzda[17] = rtc.day / 10U + '0';
  fake_gpzda[18] = rtc.day % 10U + '0';

  fake_gpzda[20] = rtc.month / 10U + '0';
  fake_gpzda[21] = rtc.month % 10U + '0';

  fake_gpzda[23] = (rtc.year / 1000U) % 10U + '0';
  fake_gpzda[24] = (rtc.year / 100U) % 10U + '0';
  fake_gpzda[25] = (rtc.year / 10U) % 10U + '0';
  fake_gpzda[26] = rtc.year % 10U + '0';

  /* compute checksum of portion of buffer between $ and *, not inclusive */
  unsigned int cksum = 0;
  for (const unsigned char * c = (const unsigned char *)fake_gpzda + 1; *c != '*'; c++) {
    cksum ^= *c;
  }

  fake_gpzda[34] = hex_byte(cksum / 16U);
  fake_gpzda[35] = hex_byte(cksum % 16U);

  if (CONTEXT.last_rmc_time <= 0 || (time(NULL) - CONTEXT.last_rmc_time > 30)) {
    bm_log_debug("sendto: %s", fake_gpzda);
    ssize_t bytes_sent = sendto(CONTEXT.gps_udp_socket_fd, fake_gpzda, strlen(fake_gpzda), 0,
                              (struct sockaddr *)&GPS_DEST, sizeof(GPS_DEST));
    if (bytes_sent == -1) {
      bm_log_error("Fake GPS ZDA sendto failed: %s", strerror(errno));
    }
  }
}

void setup(void) {
  CONTEXT.gps_udp_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (CONTEXT.gps_udp_socket_fd == -1) {
    bm_log_error("Failed to create GPS UDP socket: %s", strerror(errno));
    exit(EXIT_FAILURE);
  }
  bm_sub("gps-nmea/rmc", gprmc_callback);
  bm_sub("spotter/utc-time", utc_callback);
  await_uart_neighbor();
  get_mote_app_name();
  get_mote_system_configs();
  get_sbc_command();
  get_wifi_enable();
  gateway_ipc_init(CONTEXT.mote_node_id);
}

void loop(void) { gateway_ipc_poll(); }
