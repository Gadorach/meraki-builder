#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "socket_io.h"

#define RESPONSE_MAX 262144

static const char *socket_path(void) {
  const char *path = getenv("POSTMERKOS_CONFIGD_SOCKET");
  return path && *path ? path : "/run/postmerkos/configd.sock";
}

static const char *pid_path(void) {
  const char *path = getenv("POSTMERKOS_CONFIGD_PID");
  return path && *path ? path : "/run/configd.pid";
}

static int websocket_port(void) {
  const char *text = getenv("POSTMERKOS_WEBSOCKET_PORT");
  if (!text || !*text) return 4001;
  char *end = NULL;
  long value = strtol(text, &end, 10);
  return end && !*end && value > 0 && value <= 65535 ? (int)value : 4001;
}

static struct json_object *session_data(void);
static const char *string_member(struct json_object *object, const char *key, const char *fallback);

static int connect_socket(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -errno;
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path()) >=
      (int)sizeof(address.sun_path)) { close(fd); return -ENAMETOOLONG; }
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    int rc = -errno; close(fd); return rc;
  }
  return fd;
}


static int read_exact(int fd, void *data, size_t length) {
  unsigned char *cursor = data;
  while (length) {
    ssize_t got = read(fd, cursor, length);
    if (got < 0) { if (errno == EINTR) continue; return -errno; }
    if (!got) return -ECONNRESET;
    cursor += (size_t)got; length -= (size_t)got;
  }
  return 0;
}

static bool contains_case_insensitive(const char *text, const char *needle) {
  if (!text || !needle || !*needle) return false;
  size_t length = strlen(needle);
  for (const char *cursor = text; *cursor; cursor++)
    if (!strncasecmp(cursor, needle, length)) return true;
  return false;
}

static int websocket_read_text(int fd, char *output, size_t output_size) {
  unsigned char header[2];
  int rc = read_exact(fd, header, sizeof(header));
  if (rc != 0) return rc;
  unsigned int opcode = header[0] & 0x0fU;
  unsigned long long length = header[1] & 0x7fU;
  bool masked = (header[1] & 0x80U) != 0;
  if (length == 126U) {
    unsigned char ext[2]; if ((rc = read_exact(fd, ext, 2)) != 0) return rc;
    length = ((unsigned long long)ext[0] << 8) | ext[1];
  } else if (length == 127U) {
    unsigned char ext[8]; if ((rc = read_exact(fd, ext, 8)) != 0) return rc;
    length = 0; for (size_t i = 0; i < 8; i++) length = (length << 8) | ext[i];
  }
  unsigned char mask[4] = {0};
  if (masked && (rc = read_exact(fd, mask, 4)) != 0) return rc;
  if (length + 1 > output_size) return -EMSGSIZE;
  if ((rc = read_exact(fd, output, (size_t)length)) != 0) return rc;
  for (size_t i = 0; masked && i < (size_t)length; i++)
    output[i] = (char)((unsigned char)output[i] ^ mask[i & 3U]);
  output[length] = '\0';
  if (opcode == 0x8U) return -ECONNRESET;
  return opcode == 0x1U ? 0 : -EPROTO;
}

static int websocket_hello_probe(int port, char *error, size_t error_size) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { snprintf(error, error_size, "socket: %s", strerror(errno)); return -errno; }
  struct timeval timeout = { .tv_sec = 3, .tv_usec = 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)port); inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    int rc = -errno; snprintf(error, error_size, "TCP connect: %s", strerror(errno)); close(fd); return rc;
  }
  char request_text[512];
  int request_length = snprintf(request_text, sizeof(request_text),
      "GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: configd-ws\r\n\r\n",
      port);
  if (request_length < 0 || (size_t)request_length >= sizeof(request_text)) {
    snprintf(error, error_size, "handshake request overflow"); close(fd); return -EOVERFLOW;
  }
  int rc = socket_write_all(fd, request_text, (size_t)request_length);
  if (rc != 0) { snprintf(error, error_size, "handshake write failed"); close(fd); return rc; }
  char headers[4096]; size_t used = 0;
  while (used + 1 < sizeof(headers)) {
    ssize_t got = read(fd, headers + used, 1);
    if (got <= 0) { rc = got < 0 ? -errno : -ECONNRESET; break; }
    used += 1; headers[used] = '\0';
    if (used >= 4 && !memcmp(headers + used - 4, "\r\n\r\n", 4)) { rc = 0; break; }
  }
  if (rc != 0 || !strstr(headers, " 101 ")) {
    snprintf(error, error_size, "WebSocket upgrade was not accepted"); close(fd); return -EPROTO;
  }
  if (!contains_case_insensitive(headers, "Sec-WebSocket-Protocol: configd-ws")) {
    snprintf(error, error_size, "configd-ws subprotocol was not selected"); close(fd); return -EPROTO;
  }
  const char *payload = "{\"id\":\"health\",\"type\":\"hello\"}";
  size_t length = strlen(payload); unsigned char frame[256];
  const unsigned char mask[4] = {0x50, 0x4d, 0x4f, 0x53};
  frame[0] = 0x81; frame[1] = (unsigned char)(0x80U | length);
  memcpy(frame + 2, mask, 4);
  for (size_t i = 0; i < length; i++) frame[6 + i] = (unsigned char)payload[i] ^ mask[i & 3U];
  if ((rc = socket_write_all(fd, frame, 6 + length)) != 0) {
    snprintf(error, error_size, "hello write failed"); close(fd); return rc;
  }
  char text[4096]; bool hello = false;
  for (int attempt = 0; attempt < 4; attempt++) {
    rc = websocket_read_text(fd, text, sizeof(text));
    if (rc != 0) break;
    struct json_object *message = json_tokener_parse(text);
    struct json_object *type = NULL, *data = NULL, *service = NULL, *protocol = NULL;
    if (message && json_object_object_get_ex(message, "type", &type) &&
        !strcmp(json_object_get_string(type), "hello") &&
        json_object_object_get_ex(message, "data", &data) &&
        json_object_object_get_ex(data, "service", &service) &&
        !strcmp(json_object_get_string(service), "configd") &&
        json_object_object_get_ex(data, "protocol", &protocol) &&
        json_object_get_int(protocol) == 2) hello = true;
    if (message) json_object_put(message);
    if (hello) break;
  }
  close(fd);
  if (!hello) { snprintf(error, error_size, "configd hello response failed"); return rc ? rc : -EPROTO; }
  return 0;
}

