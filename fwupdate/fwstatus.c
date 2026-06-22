#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void append_line(const char *path, const char *line) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    (void)write(fd, line, strlen(line));
    (void)write(fd, "\n", 1);
    close(fd);
}

int main(int argc, char **argv) {
    const char *state = argc > 1 ? argv[1] : "unknown";
    const char *stage = argc > 2 ? argv[2] : "unknown";
    const char *progress = argc > 3 ? argv[3] : "0";
    const char *message = argc > 4 ? argv[4] : "";
    char line[768];
    time_t now = time(NULL);
    snprintf(line, sizeof(line), "%ld|%s|%s|%s|%s", (long)now, state,
             stage, progress, message);
    append_line("/run/fwupdate/live.log", line);
    if (access("/usr/sbin/postmerkos-ledctl", X_OK) == 0) {
        pid_t child = fork();
        if (child == 0) {
            if (!strcmp(state, "error"))
                execl("/usr/sbin/postmerkos-ledctl", "postmerkos-ledctl", "error", (char *)NULL);
            else if (!strcmp(state, "success"))
                execl("/usr/sbin/postmerkos-ledctl", "postmerkos-ledctl", "success", (char *)NULL);
            else
                execl("/usr/sbin/postmerkos-ledctl", "postmerkos-ledctl", "progress", progress, (char *)NULL);
            _exit(127);
        }
    }
    int console = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (console >= 0) {
        dprintf(console, "\r\nFWUPDATE [%s %s%%] %s\r\n", stage, progress,
                message);
        close(console);
    }
    return 0;
}
