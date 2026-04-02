#ifndef __BM_CONFIG_H__
#define __BM_CONFIG_H__

#ifdef BM_SBC_APP_NAME
#define bm_app_name BM_SBC_APP_NAME
#else
#define bm_app_name "bm_sbc"
#endif

#define bm_debug(format, ...) printf(format, ##__VA_ARGS__)

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

