#include "app_runner.h"
#include "bm_log.h"
#include "runtime.h"

int main(int argc, char **argv) {
  bm_sbc_runtime_set_argv(argc, argv);
  int rc = bm_sbc_runtime_init(argc, argv, BM_SBC_APP_NAME);
  if (rc != 0) {
    return rc;
  }

  bm_sbc_app_run();
  bm_log_shutdown();
  return 0;
}

