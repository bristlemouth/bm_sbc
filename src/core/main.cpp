#include "app_runner.h"
#include "bm_log.h"
#include "runtime.h"

#ifndef BM_SBC_APP_NAME
#define BM_SBC_APP_NAME "bm_sbc"
#endif

// Identity marker baked into read-only data at build time.
// DFU validation scans the staging binary for this string before swapping,
// ensuring a bm_sbc_gateway binary cannot be flashed onto a different app target.
// The section attribute syntax differs between ELF (Linux) and Mach-O (macOS).
#ifdef __linux__
__attribute__((used, section(".rodata")))
#else
__attribute__((used))
#endif
static const char k_image_marker[] = "BM_SBC_IMAGE:" BM_SBC_APP_NAME;

int main(int argc, char **argv) {
  int rc = bm_sbc_runtime_init(argc, argv, BM_SBC_APP_NAME);
  if (rc != 0) {
    return rc;
  }

  bm_sbc_app_run();
  bm_log_shutdown();
  return 0;
}

