#include "raw_eth_transport.h"
#include "bm_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// Maximum Ethernet frame size (header + payload, no FCS).
#define ETH_FRAME_MAX 1514

// ---------------------------------------------------------------------------
// Per-port state
// ---------------------------------------------------------------------------

typedef struct {
  int fd;
  int ifindex;
  char iface_name[IFNAMSIZ];
  pthread_t rx_thread;
  bool running;
  pthread_mutex_t tx_mutex;
  raw_eth_rx_cb rx_cb;
  void *rx_ctx;
  uint8_t port_idx;
} RawEthPort;

static RawEthPort s_ports[RAW_ETH_MAX_PORTS];
static bool s_initialized = false;

/// One-time initialization of the per-port state array. Safe to call
/// multiple times; only runs on the first call.
static void ensure_init(void) {
  if (s_initialized) {
    return;
  }
  for (int i = 0; i < RAW_ETH_MAX_PORTS; i++) {
    memset(&s_ports[i], 0, sizeof(s_ports[i]));
    s_ports[i].fd = -1;
    pthread_mutex_init(&s_ports[i].tx_mutex, NULL);
  }
  s_initialized = true;
}

// ---------------------------------------------------------------------------
// RX thread
// ---------------------------------------------------------------------------

static void *rx_thread_func(void *arg) {
  RawEthPort *port = (RawEthPort *)arg;
  uint8_t buf[ETH_FRAME_MAX];

  while (port->running) {
    struct sockaddr_ll sll;
    socklen_t sll_len = sizeof(sll);
    ssize_t n = recvfrom(port->fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&sll, &sll_len);
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
        continue;
      }
      if (port->running) {
        bm_log_error("raw_eth[%u]: recvfrom error: %s", port->port_idx,
                     strerror(errno));
      }
      break;
    }

    // Filter frames this process transmitted: AF_PACKET / ETH_P_ALL reflects
    // our own TX frames back to the socket as PACKET_OUTGOING.
    if (sll.sll_pkttype == PACKET_OUTGOING) {
      continue;
    }

    if (port->rx_cb) {
      port->rx_cb(port->port_idx, buf, (size_t)n, port->rx_ctx);
    }
  }

  return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int raw_eth_transport_init(uint8_t port_idx, const char *iface_name,
                           raw_eth_rx_cb rx_cb, void *rx_ctx) {
  ensure_init();

  if (port_idx >= RAW_ETH_MAX_PORTS) {
    bm_log_error("raw_eth: port_idx %u >= max %d", port_idx, RAW_ETH_MAX_PORTS);
    return -1;
  }

  RawEthPort *port = &s_ports[port_idx];
  if (port->fd >= 0) {
    bm_log_warn("raw_eth[%u]: already initialized", port_idx);
    return -1;
  }

  strncpy(port->iface_name, iface_name, IFNAMSIZ - 1);
  port->iface_name[IFNAMSIZ - 1] = '\0';
  port->port_idx = port_idx;
  port->rx_cb    = rx_cb;
  port->rx_ctx   = rx_ctx;

  // Open a raw socket that captures all EtherTypes.
  int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (fd < 0) {
    bm_log_error("raw_eth[%u]: socket() failed: %s", port_idx,
                 strerror(errno));
    return -1;
  }

  // Resolve the interface index.
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, iface_name, IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
    bm_log_error("raw_eth[%u]: SIOCGIFINDEX(%s) failed: %s", port_idx,
                 iface_name, strerror(errno));
    close(fd);
    return -1;
  }
  port->ifindex = ifr.ifr_ifindex;

  // Bind to this specific interface so we only receive its frames.
  struct sockaddr_ll sll;
  memset(&sll, 0, sizeof(sll));
  sll.sll_family   = AF_PACKET;
  sll.sll_protocol = htons(ETH_P_ALL);
  sll.sll_ifindex  = port->ifindex;
  if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
    bm_log_error("raw_eth[%u]: bind(%s) failed: %s", port_idx, iface_name,
                 strerror(errno));
    close(fd);
    return -1;
  }

  // Request all-multicast mode so BM multicast frames (e.g. ff03::1 used by
  // BM pub/sub) are delivered without needing to know every BM group address.
  // This is the programmatic equivalent of: ip link set <iface> allmulticast on
  struct packet_mreq mreq;
  memset(&mreq, 0, sizeof(mreq));
  mreq.mr_ifindex = port->ifindex;
  mreq.mr_type    = PACKET_MR_ALLMULTI;
  if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq,
                 sizeof(mreq)) < 0) {
    // Fall back to promiscuous mode if all-multicast is not supported.
    bm_log_warn("raw_eth[%u]: PACKET_MR_ALLMULTI unavailable on %s (%s); "
                "falling back to promiscuous mode",
                port_idx, iface_name, strerror(errno));
    mreq.mr_type = PACKET_MR_PROMISC;
    if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq,
                   sizeof(mreq)) < 0) {
      bm_log_error("raw_eth[%u]: PACKET_MR_PROMISC on %s failed: %s",
                   port_idx, iface_name, strerror(errno));
      close(fd);
      return -1;
    }
    bm_log_info("raw_eth[%u]: %s promiscuous mode", port_idx, iface_name);
  } else {
    bm_log_info("raw_eth[%u]: %s all-multicast mode", port_idx, iface_name);
  }

  port->fd      = fd;
  port->running = true;

  if (pthread_create(&port->rx_thread, NULL, rx_thread_func, port) != 0) {
    bm_log_error("raw_eth[%u]: pthread_create failed: %s", port_idx,
                 strerror(errno));
    close(fd);
    port->fd      = -1;
    port->running = false;
    return -1;
  }

  bm_log_info("raw_eth[%u]: opened %s (ifindex=%d)", port_idx, iface_name,
               port->ifindex);
  return 0;
}

