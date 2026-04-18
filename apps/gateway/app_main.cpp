#include "bm_log.h"
#include "gateway_device.h"
#include "messages/config.h"
#include "messages/neighbors.h"
#include "pubsub.h"
#include <arpa/inet.h>
#include <sys/socket.h>

#define SBC_COMMAND_KEY "sbc_command"
#define SBC_COMMAND_KEY_LEN (sizeof(SBC_COMMAND_KEY) - 1)

static struct {
  uint64_t mote_node_id = 0;
  bool mote_neighbor_found = false;
  bool sbc_command_received = false;
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

static BmErr sbc_command_reply_cb(uint8_t *payload) {
  bm_log_debug("Ticks in sbc command reply cb: %u", bm_get_tick_count());

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
        // TODO: run it
      }
    } else {
      bm_log_error("Failed to decode sbc command bcmp value, err=%d", err);
    }
  }

  return err;
}

static void send_sbc_command_request(void) {
  bm_log_debug("Ticks before bcmp config get: %u", bm_get_tick_count());
  BmErr err = BmOK;
  bool sent = bcmp_config_get(CONTEXT.mote_node_id, BM_CFG_PARTITION_SYSTEM,
                              SBC_COMMAND_KEY_LEN, SBC_COMMAND_KEY, &err,
                              sbc_command_reply_cb);
  bm_log_debug("Ticks after bcmp config get: %u", bm_get_tick_count());
  if (!sent) {
    bm_log_warn("Failed to send bcmp config get for sbc_command, err=%d", err);
  }
}

static void wait_for_sbc_command_reply(void) {
  uint32_t total_awaited_ms = 0;
  while (!CONTEXT.sbc_command_received && total_awaited_ms < 500) {
    const uint32_t delay_poll_ms = 20;
    bm_delay(delay_poll_ms);
    total_awaited_ms += delay_poll_ms;
  }
}

static void get_sbc_command(void) {
  int8_t retries_remaining = 3;
  while (!CONTEXT.sbc_command_received && retries_remaining > 0) {
    send_sbc_command_request();
    wait_for_sbc_command_reply();
    retries_remaining--;
  }
}

static void gprmc_callback(uint64_t node_id, const char *topic,
                           uint16_t topic_len, const uint8_t *data,
                           uint16_t data_len, uint8_t type, uint8_t version) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in dest = {.sin_family = AF_INET,
                             .sin_port = htons(5000),
                             .sin_addr = {.s_addr = inet_addr("127.0.0.1")}};
  sendto(sock, data, data_len, 0, (struct sockaddr *)&dest, sizeof(dest));
}

void setup(void) {
  await_uart_neighbor();
  get_sbc_command();
  bm_sub("gps-nmea/rmc", gprmc_callback);
}

void loop(void) {}
