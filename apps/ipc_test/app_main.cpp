// Minimal app that only hosts the gateway IPC listener — no UART, no mote.
//
// Used by scripts/gateway_ipc_test.sh to exercise the Python client against a
// real gateway_ipc.cpp binding without needing the full mote+UART setup.
#include "bm_log.h"
#include "gateway_ipc.h"

void setup(void) {
  if (gateway_ipc_init() != 0) {
    bm_log_fatal("ipc_test: gateway_ipc_init failed");
  }
}

void loop(void) { gateway_ipc_poll(); }