int raw_eth_send(uint8_t port_idx, const uint8_t *frame, size_t len) {
  if (port_idx >= RAW_ETH_MAX_PORTS) {
    return -1;
  }
  RawEthPort *port = &s_ports[port_idx];
  if (port->fd < 0 || !frame || len == 0) {
    return -1;
  }

  // Build the destination address from the frame's dst MAC (bytes 0–5).
  struct sockaddr_ll sll;
  memset(&sll, 0, sizeof(sll));
  sll.sll_family   = AF_PACKET;
  sll.sll_protocol = htons(ETH_P_ALL);
  sll.sll_ifindex  = port->ifindex;
  sll.sll_halen    = ETH_ALEN;
  memcpy(sll.sll_addr, frame, ETH_ALEN);

  pthread_mutex_lock(&port->tx_mutex);
  ssize_t sent = sendto(port->fd, frame, len, 0,
                        (const struct sockaddr *)&sll, sizeof(sll));
  pthread_mutex_unlock(&port->tx_mutex);

  if (sent < 0) {
    bm_log_error("raw_eth[%u]: sendto(%s) failed: %s", port_idx,
                 port->iface_name, strerror(errno));
    return -1;
  }
  return 0;
}

bool raw_eth_carrier(uint8_t port_idx) {
  if (port_idx >= RAW_ETH_MAX_PORTS) {
    return false;
  }
  const RawEthPort *port = &s_ports[port_idx];
  if (port->iface_name[0] == '\0') {
    return false;
  }
  char path[64];
  snprintf(path, sizeof(path), "/sys/class/net/%s/carrier",
           port->iface_name);
  FILE *f = fopen(path, "r");
  if (!f) {
    return false;
  }
  char c = '0';
  (void)fread(&c, 1, 1, f);
  fclose(f);
  return c == '1';
}

void raw_eth_transport_deinit(uint8_t port_idx) {
  if (port_idx >= RAW_ETH_MAX_PORTS) {
    return;
  }
  RawEthPort *port = &s_ports[port_idx];
  if (port->fd < 0) {
    return;
  }

  port->running = false;
  // Closing the fd unblocks recvfrom() in the RX thread.
  close(port->fd);
  port->fd = -1;

  pthread_join(port->rx_thread, NULL);

  port->rx_cb         = NULL;
  port->rx_ctx        = NULL;
  port->iface_name[0] = '\0';
}
