#include "socket_io.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int socket_write_all(int fd, const void *data, size_t length) {
  const unsigned char *cursor = data;
  while (length) {
#ifdef MSG_NOSIGNAL
    ssize_t wrote = send(fd, cursor, length, MSG_NOSIGNAL);
#else
    ssize_t wrote = write(fd, cursor, length);
#endif
    if (wrote < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    if (wrote == 0) return -EIO;
    cursor += (size_t)wrote;
    length -= (size_t)wrote;
  }
  return 0;
}

int socket_write_line(int fd, const char *data, size_t length) {
  if (!data || length == SIZE_MAX) return -EINVAL;
  char *line = malloc(length + 1);
  if (!line) return -ENOMEM;
  memcpy(line, data, length);
  line[length] = '\n';
  int rc = socket_write_all(fd, line, length + 1);
  free(line);
  return rc;
}

ssize_t socket_read_line(int fd, char *buffer, size_t capacity, int timeout_ms) {
  if (!buffer || capacity < 2) return -EINVAL;
  size_t used = 0;
  while (used + 1 < capacity) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLHUP};
    int ready;
    do {
      ready = poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready == 0) return -ETIMEDOUT;
    if (ready < 0) return -errno;

    ssize_t got = read(fd, buffer + used, capacity - 1 - used);
    if (got < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    if (got == 0) {
      if (used == 0) return 0;
      break;
    }

    char *newline = memchr(buffer + used, '\n', (size_t)got);
    used += (size_t)got;
    if (newline) {
      used = (size_t)(newline - buffer);
      break;
    }
  }

  if (used + 1 >= capacity) return -EMSGSIZE;
  if (used > 0 && buffer[used - 1] == '\r') used--;
  buffer[used] = '\0';
  return (ssize_t)used;
}
