#ifndef __BM_CONFIG_H__
#define __BM_CONFIG_H__

// App name set at runtime by bm_sbc_runtime_init(), so the sysinfo
// service and any other consumer of bm_app_name picks up the per-app
// value (e.g. "multinode") rather than a generic default.
extern const char *bm_sbc_app_name_runtime;
#define bm_app_name bm_sbc_app_name_runtime

#include "bm_log.h"
#define bm_debug(format, ...) bm_log_debug(format, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Compile-time device identity constants used by device_init() in runtime.cpp.
// Version numbers (BM_SBC_VERSION_MAJOR/MINOR/PATCH) and the version tag
// string (BM_SBC_VERSION_TAG) are generated at build time in git_sha.h.
// ---------------------------------------------------------------------------
#define BM_SBC_DEVICE_NAME    "bm_sbc"
#define BM_SBC_VENDOR_ID      ((uint16_t)0x0000)
#define BM_SBC_PRODUCT_ID     ((uint16_t)0x0000)
#define BM_SBC_HW_VER         ((uint8_t)0)

#endif

