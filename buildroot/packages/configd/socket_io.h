#ifndef POSTMERKOS_SOCKET_IO_H
#define POSTMERKOS_SOCKET_IO_H

#include <stddef.h>
#include <sys/types.h>

int socket_write_all(int fd, const void *data, size_t length);
int socket_write_line(int fd, const char *data, size_t length);
ssize_t socket_read_line(int fd, char *buffer, size_t capacity, int timeout_ms);

#endif