static int management_health(bool quiet) {
  int failures = 0;
  FILE *pid_file = fopen(pid_path(), "r");
  long pid = 0;
  bool process_ok = pid_file && fscanf(pid_file, "%ld", &pid) == 1 && pid > 0 &&
                    kill((pid_t)pid, 0) == 0;
  if (pid_file) fclose(pid_file);
  if (!quiet) printf("configd process:            %s%s\n", process_ok ? "PASS" : "FAIL",
                     process_ok ? "" : " (not running)");
  if (!process_ok) failures++;
  struct stat socket_status;
  bool socket_ok = stat(socket_path(), &socket_status) == 0 && S_ISSOCK(socket_status.st_mode);
  if (!quiet) printf("local management socket:   %s%s\n", socket_ok ? "PASS" : "FAIL",
                     socket_ok ? "" : " (missing)");
  if (!socket_ok) failures++;
  struct json_object *identity = session_data();
  if (!identity) {
    if (!quiet) puts("local session request:       FAIL");
    failures++;
  } else {
    if (!quiet) printf("local session request:       PASS (%s / %s)\n",
                       string_member(identity, "username", "unknown"),
                       string_member(identity, "role", "none"));
    json_object_put(identity);
  }
  char error[256] = {0};
  int rc = websocket_hello_probe(websocket_port(), error, sizeof(error));
  if (rc != 0) {
    if (!quiet) printf("WebSocket configd-ws hello:  FAIL (%s)\n", error);
    failures++;
  } else if (!quiet) puts("WebSocket configd-ws hello:  PASS (protocol 2)");
  return failures ? 1 : 0;
}

static struct json_object *request(const char *type, struct json_object *data) {
  int fd = connect_socket();
  if (fd < 0) {
    fprintf(stderr, "postmerkosctl: cannot connect to configd: %s\n", strerror(-fd));
    return NULL;
  }
  struct json_object *message = json_object_new_object();
  json_object_object_add(message, "type", json_object_new_string(type));
  if (data) json_object_object_add(message, "data", json_object_get(data));
  const char *text = json_object_to_json_string_ext(message, JSON_C_TO_STRING_PLAIN);
  int write_rc = socket_write_line(fd, text, strlen(text));
  if (write_rc != 0) {
    fprintf(stderr, "postmerkosctl: request write failed: %s\n", strerror(-write_rc));
    close(fd);
    json_object_put(message);
    return NULL;
  }
  shutdown(fd, SHUT_WR);
  char *buffer = malloc(RESPONSE_MAX + 2);
  if (!buffer) { close(fd); json_object_put(message); return NULL; }
  ssize_t got = socket_read_line(fd, buffer, RESPONSE_MAX + 2, 5000);
  close(fd);
  json_object_put(message);
  if (got <= 0) {
    if (got < 0) fprintf(stderr, "postmerkosctl: response read failed: %s\n", strerror((int)-got));
    else fprintf(stderr, "postmerkosctl: configd closed without a response\n");
    free(buffer);
    return NULL;
  }
  struct json_object *reply = json_tokener_parse(buffer);
  free(buffer);
  if (!reply) fprintf(stderr, "postmerkosctl: invalid response from configd\n");
  return reply;
}

