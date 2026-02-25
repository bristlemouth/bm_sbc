#ifndef __BM_CONFIG_H__
#define __BM_CONFIG_H__

#define bm_app_name "bm_sbc"

#define bm_debug(format, ...) printf(format, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Compile-time device identity constants used by device_init() in runtime.cpp.
// ---------------------------------------------------------------------------
#define BM_SBC_DEVICE_NAME    "bm_sbc"
#define BM_SBC_VERSION_STRING "0.1.0"
#define BM_SBC_VERSION_MAJOR  0
#define BM_SBC_VERSION_MINOR  1
#define BM_SBC_VERSION_PATCH  0
#define BM_SBC_VENDOR_ID      ((uint16_t)0x0001)
#define BM_SBC_PRODUCT_ID     ((uint16_t)0x0001)
#define BM_SBC_HW_VER         ((uint8_t)1)

#endif

