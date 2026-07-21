#include "websocket.h"
#include <libwebsockets.h>
#include "auth.h"
#include "config_apply.h"
#include "config_file.h"
#include "compatibility.h"
#include "configd.h"
#include "network.h"
#include "port_clone.h"
#include "result.h"
#include "roles.h"
#include "release.h"
#include "ssh_keys.h"
#include "session.h"
#include "status.h"
#include "system_ops.h"
#include "system_identity.h"
#include "service_ops.h"
#include "telemetry.h"
#include "time_ops.h"
#include "validation.h"

#include <libpostmerkos.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <stdbool.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_FIRMWARE_UPLOAD (16U * 1024U * 1024U)
#define TERMINAL_STREAM_LIMIT (1024U * 1024U)
#define TERMINAL_STREAM_TIMEOUT_MS 15000LL

struct queued_reply {
  char *text;
  struct queued_reply *next;
};

struct per_session_data {
  bool authenticated;
  enum postmerkos_role role;
  char username[65];
  char session_token[65];
  bool send_initial_status;
  bool send_initial_config;
  bool send_status;
  bool send_config;
  bool telemetry_subscribed;
  struct queued_reply *reply_head;
  struct queued_reply *reply_tail;
  char *receive_buffer;
  size_t receive_length;

  bool upload_active;
  bool upload_ready;
  bool upload_handed_off;
  int upload_fd;
  char upload_path[256];
  char upload_manifest_path[256];
  char upload_name[128];
  char upload_overlay[16];
  char upload_token[40];
  size_t upload_expected;
  size_t upload_received;
  bool upload_force;
  bool upload_accept_untested;

  bool terminal_active;
  int terminal_fd;
  pid_t terminal_pid;
  char terminal_token[40];
  size_t terminal_bytes;
  long long terminal_deadline_ms;
};

static struct lws *clients[MAX_CLIENTS];
static size_t client_count;
static bool status_pending;
static bool config_pending;
static char *cached_status_json;
static char *cached_config_json;
static long long config_mtime_ns;
static int configured_status_interval = 3;
static struct lws_sorted_usec_list status_sul;
static struct lws_sorted_usec_list config_sul;
static struct lws_sorted_usec_list terminal_sul;
static bool terminal_poll_scheduled;
static struct lws_sorted_usec_list telemetry_sul;
static bool telemetry_stream_scheduled;
static struct lws_context *ws_context;
static struct lws *upload_owner;

#define AUTH_BUCKET_MAX 64
#define AUTH_BLOCK_BASE_MS 500LL
#define AUTH_BLOCK_MAX_MS 30000LL
struct auth_bucket {
  char key[128];
  unsigned int failures;
  long long blocked_until_ms;
  long long last_seen_ms;
};
static struct auth_bucket auth_buckets[AUTH_BUCKET_MAX];

enum firmware_job_state {
  FIRMWARE_JOB_IDLE = 0,
  FIRMWARE_JOB_VALIDATING,
  FIRMWARE_JOB_READY,
  FIRMWARE_JOB_FAILED
};

struct firmware_validation_job {
  enum firmware_job_state state;
  pid_t pid;
  char username[65];
  char token[40];
  char path[256];
  char manifest_path[256];
  char name[128];
  char overlay[16];
  char error_path[256];
  char error[256];
  size_t expected;
  size_t received;
  bool force;
  bool accept_untested;
};
static struct firmware_validation_job firmware_job;

static void websocket_log(const char *format, ...) {
  char message[512];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  fprintf(stderr, "%s websocket: %s\n", get_time(), message);

  const char *path = getenv("POSTMERKOS_WEBSOCKET_LOG");
  if (!path || !*path) path = "/run/postmerkos/websocket.log";
  struct stat status;
  const char *mode = stat(path, &status) == 0 && status.st_size > 65536 ? "w" : "a";
  FILE *file = fopen(path, mode);
  if (file) {
    fprintf(file, "%s websocket: %s\n", get_time(), message);
    fclose(file);
  }
}

static bool management_address_changed(const struct network_runtime *before,
                                       const struct network_runtime *after) {
  if (!before || !after) return false;
  return strcmp(before->applied.address, after->applied.address) ||
         before->applied.prefix != after->applied.prefix;
}

static void rebind_management_services(const struct network_runtime *before,
                                       const struct network_runtime *after) {
  if (!before || !after || dry_run || !after->applied.address[0]) return;
  const char *script = getenv("CONFIGD_NETWORK_REBIND");
  if (!script || !*script) script = "/usr/sbin/postmerkos-network-rebind";
  if (access(script, X_OK) != 0) {
    fprintf(stderr, "%s network: rebind hook is unavailable: %s\n",
            get_time(), script);
    return;
  }
  char old_cidr[32];
  char new_cidr[32];
  snprintf(old_cidr, sizeof(old_cidr), "%s/%u",
           before->applied.address[0] ? before->applied.address : "0.0.0.0",
           before->applied.prefix);
  snprintf(new_cidr, sizeof(new_cidr), "%s/%u", after->applied.address,
           after->applied.prefix);
  fprintf(stderr, "%s network: management address changed %s -> %s; rebinding web service\n",
          get_time(), old_cidr, new_cidr);
  pid_t child = fork();
  if (child == 0) {
    execl(script, script, old_cidr, new_cidr, after->source, (char *)NULL);
    _exit(127);
  }
  if (child < 0) {
    fprintf(stderr, "%s network: unable to start rebind hook: %s\n",
            get_time(), strerror(errno));
    return;
  }
  int status = 0;
  if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0)
    fprintf(stderr, "%s network: web-service rebind hook failed\n", get_time());
}

char *wrap_message(const char *type, struct json_object *data,
                   struct json_object *request_id) {
  struct json_object *message = json_object_new_object();
  if (request_id)
    json_object_object_add(message, "id", json_object_get(request_id));
  json_object_object_add(message, "type", json_object_new_string(type));
  json_object_object_add(message, "data",
                         data ? json_object_get(data) : json_object_new_null());
  const char *text = json_object_to_json_string_ext(message,
                                                    JSON_C_TO_STRING_PLAIN);
  char *result = strdup(text);
  json_object_put(message);
  return result;
}

static void add_client(struct lws *wsi) {
  if (client_count < MAX_CLIENTS) clients[client_count++] = wsi;
}

static void remove_client(struct lws *wsi) {
  for (size_t i = 0; i < client_count; i++) {
    if (clients[i] == wsi) {
      clients[i] = clients[--client_count];
      return;
    }
  }
}

static void request_writable_all(void) {
  for (size_t i = 0; i < client_count; i++)
    lws_callback_on_writable(clients[i]);
}

static int ws_send(struct lws *wsi, const char *text) {
  if (!text) return -EINVAL;
  size_t length = strlen(text);
  unsigned char *buffer = malloc(LWS_PRE + length);
  if (!buffer) return -ENOMEM;
  memcpy(buffer + LWS_PRE, text, length);
  int written = lws_write(wsi, buffer + LWS_PRE, length, LWS_WRITE_TEXT);
  free(buffer);
  return written == (int)length ? 0 : -EIO;
}

static void refresh_status_cache(bool broadcast) {
  struct json_object *status = get_status();
  char *message = wrap_message("status", status, NULL);
  json_object_put(status);
  if (!message) return;
  if (!cached_status_json || strcmp(cached_status_json, message)) {
    free(cached_status_json);
    cached_status_json = message;
    if (broadcast) {
      status_pending = true;
      request_writable_all();
    }
  } else free(message);
}

static void refresh_config_cache(struct json_object *config, bool broadcast) {
  char *message = wrap_message("config", config, NULL);
  if (!message) return;
  free(cached_config_json);
  cached_config_json = message;
  if (broadcast) {
    config_pending = true;
    request_writable_all();
  }
}

static void status_poll_cb(struct lws_sorted_usec_list *sul) {
  refresh_status_cache(true);
  lws_sul_schedule(ws_context, 0, sul, status_poll_cb,
      (lws_usec_t)configured_status_interval * LWS_USEC_PER_SEC);
}

static struct json_object *load_known_good_configuration(void) {
  char backup[512];
  int n = snprintf(backup, sizeof(backup), "%s.bak", config_file);
  if (n < 0 || (size_t)n >= sizeof(backup)) return NULL;
  struct json_object *config = load_json_file(backup);
  char error[256] = {0};
  if (!config || validate_configuration(config, error, sizeof(error)) != 0) {
    if (config) json_object_put(config);
    return NULL;
  }
  return config;
}

