#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_IPC_SOCKET_PATH "/run/bm_sbc/gateway_ipc.sock"

// Bind the Unix-domain SOCK_DGRAM listener. Safe to call once from setup().
// Returns 0 on success, -1 on failure (error already logged).
int gateway_ipc_init(void);

// Drain any datagrams currently queued on the IPC socket without blocking.
// Call once per main-loop iteration.
void gateway_ipc_poll(void);

#ifdef __cplusplus
}
#endif
