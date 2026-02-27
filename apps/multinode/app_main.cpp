/// @file app_main.cpp
/// @brief Multinode validation app for bm_sbc.
///
/// Registers a BCMP neighbor-discovery callback and a middleware pub/sub
/// subscriber, then after a short startup delay issues a multicast ping and
/// publishes one test message.  The test script (scripts/multinode_test.sh)
/// greps the combined stdout logs for the expected event strings.
///
/// Key output markers (searched by the test script):
///   NEIGHBOR_UP   — emitted when a peer is discovered
///   NEIGHBOR_DOWN — emitted when a peer goes offline
///   PUBSUB_RX     — emitted when a pub/sub message arrives from a remote node
///   🏓            — emitted by bm_core/bcmp/ping.c when a ping reply arrives

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

// Headers without C++ guards must be wrapped so their symbols have C linkage.
// util.h is included here first so its include guard fires before
// messages/neighbors.h pulls it in outside any extern "C" block.
extern "C" {
#include "util.h"           // BmIpAddr, multicast_global_addr
#include "device.h"         // node_id()
#include "messages/ping.h"  // bcmp_send_ping_request
}

// These headers have their own #ifdef __cplusplus / extern "C" guards.
#include "pubsub.h"              // bm_sub, bm_pub, BM_COMMON_PUB_SUB_VERSION
#include "messages/neighbors.h" // bcmp_neighbor_register_discovery_callback, BcmpNeighbor

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const char    *k_topic        = "bm_sbc/test";
static const char    *k_payload      = "hello_from_multinode";
static const double   k_delay_sec    = 3.0; // wall-clock seconds before action

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool            s_start_recorded = false;
static struct timespec s_start_time;
static bool            s_actions_done   = false;

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void on_neighbor(bool discovered, BcmpNeighbor *neighbor) {
  printf("[%016" PRIx64 "] NEIGHBOR_%s node=%016" PRIx64 " port=%u\n",
         node_id(),
         discovered ? "UP" : "DOWN",
         neighbor->node_id,
         (unsigned)neighbor->port);
  fflush(stdout);
}

static void on_pubsub(uint64_t src_node_id, const char *topic,
                      uint16_t topic_len, const uint8_t *data,
                      uint16_t data_len, uint8_t /*type*/, uint8_t /*version*/) {
  printf("[%016" PRIx64 "] PUBSUB_RX from=%016" PRIx64
         " topic=%.*s data=%.*s\n",
         node_id(), src_node_id,
         (int)topic_len, topic,
         (int)data_len, reinterpret_cast<const char *>(data));
  fflush(stdout);
}

// ---------------------------------------------------------------------------
// App entry points (called by app_runner.cpp)
// ---------------------------------------------------------------------------

void setup(void) {
  bcmp_neighbor_register_discovery_callback(on_neighbor);
  bm_sub(k_topic, on_pubsub);
  printf("[%016" PRIx64 "] multinode app: setup\n", node_id());
  fflush(stdout);
}

void loop(void) {
  if (s_actions_done) { return; }

  // Use wall-clock time for the startup delay so the timing is correct
  // regardless of nanosleep resolution (which varies across platforms).
  if (!s_start_recorded) {
    clock_gettime(CLOCK_MONOTONIC, &s_start_time);
    s_start_recorded = true;
    return;
  }
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double elapsed = (now.tv_sec - s_start_time.tv_sec)
                 + (now.tv_nsec - s_start_time.tv_nsec) / 1e9;
  if (elapsed < k_delay_sec) { return; }

  s_actions_done = true;

  // Send a multicast ping — bm_core handles the echo request/reply cycle and
  // logs the reply line (🏓 ... bcmp_seq=...) via bm_debug/printf.
  bcmp_send_ping_request(0, &multicast_global_addr, nullptr, 0);

  // Publish a test message on the shared topic.  Remote peers that subscribed
  // will fire their on_pubsub callback and print PUBSUB_RX.
  bm_pub(k_topic, k_payload,
         static_cast<uint16_t>(strlen(k_payload)),
         0, BM_COMMON_PUB_SUB_VERSION);

  printf("[%016" PRIx64 "] multinode app: ping + pub sent\n", node_id());
  fflush(stdout);
}