static void config_poll_cb(struct lws_sorted_usec_list *sul) {
  long long current_mtime = 0;
  if (config_file_mtime(&current_mtime) == 0 &&
      current_mtime != config_mtime_ns) {
    struct json_object *candidate = load_config_file();
    struct json_object *known_good = load_known_good_configuration();
    char error[256] = {0};
    bool accepted = candidate &&
        validate_configuration(candidate, error, sizeof(error)) == 0;

    struct network_runtime before = *network_manager_runtime();
    struct apply_result result;
    apply_result_init(&result);
    if (accepted) {
      int rc = config_apply_full(candidate, &result);
      accepted = rc == 0 && apply_result_success(&result);
      if (!accepted && !error[0])
        snprintf(error, sizeof(error),
                 "externally edited configuration could not be applied");
    } else if (!error[0]) {
      snprintf(error, sizeof(error),
               "externally edited configuration could not be parsed");
    }

    if (accepted) {
      /* Recommit through the atomic writer.  This refreshes the known-good
       * backup only after validation and all required runtime operations have
       * succeeded. */
      if (save_config_file(candidate, error, sizeof(error)) != 0)
        accepted = false;
    }

    if (!accepted) {
      apply_result_cleanup(&result);
      apply_result_init(&result);
      if (known_good) {
        int rollback_rc = config_apply_full(known_good, &result);
        char save_error[256] = {0};
        if (rollback_rc == 0 && apply_result_success(&result) &&
            save_config_file(known_good, save_error, sizeof(save_error)) == 0) {
          config_file_set_runtime_error(error);
          refresh_config_cache(known_good, true);
        } else {
          config_file_set_runtime_error(
              "external configuration was rejected and known-good restoration was incomplete");
        }
      } else {
        config_file_set_runtime_error(
            "external configuration was rejected and no valid known-good backup is available");
      }
    } else {
      config_file_set_runtime_error(NULL);
      refresh_config_cache(candidate, true);
    }

    const struct network_runtime *after = network_manager_runtime();
    if (management_address_changed(&before, after))
      rebind_management_services(&before, after);
    if (json_object_array_length(result.warnings) > 0)
      fprintf(stderr, "%s config reload: %s\n", get_time(),
              json_object_to_json_string(result.warnings));
    apply_result_cleanup(&result);
    refresh_status_cache(true);
    if (candidate) json_object_put(candidate);
    if (known_good) json_object_put(known_good);
    (void)config_file_mtime(&config_mtime_ns);
  }
  lws_sul_schedule(ws_context, 0, sul, config_poll_cb,
                   10 * LWS_USEC_PER_SEC);
}

static struct json_object *error_data_status(int status, const char *message,
                                             const char *detail) {
  struct json_object *data = json_object_new_object();
  json_object_object_add(data, "status", json_object_new_int(status));
  json_object_object_add(data, "message",
                         json_object_new_string(message ? message : "Error"));
  if (detail && *detail)
    json_object_object_add(data, "detail", json_object_new_string(detail));
  return data;
}

static void queue_response(struct lws *wsi, struct per_session_data *session,
                           const char *type, struct json_object *data,
                           struct json_object *request_id) {
  struct queued_reply *reply = calloc(1, sizeof(*reply));
  if (!reply) return;
  reply->text = wrap_message(type, data, request_id);
  if (!reply->text) { free(reply); return; }
  if (session->reply_tail) session->reply_tail->next = reply;
  else session->reply_head = reply;
  session->reply_tail = reply;
  lws_callback_on_writable(wsi);
}

/* ---- live telemetry graph stream (subscribe-gated, server-initiated push) ---- */

static int telemetry_stream_interval_seconds(void) {
  const char *value = getenv("CONFIGD_TELEMETRY_STREAM_INTERVAL");
  int seconds = value && *value ? atoi(value) : 0;
  return seconds > 0 ? seconds : 2;
}

static size_t telemetry_subscriber_count(void) {
  size_t subscribers = 0;
  for (size_t i = 0; i < client_count; i++) {
    struct per_session_data *session = lws_wsi_user(clients[i]);
    if (session && session->telemetry_subscribed) subscribers++;
  }
  return subscribers;
}

/* Build one frame and enqueue it to every subscribed client. */
static void telemetry_push_subscribers(void) {
  struct json_object *data = telemetry_stream_json();
  if (!data) return;
  for (size_t i = 0; i < client_count; i++) {
    struct per_session_data *session = lws_wsi_user(clients[i]);
    if (session && session->telemetry_subscribed)
      queue_response(clients[i], session, "telemetry", data, NULL);
  }
  json_object_put(data);
}

/* Fires every CONFIGD_TELEMETRY_STREAM_INTERVAL s while at least one client is
   subscribed; refreshes the Click snapshot then pushes. Stops rescheduling (goes
   idle, no extra Click reads) once the last subscriber leaves. */
static void telemetry_stream_cb(struct lws_sorted_usec_list *sul) {
  if (telemetry_subscriber_count() == 0) {
    telemetry_stream_scheduled = false;
    return;
  }
  telemetry_tick();
  telemetry_push_subscribers();
  lws_sul_schedule(ws_context, 0, sul, telemetry_stream_cb,
      (lws_usec_t)telemetry_stream_interval_seconds() * LWS_USEC_PER_SEC);
}

static void telemetry_stream_ensure_scheduled(void) {
  if (telemetry_stream_scheduled || !ws_context) return;
  telemetry_stream_scheduled = true;
  lws_sul_schedule(ws_context, 0, &telemetry_sul, telemetry_stream_cb,
      (lws_usec_t)telemetry_stream_interval_seconds() * LWS_USEC_PER_SEC);
}

static void free_replies(struct per_session_data *session) {
  while (session->reply_head) {
    struct queued_reply *reply = session->reply_head;
    session->reply_head = reply->next;
    free(reply->text);
    free(reply);
  }
  session->reply_tail = NULL;
}

static void queue_error(struct lws *wsi, struct per_session_data *session,
                        struct json_object *request_id, int status,
                        const char *message, const char *detail) {
  struct json_object *data = error_data_status(status, message, detail);
  queue_response(wsi, session, "error", data, request_id);
  json_object_put(data);
}

static void queue_bad_request(struct lws *wsi,
                              struct per_session_data *session,
                              struct json_object *request_id,
                              const char *detail) {
  queue_error(wsi, session, request_id, 400, "Bad Request", detail);
}

static bool require_capability(struct lws *wsi,
                               struct per_session_data *session,
                               struct json_object *request_id,
                               const char *capability) {
  if (role_has_capability(session->role, capability)) return true;
  queue_error(wsi, session, request_id, 403, "Forbidden",
              "the current account does not have permission for this operation");
  return false;
}

static long long terminal_monotonic_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static struct auth_bucket *auth_bucket_for(const char *key, bool create) {
  struct auth_bucket *oldest = &auth_buckets[0];
  for (size_t i = 0; i < AUTH_BUCKET_MAX; i++) {
    if (auth_buckets[i].key[0] && !strcmp(auth_buckets[i].key, key))
      return &auth_buckets[i];
    if (!auth_buckets[i].key[0]) {
      if (!create) return NULL;
      snprintf(auth_buckets[i].key, sizeof(auth_buckets[i].key), "%s", key);
      return &auth_buckets[i];
    }
    if (auth_buckets[i].last_seen_ms < oldest->last_seen_ms)
      oldest = &auth_buckets[i];
  }
  if (!create) return NULL;
  memset(oldest, 0, sizeof(*oldest));
  snprintf(oldest->key, sizeof(oldest->key), "%s", key);
  return oldest;
}

static void auth_account_key(const char *username, char *out, size_t size) {
  snprintf(out, size, "account:%s",
           username && *username ? username : "<invalid>");
}

static void auth_peer_key(struct lws *wsi, char *out, size_t size) {
  char peer[80] = {0};
  if (!lws_get_peer_simple(wsi, peer, sizeof(peer)) || !peer[0])
    snprintf(peer, sizeof(peer), "%s", "<unknown>");
  snprintf(out, size, "peer:%s", peer);
}

static long long auth_retry_after_key(const char *key, long long now) {
  struct auth_bucket *bucket = auth_bucket_for(key, false);
  if (!bucket || bucket->blocked_until_ms <= now) return 0;
  bucket->last_seen_ms = now;
  return bucket->blocked_until_ms - now;
}

static long long auth_retry_after_ms(struct lws *wsi, const char *username) {
  char account[128], peer[128];
  auth_account_key(username, account, sizeof(account));
  auth_peer_key(wsi, peer, sizeof(peer));
  long long now = terminal_monotonic_ms();
  long long account_wait = auth_retry_after_key(account, now);
  long long peer_wait = auth_retry_after_key(peer, now);
  long long global_wait = auth_retry_after_key("global", now);
  long long wait = account_wait > peer_wait ? account_wait : peer_wait;
  return wait > global_wait ? wait : global_wait;
}

static void auth_record_failure_key(const char *key, long long now) {
  struct auth_bucket *bucket = auth_bucket_for(key, true);
  bucket->last_seen_ms = now;
  bucket->failures++;
  if (bucket->failures >= 3) {
    unsigned int shift = bucket->failures - 3;
    if (shift > 6) shift = 6;
    long long delay = AUTH_BLOCK_BASE_MS << shift;
    if (delay > AUTH_BLOCK_MAX_MS) delay = AUTH_BLOCK_MAX_MS;
    bucket->blocked_until_ms = now + delay;
  }
}

static void auth_record_failure(struct lws *wsi, const char *username) {
  char account[128], peer[128];
  auth_account_key(username, account, sizeof(account));
  auth_peer_key(wsi, peer, sizeof(peer));
  long long now = terminal_monotonic_ms();
  auth_record_failure_key(account, now);
  auth_record_failure_key(peer, now);
  auth_record_failure_key("global", now);
}

static void auth_clear_bucket(const char *key) {
  struct auth_bucket *bucket = auth_bucket_for(key, false);
  if (bucket) memset(bucket, 0, sizeof(*bucket));
}

static void auth_record_success(struct lws *wsi, const char *username) {
  char account[128], peer[128];
  auth_account_key(username, account, sizeof(account));
  auth_peer_key(wsi, peer, sizeof(peer));
  auth_clear_bucket(account);
  auth_clear_bucket(peer);
  auth_clear_bucket("global");
}

static void terminal_stop(struct per_session_data *session, bool kill_process) {
  if (!session) return;
  if (kill_process && session->terminal_pid > 0) {
    kill(-session->terminal_pid, SIGKILL);
    kill(session->terminal_pid, SIGKILL);
  }
  if (session->terminal_fd >= 0) close(session->terminal_fd);
  if (session->terminal_pid > 0) waitpid(session->terminal_pid, NULL, WNOHANG);
  session->terminal_active = false;
  session->terminal_fd = -1;
  session->terminal_pid = 0;
  session->terminal_bytes = 0;
  session->terminal_deadline_ms = 0;
}

