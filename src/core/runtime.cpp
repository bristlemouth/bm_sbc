#include "runtime.h"

// TODO: Implement CLI parsing (--node-id, storage paths, gateway flags, UART params)
// TODO: DeviceCfg initialization
// TODO: Bristlemouth startup sequence:
//   bm_l2_init(custom_network_device)
//   bm_ip_init()
//   bcmp_init(custom_network_device)
//   topology_init(...)
//   bm_service_init()
//   bm_middleware_init(...)

int bm_sbc_runtime_init(int argc, char **argv) {
  (void)argc;
  (void)argv;
  // Placeholder – will be implemented in milestone 2+
  return 0;
}

