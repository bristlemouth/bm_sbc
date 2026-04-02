#include "pcap_file_sink.h"

extern "C" {
#include "pcap.h"
}

#include <pthread.h>
#include <stdio.h>

static FILE *s_file;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

static void file_write_cb(const uint8_t *data, size_t len, void *ctx) {
  (void)ctx;
  pthread_mutex_lock(&s_mutex);
  if (s_file) {
    fwrite(data, 1, len, s_file);
    fflush(s_file);
  }
  pthread_mutex_unlock(&s_mutex);
}

int pcap_file_sink_open(const char *path) {
  if (!path) {
    return -1;
  }

  pthread_mutex_lock(&s_mutex);
  s_file = fopen(path, "wb");
  pthread_mutex_unlock(&s_mutex);

  if (!s_file) {
    return -1;
  }

  pcap_init(file_write_cb, NULL);
  return 0;
}

void pcap_file_sink_close(void) {
  pthread_mutex_lock(&s_mutex);
  if (s_file) {
    fflush(s_file);
    fclose(s_file);
    s_file = NULL;
  }
  pthread_mutex_unlock(&s_mutex);
}