static void terminal_emit_exit(struct lws *wsi,
                               struct per_session_data *session,
                               int exit_code, bool timed_out,
                               bool truncated) {
  struct json_object *data = json_object_new_object();
  json_object_object_add(data, "token",
                         json_object_new_string(session->terminal_token));
  json_object_object_add(data, "exit_code", json_object_new_int(exit_code));
  json_object_object_add(data, "timed_out", json_object_new_boolean(timed_out));
  json_object_object_add(data, "truncated", json_object_new_boolean(truncated));
  queue_response(wsi, session, "terminal_exit", data, NULL);
  json_object_put(data);
}

static void terminal_poll_cb(struct lws_sorted_usec_list *sul) {
  (void)sul;
  terminal_poll_scheduled = false;
  bool active = false;
  for (size_t i = 0; i < client_count; i++) {
    struct lws *wsi = clients[i];
    struct per_session_data *session = lws_wsi_user(wsi);
    if (!session || !session->terminal_active) continue;
    active = true;
    bool truncated = false;
    char buffer[2048];
    for (;;) {
      ssize_t got = read(session->terminal_fd, buffer, sizeof(buffer));
      if (got > 0) {
        size_t keep = (size_t)got;
        if (session->terminal_bytes + keep > TERMINAL_STREAM_LIMIT) {
          keep = TERMINAL_STREAM_LIMIT - session->terminal_bytes;
          truncated = true;
        }
        if (keep) {
          struct json_object *data = json_object_new_object();
          json_object_object_add(data, "token",
              json_object_new_string(session->terminal_token));
          json_object_object_add(data, "output",
              json_object_new_string_len(buffer, (int)keep));
          queue_response(wsi, session, "terminal_output", data, NULL);
          json_object_put(data);
          session->terminal_bytes += keep;
        }
        if (truncated || session->terminal_bytes >= TERMINAL_STREAM_LIMIT) break;
        continue;
      }
      if (got < 0 && errno == EINTR) continue;
      break;
    }

    bool timed_out = terminal_monotonic_ms() >= session->terminal_deadline_ms;
    if (truncated || timed_out) {
      pid_t pid = session->terminal_pid;
      if (pid > 0) { kill(-pid, SIGKILL); kill(pid, SIGKILL); }
    }
    int status = 0;
    pid_t waited = waitpid(session->terminal_pid, &status, WNOHANG);
    if (waited == session->terminal_pid || truncated || timed_out) {
      if (waited <= 0) waitpid(session->terminal_pid, &status, 0);
      int exit_code = 127;
      if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
      else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);
      terminal_emit_exit(wsi, session, exit_code, timed_out, truncated);
      terminal_stop(session, false);
      active = false;
      for (size_t n = 0; n < client_count; n++) {
        struct per_session_data *other = lws_wsi_user(clients[n]);
        if (other && other->terminal_active) { active = true; break; }
      }
    }
  }
  if (active && ws_context) {
    terminal_poll_scheduled = true;
    lws_sul_schedule(ws_context, 0, &terminal_sul, terminal_poll_cb,
                     100000);
  }
}

static int terminal_start(struct lws *wsi, struct per_session_data *session,
                          const char *command, char *error,
                          size_t error_size) {
  if (!command || !*command || strlen(command) > 4096) {
    snprintf(error, error_size, "command must contain 1-4096 characters");
    return -EINVAL;
  }
  if (session->terminal_active) {
    snprintf(error, error_size, "a terminal command is already running");
    return -EBUSY;
  }
  int output_pipe[2];
  if (pipe(output_pipe) != 0) {
    snprintf(error, error_size, "%s", strerror(errno));
    return -errno;
  }
  pid_t child = fork();
  if (child == 0) {
    setpgid(0, 0);
    dup2(output_pipe[1], STDOUT_FILENO);
    dup2(output_pipe[1], STDERR_FILENO);
    int nullfd = open("/dev/null", O_RDONLY);
    if (nullfd >= 0) {
      dup2(nullfd, STDIN_FILENO);
      if (nullfd > STDERR_FILENO) close(nullfd);
    }
    close(output_pipe[0]); close(output_pipe[1]);
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("TERM", "dumb", 1);
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  close(output_pipe[1]);
  if (child < 0) {
    int saved = errno; close(output_pipe[0]);
    snprintf(error, error_size, "%s", strerror(saved));
    return -saved;
  }
  int flags = fcntl(output_pipe[0], F_GETFL, 0);
  if (flags >= 0) fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
  session->terminal_active = true;
  session->terminal_fd = output_pipe[0];
  session->terminal_pid = child;
  session->terminal_bytes = 0;
  session->terminal_deadline_ms = terminal_monotonic_ms() + TERMINAL_STREAM_TIMEOUT_MS;
  snprintf(session->terminal_token, sizeof(session->terminal_token),
           "%ld-%ld-%p", (long)time(NULL), (long)child, (void *)wsi);
  if (!terminal_poll_scheduled && ws_context) {
    terminal_poll_scheduled = true;
    lws_sul_schedule(ws_context, 0, &terminal_sul, terminal_poll_cb, 100000);
  }
  return 0;
}

static bool delta_is_operator_safe(struct json_object *delta) {
  if (!delta || !json_object_is_type(delta, json_type_object)) return false;
  json_object_object_foreach(delta, key, value) {
    (void)value;
    if (strcmp(key, "ports") && strcmp(key, "stp") && strcmp(key, "lacp") &&
        strcmp(key, "multicast")) return false;
  }
  return true;
}

static struct json_object *request_data_object(struct json_object *message) {
  struct json_object *data = NULL;
  if (!json_object_object_get_ex(message, "data", &data) ||
      !json_object_is_type(data, json_type_object)) return NULL;
  return data;
}

static const char *object_string(struct json_object *object, const char *key) {
  struct json_object *value = NULL;
  if (!object || !json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_string)) return NULL;
  return json_object_get_string(value);
}

static bool valid_overlay(const char *value) {
  return value && (!strcmp(value, "preserve") || !strcmp(value, "migrate") ||
                   !strcmp(value, "reset") || !strcmp(value, "image"));
}

static bool valid_artifact_name(const char *name) {
  if (!name || !*name || strlen(name) >= 128 || strchr(name, '/')) return false;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    if (!(isalnum(*p) || *p == '.' || *p == '_' || *p == '-')) return false;
  return true;
}

static int write_manifest_object(const char *path, struct json_object *manifest) {
  if (!path || !manifest || !json_object_is_type(manifest, json_type_object)) return -EINVAL;
  int rc = json_object_to_file_ext(path, manifest, JSON_C_TO_STRING_PRETTY);
  if (rc == 0) chmod(path, 0600);
  return rc == 0 ? 0 : -EIO;
}

static void purge_upload_cache(const char *keep) {
  const char *directory_path = "/run/fwupdate/uploads";
  mkdir("/run/fwupdate", 0700);
  mkdir(directory_path, 0700);
  DIR *directory = opendir(directory_path);
  if (!directory) return;
  time_t now = time(NULL);
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] == '.') continue;
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", directory_path, entry->d_name);
    if (keep && !strcmp(path, keep)) continue;
    struct stat st;
    if (stat(path, &st) == 0 && now >= st.st_mtime && now - st.st_mtime > 3600)
      unlink(path);
  }
  closedir(directory);
}

static void cleanup_upload(struct lws *wsi, struct per_session_data *session) {
  if (session->upload_fd >= 0) close(session->upload_fd);
  session->upload_fd = -1;
  if (!session->upload_handed_off) {
    if (session->upload_path[0]) unlink(session->upload_path);
    if (session->upload_manifest_path[0]) unlink(session->upload_manifest_path);
  }
  session->upload_active = false;
  session->upload_ready = false;
  session->upload_expected = 0;
  session->upload_received = 0;
  session->upload_path[0] = '\0';
  session->upload_manifest_path[0] = '\0';
  if (upload_owner == wsi) upload_owner = NULL;
}

static struct json_object *upload_status_json(
    const struct per_session_data *session) {
  struct json_object *data = json_object_new_object();
  json_object_object_add(data, "active",
                         json_object_new_boolean(session->upload_active));
  json_object_object_add(data, "ready",
                         json_object_new_boolean(session->upload_ready));
  json_object_object_add(data, "token",
                         json_object_new_string(session->upload_token));
  json_object_object_add(data, "name",
                         json_object_new_string(session->upload_name));
  json_object_object_add(data, "received",
                         json_object_new_int64((int64_t)session->upload_received));
  json_object_object_add(data, "expected",
                         json_object_new_int64((int64_t)session->upload_expected));
  int progress = session->upload_expected
      ? (int)((session->upload_received * 100U) / session->upload_expected) : 0;
  json_object_object_add(data, "progress", json_object_new_int(progress));
  return data;
}

static const char *firmware_job_state_name(enum firmware_job_state state) {
  switch (state) {
    case FIRMWARE_JOB_VALIDATING: return "validating";
    case FIRMWARE_JOB_READY: return "ready";
    case FIRMWARE_JOB_FAILED: return "failed";
    default: return "idle";
  }
}

