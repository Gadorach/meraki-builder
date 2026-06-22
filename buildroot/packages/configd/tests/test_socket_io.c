#include "socket_io.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(pair[0]);
    assert(socket_write_all(pair[1], "{\"type\":", 8) == 0);
    usleep(1000);
    assert(socket_write_all(pair[1], "\"session\"}\n", 11) == 0);
    close(pair[1]);
    _exit(0);
  }
  close(pair[1]);
  char buffer[64];
  ssize_t got = socket_read_line(pair[0], buffer, sizeof(buffer), 1000);
  assert(got == 18);
  assert(strcmp(buffer, "{\"type\":\"session\"}") == 0);
  close(pair[0]);
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  close(pair[1]);
  int rc = socket_write_line(pair[0], "{}", 2);
  assert(rc == -EPIPE || rc == -ECONNRESET);
  close(pair[0]);

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(socket_write_all(pair[1], "1234567", 7) == 0);
  got = socket_read_line(pair[0], buffer, 8, 1000);
  assert(got == -EMSGSIZE);
  close(pair[0]);
  close(pair[1]);

  puts("configd socket framing tests passed");
  return 0;
}
