#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <linux/input.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SECURITY_DEFAULT "/config/postmerkos/security.json"
#define CONTROLS_DEFAULT "/run/postmerkos/hardware-controls.json"
#define STATUS_DEFAULT "/run/postmerkos/button-status.json"
#define BIT_WORD(n) ((n) / (8U * sizeof(unsigned long)))
#define BIT_MASK(n) (1UL << ((n) % (8U * sizeof(unsigned long))))

static volatile sig_atomic_t running = 1;

struct reset_policy {
  bool enabled;
  unsigned int hold_seconds;
};

struct reset_control {
  char backend[24];
  char path[256];
  char device[256];
  int code;
  int active;
  bool available;
  bool verified;
};

static void stop_handler(int signal_number) {
  (void)signal_number;
  running = 0;
}

static long long monotonic_ms(void) {
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (long long)value.tv_sec * 1000LL + value.tv_nsec / 1000000LL;
}

static struct json_object *member(struct json_object *object, const char *key) {
  struct json_object *value = NULL;
  return object && json_object_is_type(object, json_type_object) &&
      json_object_object_get_ex(object, key, &value) ? value : NULL;
}

static const char *string_member(struct json_object *object, const char *key,
                                 const char *fallback) {
  struct json_object *value = member(object, key);
  return value && json_object_is_type(value, json_type_string)
      ? json_object_get_string(value) : fallback;
}

static int int_member(struct json_object *object, const char *key, int fallback) {
  struct json_object *value = member(object, key);
  return value && json_object_is_type(value, json_type_int)
      ? json_object_get_int(value) : fallback;
}

static bool bool_member(struct json_object *object, const char *key,
                        bool fallback) {
  struct json_object *value = member(object, key);
  return value && json_object_is_type(value, json_type_boolean)
      ? json_object_get_boolean(value) : fallback;
}

static void load_policy(struct reset_policy *policy) {
  policy->enabled = true;
  policy->hold_seconds = 10;
  const char *path = getenv("POSTMERKOS_SECURITY_FILE");
  if (!path || !*path) path = SECURITY_DEFAULT;
  struct json_object *root = json_object_from_file(path);
  if (!root) return;
  struct json_object *button = member(root, "reset_button");
  if (button) {
    policy->enabled = bool_member(button, "enabled", true);
    int seconds = int_member(button, "hold_seconds", 10);
    if (seconds >= 3 && seconds <= 60) policy->hold_seconds = (unsigned int)seconds;
  }
  json_object_put(root);
}

static bool load_control(struct reset_control *control) {
  memset(control, 0, sizeof(*control));
  snprintf(control->backend, sizeof(control->backend), "unidentified");
  const char *path = getenv("POSTMERKOS_HARDWARE_CONTROLS");
  if (!path || !*path) path = CONTROLS_DEFAULT;
  struct json_object *root = json_object_from_file(path);
  if (!root) return false;
  struct json_object *button = member(root, "reset_button");
  if (button) {
    snprintf(control->backend, sizeof(control->backend), "%s",
             string_member(button, "backend", "unidentified"));
    snprintf(control->path, sizeof(control->path), "%s",
             string_member(button, "path", ""));
    snprintf(control->device, sizeof(control->device), "%s",
             string_member(button, "device", ""));
    control->code = int_member(button, "code", KEY_RESTART);
    control->active = int_member(button, "active", 1);
    control->available = bool_member(button, "available", false);
    control->verified = bool_member(button, "verified", false);
  }
  json_object_put(root);
  return control->available;
}

static void write_status(const char *state, const struct reset_policy *policy,
                         const struct reset_control *control, int progress,
                         const char *detail) {
  const char *path = getenv("POSTMERKOS_BUTTON_STATUS");
  if (!path || !*path) path = STATUS_DEFAULT;
  struct json_object *root = json_object_new_object();
  json_object_object_add(root, "state", json_object_new_string(state));
  json_object_object_add(root, "enabled", json_object_new_boolean(policy->enabled));
  json_object_object_add(root, "hold_seconds",
                         json_object_new_int((int)policy->hold_seconds));
  json_object_object_add(root, "backend",
                         json_object_new_string(control->backend));
  json_object_object_add(root, "available",
                         json_object_new_boolean(control->available));
  json_object_object_add(root, "verified",
                         json_object_new_boolean(control->verified));
  json_object_object_add(root, "progress", json_object_new_int(progress));
  if (detail) json_object_object_add(root, "detail", json_object_new_string(detail));
  json_object_to_file_ext(path, root, JSON_C_TO_STRING_PRETTY);
  json_object_put(root);
}

