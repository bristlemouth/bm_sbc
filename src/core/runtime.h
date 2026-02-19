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
/// @return 0 on success, non-zero on failure
int bm_sbc_runtime_init(int argc, char **argv);