static struct json_object *firmware_job_status_json(void) {
  struct json_object *data = json_object_new_object();
  bool active = firmware_job.state == FIRMWARE_JOB_VALIDATING ||
                firmware_job.state == FIRMWARE_JOB_READY;
  json_object_object_add(data, "active", json_object_new_boolean(active));
  json_object_object_add(data, "validating", json_object_new_boolean(
      firmware_job.state == FIRMWARE_JOB_VALIDATING));
  json_object_object_add(data, "ready", json_object_new_boolean(
      firmware_job.state == FIRMWARE_JOB_READY));
  json_object_object_add(data, "state", json_object_new_string(
      firmware_job_state_name(firmware_job.state)));
  json_object_object_add(data, "token", json_object_new_string(firmware_job.token));
  json_object_object_add(data, "name", json_object_new_string(firmware_job.name));
  json_object_object_add(data, "received",
      json_object_new_int64((int64_t)firmware_job.received));
  json_object_object_add(data, "expected",
      json_object_new_int64((int64_t)firmware_job.expected));
  int progress = firmware_job.expected
      ? (int)((firmware_job.received * 100U) / firmware_job.expected) : 0;
  json_object_object_add(data, "progress", json_object_new_int(progress));
  if (firmware_job.error[0])
    json_object_object_add(data, "error",
                           json_object_new_string(firmware_job.error));
  return data;
}

static void firmware_job_forget(bool unlink_files) {
  if (unlink_files) {
    if (firmware_job.path[0]) unlink(firmware_job.path);
    if (firmware_job.manifest_path[0]) unlink(firmware_job.manifest_path);
  }
  if (firmware_job.error_path[0]) unlink(firmware_job.error_path);
  memset(&firmware_job, 0, sizeof(firmware_job));
}

static void firmware_job_cancel(void) {
  if (firmware_job.state == FIRMWARE_JOB_VALIDATING && firmware_job.pid > 0) {
    kill(-firmware_job.pid, SIGKILL);
    kill(firmware_job.pid, SIGKILL);
    while (waitpid(firmware_job.pid, NULL, 0) < 0 && errno == EINTR) {}
  }
  firmware_job_forget(true);
}

static void firmware_job_notify(const char *type) {
  struct json_object *data = firmware_job_status_json();
  for (size_t i = 0; i < client_count; i++) {
    struct per_session_data *session = lws_wsi_user(clients[i]);
    if (!session || !session->authenticated ||
        !role_has_capability(session->role, "firmware.update")) continue;
    queue_response(clients[i], session, type, data, NULL);
  }
  json_object_put(data);
}

static int firmware_job_start(struct per_session_data *session,
                              char *error, size_t error_size) {
  if (firmware_job.state == FIRMWARE_JOB_VALIDATING ||
      firmware_job.state == FIRMWARE_JOB_READY) {
    snprintf(error, error_size, "another firmware validation is active");
    return -EBUSY;
  }
  if (firmware_job.state == FIRMWARE_JOB_FAILED) firmware_job_forget(true);
  memset(&firmware_job, 0, sizeof(firmware_job));
  firmware_job.state = FIRMWARE_JOB_VALIDATING;
  snprintf(firmware_job.username, sizeof(firmware_job.username), "%s",
           session->username);
  snprintf(firmware_job.token, sizeof(firmware_job.token), "%s",
           session->upload_token);
  snprintf(firmware_job.path, sizeof(firmware_job.path), "%s",
           session->upload_path);
  snprintf(firmware_job.manifest_path, sizeof(firmware_job.manifest_path), "%s",
           session->upload_manifest_path);
  snprintf(firmware_job.name, sizeof(firmware_job.name), "%s",
           session->upload_name);
  snprintf(firmware_job.overlay, sizeof(firmware_job.overlay), "%s",
           session->upload_overlay);
  snprintf(firmware_job.error_path, sizeof(firmware_job.error_path),
           "/run/fwupdate/uploads/%s.verify", firmware_job.token);
  firmware_job.expected = session->upload_expected;
  firmware_job.received = session->upload_received;
  firmware_job.force = session->upload_force;
  firmware_job.accept_untested = session->upload_accept_untested;

  pid_t child = fork();
  if (child < 0) {
    int saved = errno;
    firmware_job_forget(false);
    snprintf(error, error_size, "%s", strerror(saved));
    return -saved;
  }
  if (child == 0) {
    setpgid(0, 0);
    char validation_error[256] = {0};
    int rc = firmware_validate_update(
        firmware_job.path, firmware_job.name,
        firmware_job.manifest_path[0] ? firmware_job.manifest_path : NULL,
        firmware_job.overlay, firmware_job.force,
        firmware_job.accept_untested, validation_error,
        sizeof(validation_error));
    FILE *file = fopen(firmware_job.error_path, "w");
    if (file) {
      fprintf(file, "%s\n", validation_error);
      fclose(file);
    }
    _exit(rc == 0 ? 0 : 1);
  }
  /* Establish the process group from the parent too, closing the race where a
   * cancellation could arrive before the child executes setpgid(). */
  if (setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH)
    websocket_log("firmware validator setpgid failed: %s", strerror(errno));
  firmware_job.pid = child;
  return 0;
}

static void firmware_job_poll(void) {
  if (firmware_job.state != FIRMWARE_JOB_VALIDATING || firmware_job.pid <= 0)
    return;
  int status = 0;
  pid_t waited = waitpid(firmware_job.pid, &status, WNOHANG);
  if (waited == 0) return;
  if (waited < 0) {
    if (errno == EINTR) return;
    snprintf(firmware_job.error, sizeof(firmware_job.error),
             "unable to observe firmware validator: %s", strerror(errno));
    firmware_job.state = FIRMWARE_JOB_FAILED;
  } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    firmware_job.state = FIRMWARE_JOB_READY;
  } else {
    FILE *file = fopen(firmware_job.error_path, "r");
    if (file) {
      while (fgets(firmware_job.error, sizeof(firmware_job.error), file)) {}
      fclose(file);
      firmware_job.error[strcspn(firmware_job.error, "\r\n")] = '\0';
    }
    if (!firmware_job.error[0])
      snprintf(firmware_job.error, sizeof(firmware_job.error),
               "firmware validation failed");
    firmware_job.state = FIRMWARE_JOB_FAILED;
  }
  firmware_job.pid = 0;
  if (firmware_job.error_path[0]) unlink(firmware_job.error_path);
  if (firmware_job.state == FIRMWARE_JOB_FAILED) {
    if (firmware_job.path[0]) unlink(firmware_job.path);
    if (firmware_job.manifest_path[0]) unlink(firmware_job.manifest_path);
    firmware_job.path[0] = '\0';
    firmware_job.manifest_path[0] = '\0';
  }
  firmware_job_notify(firmware_job.state == FIRMWARE_JOB_READY
                        ? "firmware_ready" : "firmware_validation_failed");
}

static void deauthenticate_session(struct lws *wsi,
                                   struct per_session_data *session,
                                   bool revoke_token) {
  if (!session) return;
  if (revoke_token && session->session_token[0])
    session_revoke_token(session->session_token);
  cleanup_upload(wsi, session);
  terminal_stop(session, true);
  session->authenticated = false;
  session->role = POSTMERKOS_ROLE_NONE;
  session->username[0] = '\0';
  session->session_token[0] = '\0';
  session->send_initial_status = false;
  session->send_initial_config = false;
  session->send_status = false;
  session->send_config = false;
}

static bool revalidate_session(struct lws *wsi,
                               struct per_session_data *session,
                               struct json_object *request_id) {
  char username[65] = {0};
  if (!session->authenticated || !session->session_token[0] ||
      session_lookup(session->session_token, (long)time(NULL), username,
                     sizeof(username)) != 0 ||
      strcmp(username, session->username)) {
    deauthenticate_session(wsi, session, false);
    queue_error(wsi, session, request_id, 401, "Unauthorized",
                "session expired or was revoked");
    return false;
  }
  enum postmerkos_role role = role_for_username(username);
  if (role == POSTMERKOS_ROLE_NONE) {
    deauthenticate_session(wsi, session, true);
    queue_error(wsi, session, request_id, 403, "Forbidden",
                "account no longer has a postmerkOS management role");
    return false;
  }
  session->role = role;
  return true;
}

void ws_revoke_user_sessions(const char *username) {
  if (!username || !*username) return;
  session_revoke_user(username);
  if (firmware_job.state != FIRMWARE_JOB_IDLE &&
      !strcmp(firmware_job.username, username))
    firmware_job_cancel();
  for (size_t i = 0; i < client_count; i++) {
    struct lws *wsi = clients[i];
    struct per_session_data *session = lws_wsi_user(wsi);
    if (!session || !session->authenticated ||
        strcmp(session->username, username)) continue;
    deauthenticate_session(wsi, session, false);
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "message",
                           json_object_new_string("Session revoked"));
    queue_response(wsi, session, "auth_required", data, NULL);
    json_object_put(data);
  }
}

static int replace_configuration(struct json_object *candidate,
                                 struct apply_result *result,
                                 char *error, size_t error_size) {
  struct network_runtime before = *network_manager_runtime();
  int rc = config_replace_validate_save_apply(candidate, result,
                                               error, error_size);
  const struct network_runtime *after = network_manager_runtime();
  if (rc == 0 && management_address_changed(&before, after))
    rebind_management_services(&before, after);
  if (rc == 0) {
    char ssh_error[200] = {0};
    if (ssh_keys_render(ssh_error, sizeof(ssh_error)) != 0)
      apply_result_warn(result, "authorized_keys render failed: %s", ssh_error);
  }
  return rc;
}