static struct json_object *reply_data(struct json_object *reply) {
  struct json_object *type = NULL, *data = NULL;
  if (!reply || !json_object_object_get_ex(reply, "type", &type)) return NULL;
  if (!strcmp(json_object_get_string(type), "error")) {
    if (json_object_object_get_ex(reply, "data", &data)) {
      struct json_object *message = NULL;
      if (json_object_object_get_ex(data, "message", &message))
        fprintf(stderr, "postmerkosctl: %s\n", json_object_get_string(message));
    }
    return NULL;
  }
  return json_object_object_get_ex(reply, "data", &data) ? data : NULL;
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

static bool bool_member(struct json_object *object, const char *key, bool fallback) {
  struct json_object *value = member(object, key);
  return value && json_object_is_type(value, json_type_boolean)
      ? json_object_get_boolean(value) : fallback;
}

static bool capability_present(struct json_object *identity, const char *name) {
  struct json_object *caps = member(identity, "capabilities");
  if (!caps || !json_object_is_type(caps, json_type_array)) return false;
  for (size_t i = 0; i < json_object_array_length(caps); i++) {
    struct json_object *cap = json_object_array_get_idx(caps, i);
    if (cap && !strcmp(json_object_get_string(cap), name)) return true;
  }
  return false;
}

static void print_session_shell(struct json_object *identity) {
  const char *username = string_member(identity, "username", "");
  const char *role = string_member(identity, "role", "none");
  struct json_object *caps = member(identity, "capabilities");
  printf("POSTMERKOS_USERNAME='%s'\n", username);
  printf("POSTMERKOS_ROLE='%s'\n", role);
  fputs("POSTMERKOS_CAPABILITIES='", stdout);
  for (size_t i = 0; caps && json_object_is_type(caps, json_type_array) &&
       i < json_object_array_length(caps); i++) {
    struct json_object *cap = json_object_array_get_idx(caps, i);
    if (cap && json_object_is_type(cap, json_type_string))
      printf("%s%s", i ? " " : "", json_object_get_string(cap));
  }
  puts("'");
}

static void print_service_summary(struct json_object *data) {
  puts("SERVICE       STATE       ENABLED     AUTOSTART");
  puts("------------- ----------- ----------- -----------");
  const char *names[] = {"ssh", "web", "chrony", "mdns"};
  const char *labels[] = {"SSH", "Web UI", "Chrony", "mDNS"};
  for (size_t i = 0; i < 4; i++) {
    struct json_object *service = member(data, names[i]);
    printf("%-13s %-11s %-11s %-11s\n", labels[i],
           bool_member(service, "running", false) ? "running" : "stopped",
           bool_member(service, "enabled", false) ? "yes" : "no",
           bool_member(service, "autostart", false) ? "yes" : "no");
  }
}

static struct json_object *session_data(void) {
  struct json_object *reply = request("session", NULL);
  struct json_object *data = reply_data(reply);
  struct json_object *copy = data ? json_object_get(data) : NULL;
  if (reply) json_object_put(reply);
  return copy;
}

static struct json_object *snapshot_data(void) {
  struct json_object *reply = request("snapshot.get", NULL);
  struct json_object *data = reply_data(reply);
  struct json_object *copy = data ? json_object_get(data) : NULL;
  if (reply) json_object_put(reply);
  return copy;
}

static struct json_object *port_object(struct json_object *root, unsigned int port) {
  struct json_object *ports = member(root, "ports");
  char key[16]; snprintf(key, sizeof(key), "%u", port);
  return member(ports, key);
}

static void print_summary(struct json_object *snapshot) {
  struct json_object *status = member(snapshot, "status");
  struct json_object *caps = member(status, "capabilities");
  struct json_object *release = member(status, "release");
  struct json_object *network = member(member(status, "network"), "ipv4");
  printf("Model:              %s\n", string_member(status, "device", "unknown"));
  printf("Firmware:           %s\n", string_member(release, "version", "unknown"));
  printf("Compatibility:      %s\n", string_member(caps, "compatibility", "untested"));
  printf("Ports:              %d total, %d copper, %d uplink\n",
      int_member(caps, "port_count", 0), int_member(caps, "copper_ports", 0),
      int_member(caps, "uplink_ports", 0));
  printf("Management mode:    %s\n", string_member(network, "configured_mode", "unknown"));
  printf("Management state:   %s\n", string_member(network, "state", "unknown"));
  printf("Management address: %s\n", string_member(network, "address", "unavailable"));
  printf("Date/time:          %s\n", string_member(status, "datetime", "unknown"));
  if (bool_member(member(status, "security"), "default_password_active", false))
    puts("WARNING: The root password is still set to the device serial number.");
}

static void print_ports(struct json_object *snapshot, unsigned int first,
                        unsigned int last) {
  struct json_object *status = member(snapshot, "status");
  struct json_object *config = member(snapshot, "config");
  puts("PORT  LINK   SPEED  ADMIN     POE          VLAN  MODE    NAME");
  puts("----  -----  -----  --------  -----------  ----  ------  ------------------------");
  for (unsigned int port = first; port <= last; port++) {
    struct json_object *runtime = port_object(status, port);
    struct json_object *desired = port_object(config, port);
    struct json_object *link = member(runtime, "link");
    struct json_object *vlan = member(desired, "vlan");
    struct json_object *poe = member(desired, "poe");
    char poe_text[24] = "n/a";
    if (poe) snprintf(poe_text, sizeof(poe_text), "%s",
        bool_member(poe, "enabled", false) ? string_member(poe, "mode", "on") : "off");
    printf("%-4u  %-5s  %-5d  %-8s  %-11s  %-4d  %-6s  %s\n", port,
        bool_member(link, "established", false) ? "up" : "down",
        int_member(link, "speed", 0), bool_member(desired, "enabled", true) ? "enabled" : "disabled",
        poe_text, int_member(vlan, "pvid", 1), string_member(vlan, "mode", "access"),
        string_member(desired, "name", ""));
  }
}

static void print_port(struct json_object *snapshot, unsigned int port) {
  struct json_object *status = member(snapshot, "status");
  struct json_object *config = member(snapshot, "config");
  struct json_object *runtime = port_object(status, port);
  struct json_object *desired = port_object(config, port);
  if (!desired) { fprintf(stderr, "postmerkosctl: unknown port %u\n", port); return; }
  struct json_object *link = member(runtime, "link");
  struct json_object *vlan = member(desired, "vlan");
  struct json_object *stp = member(desired, "stp");
  struct json_object *poe = member(desired, "poe");
  printf("Port %u\n", port);
  printf("  Name:              %s\n", string_member(desired, "name", ""));
  printf("  Administrative:    %s\n", bool_member(desired, "enabled", true) ? "enabled" : "disabled");
  printf("  Link:              %s at %d Mbps\n", bool_member(link, "established", false) ? "up" : "down", int_member(link, "speed", 0));
  printf("  Configured speed:  %s\n", string_member(desired, "speed", "auto"));
  printf("  VLAN:              %s, PVID %d, allowed %s\n", string_member(vlan, "mode", "access"), int_member(vlan, "pvid", 1), string_member(vlan, "allowed", ""));
  printf("  STP:               %s, priority %d, cost %d\n", bool_member(stp, "enabled", true) ? "enabled" : "disabled", int_member(stp, "priority", 128), int_member(stp, "cost", 0));
  if (poe) printf("  PoE:               %s (%s)\n", bool_member(poe, "enabled", false) ? "enabled" : "disabled", string_member(poe, "mode", "at"));
  else puts("  PoE:               not supported");
}

static struct json_object *csv_integer_array(const char *text) {
  struct json_object *array = json_object_new_array();
  char *copy = strdup(text ? text : "");
  if (!copy) return array;
  char *save = NULL;
  for (char *token = strtok_r(copy, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
    while (*token == ' ' || *token == '\t') token++;
    char *end = NULL; long value = strtol(token, &end, 10);
    while (end && (*end == ' ' || *end == '\t')) end++;
    if (end && !*end && value > 0) json_object_array_add(array, json_object_new_int((int)value));
  }
  free(copy); return array;
}

static struct json_object *csv_string_array(const char *text) {
  struct json_object *array = json_object_new_array();
  char *copy = strdup(text ? text : "");
  if (!copy) return array;
  char *save = NULL;
  for (char *token = strtok_r(copy, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
    while (*token == ' ' || *token == '\t') token++;
    char *end = token + strlen(token);
    while (end > token && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    if (*token) json_object_array_add(array, json_object_new_string(token));
  }
  free(copy); return array;
}

static void print_timezones(struct json_object *catalogue) {
  if (!catalogue || !json_object_is_type(catalogue, json_type_array)) return;
  for (size_t i = 0; i < json_object_array_length(catalogue); i++) {
    struct json_object *group = json_object_array_get_idx(catalogue, i);
    printf("\n%s\n", string_member(group, "label", "Timezones"));
    struct json_object *zones = member(group, "zones");
    for (size_t j = 0; zones && j < json_object_array_length(zones); j++) {
      struct json_object *zone = json_object_array_get_idx(zones, j);
      printf("  %-36s %s\n", string_member(zone, "id", ""),
             string_member(zone, "label", ""));
    }
  }
}

static struct json_object *timezone_policy_by_id(struct json_object *catalogue,
                                                   const char *wanted) {
  if (!catalogue || !json_object_is_type(catalogue, json_type_array) || !wanted) return NULL;
  for (size_t i = 0; i < json_object_array_length(catalogue); i++) {
    struct json_object *group = json_object_array_get_idx(catalogue, i);
    struct json_object *zones = member(group, "zones");
    if (!zones || !json_object_is_type(zones, json_type_array)) continue;
    for (size_t j = 0; j < json_object_array_length(zones); j++) {
      struct json_object *zone = json_object_array_get_idx(zones, j);
      if (zone && !strcmp(string_member(zone, "id", ""), wanted)) {
        struct json_object *policy = json_object_new_object();
        json_object_object_add(policy, "timezone", json_object_new_string(wanted));
        struct json_object *offset = member(zone, "standard_offset_minutes");
        struct json_object *dst = member(zone, "dst");
        if (offset) json_object_object_add(policy, "standard_offset_minutes", json_object_get(offset));
        if (dst) json_object_object_add(policy, "dst", json_object_get(dst));
        return policy;
      }
    }
  }
  return NULL;
}

static int print_request_data(const char *type, struct json_object *data) {
  struct json_object *reply = request(type, data);
  struct json_object *result = reply_data(reply);
  if (!result) { if (reply) json_object_put(reply); return 1; }
  puts(json_object_to_json_string_ext(result, JSON_C_TO_STRING_PRETTY));
  json_object_put(reply); return 0;
}

static void usage(FILE *stream) {
  fputs("usage: postmerkosctl COMMAND [arguments]\n"
        "  session [--json|--shell] | role | has CAPABILITY\n"
        "  management-health [--quiet]\n"
        "  status | summary | ports FIRST-LAST | port PORT\n"
        "  get PATH | set PATH VALUE | set-string PATH TEXT | apply-json JSON\n"
        "  config | backup FILE | validate FILE | restore FILE | reboot\n"
        "  compatibility-needed | compatibility-report | compatibility-ack\n"
        "  default-password-active\n"
        "  users | user-create USER PASSWORD ROLE | user-role USER ROLE | user-delete USER\n"
        "  ssh-keys | ssh-key-add LABEL KEY | ssh-key-remove KEY\n"
        "  ports-clone SOURCE TARGETS_CSV FIELDS_CSV\n"
        "  services | services-summary | services-set JSON | services-apply | service NAME ACTION\n"
        "  service-get PATH | service-set PATH VALUE\n"
        "  system-info | system-identity | hostname-set NAME\n"
        "  time | timezones | timezone-set ID | time-set JSON | time-get PATH | time-set-field PATH VALUE\n"
        "  time-sync | time-set-clock EPOCH | time-set-local 'HH:MM:SS - DD:MM:YYYY'\n"
        "  firmware-repositories | firmware-repositories-set JSON | firmware-repository-check URL\n", stream);
}

static int write_config_file(const char *path, struct json_object *config) {
  mode_t mask = umask(0077);
  int rc = json_object_to_file_ext(path, config, JSON_C_TO_STRING_PRETTY);
  umask(mask);
  if (rc != 0) fprintf(stderr, "postmerkosctl: unable to write %s\n", path);
  return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  if (argc < 2) { usage(stderr); return 2; }
  const char *command = argv[1];
  if (!strcmp(command, "management-health"))
    return management_health(argc > 2 && !strcmp(argv[2], "--quiet"));
  if (!strcmp(command, "session") || !strcmp(command, "role") || !strcmp(command, "has")) {
    struct json_object *identity = session_data();
    if (!identity) return 1;
    int rc = 0;
    if (!strcmp(command, "role")) puts(string_member(identity, "role", "none"));
    else if (!strcmp(command, "has")) {
      if (argc != 3) { usage(stderr); rc = 2; }
      else rc = capability_present(identity, argv[2]) ? 0 : 1;
    } else if (argc > 2 && !strcmp(argv[2], "--json"))
      puts(json_object_to_json_string_ext(identity, JSON_C_TO_STRING_PRETTY));
    else if (argc > 2 && !strcmp(argv[2], "--shell"))
      print_session_shell(identity);
    else printf("%s (%s)\n", string_member(identity, "username", ""), string_member(identity, "role", "none"));
    json_object_put(identity); return rc;
  }
  if (!strcmp(command, "summary") || !strcmp(command, "ports") || !strcmp(command, "port")) {
    struct json_object *snapshot = snapshot_data();
    if (!snapshot) return 1;
    int rc = 0;
    if (!strcmp(command, "summary")) print_summary(snapshot);
    else if (!strcmp(command, "ports")) {
      unsigned int first = 0, last = 0;
      if (argc != 3 || (sscanf(argv[2], "%u-%u", &first, &last) != 2 && sscanf(argv[2], "%u", &first) != 1)) { usage(stderr); rc = 2; }
      else { if (!last) last = first; print_ports(snapshot, first, last); }
    } else {
      unsigned int port = argc == 3 ? (unsigned int)strtoul(argv[2], NULL, 10) : 0;
      if (!port) { usage(stderr); rc = 2; } else print_port(snapshot, port);
    }
    json_object_put(snapshot); return rc;
  }
  if (!strcmp(command, "status") || !strcmp(command, "config")) {
    struct json_object *reply = request(!strcmp(command, "status") ? "status.get" : "config.get", NULL);
    struct json_object *data = reply_data(reply);
    if (!data) { if (reply) json_object_put(reply); return 1; }
    puts(json_object_to_json_string_ext(data, JSON_C_TO_STRING_PRETTY));
    json_object_put(reply); return 0;
  }
  if (!strcmp(command, "get")) {
    if (argc != 3) { usage(stderr); return 2; }
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "path", json_object_new_string(argv[2]));
    struct json_object *reply = request("config.path.get", data); json_object_put(data);
    struct json_object *value = reply_data(reply);
    if (!value) { if (reply) json_object_put(reply); return 1; }
    if (json_object_is_type(value, json_type_string)) puts(json_object_get_string(value));
    else puts(json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN));
    json_object_put(reply); return 0;
  }
  if (!strcmp(command, "set") || !strcmp(command, "set-string")) {
    if (argc < 4) { usage(stderr); return 2; }
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "path", json_object_new_string(argv[2]));
    json_object_object_add(data, "value", json_object_new_string(argv[3]));
    json_object_object_add(data, "string", json_object_new_boolean(!strcmp(command, "set-string")));
    struct json_object *reply = request("config.path.set", data); json_object_put(data);
    struct json_object *result = reply_data(reply);
    if (!result) { if (reply) json_object_put(reply); return 1; }
    puts(string_member(result, "message", "Configuration accepted"));
    json_object_put(reply); return 0;
  }
  if (!strcmp(command, "apply-json")) {
    if (argc != 3) { usage(stderr); return 2; }
    struct json_object *delta = json_tokener_parse(argv[2]);
    if (!delta || !json_object_is_type(delta, json_type_object)) {
      if (delta) json_object_put(delta);
      fprintf(stderr, "postmerkosctl: invalid JSON delta\n");
      return 1;
    }
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "delta", json_object_get(delta));
    struct json_object *reply = request("config.delta", data);
    json_object_put(data); json_object_put(delta);
    struct json_object *result = reply_data(reply);
    if (!result) { if (reply) json_object_put(reply); return 1; }
    puts(string_member(result, "message", "Configuration accepted"));
    json_object_put(reply); return 0;
  }
  if (!strcmp(command, "backup")) {
    if (argc != 3) { usage(stderr); return 2; }
    struct json_object *reply = request("config.get", NULL);
    struct json_object *config = reply_data(reply);
    int rc = config ? write_config_file(argv[2], config) : 1;
    if (reply) json_object_put(reply);
    return rc;
  }
  if (!strcmp(command, "validate")) {
    if (argc != 3) { usage(stderr); return 2; }
    struct json_object *config = json_object_from_file(argv[2]);
    if (!config || !json_object_is_type(config, json_type_object)) {
      if (config) json_object_put(config);
      fprintf(stderr, "postmerkosctl: invalid JSON configuration file\n");
      return 1;
    }
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "config", json_object_get(config));
    struct json_object *reply = request("config.validate", data);
    json_object_put(data); json_object_put(config);
    struct json_object *result = reply_data(reply);
    if (!result) { if (reply) json_object_put(reply); return 1; }
    puts(string_member(result, "message", "Configuration is valid"));
    json_object_put(reply); return 0;
  }
  if (!strcmp(command, "restore")) {
    if (argc != 3) { usage(stderr); return 2; }
    struct json_object *config = json_object_from_file(argv[2]);
    if (!config || !json_object_is_type(config, json_type_object)) { if (config) json_object_put(config); fprintf(stderr, "postmerkosctl: invalid configuration file\n"); return 1; }
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "config", json_object_get(config));
    struct json_object *reply = request("config.replace", data);
    json_object_put(data); json_object_put(config);
    struct json_object *result = reply_data(reply);
    if (!result) { if (reply) json_object_put(reply); return 1; }
    puts(string_member(result, "message", "Configuration restored"));
    json_object_put(reply); return 0;
  }
  if (!strcmp(command, "compatibility-needed") ||
      !strcmp(command, "compatibility-report") ||
      !strcmp(command, "compatibility-ack")) {
    const char *type = !strcmp(command, "compatibility-needed") ? "compatibility.notice" :
                       !strcmp(command, "compatibility-report") ? "compatibility.report" :
                                                                  "compatibility.ack";
    struct json_object *reply = request(type, NULL);
    struct json_object *data = reply_data(reply);
    if (!data) { if (reply) json_object_put(reply); return 1; }
    if (!strcmp(command, "compatibility-needed"))
      puts(bool_member(data, "required", false) ? "yes" : "no");
    else if (!strcmp(command, "compatibility-ack"))
      puts(string_member(data, "message", "Compatibility notice acknowledged"));
    else
      puts(json_object_to_json_string_ext(data, JSON_C_TO_STRING_PRETTY));
    json_object_put(reply);
    return 0;
  }
  if (!strcmp(command, "default-password-active")) {
    struct json_object *reply = request("status.get", NULL);
    struct json_object *data = reply_data(reply);
    if (!data) { if (reply) json_object_put(reply); return 1; }
    puts(bool_member(member(data, "security"), "default_password_active", false) ? "yes" : "no");
    json_object_put(reply);
    return 0;
  }
  if (!strcmp(command, "users")) {
    struct json_object *reply = request("users.get", NULL);
    struct json_object *data = reply_data(reply);
    if (!data) { if (reply) json_object_put(reply); return 1; }
    puts(json_object_to_json_string_ext(data, JSON_C_TO_STRING_PRETTY));
    json_object_put(reply); return 0;
  }
  if (!strcmp(command, "user-create") || !strcmp(command, "user-role") ||
      !strcmp(command, "user-delete")) {
    int needed = !strcmp(command, "user-create") ? 5 :
                 !strcmp(command, "user-role") ? 4 : 3;
    if (argc != needed) { usage(stderr); return 2; }
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "username", json_object_new_string(argv[2]));
    const char *type = NULL;
    if (!strcmp(command, "user-create")) {
      json_object_object_add(data, "password", json_object_new_string(argv[3]));
      json_object_object_add(data, "role", json_object_new_string(argv[4]));
      type = "users.create";
    } else if (!strcmp(command, "user-role")) {
      json_object_object_add(data, "role", json_object_new_string(argv[3]));
      type = "users.role";
    } else type = "users.delete";
    struct json_object *reply = request(type, data); json_object_put(data);
    struct json_object *result = reply_data(reply);
    if (!result) { if (reply) json_object_put(reply); return 1; }
    puts(json_object_to_json_string_ext(result, JSON_C_TO_STRING_PRETTY));
    json_object_put(reply); return 0;
  }
  if (!strcmp(command, "ssh-keys") || !strcmp(command, "ssh-key-add") ||
      !strcmp(command, "ssh-key-remove")) {
    struct json_object *data = NULL; const char *type = "ssh.keys.get";
    if (!strcmp(command, "ssh-key-add")) {
      if (argc != 4) { usage(stderr); return 2; }
      data = json_object_new_object();
      json_object_object_add(data, "label", json_object_new_string(argv[2]));
      json_object_object_add(data, "key", json_object_new_string(argv[3]));
      type = "ssh.keys.add";
    } else if (!strcmp(command, "ssh-key-remove")) {
      if (argc != 3) { usage(stderr); return 2; }
      data = json_object_new_object();
      json_object_object_add(data, "key", json_object_new_string(argv[2]));
      type = "ssh.keys.remove";
    } else if (argc != 2) { usage(stderr); return 2; }
    int rc = print_request_data(type, data); if (data) json_object_put(data); return rc;
  }
  if (!strcmp(command, "ports-clone")) {
    if (argc != 5) { usage(stderr); return 2; }
    char *end = NULL; long source = strtol(argv[2], &end, 10);
    if (!end || *end || source <= 0) { fprintf(stderr, "postmerkosctl: invalid source port\n"); return 2; }
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "source", json_object_new_int((int)source));
    json_object_object_add(data, "targets", csv_integer_array(argv[3]));
    json_object_object_add(data, "fields", csv_string_array(argv[4]));
    int rc = print_request_data("ports.clone", data); json_object_put(data); return rc;
  }
  if (!strcmp(command, "system-info") || !strcmp(command, "system-identity"))
    return print_request_data(!strcmp(command, "system-info") ? "status.get" : "system.identity.get", NULL);
  if (!strcmp(command, "hostname-set")) {
    if (argc != 3) { usage(stderr); return 2; }
    struct json_object *current_reply = request("system.identity.get", NULL);
    struct json_object *current = reply_data(current_reply);
    struct json_object *policy = current ? member(current, "policy") : NULL;
    struct json_object *data = policy ? json_object_get(policy) : json_object_new_object();
    json_object_object_add(data, "hostname", json_object_new_string(argv[2]));
    struct json_object *reply = request("system.identity.set", data); json_object_put(data);
    if (current_reply) json_object_put(current_reply);
    struct json_object *result = reply_data(reply);
    if (!result) { if (reply) json_object_put(reply); return 1; }
    puts(string_member(result, "message", "System identity updated")); json_object_put(reply); return 0;
  }
  if (!strcmp(command, "firmware-repositories"))
    return print_request_data("firmware.repositories.get", NULL);
  if (!strcmp(command, "firmware-repositories-set")) {
    if (argc != 3) { usage(stderr); return 2; }
    struct json_object *data = json_tokener_parse(argv[2]);
    if (!data || !json_object_is_type(data, json_type_object)) { if (data) json_object_put(data); fprintf(stderr, "postmerkosctl: invalid repository JSON\n"); return 2; }
    int rc = print_request_data("firmware.repositories.set", data); json_object_put(data); return rc;
  }
  if (!strcmp(command, "firmware-repository-check")) {
    if (argc != 3) { usage(stderr); return 2; }
    struct json_object *data = json_object_new_object(); json_object_object_add(data, "source", json_object_new_string(argv[2]));
    int rc = print_request_data("firmware.repository.check", data); json_object_put(data); return rc;
  }
  if (!strcmp(command, "services") || !strcmp(command, "services-summary") ||
      !strcmp(command, "time")) {
    bool service_command = strcmp(command, "time") != 0;
    struct json_object *reply=request(service_command ? "services.get" : "time.get",NULL);
    struct json_object *data=reply_data(reply); if(!data){if(reply)json_object_put(reply);return 1;}
    if (!strcmp(command, "services-summary")) print_service_summary(data);
    else puts(json_object_to_json_string_ext(data,JSON_C_TO_STRING_PRETTY));
    json_object_put(reply);return 0;
  }
  if (!strcmp(command, "service-get") || !strcmp(command, "time-get")) {
    if(argc!=3){usage(stderr);return 2;}struct json_object *data=json_object_new_object();json_object_object_add(data,"path",json_object_new_string(argv[2]));
    struct json_object *reply=request(!strcmp(command,"service-get")?"services.path.get":"time.path.get",data);json_object_put(data);struct json_object *value=reply_data(reply);if(!value){if(reply)json_object_put(reply);return 1;}if(json_object_is_type(value,json_type_string))puts(json_object_get_string(value));else puts(json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN));json_object_put(reply);return 0;
  }
  if (!strcmp(command, "service-set") || !strcmp(command, "time-set-field")) {
    if(argc!=4){usage(stderr);return 2;}struct json_object *data=json_object_new_object();json_object_object_add(data,"path",json_object_new_string(argv[2]));json_object_object_add(data,"value",json_object_new_string(argv[3]));
    struct json_object *reply=request(!strcmp(command,"service-set")?"services.path.set":"time.path.set",data);json_object_put(data);struct json_object *result=reply_data(reply);if(!result){if(reply)json_object_put(reply);return 1;}puts(string_member(result,"message","Policy updated"));json_object_put(reply);return 0;
  }
  if (!strcmp(command, "services-apply")) {
    struct json_object *reply=request("services.apply",NULL);struct json_object *result=reply_data(reply);if(!result){if(reply)json_object_put(reply);return 1;}puts(string_member(result,"message","Service policy applied"));json_object_put(reply);return 0;
  }
  if (!strcmp(command, "services-set") || !strcmp(command, "time-set")) {
    if (argc != 3) { usage(stderr); return 2; }
    struct json_object *data = json_tokener_parse(argv[2]);
    if (!data || !json_object_is_type(data, json_type_object)) {
      if (data) json_object_put(data);
      fprintf(stderr, "postmerkosctl: invalid JSON policy\n");
      return 1;
    }
    struct json_object *reply = request(!strcmp(command, "services-set") ?
                                        "services.set" : "time.set", data);
    json_object_put(data);
    struct json_object *result = reply_data(reply);
    if (!result) { if (reply) json_object_put(reply); return 1; }
    puts(string_member(result, "message", "Policy saved"));
    json_object_put(reply);
    return 0;
  }
  if (!strcmp(command, "service")) {
    if(argc!=4){usage(stderr);return 2;}struct json_object *data=json_object_new_object();
    json_object_object_add(data,"service",json_object_new_string(argv[2]));json_object_object_add(data,"action",json_object_new_string(argv[3]));
    struct json_object *reply=request("services.action",data);json_object_put(data);struct json_object *result=reply_data(reply);if(!result){if(reply)json_object_put(reply);return 1;}puts(string_member(result,"message","Service action completed"));json_object_put(reply);return 0;
  }
  if (!strcmp(command, "timezones")) {
    struct json_object *reply = request("timezones.get", NULL);
    struct json_object *data = reply_data(reply);
    if (!data) { if (reply) json_object_put(reply); return 1; }
    print_timezones(data); json_object_put(reply); return 0;
  }
  if (!strcmp(command, "timezone-set")) {
    if (argc != 3) { usage(stderr); return 2; }
    struct json_object *zones_reply = request("timezones.get", NULL);
    struct json_object *zones = reply_data(zones_reply);
    struct json_object *policy = timezone_policy_by_id(zones, argv[2]);
    if (!policy) { if (zones_reply) json_object_put(zones_reply); fprintf(stderr, "postmerkosctl: unknown timezone\n"); return 2; }
    struct json_object *time_reply = request("time.get", NULL);
    struct json_object *time_data = reply_data(time_reply);
    struct json_object *current = time_data ? member(time_data, "policy") : NULL;
    if (current) {
      struct json_object *value = member(current, "ntp_enabled"); if (value) json_object_object_add(policy, "ntp_enabled", json_object_get(value));
      value = member(current, "servers"); if (value) json_object_object_add(policy, "servers", json_object_get(value));
    }
    struct json_object *reply = request("time.set", policy); json_object_put(policy);
    if (zones_reply) json_object_put(zones_reply);
    if (time_reply) json_object_put(time_reply);
    struct json_object *result = reply_data(reply); if (!result) { if (reply) json_object_put(reply); return 1; }
    puts(string_member(result, "message", "Time policy saved")); json_object_put(reply); return 0;
  }
  if (!strcmp(command, "time-sync")) {
    struct json_object *reply=request("time.sync",NULL);struct json_object *result=reply_data(reply);if(!result){if(reply)json_object_put(reply);return 1;}puts(string_member(result,"message","Time synchronization requested"));json_object_put(reply);return 0;
  }
  if (!strcmp(command, "time-set-local")) {
    if (argc != 3) { usage(stderr); return 2; }
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "local", json_object_new_string(argv[2]));
    struct json_object *reply = request("time.set_clock", data); json_object_put(data);
    struct json_object *result = reply_data(reply); if (!result) { if (reply) json_object_put(reply); return 1; }
    puts(string_member(result, "message", "System clock updated")); json_object_put(reply); return 0;
  }
  if (!strcmp(command, "time-set-clock")) {
    if (argc != 3) { usage(stderr); return 2; }
    char *end = NULL;
    long long epoch = strtoll(argv[2], &end, 10);
    if (!end || *end) {
      fprintf(stderr, "postmerkosctl: invalid epoch\n");
      return 2;
    }
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "epoch", json_object_new_int64(epoch));
    struct json_object *reply = request("time.set_clock", data);
    json_object_put(data);
    struct json_object *result = reply_data(reply);
    if (!result) { if (reply) json_object_put(reply); return 1; }
    puts(string_member(result, "message", "System clock updated"));
    json_object_put(reply);
    return 0;
  }
  if (!strcmp(command, "reboot")) {
    struct json_object *reply = request("system.reboot", NULL);
    struct json_object *data = reply_data(reply);
    if (!data) { if (reply) json_object_put(reply); return 1; }
    puts(string_member(data, "message", "Reboot scheduled"));
    json_object_put(reply); return 0;
  }
  usage(stderr); return 2;
}
