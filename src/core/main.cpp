#include "app_runner.h"
#include "bm_log.h"
#include "runtime.h"

#ifndef BM_SBC_APP_NAME
#define BM_SBC_APP_NAME "bm_sbc"
#endif

int main(int argc, char **argv) {
  int rc = bm_sbc_runtime_init(argc, argv, BM_SBC_APP_NAME);
  if (rc != 0) {
    return rc;
  }

  bm_sbc_app_run();
  bm_log_shutdown();
  return 0;
}

