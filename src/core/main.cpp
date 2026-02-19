#include "app_runner.h"
#include "runtime.h"

int main(int argc, char **argv) {
  int rc = bm_sbc_runtime_init(argc, argv);
  if (rc != 0) {
    return rc;
  }

  bm_sbc_app_run();
  return 0;
}