static int handle_request(struct lws *wsi, struct per_session_data *session,
                          struct json_object *message) {
  if (!json_object_is_type(message, json_type_object)) {
    queue_bad_request(wsi, session, NULL, "request must be a JSON object");
    return 0;
  }
  struct json_object *request_id = NULL;
  json_object_object_get_ex(message, "id", &request_id);
  struct json_object *type_object = NULL;
  if (!json_object_object_get_ex(message, "type", &type_object) ||
      !json_object_is_type(type_object, json_type_string)) {
    queue_bad_request(wsi, session, request_id, "type must be a string");
    return 0;
  }
  const char *type = json_object_get_string(type_object);

  if (!strcmp(type, "hello")) {
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "service", json_object_new_string("configd"));
    json_object_object_add(data, "protocol", json_object_new_int(2));
    json_object_object_add(data, "websocket_protocol",
                           json_object_new_string("configd-ws"));
    json_object_object_add(data, "authentication_required",
                           json_object_new_boolean(!session->authenticated));
    json_object_object_add(data, "firmware",
                           json_object_new_string(release_version()));
    queue_response(wsi, session, "hello", data, request_id);
    json_object_put(data);
    return 0;
  }

  if (!strcmp(type, "ping")) {
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "message", json_object_new_string("pong"));
    queue_response(wsi, session, "pong", data, request_id);
    json_object_put(data);
    return 0;
  }

  if (!strcmp(type, "auth")) {
    struct json_object *data = request_data_object(message);
    const char *username = object_string(data, "username");
    const char *password = object_string(data, "password");
    char error[256] = {0};
    long long retry_after = auth_retry_after_ms(wsi, username);
    if (retry_after > 0) {
      char detail[160];
      snprintf(detail, sizeof(detail),
               "too many failed attempts; retry in %lld ms", retry_after);
      queue_error(wsi, session, request_id, 429, "Too Many Requests", detail);
      return 0;
    }
    if (!data || auth_verify_user(username, password, error, sizeof(error)) != 0) {
      auth_record_failure(wsi, username);
      deauthenticate_session(wsi, session, true);
      queue_error(wsi, session, request_id, 401, "Unauthorized", error);
      return 0;
    }
    session->role = role_for_username(username);
    if (session->role == POSTMERKOS_ROLE_NONE) {
      session->authenticated = false;
      session->username[0] = '\0';
      queue_error(wsi, session, request_id, 403, "Forbidden",
                  "account has no postmerkOS management role");
      return 0;
    }
    auth_record_success(wsi, username);
    const char *token = NULL;
    struct json_object *remember_obj = NULL;
    bool remember = data && json_object_object_get_ex(data, "remember", &remember_obj) &&
                    json_object_get_boolean(remember_obj);
    long now = (long)time(NULL);
    long ttl = remember ? SESSION_TTL_REMEMBER : SESSION_TTL_DEFAULT;
    token = session_create(username, ttl, now);
    if (!token) {
      deauthenticate_session(wsi, session, true);
      queue_error(wsi, session, request_id, 503, "Service Unavailable",
                  "unable to create a secure management session");
      return 0;
    }
    session->authenticated = true;
    snprintf(session->username, sizeof(session->username), "%s", username);
    session->send_initial_status = true;
    session->send_initial_config = true;
    struct json_object *auth = role_identity_json(username);
    snprintf(session->session_token, sizeof(session->session_token), "%s", token);
    json_object_object_add(auth, "token", json_object_new_string(token));
    json_object_object_add(auth, "expires_at",
                           json_object_new_int64((int64_t)(now + ttl)));
    if (role_has_capability(session->role, "users.manage"))
      json_object_object_add(auth, "users", auth_list_users());
    queue_response(wsi, session, "auth", auth, request_id);
    json_object_put(auth);
    return 0;
  }

  if (!strcmp(type, "auth_token")) {
    struct json_object *data = request_data_object(message);
    const char *token = object_string(data, "token");
    char username[65] = {0};
    long now = (long)time(NULL);
    if (!token || session_lookup(token, now, username, sizeof(username)) != 0) {
      queue_error(wsi, session, request_id, 401, "Unauthorized", "session expired");
      return 0;
    }
    enum postmerkos_role role = role_for_username(username);
    if (role == POSTMERKOS_ROLE_NONE) {
      session_revoke_token(token);
      queue_error(wsi, session, request_id, 403, "Forbidden",
                  "account has no postmerkOS management role");
      return 0;
    }
    if (session->session_token[0] && strcmp(session->session_token, token))
      session_revoke_token(session->session_token);
    session->authenticated = true;
    session->role = role;
    snprintf(session->username, sizeof(session->username), "%s", username);
    snprintf(session->session_token, sizeof(session->session_token), "%s", token);
    session->send_initial_status = true;
    session->send_initial_config = true;
    struct json_object *auth = role_identity_json(username);
    if (role_has_capability(session->role, "users.manage"))
      json_object_object_add(auth, "users", auth_list_users());
    queue_response(wsi, session, "auth", auth, request_id);
    json_object_put(auth);
    return 0;
  }

  if (!strcmp(type, "logout")) {
    deauthenticate_session(wsi, session, true);
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "message", json_object_new_string("Logged out"));
    queue_response(wsi, session, "auth_required", data, request_id);
    json_object_put(data);
    return 0;
  }

  if (!revalidate_session(wsi, session, request_id)) return 0;

  if (!strcmp(type, "get_auth")) {
    struct json_object *data = role_identity_json(session->username);
    if (role_has_capability(session->role, "users.manage"))
      json_object_object_add(data, "users", auth_list_users());
    queue_response(wsi, session, "auth", data, request_id);
    json_object_put(data);
    return 0;
  }

  if (!strcmp(type, "user_list")) {
    if (!require_capability(wsi, session, request_id, "users.manage")) return 0;
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "users", auth_list_users());
    queue_response(wsi, session, "users", data, request_id);
    json_object_put(data);
    return 0;
  }

  if (!strcmp(type, "user_create")) {
    if (!require_capability(wsi, session, request_id, "users.manage")) return 0;
    struct json_object *data=request_data_object(message);const char *username=object_string(data,"username"),*password=object_string(data,"password"),*role=object_string(data,"role");char error[256]={0};
    if(auth_create_user(username,password,role,error,sizeof(error))!=0){queue_bad_request(wsi,session,request_id,error);return 0;}
    struct json_object *reply=json_object_new_object();json_object_object_add(reply,"message",json_object_new_string("Account created"));json_object_object_add(reply,"users",auth_list_users());queue_response(wsi,session,"users",reply,request_id);json_object_put(reply);return 0;
  }
  if (!strcmp(type, "user_delete")) {
    if (!require_capability(wsi, session, request_id, "users.manage")) return 0;
    struct json_object *data=request_data_object(message);const char *username=object_string(data,"username");char error[256]={0};
    if(auth_delete_user(username,error,sizeof(error))!=0){queue_bad_request(wsi,session,request_id,error);return 0;}
    ws_revoke_user_sessions(username);
    struct json_object *reply=json_object_new_object();json_object_object_add(reply,"message",json_object_new_string("Account deleted"));json_object_object_add(reply,"users",auth_list_users());queue_response(wsi,session,"users",reply,request_id);json_object_put(reply);return 0;
  }
  if (!strcmp(type, "user_role")) {
    if (!require_capability(wsi, session, request_id, "users.manage")) return 0;
    struct json_object *data=request_data_object(message);const char *username=object_string(data,"username"),*role=object_string(data,"role");char error[256]={0};
    if(auth_set_role(username,role,error,sizeof(error))!=0){queue_bad_request(wsi,session,request_id,error);return 0;}
    ws_revoke_user_sessions(username);
    struct json_object *reply=json_object_new_object();json_object_object_add(reply,"message",json_object_new_string("Account role updated"));json_object_object_add(reply,"users",auth_list_users());queue_response(wsi,session,"users",reply,request_id);json_object_put(reply);return 0;
  }
  if (!strcmp(type, "ssh_key_list")) {
    if (!require_capability(wsi, session, request_id, "users.manage")) return 0;
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "keys", ssh_keys_list());
    queue_response(wsi, session, "ssh_keys", data, request_id);
    json_object_put(data);
    return 0;
  }
  if (!strcmp(type, "ssh_key_add")) {
    if (!require_capability(wsi, session, request_id, "users.manage")) return 0;
    struct json_object *data = request_data_object(message);
    const char *label = object_string(data, "label");
    const char *key = object_string(data, "key");
    char error[256] = {0};
    if (ssh_keys_add(label, key, error, sizeof(error)) != 0) {
      queue_bad_request(wsi, session, request_id, error);
      return 0;
    }
    struct json_object *reply = json_object_new_object();
    json_object_object_add(reply, "message", json_object_new_string("Key added"));
    json_object_object_add(reply, "keys", ssh_keys_list());
    queue_response(wsi, session, "ssh_keys", reply, request_id);
    json_object_put(reply);
    return 0;
  }
  if (!strcmp(type, "ssh_key_remove")) {
    if (!require_capability(wsi, session, request_id, "users.manage")) return 0;
    struct json_object *data = request_data_object(message);
    const char *key = object_string(data, "key");
    char error[256] = {0};
    if (ssh_keys_remove(key, error, sizeof(error)) != 0) {
      queue_bad_request(wsi, session, request_id, error);
      return 0;
    }
    struct json_object *reply = json_object_new_object();
    json_object_object_add(reply, "message", json_object_new_string("Key removed"));
    json_object_object_add(reply, "keys", ssh_keys_list());
    queue_response(wsi, session, "ssh_keys", reply, request_id);
    json_object_put(reply);
    return 0;
  }

  if (!strcmp(type, "password_change")) {
    struct json_object *data = request_data_object(message);
    const char *target = object_string(data, "username");
    const char *current = object_string(data, "current_password");
    const char *replacement = object_string(data, "new_password");
    if (!target || !*target) target = session->username;
    char error[256] = {0};
    if (!data || auth_change_password(session->username, target, current,
                                      replacement, error, sizeof(error)) != 0) {
      queue_error(wsi, session, request_id, 400, "Password update failed", error);
      return 0;
    }
    struct json_object *ack = json_object_new_object();
    json_object_object_add(ack, "message",
                           json_object_new_string("Password updated"));
    json_object_object_add(ack, "username", json_object_new_string(target));
    queue_response(wsi, session, "ack", ack, request_id);
    json_object_put(ack);
    ws_revoke_user_sessions(target);
    return 0;
  }

  if (!strcmp(type, "terminal_start")) {
    if (!require_capability(wsi, session, request_id, "terminal.exec")) return 0;
    struct json_object *data = request_data_object(message);
    const char *command = object_string(data, "command");
    char error[256] = {0};
    if (terminal_start(wsi, session, command, error, sizeof(error)) != 0) {
      queue_bad_request(wsi, session, request_id, error);
      return 0;
    }
    struct json_object *ack = json_object_new_object();
    json_object_object_add(ack, "token",
                           json_object_new_string(session->terminal_token));
    json_object_object_add(ack, "message",
                           json_object_new_string("Terminal command started"));
    queue_response(wsi, session, "terminal_started", ack, request_id);
    json_object_put(ack);
    return 0;
  }

  if (!strcmp(type, "terminal_exec")) {
    if (!require_capability(wsi, session, request_id, "terminal.exec")) return 0;
    struct json_object *data = request_data_object(message);
    const char *command = object_string(data, "command");
    if (!command) {
      queue_bad_request(wsi, session, request_id,
                        "terminal_exec.data.command must be a string");
      return 0;
    }
    struct json_object *result = terminal_execute(command);
    queue_response(wsi, session, "terminal", result, request_id);
    json_object_put(result);
    return 0;
  }

  if (!strcmp(type, "firmware_status")) {
    if (!require_capability(wsi, session, request_id, "firmware.history.read")) return 0;
    struct json_object *status = firmware_status_json();
    queue_response(wsi, session, "firmware_status", status, request_id);
    json_object_put(status);
    return 0;
  }

  if (!strcmp(type, "firmware_upload_status")) {
    if (!require_capability(wsi, session, request_id, "firmware.update")) return 0;
    struct json_object *data = firmware_job.state != FIRMWARE_JOB_IDLE
        ? firmware_job_status_json() : upload_status_json(session);
    queue_response(wsi, session, "firmware_upload_status", data, request_id);
    json_object_put(data);
    return 0;
  }

  if (!strcmp(type, "firmware_upload_cancel")) {
    if (!require_capability(wsi, session, request_id, "firmware.update")) return 0;
    firmware_job_cancel();
    cleanup_upload(wsi, session);
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "message",
                           json_object_new_string("Firmware upload cancelled"));
    queue_response(wsi, session, "ack", data, request_id);
    json_object_put(data);
    return 0;
  }

  if (!strcmp(type, "firmware_upload_start")) {
    if (!require_capability(wsi, session, request_id, "firmware.update")) return 0;
    struct json_object *data = request_data_object(message);
    const char *name = object_string(data, "name");
    const char *overlay = object_string(data, "overlay");
    struct json_object *size_object = NULL;
    struct json_object *force_object = NULL;
    struct json_object *accept_object = NULL;
    struct json_object *manifest_object = NULL;
    if (!data || !json_object_object_get_ex(data, "size", &size_object)) {
      queue_bad_request(wsi, session, request_id, "firmware size is required");
      return 0;
    }
    int64_t size_value = json_object_get_int64(size_object);
    bool force = json_object_object_get_ex(data, "force", &force_object) &&
                 json_object_get_boolean(force_object);
    bool accept_untested = json_object_object_get_ex(data, "accept_untested", &accept_object) &&
                           json_object_get_boolean(accept_object);
    if (size_value <= 0 || size_value > MAX_FIRMWARE_UPLOAD ||
        !valid_overlay(overlay) || !valid_artifact_name(name)) {
      queue_bad_request(wsi, session, request_id,
                        "invalid firmware size or overlay policy");
      return 0;
    }
    if (firmware_job.state == FIRMWARE_JOB_VALIDATING ||
        firmware_job.state == FIRMWARE_JOB_READY) {
      queue_error(wsi, session, request_id, 409, "Conflict",
                  "a firmware image is validating or awaiting confirmation");
      return 0;
    }
    if (firmware_job.state == FIRMWARE_JOB_FAILED) firmware_job_forget(true);
    if (upload_owner && upload_owner != wsi) {
      queue_error(wsi, session, request_id, 409, "Conflict",
                  "another firmware upload is active");
      return 0;
    }
    cleanup_upload(wsi, session);
    purge_upload_cache(NULL);
    session->upload_handed_off = false;
    snprintf(session->upload_token, sizeof(session->upload_token), "%08lx%08lx",
             (unsigned long)time(NULL), (unsigned long)((uintptr_t)wsi ^ (uintptr_t)getpid()));
    snprintf(session->upload_path, sizeof(session->upload_path),
             "/run/fwupdate/uploads/%s.bin", session->upload_token);
    if (json_object_object_get_ex(data, "manifest", &manifest_object)) {
      if (!json_object_is_type(manifest_object, json_type_object)) {
        queue_bad_request(wsi, session, request_id, "manifest must be a JSON object");
        cleanup_upload(wsi, session); return 0;
      }
      snprintf(session->upload_manifest_path, sizeof(session->upload_manifest_path),
               "/run/fwupdate/uploads/%s.manifest.json", session->upload_token);
      if (write_manifest_object(session->upload_manifest_path, manifest_object) != 0) {
        queue_error(wsi, session, request_id, 500, "Manifest initialization failed",
                    "unable to store release manifest");
        cleanup_upload(wsi, session); return 0;
      }
    }
    session->upload_fd = open(session->upload_path,
                              O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (session->upload_fd < 0) {
      queue_error(wsi, session, request_id, 500, "Upload initialization failed",
                  strerror(errno));
      cleanup_upload(wsi, session);
      return 0;
    }
    snprintf(session->upload_name, sizeof(session->upload_name), "%s",
             name && *name ? name : "firmware.bin");
    snprintf(session->upload_overlay, sizeof(session->upload_overlay), "%s",
             overlay);
    session->upload_expected = (size_t)size_value;
    session->upload_received = 0;
    session->upload_force = force;
    session->upload_accept_untested = accept_untested;
    session->upload_active = true;
    session->upload_ready = false;
    upload_owner = wsi;
    struct json_object *ready = upload_status_json(session);
    queue_response(wsi, session, "firmware_upload_ready", ready, request_id);
    json_object_put(ready);
    return 0;
  }

  if (!strcmp(type, "firmware_upload_finish")) {
    if (!require_capability(wsi, session, request_id, "firmware.update")) return 0;
    if (!session->upload_active || session->upload_fd < 0) {
      queue_bad_request(wsi, session, request_id, "no firmware upload is active");
      return 0;
    }
    if (fsync(session->upload_fd) != 0) {
      queue_error(wsi, session, request_id, 500, "Upload failed", strerror(errno));
      cleanup_upload(wsi, session);
      return 0;
    }
    close(session->upload_fd);
    session->upload_fd = -1;
    if (session->upload_received != session->upload_expected) {
      queue_bad_request(wsi, session, request_id,
                        "uploaded byte count does not match the declared size");
      cleanup_upload(wsi, session);
      return 0;
    }
    char error[256] = {0};
    if (firmware_job_start(session, error, sizeof(error)) != 0) {
      queue_error(wsi, session, request_id, 500,
                  "Firmware validation could not be started", error);
      cleanup_upload(wsi, session);
      return 0;
    }
    session->upload_active = false;
    session->upload_ready = false;
    session->upload_handed_off = true;
    upload_owner = NULL;
    struct json_object *validating = firmware_job_status_json();
    json_object_object_add(validating, "message", json_object_new_string(
        "Upload complete; firmware validation is running asynchronously."));
    queue_response(wsi, session, "firmware_validating", validating, request_id);
    json_object_put(validating);
    return 0;
  }

  if (!strcmp(type, "firmware_begin_flash")) {
    if (!require_capability(wsi, session, request_id, "firmware.update")) return 0;
    struct json_object *data = request_data_object(message);
    const char *token = object_string(data, "token");
    if (firmware_job.state != FIRMWARE_JOB_READY || !firmware_job.path[0] ||
        !token || strcmp(token, firmware_job.token)) {
      queue_bad_request(wsi, session, request_id,
                        "firmware upload token is stale or not ready");
      return 0;
    }
    char error[256] = {0};
    if (firmware_start_update(firmware_job.path, firmware_job.name,
                              firmware_job.manifest_path[0] ? firmware_job.manifest_path : NULL,
                              firmware_job.overlay, firmware_job.force,
                              firmware_job.accept_untested,
                              error, sizeof(error)) != 0) {
      queue_error(wsi, session, request_id, 500,
                  "Firmware update could not be started", error);
      return 0;
    }
    session->upload_active = false;
    session->upload_ready = false;
    session->upload_handed_off = true;
    upload_owner = NULL;
    struct json_object *started = json_object_new_object();
    json_object_object_add(started, "message", json_object_new_string(
        "Starting firmware update. Management services will stop shortly; LED progress and serial status remain available."));
    json_object_object_add(started, "name", json_object_new_string(firmware_job.name));
    json_object_object_add(started, "token", json_object_new_string(firmware_job.token));
    queue_response(wsi, session, "firmware_starting", started, request_id);
    json_object_put(started);
    firmware_job_forget(false);
    return 0;
  }

  if (!strcmp(type, "firmware_repo_get")) {
    if (!require_capability(wsi, session, request_id, "firmware.history.read")) return 0;
    struct json_object *repos = firmware_repositories_json();
    queue_response(wsi, session, "firmware_repositories", repos, request_id);
    json_object_put(repos);
    return 0;
  }
  if (!strcmp(type, "firmware_repo_check")) {
    if (!require_capability(wsi, session, request_id, "firmware.history.read")) return 0;
    struct json_object *data = request_data_object(message);
    const char *url = object_string(data, "url");
    struct json_object *result = firmware_repository_check(url);
    queue_response(wsi, session, "firmware_repository_check", result, request_id);
    json_object_put(result);
    return 0;
  }
  if (!strcmp(type, "firmware_repo_set")) {
    if (!require_capability(wsi, session, request_id, "firmware.update")) return 0;
    struct json_object *data = request_data_object(message);
    char error[256] = {0};
    if (firmware_repositories_save(data, error, sizeof(error)) != 0) {
      queue_bad_request(wsi, session, request_id, error); return 0;
    }
    struct json_object *ack = json_object_new_object();
    json_object_object_add(ack, "message", json_object_new_string("Firmware repositories saved"));
    queue_response(wsi, session, "ack", ack, request_id); json_object_put(ack);
    return 0;
  }

  if (!strcmp(type, "compatibility_report")) {
    if (!require_capability(wsi, session, request_id, "status.read")) return 0;
    struct json_object *report=compatibility_report_json();queue_response(wsi,session,"compatibility_report",report,request_id);json_object_put(report);return 0;
  }
  if (!strcmp(type, "compatibility_ack")) {
    if (!require_capability(wsi, session, request_id, "status.read")) return 0;
    char error[256]={0};if(compatibility_acknowledge(error,sizeof(error))!=0){queue_bad_request(wsi,session,request_id,error);return 0;}
    struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Compatibility notice dismissed for this firmware"));queue_response(wsi,session,"ack",ack,request_id);json_object_put(ack);refresh_status_cache(true);return 0;
  }
  if (!strcmp(type, "system_identity_get")) {
    if (!require_capability(wsi, session, request_id, "status.read")) return 0;
    struct json_object *data=system_identity_status_json();
    struct json_object *policy=system_identity_policy_load();
    json_object_object_add(data,"policy",policy);
    queue_response(wsi,session,"system_identity",data,request_id);json_object_put(data);return 0;
  }
  if (!strcmp(type, "system_identity_set")) {
    if (!require_capability(wsi, session, request_id, "network.write")) return 0;
    struct json_object *data=request_data_object(message);char error[256]={0};
    if(system_identity_save(data,error,sizeof(error))!=0){queue_bad_request(wsi,session,request_id,error[0]?error:"system identity update failed");return 0;}
    struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("System identity updated"));queue_response(wsi,session,"ack",ack,request_id);json_object_put(ack);refresh_status_cache(true);return 0;
  }
  if (!strcmp(type, "timezones_get")) {
    if (!require_capability(wsi, session, request_id, "status.read")) return 0;
    struct json_object *data=timezones_json();queue_response(wsi,session,"timezones",data,request_id);json_object_put(data);return 0;
  }
  if (!strcmp(type, "services_get")) {
    if (!require_capability(wsi, session, request_id, "status.read")) return 0;
    struct json_object *data=service_status_json(); queue_response(wsi,session,"services",data,request_id); json_object_put(data); return 0;
  }
  if (!strcmp(type, "services_set")) {
    if (!require_capability(wsi, session, request_id, "services.manage")) return 0;
    struct json_object *data=request_data_object(message); char error[256]={0};
    if(service_policy_save(data,error,sizeof(error))!=0){queue_bad_request(wsi,session,request_id,error);return 0;}
    struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Service policy saved"));queue_response(wsi,session,"ack",ack,request_id);json_object_put(ack);return 0;
  }
  if (!strcmp(type, "services_action")) {
    if (!require_capability(wsi, session, request_id, "services.manage")) return 0;
    struct json_object *data=request_data_object(message);const char *service=object_string(data,"service"),*action=object_string(data,"action");char error[256]={0};
    if(service_action(service,action,error,sizeof(error))!=0){queue_bad_request(wsi,session,request_id,error);return 0;}
    struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Service action completed"));queue_response(wsi,session,"ack",ack,request_id);json_object_put(ack);return 0;
  }
  if (!strcmp(type, "time_get")) {
    if (!require_capability(wsi, session, request_id, "status.read")) return 0;
    struct json_object *data=time_status_json();queue_response(wsi,session,"time",data,request_id);json_object_put(data);return 0;
  }
  if (!strcmp(type, "time_set")) {
    if (!require_capability(wsi, session, request_id, "services.manage")) return 0;
    struct json_object *data=request_data_object(message);char error[256]={0};if(time_policy_save(data,error,sizeof(error))!=0){queue_bad_request(wsi,session,request_id,error);return 0;}
    struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Time policy saved"));queue_response(wsi,session,"ack",ack,request_id);json_object_put(ack);return 0;
  }
  if (!strcmp(type, "time_set_clock")) {
    if (!require_capability(wsi, session, request_id, "services.manage")) return 0;
    struct json_object *data=request_data_object(message),*epoch=NULL,*local=NULL;char error[256]={0};int rc=-EINVAL;
    if(data && json_object_object_get_ex(data,"local",&local) && json_object_is_type(local,json_type_string))
      rc=time_set_local(json_object_get_string(local),error,sizeof(error));
    else if(data && json_object_object_get_ex(data,"epoch",&epoch) && json_object_is_type(epoch,json_type_int))
      rc=time_set_epoch((time_t)json_object_get_int64(epoch),error,sizeof(error));
    else {queue_bad_request(wsi,session,request_id,"local or epoch is required");return 0;}
    if(rc!=0){queue_bad_request(wsi,session,request_id,error[0]?error:"system clock update failed");return 0;}
    struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("System clock updated"));queue_response(wsi,session,"ack",ack,request_id);json_object_put(ack);refresh_status_cache(true);return 0;
  }
  if (!strcmp(type, "time_sync")) {
    if (!require_capability(wsi, session, request_id, "services.manage")) return 0;
    char error[256]={0};if(time_force_sync(error,sizeof(error))!=0){queue_bad_request(wsi,session,request_id,error);return 0;}
    struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Time synchronization requested"));queue_response(wsi,session,"ack",ack,request_id);json_object_put(ack);return 0;
  }
  if (!strcmp(type, "ports_clone")) {
    if (!require_capability(wsi, session, request_id, "switching.write")) return 0;
    struct json_object *data=request_data_object(message),*result=NULL;char error[256]={0};
    if(port_clone_apply(data,&result,error,sizeof(error))!=0){queue_bad_request(wsi,session,request_id,error);return 0;}
    queue_response(wsi,session,"ports_cloned",result,request_id);json_object_put(result);refresh_status_cache(true);struct json_object *updated=load_config_file();if(updated){refresh_config_cache(updated,true);json_object_put(updated);}return 0;
  }
  if (!strcmp(type, "reset_button_status")) {
    if (!require_capability(wsi, session, request_id, "status.read")) return 0;
    struct json_object *status = reset_button_status_json();
    queue_response(wsi, session, "reset_button_status", status, request_id);
    json_object_put(status);
    return 0;
  }
  if (!strcmp(type, "get_status")) {
    if (!require_capability(wsi, session, request_id, "status.read")) return 0;
    struct json_object *status = get_status();
    queue_response(wsi, session, "status", status, request_id);
    json_object_put(status);
    return 0;
  }
  if (!strcmp(type, "telemetry_subscribe")) {
    if (!require_capability(wsi, session, request_id, "status.read")) return 0;
    session->telemetry_subscribed = true;
    telemetry_tick();  /* fresh snapshot for the immediate frame */
    struct json_object *frame = telemetry_stream_json();
    if (frame) {
      queue_response(wsi, session, "telemetry", frame, NULL);
      json_object_put(frame);
    }
    telemetry_stream_ensure_scheduled();
    struct json_object *ack = json_object_new_object();
    json_object_object_add(ack, "subscribed", json_object_new_boolean(1));
    json_object_object_add(ack, "interval_s",
                           json_object_new_int(telemetry_stream_interval_seconds()));
    queue_response(wsi, session, "ack", ack, request_id);
    json_object_put(ack);
    return 0;
  }
  if (!strcmp(type, "telemetry_unsubscribe")) {
    if (!require_capability(wsi, session, request_id, "status.read")) return 0;
    session->telemetry_subscribed = false;
    struct json_object *ack = json_object_new_object();
    json_object_object_add(ack, "subscribed", json_object_new_boolean(0));
    queue_response(wsi, session, "ack", ack, request_id);
    json_object_put(ack);
    return 0;
  }
  if (!strcmp(type, "get_config")) {
    if (!require_capability(wsi, session, request_id, "config.read")) return 0;
    struct json_object *config = load_config_file();
    if (!config) queue_bad_request(wsi, session, request_id,
                                   "persistent configuration is unavailable");
    else {
      queue_response(wsi, session, "config", config, request_id);
      json_object_put(config);
    }
    return 0;
  }

  if (!strcmp(type, "replace_config")) {
    if (!require_capability(wsi, session, request_id, "config.restore")) return 0;
    struct json_object *candidate = request_data_object(message);
    if (!candidate) {
      queue_bad_request(wsi, session, request_id,
                        "replace_config.data must be a complete configuration object");
      return 0;
    }
    struct apply_result result;
    apply_result_init(&result);
    char error[256] = {0};
    int rc = replace_configuration(candidate, &result, error, sizeof(error));
    if (rc != 0) {
      queue_bad_request(wsi, session, request_id,
                        error[0] ? error : "configuration replacement failed");
      apply_result_cleanup(&result);
      return 0;
    }
    struct json_object *ack = apply_result_json(&result,
                                                "Configuration restored");
    queue_response(wsi, session, "ack", ack, request_id);
    json_object_put(ack);
    apply_result_cleanup(&result);
    config_file_mtime(&config_mtime_ns);
    refresh_config_cache(candidate, true);
    refresh_status_cache(true);
    return 0;
  }

  if (strcmp(type, "config")) {
    queue_bad_request(wsi, session, request_id, "unknown request type");
    return 0;
  }

  struct json_object *delta = request_data_object(message);
  if (!delta) {
    queue_bad_request(wsi, session, request_id,
                      "config.data must be a JSON object");
    return 0;
  }
  const char *needed_capability = delta_is_operator_safe(delta)
      ? "switching.write" : "network.write";
  if (!require_capability(wsi, session, request_id, needed_capability)) return 0;
  struct apply_result result;
  apply_result_init(&result);
  struct json_object *saved = NULL;
  char error[256] = {0};
  int rc = config_merge_validate_save_apply(delta, &saved, &result, true,
                                             error, sizeof(error));
  if (rc != 0) {
    queue_bad_request(wsi, session, request_id, error);
    apply_result_cleanup(&result);
    return 0;
  }
  struct json_object *ack = apply_result_json(&result,
                                              "Configuration accepted");
  queue_response(wsi, session, "ack", ack, request_id);
  json_object_put(ack);
  apply_result_cleanup(&result);
  config_file_mtime(&config_mtime_ns);
  refresh_config_cache(saved, true);
  refresh_status_cache(true);
  json_object_put(saved);
  return 0;
}

