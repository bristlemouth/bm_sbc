#include "runtime.h"
#include "virtual_port_device.h"
#include "gateway_device.h"
#include "uart_l2_transport.h"
#include "bm_config.h"
// topology.h, bm_service.h, l2.h, pubsub.h have extern "C" guards.
// device.h, bm_ip.h, bcmp.h, middleware.h do not — wrap them explicitly.
#include "l2.h"
#include "topology.h"
#include "bm_service.h"
#include "pubsub.h"
extern "C" {
#include "device.h"
#include "bm_ip.h"
#include "bcmp.h"
#include "middleware.h"
}
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_usage =
    "Usage: bm_sbc --node-id <hex64> [--peer <hex64>]... [--socket-dir <path>]\n"
    "              [--uart <device>] [--baud <rate>]\n"
    "\n"
    "  --node-id    <hex64>   This node's 64-bit Bristlemouth node ID (required).\n"
    "  --peer       <hex64>   A peer node ID; repeat up to 16 times (optional).\n"
    "                         (16 peers triggers a truncation warning; 15 are used.)\n"
    "  --socket-dir <path>    Unix socket directory (default: /tmp).\n"
    "  --uart       <device>  Serial device path for UART gateway mode.\n"
    "  --baud       <rate>    Baud rate for UART (default: 115200).\n";

int bm_sbc_runtime_init(int argc, char **argv) {
  // Make stdout line-buffered so every bm_debug/printf call ending in '\n'
  // flushes immediately, even when output is redirected to a file.  Without
  // this, ping-reply lines printed by the BCMP thread can be lost when the
  // process is killed before the 8 KiB libc buffer fills up.
  setvbuf(stdout, NULL, _IOLBF, 0);
  // --- Task 3a: CLI parsing -------------------------------------------
  VirtualPortCfg vpc;
  memset(&vpc, 0, sizeof(vpc));
  strncpy(vpc.socket_dir, VIRTUAL_PORT_DEFAULT_SOCKET_DIR,
          sizeof(vpc.socket_dir) - 1);
  bool node_id_set = false;
  char uart_path[128] = {0};
  int baud_rate = 115200;

  static const struct option long_opts[] = {
      {"node-id",    required_argument, NULL, 'n'},
      {"peer",       required_argument, NULL, 'p'},
      {"socket-dir", required_argument, NULL, 's'},
      {"uart",       required_argument, NULL, 'u'},
      {"baud",       required_argument, NULL, 'b'},
      {NULL, 0, NULL, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
    switch (opt) {
      case 'n': {
        char *end = NULL;
        vpc.own_node_id = strtoull(optarg, &end, 16);
        if (!end || *end != '\0') {
          fprintf(stderr, "bm_sbc: invalid --node-id value: %s\n", optarg);
          fprintf(stderr, "%s", k_usage);
          return 1;
        }
        node_id_set = true;
        break;
      }
      case 'p': {
        if (vpc.num_peers >= VIRTUAL_PORT_CFG_MAX_PEERS) {
          fprintf(stderr,
                  "bm_sbc: too many --peer flags (max %d); ignoring %s\n",
                  VIRTUAL_PORT_CFG_MAX_PEERS, optarg);
          break;
        }
        char *end = NULL;
        uint64_t pid = strtoull(optarg, &end, 16);
        if (!end || *end != '\0') {
          fprintf(stderr, "bm_sbc: invalid --peer value: %s\n", optarg);
          fprintf(stderr, "%s", k_usage);
          return 1;
        }
        vpc.peer_ids[vpc.num_peers++] = pid;
        break;
      }
      case 's': {
        strncpy(vpc.socket_dir, optarg, sizeof(vpc.socket_dir) - 1);
        break;
      }
      case 'u': {
        strncpy(uart_path, optarg, sizeof(uart_path) - 1);
        break;
      }
      case 'b': {
        char *end = NULL;
        baud_rate = (int)strtol(optarg, &end, 10);
        if (!end || *end != '\0' || baud_rate <= 0) {
          fprintf(stderr, "bm_sbc: invalid --baud value: %s\n", optarg);
          fprintf(stderr, "%s", k_usage);
          return 1;
        }
        break;
      }
      default: {
        fprintf(stderr, "bm_sbc: unrecognised option\n");
        fprintf(stderr, "%s", k_usage);
        return 1;
      }
    }
  }

  if (!node_id_set) {
    fprintf(stderr, "bm_sbc: --node-id is required\n");
    fprintf(stderr, "%s", k_usage);
    return 1;
  }

  bool gateway_mode = (uart_path[0] != '\0');
  bm_debug("bm_sbc: node_id=0x%016" PRIx64 " peers=%u socket_dir=%s%s%s\n",
           vpc.own_node_id, (unsigned)vpc.num_peers, vpc.socket_dir,
           gateway_mode ? " uart=" : "", gateway_mode ? uart_path : "");

  // --- Task 3b: device_init -------------------------------------------
  DeviceCfg dev_cfg;
  memset(&dev_cfg, 0, sizeof(dev_cfg));
  dev_cfg.node_id        = vpc.own_node_id;
  dev_cfg.git_sha        = 0;
  dev_cfg.device_name    = BM_SBC_DEVICE_NAME;
  dev_cfg.version_string = BM_SBC_VERSION_STRING;
  dev_cfg.vendor_id      = BM_SBC_VENDOR_ID;
  dev_cfg.product_id     = BM_SBC_PRODUCT_ID;
  dev_cfg.hw_ver         = BM_SBC_HW_VER;
  dev_cfg.ver_major      = BM_SBC_VERSION_MAJOR;
  dev_cfg.ver_minor      = BM_SBC_VERSION_MINOR;
  dev_cfg.ver_patch      = BM_SBC_VERSION_PATCH;
  device_init(dev_cfg);

  // --- Task 3c: VirtualPortDevice setup --------------------------------
  NetworkDevice vpd_dev = virtual_port_device_get(&vpc);
  NetworkDevice net_dev;

  if (gateway_mode) {
    // Gateway mode: composite device wrapping VPD + UART.
    int uart_err = uart_l2_transport_init(uart_path, baud_rate,
                                          gateway_uart_rx_cb,
                                          nullptr);
    if (uart_err != 0) {
      fprintf(stderr, "bm_sbc: UART transport init failed\n");
      return 1;
    }
    net_dev = gateway_device_get(&vpd_dev);
  } else {
    // Normal mode: VPD only.
    net_dev = vpd_dev;
  }

  // --- Task 3d: Bristlemouth startup sequence --------------------------
  BmErr err = BmOK;
  bm_err_check(err, bm_l2_init(net_dev));
  bm_err_check(err, bm_ip_init());
  bm_err_check(err, bcmp_init(net_dev));
  uint8_t total_ports = net_dev.trait->num_ports();
  bm_err_check(err, topology_init(total_ports));
  bm_err_check(err, bm_service_init());
  bm_err_check(err, bm_pubsub_init());
  bm_err_check(err, bm_middleware_init());

  if (err != BmOK) {
    bm_debug("bm_sbc: startup sequence failed err=%d\n", (int)err);
    return (int)err;
  }
  bm_debug("bm_sbc: stack initialized\n");
  return 0;
}

