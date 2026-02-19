#include "app_runner.h"

#include <unistd.h> // usleep

void bm_sbc_app_run(void) {
  setup();

  // TODO: Replace with a proper scheduler-friendly cadence.
  for (;;) {
    loop();
    usleep(1000); // 1 ms yield
  }
}