static void reset_receive(struct per_session_data *session) {
  free(session->receive_buffer);
  session->receive_buffer = NULL;
  session->receive_length = 0;
}

static int receive_binary(struct lws *wsi, struct per_session_data *session,
                          const void *input, size_t length) {
  if (!session->authenticated || !session->upload_active ||
      session->upload_fd < 0 || upload_owner != wsi) {
    queue_error(wsi, session, NULL, 400, "Unexpected binary data",
                "start an authenticated firmware upload first");
    return 0;
  }
  if (session->upload_received + length > session->upload_expected) {
    queue_bad_request(wsi, session, NULL,
                      "firmware upload exceeds the declared size");
    cleanup_upload(wsi, session);
    return 0;
  }
  const unsigned char *cursor = input;
  size_t remaining = length;
  while (remaining) {
    ssize_t written = write(session->upload_fd, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      queue_error(wsi, session, NULL, 500, "Firmware upload failed",
                  strerror(errno));
      cleanup_upload(wsi, session);
      return 0;
    }
    if (written == 0) {
      queue_error(wsi, session, NULL, 500, "Firmware upload failed",
                  "temporary upload file stopped accepting data");
      cleanup_upload(wsi, session);
      return 0;
    }
    cursor += written;
    remaining -= (size_t)written;
    session->upload_received += (size_t)written;
  }
  return 0;
}

