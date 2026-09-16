#include "uart_l2_transport.h"
#include "bm_log.h"
#include "bm_os.h"
#include "cobs.h"
#include "frame_codec.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

// Darwin's termios.h doesn't define these baud rates, return B0, unsupported
#ifndef B1000000
#define B1000000 B0
#endif

#ifndef B1500000
#define B1500000 B0
#endif

#ifndef B2000000
#define B2000000 B0
#endif

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static int s_fd = -1;
static pthread_t s_rx_thread;
static bool s_rx_running = false;
static uart_l2_rx_cb s_rx_cb = nullptr;
static void *s_rx_ctx = nullptr;
static pthread_mutex_t s_tx_mutex = PTHREAD_MUTEX_INITIALIZER;

// Kept so the RX thread can reopen the port after a hangup.
static char s_dev_path[PATH_MAX] = {0};
static int s_baud = 0;

/// Delay between reopen attempts after a hangup.
#define UART_L2_REOPEN_DELAY_MS 100

/// How long poll() waits before re-checking s_rx_running.
#define UART_L2_POLL_TIMEOUT_MS 200

// ---------------------------------------------------------------------------
// Serial port helpers
// ---------------------------------------------------------------------------

/// Map an integer baud rate to a termios speed constant.
static speed_t baud_to_speed(int baud) {
  switch (baud) {
  case 9600:
    return B9600;
  case 19200:
    return B19200;
  case 38400:
    return B38400;
  case 57600:
    return B57600;
  case 115200:
    return B115200;
  case 230400:
    return B230400;
  case 1500000:
    return B1500000;
  case 1000000:
    return B1000000;
  case 2000000:
    return B2000000;
  default:
    return B0; // unsupported
  }
}

/// Open and configure a serial port for raw 8N1 operation.
/// Returns the file descriptor, or -1 on error.
static int serial_open(const char *path, int baud) {
  speed_t speed = baud_to_speed(baud);
  if (speed == B0) {
    bm_log_error("uart_l2: unsupported baud rate %d", baud);
    return -1;
  }

  // O_CLOEXEC: don't leak the port into forked children (safe_cmd, system()).
  // A child holding this fd can read our bytes or reconfigure the line.
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    bm_log_error("uart_l2: open(%s) failed: %s", path, strerror(errno));
    return -1;
  }

  // Clear O_NONBLOCK after open (we want blocking reads in the RX thread).
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  }

  struct termios tty;
  memset(&tty, 0, sizeof(tty));
  if (tcgetattr(fd, &tty) != 0) {
    bm_log_error("uart_l2: tcgetattr failed: %s", strerror(errno));
    close(fd);
    return -1;
  }

  // Raw mode: no echo, no canonical processing, no signals.
  cfmakeraw(&tty);

  // 8N1: 8 data bits, no parity, 1 stop bit.
  tty.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
  tty.c_cflag |= CS8;

  // No hardware flow control.
  tty.c_cflag &= ~CRTSCTS;

  // HUPCL drops DTR/RTS when the last fd on this tty closes. cfmakeraw
  // leaves it set; clear it so no other process's close() can toggle our
  // hardware lines.
  tty.c_cflag &= ~HUPCL;

  // Enable receiver, ignore modem status lines.
  tty.c_cflag |= (CLOCAL | CREAD);

  // Baud rate.
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);

  // VMIN = 1, VTIME = 1 (100 ms inter-byte timeout).
  // Blocks until at least 1 byte available, then returns what's ready.
  tty.c_cc[VMIN] = 1;
  tty.c_cc[VTIME] = 1;

  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    bm_log_error("uart_l2: tcsetattr failed: %s", strerror(errno));
    close(fd);
    return -1;
  }

  // Flush any stale data.
  tcflush(fd, TCIOFLUSH);

  return fd;
}

// ---------------------------------------------------------------------------
// RX thread
// ---------------------------------------------------------------------------

