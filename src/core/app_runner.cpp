#include "app_runner.h"

#include <time.h>

void bm_sbc_app_run(void) {
  setup();

  // TODO: Replace with a proper scheduler-friendly cadence.
  for (;;) {
    loop();
    struct timespec delay = {0, 1000000L}; // 1 ms yield
    nanosleep(&delay, NULL);
  }
}