static int receive_fragment(struct lws *wsi, struct per_session_data *session,
                            const void *input, size_t length) {
  if (lws_frame_is_binary(wsi))
    return receive_binary(wsi, session, input, length);
  if (session->receive_length + length > MAX_MSG_LEN) {
    queue_bad_request(wsi, session, NULL, "request exceeds maximum size");
    reset_receive(session);
    return 0;
  }
  char *expanded = realloc(session->receive_buffer,
                           session->receive_length + length + 1);
  if (!expanded) {
    queue_bad_request(wsi, session, NULL, "request buffer allocation failed");
    reset_receive(session);
    return 0;
  }
  session->receive_buffer = expanded;
  memcpy(session->receive_buffer + session->receive_length, input, length);
  session->receive_length += length;
  session->receive_buffer[session->receive_length] = '\0';
  if (!lws_is_final_fragment(wsi) || lws_remaining_packet_payload(wsi) != 0)
    return 0;
  struct json_tokener *tokener = json_tokener_new();
  if (!tokener) {
    queue_bad_request(wsi, session, NULL, "JSON parser allocation failed");
    reset_receive(session);
    return 0;
  }
  struct json_object *message = json_tokener_parse_ex(
      tokener, session->receive_buffer, (int)session->receive_length);
  enum json_tokener_error parse_error = json_tokener_get_error(tokener);
  if (parse_error != json_tokener_success || !message)
    queue_bad_request(wsi, session, NULL, "malformed JSON");
  else {
    handle_request(wsi, session, message);
    json_object_put(message);
  }
  json_tokener_free(tokener);
  reset_receive(session);
  return 0;
}