static int run_led(const char *action, int value) {
  pid_t child = fork();
  if (child < 0) return -errno;
  if (child == 0) {
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    if (!strcmp(action, "acquire") || !strcmp(action, "release"))
      execl("/usr/sbin/postmerkos-ledctl", "postmerkos-ledctl", action,
            "reset", (char *)NULL);
    else if (value >= 0)
      execl("/usr/sbin/postmerkos-ledctl", "postmerkos-ledctl", action,
            text, (char *)NULL);
    else
      execl("/usr/sbin/postmerkos-ledctl", "postmerkos-ledctl", action,
            (char *)NULL);
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO;
}

static int read_level(const char *path, int *level) {
  FILE *file = fopen(path, "r");
  if (!file) return -errno;
  int value = 0;
  int rc = fscanf(file, "%d", &value) == 1 ? 0 : -EIO;
  fclose(file);
  if (rc == 0) *level = value;
  return rc;
}

static int evdev_pressed(int fd, int code, bool *pressed) {
  if (code < 0 || code > KEY_MAX) return -EINVAL;
  unsigned long bits[BIT_WORD(KEY_MAX) + 1];
  memset(bits, 0, sizeof(bits));
  if (ioctl(fd, EVIOCGKEY(sizeof(bits)), bits) < 0) return -errno;
  *pressed = (bits[BIT_WORD(code)] & BIT_MASK(code)) != 0;
  return 0;
}

static int control_pressed(const struct reset_control *control, int evdev_fd,
                           bool *pressed) {
  if (!strcmp(control->backend, "evdev"))
    return evdev_pressed(evdev_fd, control->code, pressed);
  if (!strcmp(control->backend, "sysfs") || !strcmp(control->backend, "click")) {
    int value = 0;
    int rc = read_level(control->path, &value);
    if (rc == 0) *pressed = value == control->active;
    return rc;
  }
  return -ENODEV;
}

static int open_control(const struct reset_control *control) {
  if (!strcmp(control->backend, "evdev"))
    return open(control->device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  return -1;
}

static void invoke_factory_reset(void) {
  fprintf(stderr, "reset button: hold completed; erasing JFFS2 configuration\n");
  sync();
  execl("/bin/fw_factory_reset", "fw_factory_reset", "--yes", (char *)NULL);
  fprintf(stderr, "reset button: unable to execute fw_factory_reset: %s\n",
          strerror(errno));
}

int main(void) {
  signal(SIGINT, stop_handler);
  signal(SIGTERM, stop_handler);
  mkdir("/run/postmerkos", 0755);

  while (running) {
    struct reset_policy policy;
    struct reset_control control;
    load_policy(&policy);
    load_control(&control);
    if (!policy.enabled) {
      write_status("disabled", &policy, &control, 0,
                   "reset-button policy is disabled");
      sleep(10);
      continue;
    }
    if (!control.available || !control.verified ||
        !strcmp(control.backend, "unidentified")) {
      write_status("unidentified", &policy, &control, 0,
                   "run postmerkos-hwprobe reset-button --watch; destructive reset remains disabled until the mapping is verified");
      fprintf(stderr, "reset button: no verified input mapping; discovery required\n");
      sleep(30);
      continue;
    }

    int evdev_fd = open_control(&control);
    if (!strcmp(control.backend, "evdev") && evdev_fd < 0) {
      control.available = false;
      write_status("unavailable", &policy, &control, 0, strerror(errno));
      sleep(10);
      continue;
    }
    write_status("ready", &policy, &control, 0, NULL);
    bool held = false;
    long long held_since = 0;
    int last_progress = -1;
    while (running) {
      bool pressed = false;
      int rc = control_pressed(&control, evdev_fd, &pressed);
      if (rc != 0) {
        write_status("unavailable", &policy, &control, 0, strerror(-rc));
        break;
      }
      long long now = monotonic_ms();
      if (pressed && !held) {
        held = true;
        held_since = now;
        last_progress = -1;
        run_led("acquire", -1);
        fprintf(stderr, "reset button: held; release to cancel\n");
      }
      if (pressed && held) {
        long long elapsed = now - held_since;
        int progress = (int)((elapsed * 100LL) /
                             ((long long)policy.hold_seconds * 1000LL));
        if (progress > 100) progress = 100;
        int display_progress = progress >= 100 ? 100 : (progress / 10) * 10;
        if (display_progress != last_progress) {
          run_led("reset-progress", display_progress);
          write_status("countdown", &policy, &control, display_progress,
                       "release the button to cancel");
          last_progress = display_progress;
        }
        if (elapsed >= (long long)policy.hold_seconds * 1000LL) {
          run_led("all-off", -1);
          write_status("triggered", &policy, &control, 100,
                       "factory reset requested");
          if (evdev_fd >= 0) close(evdev_fd);
          invoke_factory_reset();
          return 1;
        }
      } else if (!pressed && held) {
        held = false;
        held_since = 0;
        last_progress = -1;
        run_led("release", -1);
        write_status("ready", &policy, &control, 0, "countdown cancelled");
        fprintf(stderr, "reset button: released; reset cancelled\n");
      }
      usleep(100000);
    }
    if (evdev_fd >= 0) close(evdev_fd);
    run_led("release", -1);
    sleep(2);
  }
  return 0;
}