static void *rx_thread_func(void *arg) {
  (void)arg;

  // Accumulation buffer — gather bytes until we see a 0x00 delimiter.
  uint8_t accum[FRAME_CODEC_MAX_WIRE_SIZE];
  size_t accum_len = 0;

  uint8_t read_buf[256];
  uint8_t l2_frame[FRAME_CODEC_MAX_L2_SIZE];
  size_t decode_error_count = 0;

  while (s_rx_running) {
    // Poll before reading so the thread wakes up regularly and notices
    // s_rx_running going false — closing the fd does NOT reliably unblock a
    // thread already parked in read(), which used to hang deinit forever.
    struct pollfd pfd = {s_fd, POLLIN, 0};
    int pr = poll(&pfd, 1, UART_L2_POLL_TIMEOUT_MS);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      bm_log_error("uart_l2: poll error: %s", strerror(errno));
      break;
    }
    if (pr == 0) {
      continue; // idle
    }

    ssize_t n = read(s_fd, read_buf, sizeof(read_buf));
    // On hangup poll() keeps returning POLLHUP and read() returns 0 once the
    // buffer drains, so testing n == 0 catches it without dropping the last
    // frame that arrived before the hangup.
    if (n == 0) {
      // Hangup, not "no data yet" — every subsequent read() returns 0 too,
      // so retrying here spins forever and the link never comes back.
      bm_log_warn("uart_l2: hangup on %s, reopening", s_dev_path);
      pthread_mutex_lock(&s_tx_mutex);
      close(s_fd);
      s_fd = -1;
      pthread_mutex_unlock(&s_tx_mutex);

      accum_len = 0; // whatever we had is half a frame now

      while (s_rx_running) {
        bm_delay(UART_L2_REOPEN_DELAY_MS);
        int fd = serial_open(s_dev_path, s_baud);
        if (fd >= 0) {
          pthread_mutex_lock(&s_tx_mutex);
          s_fd = fd;
          pthread_mutex_unlock(&s_tx_mutex);
          bm_log_info("uart_l2: reopened %s", s_dev_path);
          break;
        }
        // ponytail: fixed retry interval, no backoff. serial_open already
        // logs the failure; add backoff if the logs get noisy.
      }
      continue;
    }
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) {
        continue;
      }
      // Fatal read error — stop.
      bm_log_error("uart_l2: read error: %s", strerror(errno));
      break;
    }

    for (ssize_t i = 0; i < n; i++) {
      if (read_buf[i] == 0x00) {
        // End of frame — decode if we have accumulated data.
        if (accum_len > 0 && s_rx_cb) {
          size_t l2_len =
              frame_decode(l2_frame, sizeof(l2_frame), accum, accum_len);
          if (l2_len > 0) {
            s_rx_cb(l2_frame, l2_len, s_rx_ctx);
          } else {
            bm_log_error("uart_l2: decode error, count - %zu",
                         ++decode_error_count);
          }
          // else: CRC/length error — silently drop
        }
        accum_len = 0;
      } else {
        if (accum_len < sizeof(accum)) {
          accum[accum_len++] = read_buf[i];
        } else {
          // Overflow — discard and wait for next delimiter.
          accum_len = 0;
        }
      }
    }
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int uart_l2_transport_init(const char *device_path, int baud_rate,
                           uart_l2_rx_cb rx_cb, void *rx_ctx) {
  if (s_fd >= 0) {
    bm_log_warn("uart_l2: already initialized");
    return -1;
  }

  // Stash these so the RX thread can reopen the port after a hangup.
  snprintf(s_dev_path, sizeof(s_dev_path), "%s", device_path);
  s_baud = baud_rate;

  s_fd = serial_open(device_path, baud_rate);
  if (s_fd < 0) {
    return -1;
  }

  s_rx_cb = rx_cb;
  s_rx_ctx = rx_ctx;
  s_rx_running = true;

  if (pthread_create(&s_rx_thread, nullptr, rx_thread_func, nullptr) != 0) {
    bm_log_error("uart_l2: pthread_create failed: %s", strerror(errno));
    close(s_fd);
    s_fd = -1;
    s_rx_running = false;
    return -1;
  }

  return 0;
}

int uart_l2_send(const uint8_t *l2_frame, size_t l2_len) {
  if (s_fd < 0 || !l2_frame || l2_len == 0) {
    return -1;
  }

  uint8_t wire[FRAME_CODEC_MAX_WIRE_SIZE];
  size_t wire_len = frame_encode(wire, sizeof(wire), l2_frame, l2_len);
  if (wire_len == 0) {
    return -1;
  }

  // Write the full wire frame atomically (serialized by mutex).
  pthread_mutex_lock(&s_tx_mutex);
  if (s_fd < 0) {
    // Port is mid-reopen after a hangup; drop quietly rather than log per TX.
    pthread_mutex_unlock(&s_tx_mutex);
    return -1;
  }
  const uint8_t *p = wire;
  size_t remaining = wire_len;
  int result = 0;
  while (remaining > 0) {
    ssize_t written = write(s_fd, p, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      bm_log_error("uart_l2: write error: %s", strerror(errno));
      result = -1;
      break;
    }
    p += written;
    remaining -= (size_t)written;
  }
  pthread_mutex_unlock(&s_tx_mutex);

  return result;
}

void uart_l2_transport_deinit(void) {
  if (!s_rx_running) {
    return;
  }

  // Clear this first: it also breaks the RX thread out of a reopen retry.
  s_rx_running = false;

  // The RX thread polls with a timeout, so it exits within one poll period.
  pthread_mutex_lock(&s_tx_mutex);
  if (s_fd >= 0) {
    close(s_fd);
    s_fd = -1;
  }
  pthread_mutex_unlock(&s_tx_mutex);

  pthread_join(s_rx_thread, nullptr);

  s_rx_cb = nullptr;
  s_rx_ctx = nullptr;
}
