#include "uart_l2_transport.h"
#include "cobs.h"
#include "frame_codec.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static int s_fd = -1;
static pthread_t s_rx_thread;
static bool s_rx_running = false;
static uart_l2_rx_cb s_rx_cb = nullptr;
static void *s_rx_ctx = nullptr;
static pthread_mutex_t s_tx_mutex = PTHREAD_MUTEX_INITIALIZER;

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
  default:
    return B0; // unsupported
  }
}

/// Open and configure a serial port for raw 8N1 operation.
/// Returns the file descriptor, or -1 on error.
static int serial_open(const char *path, int baud) {
  speed_t speed = baud_to_speed(baud);
  if (speed == B0) {
    fprintf(stderr, "uart_l2: unsupported baud rate %d\n", baud);
    return -1;
  }

  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "uart_l2: open(%s) failed: %s\n", path, strerror(errno));
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
    fprintf(stderr, "uart_l2: tcgetattr failed: %s\n", strerror(errno));
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
    fprintf(stderr, "uart_l2: tcsetattr failed: %s\n", strerror(errno));
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

  while (s_rx_running) {
    ssize_t n = read(s_fd, read_buf, sizeof(read_buf));
    if (n <= 0) {
      if (n == 0 || errno == EAGAIN || errno == EINTR) {
        continue;
      }
      // Fatal read error — stop.
      fprintf(stderr, "uart_l2: read error: %s\n", strerror(errno));
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
    fprintf(stderr, "uart_l2: already initialized\n");
    return -1;
  }

  s_fd = serial_open(device_path, baud_rate);
  if (s_fd < 0) {
    return -1;
  }

  s_rx_cb = rx_cb;
  s_rx_ctx = rx_ctx;
  s_rx_running = true;

  if (pthread_create(&s_rx_thread, nullptr, rx_thread_func, nullptr) != 0) {
    fprintf(stderr, "uart_l2: pthread_create failed: %s\n", strerror(errno));
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
  const uint8_t *p = wire;
  size_t remaining = wire_len;
  int result = 0;
  while (remaining > 0) {
    ssize_t written = write(s_fd, p, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "uart_l2: write error: %s\n", strerror(errno));
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
  if (s_fd < 0) {
    return;
  }

  s_rx_running = false;
  // The RX thread is blocked on read() — closing the fd will unblock it.
  close(s_fd);
  s_fd = -1;

  pthread_join(s_rx_thread, nullptr);

  s_rx_cb = nullptr;
  s_rx_ctx = nullptr;
}
