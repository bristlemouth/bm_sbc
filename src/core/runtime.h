#pragma once

/// @file runtime.h
/// @brief Bristlemouth stack runtime bootstrap for bm_sbc.
///
/// Handles CLI parsing, DeviceCfg initialization, and the full
/// Bristlemouth startup sequence (bm_l2_init, bm_ip_init, bcmp_init,
/// topology_init, bm_service_init, bm_middleware_init).

/// Parse command-line arguments and initialize the runtime.
/// @param argc argument count
/// @param argv argument vector
/// @param app_name short application name (e.g. "multinode"), used in
///        the BCMP version string and sysinfo service replies.
/// @return 0 on success, non-zero on failure
int bm_sbc_runtime_init(int argc, char **argv, const char *app_name);

/// Register a callback invoked immediately before execv() in both
/// set_pending_and_reset() and fail_update_and_reset().
/// Use this for application-level cleanup that must happen before the process
/// image is replaced (e.g. stopping systemd services started by sbc_command).
/// Only one callback is supported; calling this again replaces the previous one.
/// The callback must not call any bm_log functions (logging is already shut
/// down at the call site) but may call system() or other libc functions.
void bm_sbc_runtime_set_pre_exec_cb(void (*cb)(void));

/// Store argc/argv so execv() can restart the process after a DFU binary swap.
/// Also derives all DFU file paths from /proc/self/exe:
///   <exe>           — install path
///   <exe>.staging   — incoming binary is written here during transfer
///   <exe>.bak       — hard link to the previous binary (rollback target)
///   <dir>/dfu_pending.bin — serialised PlatformDfuMarker (noinit substitute)
/// Must be called once at the very top of main(), before anything else.
/// @param argc  Argument count from main().
/// @param argv  Argument vector from main().
void bm_sbc_runtime_set_argv(int argc, char **argv);