static int configd_ws_callback(struct lws *wsi,
                               enum lws_callback_reasons reason,
                               void *user, void *input, size_t length) {
  struct per_session_data *session = user;
  switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
      if (client_count >= MAX_CLIENTS) return -1;
      memset(session, 0, sizeof(*session));
      session->upload_fd = -1;
      session->terminal_fd = -1;
      add_client(wsi);
      struct json_object *data = json_object_new_object();
      json_object_object_add(data, "message", json_object_new_string(
          "Local Linux account authentication is required"));
      json_object_object_add(data, "protocol", json_object_new_int(2));
      json_object_object_add(data, "websocket_protocol",
                             json_object_new_string("configd-ws"));
      queue_response(wsi, session, "auth_required", data, NULL);
      json_object_put(data);
      websocket_log("client connected (%zu total)", client_count);
      break;
    }
    case LWS_CALLBACK_CLOSED:
      remove_client(wsi);
      cleanup_upload(wsi, session);
      terminal_stop(session, true);
      reset_receive(session);
      free_replies(session);
      websocket_log("client disconnected (%zu total)", client_count);
      break;
    case LWS_CALLBACK_RECEIVE:
      receive_fragment(wsi, session, input, length);
      break;
    case LWS_CALLBACK_SERVER_WRITEABLE:
      if (session->reply_head) {
        struct queued_reply *reply = session->reply_head;
        session->reply_head = reply->next;
        if (!session->reply_head) session->reply_tail = NULL;
        ws_send(wsi, reply->text);
        free(reply->text);
        free(reply);
      } else if (session->authenticated && session->send_initial_status &&
                 cached_status_json) {
        ws_send(wsi, cached_status_json);
        session->send_initial_status = false;
      } else if (session->authenticated && session->send_initial_config &&
                 cached_config_json) {
        ws_send(wsi, cached_config_json);
        session->send_initial_config = false;
      } else if (session->authenticated && session->send_status &&
                 cached_status_json) {
        ws_send(wsi, cached_status_json);
        session->send_status = false;
      } else if (session->authenticated && session->send_config &&
                 cached_config_json) {
        ws_send(wsi, cached_config_json);
        session->send_config = false;
      }
      if (session->reply_head ||
          (session->authenticated &&
           (session->send_initial_status || session->send_initial_config ||
            session->send_status || session->send_config)))
        lws_callback_on_writable(wsi);
      break;
    default:
      break;
  }
  return 0;
}

void mark_clients_pending(void) {
  if (!status_pending && !config_pending) return;
  for (size_t i = 0; i < client_count; i++) {
    struct per_session_data *session = lws_wsi_user(clients[i]);
    if (!session || !session->authenticated) continue;
    if (status_pending) session->send_status = true;
    if (config_pending) session->send_config = true;
    lws_callback_on_writable(clients[i]);
  }
  status_pending = false;
  config_pending = false;
}

static const struct lws_protocols protocols[] = {
  {
    .name = "configd-ws",
    .callback = configd_ws_callback,
    .per_session_data_size = sizeof(struct per_session_data),
    .rx_buffer_size = MAX_MSG_LEN,
  },
  LWS_PROTOCOL_LIST_TERM
};

struct lws_context *ws_init(int port) {
  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.port = port;
  info.protocols = protocols;
  info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE |
                 LWS_SERVER_OPTION_ALLOW_LISTEN_SHARE;
  lws_set_log_level(LLL_ERR | LLL_WARN, NULL);
  ws_context = lws_create_context(&info);
  if (!ws_context) websocket_log("context creation failed on port %d", port);
  else websocket_log("listener initialized on port %d using protocol configd-ws", port);
  return ws_context;
}

void ws_schedule_timers(struct lws_context *context, int status_interval) {
  configured_status_interval = status_interval;
  config_file_mtime(&config_mtime_ns);
  refresh_status_cache(false);
  struct json_object *config = load_config_file();
  if (config) {
    refresh_config_cache(config, false);
    json_object_put(config);
  }
  lws_sul_schedule(context, 0, &status_sul, status_poll_cb,
      (lws_usec_t)configured_status_interval * LWS_USEC_PER_SEC);
  lws_sul_schedule(context, 0, &config_sul, config_poll_cb,
      10 * LWS_USEC_PER_SEC);
}

int ws_service_once(struct lws_context *context, int timeout_ms) {
  firmware_job_poll();
  return lws_service(context, timeout_ms);
}

void ws_shutdown(struct lws_context *context) {
  lws_sul_cancel(&status_sul);
  lws_sul_cancel(&config_sul);
  lws_sul_cancel(&terminal_sul);
  lws_sul_cancel(&telemetry_sul);
  terminal_poll_scheduled = false;
  telemetry_stream_scheduled = false;
  firmware_job_cancel();
  for (size_t i = 0; i < client_count; i++) {
    struct per_session_data *session = lws_wsi_user(clients[i]);
    if (session) terminal_stop(session, true);
  }
  free(cached_status_json);
  free(cached_config_json);
  cached_status_json = NULL;
  cached_config_json = NULL;
  lws_context_destroy(context);
  ws_context = NULL;
}
