#include "runtime.h"
#include "virtual_port_device.h"

// TODO (task 3a): CLI parsing — populate a VirtualPortCfg from argv:
//   --node-id    <hex64>   required; strtoull(optarg, NULL, 16) → cfg.own_node_id
//   --peer       <hex64>   repeatable 0–15×; appended in order to cfg.peer_ids[]
//                          fatal error (usage + exit 1) if a 16th --peer is given
//   --socket-dir <path>    optional; strncpy into cfg.socket_dir,
//                          default VIRTUAL_PORT_DEFAULT_SOCKET_DIR ("/tmp")
//   Unknown flags or missing --node-id → print usage and return non-zero.
//
// TODO (task 3b): DeviceCfg initialization — call device_init() with a
//   DeviceCfg populated from cfg.own_node_id and compile-time constants
//   (BM_SBC_DEVICE_NAME, BM_SBC_VERSION_*, vendor/product IDs).
//
// TODO (task 3c): VirtualPortDevice setup — call virtual_port_device_get(&cfg)
//   to obtain the NetworkDevice; store in a module-level variable for use in
//   the startup sequence and by the application via a getter if needed.
//
// TODO (task 3d): Bristlemouth startup sequence — in order:
//   bm_l2_init(network_device)
//   bm_ip_init()
//   bcmp_init(network_device)
//   topology_init(VIRTUAL_PORT_MAX_PEERS)
//   bm_service_init()
//   bm_middleware_init()
//   Check each return code; log and return non-zero on failure.

int bm_sbc_runtime_init(int argc, char **argv) {
  (void)argc;
  (void)argv;
  // Placeholder – will be implemented in tasks 3a–3d.
  return 0;
}

