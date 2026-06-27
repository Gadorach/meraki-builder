#include "click_global.h"
#include "click_port.h"
#include "config_apply.h"
#include "config_file.h"
#include "configd.h"
#include "console_cli.h"
#include "network.h"
#include "local_socket.h"
#include "result.h"
#include "status.h"
#include "telemetry.h"
#include "validation.h"
#include "websocket.h"

#include <libpostmerkos.h>
#include <libpd690xx.h>
#include "pd690xx_meraki.h"

#include <errno.h>
#include <getopt.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

bool dry_run = false;
const char *config_file = "/etc/switch.json";
char meraki_mac[18] = "";
struct hardware_info hardware;
struct pd690xx_cfg pd690xx = {
  {-1, -1},
  {PD690XX0_I2C_ADDR, PD690XX1_I2C_ADDR,
   PD690XX2_I2C_ADDR, PD690XX3_I2C_ADDR},
  {0, 0, 0, 0}
};

static volatile sig_atomic_t running = 1;
static time_t service_started_at;

static void write_exit_record(const char *reason, int exit_code) {
  const char *path = getenv("POSTMERKOS_CONFIGD_EXIT");
  if (!path || !*path) path = "/run/postmerkos/configd.exit";
  mkdir("/run/postmerkos", 0755);
  char temporary[256];
  snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid());
  FILE *file = fopen(temporary, "w");
  if (!file) return;
  time_t now = time(NULL);
  fprintf(file, "exit_code=%d\nreason=%s\ntimestamp=%ld\nuptime_seconds=%ld\n",
          exit_code, reason ? reason : "unknown", (long)now,
          service_started_at ? (long)(now - service_started_at) : 0L);
  if (fclose(file) == 0) rename(temporary, path);
  else unlink(temporary);
}

static void signal_handler(int signal_number) {
  (void)signal_number;
  running = 0;
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
  fprintf(stderr, "%s network: management address changed %s -> %s; rebinding management services\n",
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
    fprintf(stderr, "%s network: management-service rebind hook failed\n", get_time());
}

static void service_network_poll(void) {
  struct network_runtime before = *network_manager_runtime();
  struct apply_result result;
  apply_result_init(&result);
  bool changed = network_manager_poll(&result);
  const struct network_runtime *after = network_manager_runtime();
  if (json_object_array_length(result.warnings) > 0)
    fprintf(stderr, "%s network: %s\n", get_time(),
            json_object_to_json_string(result.warnings));
  apply_result_cleanup(&result);
  if (management_address_changed(&before, after))
    rebind_management_services(&before, after);
  if (changed) mark_clients_pending();
}

static void usage(FILE *stream, const char *program) {
  fprintf(stream,
      "Usage: %s [options]\n"
      "  -c, --config PATH          persistent configuration file\n"
      "  -d, --dry-run              log writes without changing hardware\n"
      "  -p, --status-interval SEC  status broadcast interval (1-3600)\n"
      "  -w, --websocket-port PORT  WebSocket listen port (1-65535)\n"
      "  -s, --socket PATH          local management Unix socket\n"
      "  -N, --network-bootstrap    apply DHCP/static management IP and exit\n"
      "  -W, --network-wait SEC     DHCP wait during bootstrap (0-300, default 60)\n"
      "      --boot-output          concise bootstrap output; save JSON under /run\n"
      "      --get-config           print the current persistent JSON envelope\n"
      "      --get-config-raw       print only the current configuration object\n"
      "      --get-status           print current status JSON\n"
      "      --get-path PATH        print one configuration value\n"
      "      --set-path PATH VALUE  update/apply a JSON-typed configuration value\n"
      "      --set-string PATH TEXT update/apply a literal string value\n"
      "      --show-summary         print a text system summary\n"
      "      --show-ports RANGE     print a text port table (for example 1-12)\n"
      "      --show-port PORT       print detailed text state for one port\n"
      "      --export-config FILE   write a plain JSON configuration backup\n"
      "      --validate FILE        validate a complete configuration file\n"
      "      --apply-file FILE      merge and apply a JSON configuration delta\n"
      "      --apply-json JSON      merge and apply an inline JSON delta\n"
      "      --replace-file FILE    validate, replace, and apply full config\n"
      "  -h, --help                 show this help\n",
      program);
}

static int parse_int(const char *text, int minimum, int maximum, int *output) {
  char *end = NULL;
  errno = 0;
  long value = strtol(text, &end, 10);
  if (errno || !text || !*text || !end || *end ||
      value < minimum || value > maximum) return -EINVAL;
  *output = (int)value;
  return 0;
}

static void read_meraki_mac(void) {
  const char *paths[] = {"/tmp/MERAKI_MAC", "/etc/MERAKI_MAC"};
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    FILE *file = fopen(paths[i], "r");
    if (!file) continue;
    if (fgets(meraki_mac, sizeof(meraki_mac), file)) {
      meraki_mac[strcspn(meraki_mac, "\r\n")] = '\0';
      fclose(file);
      return;
    }
    fclose(file);
  }
}

static struct json_object *make_envelope(const char *type, struct json_object *data) {
  struct json_object *message = json_object_new_object();
  json_object_object_add(message, "type", json_object_new_string(type));
  json_object_object_add(message, "data", json_object_get(data));
  return message;
}

static void print_envelope(const char *type, struct json_object *data) {
  struct json_object *message = json_object_new_object();
  json_object_object_add(message, "type", json_object_new_string(type));
  json_object_object_add(message, "data", json_object_get(data));
  puts(json_object_to_json_string_ext(message, JSON_C_TO_STRING_PRETTY));
  json_object_put(message);
}

static int print_bad_request(const char *detail) {
  struct json_object *data = json_object_new_object();
  json_object_object_add(data, "status", json_object_new_int(400));
  json_object_object_add(data, "message", json_object_new_string("Bad Request"));
  if (detail) json_object_object_add(data, "detail", json_object_new_string(detail));
  print_envelope("error", data);
  json_object_put(data);
  return 2;
}

enum command_mode {
  COMMAND_SERVICE,
  COMMAND_NETWORK_BOOTSTRAP,
  COMMAND_GET_CONFIG,
  COMMAND_GET_CONFIG_RAW,
  COMMAND_GET_STATUS,
  COMMAND_GET_PATH,
  COMMAND_SET_PATH,
  COMMAND_SET_STRING,
  COMMAND_SHOW_SUMMARY,
  COMMAND_SHOW_PORTS,
  COMMAND_SHOW_PORT,
  COMMAND_EXPORT_CONFIG,
  COMMAND_VALIDATE,
  COMMAND_APPLY_FILE,
  COMMAND_APPLY_JSON,
  COMMAND_REPLACE_FILE,
  COMMAND_FEATURES,
};

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  int websocket_port = 4001;
  int status_interval = 3;
  int network_wait = 60;
  bool boot_output = false;
  const char *local_socket_path = "/run/postmerkos/configd.sock";
  enum command_mode command = COMMAND_SERVICE;
  const char *command_value = NULL;
  const char *command_value2 = NULL;

  enum { OPT_GET_CONFIG = 1000, OPT_GET_CONFIG_RAW, OPT_GET_STATUS,
         OPT_GET_PATH, OPT_SET_PATH, OPT_SET_STRING, OPT_SHOW_SUMMARY, OPT_SHOW_PORTS,
         OPT_SHOW_PORT, OPT_EXPORT_CONFIG, OPT_VALIDATE,
         OPT_APPLY_FILE, OPT_APPLY_JSON, OPT_REPLACE_FILE, OPT_FEATURES,
         OPT_BOOT_OUTPUT };
  static const struct option options[] = {
    {"config", required_argument, NULL, 'c'},
    {"dry-run", no_argument, NULL, 'd'},
    {"status-interval", required_argument, NULL, 'p'},
    {"websocket-port", required_argument, NULL, 'w'},
    {"socket", required_argument, NULL, 's'},
    {"network-bootstrap", no_argument, NULL, 'N'},
    {"network-wait", required_argument, NULL, 'W'},
    {"boot-output", no_argument, NULL, OPT_BOOT_OUTPUT},
    {"get-config", no_argument, NULL, OPT_GET_CONFIG},
    {"get-config-raw", no_argument, NULL, OPT_GET_CONFIG_RAW},
    {"get-status", no_argument, NULL, OPT_GET_STATUS},
    {"get-path", required_argument, NULL, OPT_GET_PATH},
    {"set-path", required_argument, NULL, OPT_SET_PATH},
    {"set-string", required_argument, NULL, OPT_SET_STRING},
    {"show-summary", no_argument, NULL, OPT_SHOW_SUMMARY},
    {"show-ports", required_argument, NULL, OPT_SHOW_PORTS},
    {"show-port", required_argument, NULL, OPT_SHOW_PORT},
    {"export-config", required_argument, NULL, OPT_EXPORT_CONFIG},
    {"validate", required_argument, NULL, OPT_VALIDATE},
    {"apply-file", required_argument, NULL, OPT_APPLY_FILE},
    {"apply-json", required_argument, NULL, OPT_APPLY_JSON},
    {"replace-file", required_argument, NULL, OPT_REPLACE_FILE},
    {"features", no_argument, NULL, OPT_FEATURES},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
  };

  int option;
  while ((option = getopt_long(argc, argv, "c:dp:w:s:NW:h", options, NULL)) != -1) {
    switch (option) {
      case 'c': config_file = optarg; break;
      case 'd': dry_run = true; break;
      case 'p':
        if (parse_int(optarg, 1, 3600, &status_interval) != 0) {
          fprintf(stderr, "invalid status interval\n");
          return 2;
        }
        break;
      case 's': local_socket_path = optarg; break;
      case 'w':
        if (parse_int(optarg, 1, 65535, &websocket_port) != 0) {
          fprintf(stderr, "invalid WebSocket port\n");
          return 2;
        }
        break;
      case 'N': command = COMMAND_NETWORK_BOOTSTRAP; break;
      case 'W':
        if (parse_int(optarg, 0, 300, &network_wait) != 0) {
          fprintf(stderr, "invalid network wait interval\n");
          return 2;
        }
        break;
      case OPT_BOOT_OUTPUT: boot_output = true; break;
      case OPT_GET_CONFIG: command = COMMAND_GET_CONFIG; break;
      case OPT_GET_CONFIG_RAW: command = COMMAND_GET_CONFIG_RAW; break;
      case OPT_GET_STATUS: command = COMMAND_GET_STATUS; break;
      case OPT_GET_PATH: command = COMMAND_GET_PATH; command_value = optarg; break;
      case OPT_SET_PATH: command = COMMAND_SET_PATH; command_value = optarg; break;
      case OPT_SET_STRING: command = COMMAND_SET_STRING; command_value = optarg; break;
      case OPT_SHOW_SUMMARY: command = COMMAND_SHOW_SUMMARY; break;
      case OPT_SHOW_PORTS: command = COMMAND_SHOW_PORTS; command_value = optarg; break;
      case OPT_SHOW_PORT: command = COMMAND_SHOW_PORT; command_value = optarg; break;
      case OPT_EXPORT_CONFIG: command = COMMAND_EXPORT_CONFIG; command_value = optarg; break;
      case OPT_VALIDATE: command = COMMAND_VALIDATE; command_value = optarg; break;
      case OPT_APPLY_FILE: command = COMMAND_APPLY_FILE; command_value = optarg; break;
      case OPT_APPLY_JSON: command = COMMAND_APPLY_JSON; command_value = optarg; break;
      case OPT_REPLACE_FILE: command = COMMAND_REPLACE_FILE; command_value = optarg; break;
      case OPT_FEATURES: command = COMMAND_FEATURES; break;
      case 'h': usage(stdout, argv[0]); return 0;
      default: usage(stderr, argv[0]); return 2;
    }
  }
  if (command == COMMAND_SET_PATH || command == COMMAND_SET_STRING) {
    if (optind + 1 != argc) {
      fprintf(stderr, "%s requires exactly one VALUE argument\n",
              command == COMMAND_SET_STRING ? "--set-string" : "--set-path");
      return 2;
    }
    command_value2 = argv[optind++];
  }
  if (optind != argc) {
    usage(stderr, argv[0]);
    return 2;
  }

  if (command == COMMAND_FEATURES) {
    puts("core: enabled");
    puts("unix-socket: enabled");
#ifdef CONFIGD_ENABLE_WEBSOCKET
    puts("websocket: enabled");
    printf("websocket-port: %d\n", websocket_port);
    puts("websocket-protocol: configd-ws");
#else
    puts("websocket: disabled");
#endif
    return 0;
  }

  /* S10 invokes bootstrap before the PoE init script. Keep this path
   * network-only: do not probe I2C and do not create the complete switch
   * configuration from a graph that is still receiving its baseline state. */
  if (command == COMMAND_NETWORK_BOOTSTRAP) {
    char network_error[256];
    struct json_object *config = load_config_file();
    if (!config || network_validate_config(config, network_error,
                                            sizeof(network_error)) != 0) {
      if (config) json_object_put(config);
      config = json_object_new_object();
      json_object_object_add(config, "network", network_default_config());
    }
    if (boot_output) setenv("POSTMERKOS_BOOT_OUTPUT", "1", 1);
    network_manager_observe(config);
    const struct network_runtime *observed = network_manager_runtime();
    if (!strcmp(observed->configured_mode, "dhcp") &&
        strcmp(observed->source, "dhcp") && network_wait > 0) {
      if (boot_output) {
        fprintf(stdout, "postmerkOS network: WAIT source=dhcp timeout=%ds\n", network_wait);
        fflush(stdout);
      } else {
        fprintf(stderr, "Waiting up to %d seconds for a DHCP lease", network_wait);
        fflush(stderr);
      }
      for (int elapsed = 0; elapsed < network_wait; elapsed++) {
        sleep(1);
        network_manager_observe(config);
        observed = network_manager_runtime();
        if (!strcmp(observed->source, "dhcp")) break;
        if (!boot_output) { fputc('.', stderr); fflush(stderr); }
      }
      if (!boot_output) fputc('\n', stderr);
      observed = network_manager_runtime();
      if (!boot_output) {
        if (!strcmp(observed->source, "dhcp"))
          fprintf(stderr, "DHCP lease detected: %s/%u via %s\n",
                  observed->applied.address, observed->applied.prefix,
                  observed->applied.gateway);
        else
          fprintf(stderr, "No DHCP lease detected during bootstrap; applying fallback\n");
      }
    }

    struct apply_result result;
    apply_result_init(&result);
    int network_rc = network_manager_init(config, &result);
    const struct network_runtime *runtime = network_manager_runtime();
    struct json_object *data = apply_result_json(&result,
                                                 "Network bootstrap complete");
    if (boot_output) {
      struct json_object *envelope = make_envelope("ack", data);
      const char *record_path = getenv("POSTMERKOS_NETWORK_BOOTSTRAP_RECORD");
      if (!record_path || !*record_path) {
        mkdir("/run/postmerkos", 0755);
        record_path = "/run/postmerkos/network-bootstrap.json";
      }
      json_object_to_file_ext(record_path, envelope, JSON_C_TO_STRING_PRETTY);
      json_object_put(envelope);
      size_t warnings = json_object_array_length(result.warnings);
      size_t failed = json_object_array_length(result.failures);
      const char *state = network_rc != 0 || failed ? "FAIL" : warnings ? "WARN" : "PASS";
      printf("postmerkOS network: %s source=%s address=%s/%u gateway=%s broadcast=%s mtu=%u applied=%u warnings=%zu failed=%zu\n",
             state, runtime->source[0] ? runtime->source : "unknown",
             runtime->applied.address[0] ? runtime->applied.address : "0.0.0.0",
             runtime->applied.prefix,
             runtime->applied.gateway[0] ? runtime->applied.gateway : "0.0.0.0",
             runtime->applied.broadcast[0] ? runtime->applied.broadcast : "0.0.0.0",
             runtime->applied.mtu, result.applied, warnings, failed);
      for (size_t i = 0; i < warnings; i++)
        printf("postmerkOS network: WARN: %s\n",
               json_object_get_string(json_object_array_get_idx(result.warnings, i)));
      for (size_t i = 0; i < failed; i++)
        printf("postmerkOS network: FAIL: %s\n",
               json_object_get_string(json_object_array_get_idx(result.failures, i)));
    } else {
      if (runtime->applied.address[0])
        fprintf(stderr, "Management IPv4 configured: source=%s address=%s/%u gateway=%s broadcast=%s mtu=%u\n",
                runtime->source, runtime->applied.address, runtime->applied.prefix,
                runtime->applied.gateway, runtime->applied.broadcast, runtime->applied.mtu);
      print_envelope("ack", data);
    }
    json_object_put(data);
    apply_result_cleanup(&result);
    json_object_put(config);
    return network_rc == 0 ? 0 : 1;
  }

  hardware_init(&hardware, &pd690xx);
  read_meraki_mac();

  if (command == COMMAND_VALIDATE) {
    struct json_object *candidate = load_json_file(command_value);
    if (!candidate) return print_bad_request("unable to parse validation file");
    char error[256];
    int rc = validate_configuration(candidate, error, sizeof(error));
    json_object_put(candidate);
    if (rc != 0) return print_bad_request(error);
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "message",
                           json_object_new_string("Configuration is valid"));
    print_envelope("ack", data);
    json_object_put(data);
    i2c_close(&pd690xx);
    return 0;
  }

  char error[256] = "";
  struct json_object *config = config_load_or_create(error, sizeof(error));
  if (!config) {
    fprintf(stderr, "configd: %s\n", error);
    i2c_close(&pd690xx);
    return 1;
  }

  if (command == COMMAND_GET_CONFIG || command == COMMAND_GET_CONFIG_RAW) {
    if (command == COMMAND_GET_CONFIG)
      print_envelope("config", config);
    else
      puts(json_object_to_json_string_ext(config, JSON_C_TO_STRING_PRETTY));
    json_object_put(config);
    i2c_close(&pd690xx);
    return 0;
  }

  if (command == COMMAND_GET_PATH) {
    int rc = console_print_path(config, command_value);
    if (rc != 0) fprintf(stderr, "configuration path not found: %s\n", command_value);
    json_object_put(config);
    i2c_close(&pd690xx);
    return rc == 0 ? 0 : 2;
  }

  if (command == COMMAND_EXPORT_CONFIG) {
    int rc = console_export_config(config, command_value, error, sizeof(error));
    if (rc != 0) fprintf(stderr, "configd: backup failed: %s\n", error);
    json_object_put(config);
    i2c_close(&pd690xx);
    return rc == 0 ? 0 : 1;
  }

  if (command == COMMAND_SHOW_SUMMARY) {
    int rc = console_print_summary(config);
    json_object_put(config);
    i2c_close(&pd690xx);
    return rc == 0 ? 0 : 1;
  }

  if (command == COMMAND_SHOW_PORTS) {
    unsigned int first = 0, last = 0;
    char trailing = '\0';
    int count = sscanf(command_value, "%u-%u%c", &first, &last, &trailing);
    if (count == 1) last = first;
    if ((count != 1 && count != 2) || !first || last < first ||
        (hardware.port_count && last > hardware.port_count)) {
      fprintf(stderr, "invalid port range: %s\n", command_value);
      json_object_put(config);
      i2c_close(&pd690xx);
      return 2;
    }
    int rc = console_print_ports(config, first, last);
    json_object_put(config);
    i2c_close(&pd690xx);
    return rc == 0 ? 0 : 1;
  }

  if (command == COMMAND_SHOW_PORT) {
    int port = 0;
    if (parse_int(command_value, 1, 128, &port) != 0 ||
        !hardware_port_valid(&hardware, (unsigned int)port)) {
      fprintf(stderr, "invalid hardware port: %s\n", command_value);
      json_object_put(config);
      i2c_close(&pd690xx);
      return 2;
    }
    int rc = console_print_port(config, (unsigned int)port);
    json_object_put(config);
    i2c_close(&pd690xx);
    return rc == 0 ? 0 : 1;
  }

  struct apply_result startup;
  apply_result_init(&startup);
  if (command == COMMAND_GET_STATUS) {
    /* Populate network status without writing Click handlers from a read-only
     * CLI request. */
    network_manager_observe(config);
    struct json_object *status = get_status();
    print_envelope("status", status);
    json_object_put(status);
    apply_result_cleanup(&startup);
    json_object_put(config);
    i2c_close(&pd690xx);
    return 0;
  }

  network_manager_init(config, &startup);
  const struct network_runtime *net_rt = network_manager_runtime();
  const char *mgmt_addr = (net_rt && net_rt->applied.address[0]) ? net_rt->applied.address : NULL;
  telemetry_apply(config, mgmt_addr);

  if (command == COMMAND_REPLACE_FILE) {
    struct json_object *candidate = load_json_file(command_value);
    if (!candidate) {
      apply_result_cleanup(&startup);
      json_object_put(config);
      i2c_close(&pd690xx);
      return print_bad_request("unable to parse replacement configuration");
    }
    struct apply_result result;
    apply_result_init(&result);
    int rc = config_replace_validate_save_apply(candidate, &result,
                                                 error, sizeof(error));
    if (rc != 0) {
      json_object_put(candidate);
      apply_result_cleanup(&result);
      apply_result_cleanup(&startup);
      json_object_put(config);
      i2c_close(&pd690xx);
      return print_bad_request(error[0] ? error : "replacement configuration could not be applied");
    }
    struct json_object *data = apply_result_json(&result,
                                                 "Configuration replaced");
    print_envelope("ack", data);
    json_object_put(data);
    json_object_put(candidate);
    apply_result_cleanup(&result);
    apply_result_cleanup(&startup);
    json_object_put(config);
    i2c_close(&pd690xx);
    return 0;
  }

  if (command == COMMAND_APPLY_FILE || command == COMMAND_APPLY_JSON ||
      command == COMMAND_SET_PATH || command == COMMAND_SET_STRING) {
    struct json_object *delta = NULL;
    if (command == COMMAND_APPLY_FILE)
      delta = load_json_file(command_value);
    else if (command == COMMAND_APPLY_JSON)
      delta = json_tokener_parse(command_value);
    else if (command == COMMAND_SET_PATH)
      delta = console_delta_from_path(command_value, command_value2,
                                      error, sizeof(error));
    else
      delta = console_delta_from_string_path(command_value, command_value2,
                                             error, sizeof(error));
    if (!delta) {
      apply_result_cleanup(&startup);
      json_object_put(config);
      i2c_close(&pd690xx);
      return print_bad_request(error[0] ? error : "unable to parse configuration delta");
    }
    struct apply_result result;
    apply_result_init(&result);
    struct json_object *saved = NULL;
    int rc = config_merge_validate_save_apply(delta, &saved, &result, false,
                                               error, sizeof(error));
    json_object_put(delta);
    if (rc != 0) {
      apply_result_cleanup(&result);
      apply_result_cleanup(&startup);
      json_object_put(config);
      i2c_close(&pd690xx);
      return print_bad_request(error);
    }
    struct json_object *data = apply_result_json(&result,
                                                 "Configuration accepted");
    print_envelope("ack", data);
    json_object_put(data);
    json_object_put(saved);
    apply_result_cleanup(&result);
    apply_result_cleanup(&startup);
    json_object_put(config);
    i2c_close(&pd690xx);
    return 0;
  }

  /* Desired state is replayed on every daemon start. This is required for
   * write-only Click settings such as storm control. */
  click_apply_globals_full(config, &startup);
  click_apply_ports_full(config, &startup);
  if (json_object_array_length(startup.warnings) > 0)
    fprintf(stderr, "%s configd: startup completed with warnings: %s\n",
            get_time(), json_object_to_json_string(startup.warnings));
  apply_result_cleanup(&startup);
  json_object_put(config);

  service_started_at = time(NULL);
  unlink("/run/postmerkos/configd.exit");
  int local_fd = local_socket_init(local_socket_path);
  if (local_fd < 0) {
    fprintf(stderr, "configd: local socket setup failed: %s\n", strerror(-local_fd));
    write_exit_record("local-socket-setup-failed", 1);
    i2c_close(&pd690xx);
    return 1;
  }
  printf("configd: local management socket listening at %s\n", local_socket_path);

#ifdef CONFIGD_ENABLE_WEBSOCKET
  struct lws_context *context = ws_init(websocket_port);
  if (!context) {
    fprintf(stderr, "configd: WebSocket context creation failed\n");
    write_exit_record("websocket-context-creation-failed", 1);
    local_socket_shutdown(local_fd, local_socket_path);
    i2c_close(&pd690xx);
    return 1;
  }
  ws_schedule_timers(context, status_interval);
  printf("configd: WebSocket server listening on port %d\n", websocket_port);
#else
  (void)websocket_port;
  (void)status_interval;
#endif

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  long network_poll_due = 0;
  long telemetry_poll_due = 0;
  int service_exit_code = 0;
  const char *service_exit_reason = "signal-requested";
  while (running) {
    long now = time(NULL);
    if (now >= network_poll_due) {
      service_network_poll();
      unsigned int next_poll = network_manager_next_poll_seconds();
      if (next_poll < 1) next_poll = 1;
      network_poll_due = now + (long)next_poll;
    }
    if (now >= telemetry_poll_due) {
      telemetry_tick();
      telemetry_poll_due = now + telemetry_interval_seconds();
    }
    int local_rc = local_socket_service_once(local_fd, 50);
    if (local_rc < 0) {
      fprintf(stderr, "configd: FAIL local-socket-service rc=%d error=%s\n",
              local_rc, strerror(-local_rc));
      service_exit_code = 1;
      service_exit_reason = "local-socket-service-error";
      break;
    }
#ifdef CONFIGD_ENABLE_WEBSOCKET
    int ws_rc = ws_service_once(context, 0);
    if (ws_rc < 0) {
      fprintf(stderr, "configd: FAIL websocket-service rc=%d\n", ws_rc);
      service_exit_code = 1;
      service_exit_reason = "websocket-service-error";
      break;
    }
    mark_clients_pending();
#endif
  }

  fprintf(stderr, "configd: shutting down reason=%s exit_code=%d\n",
          service_exit_reason, service_exit_code);
  write_exit_record(service_exit_reason, service_exit_code);
#ifdef CONFIGD_ENABLE_WEBSOCKET
  ws_shutdown(context);
#endif
  local_socket_shutdown(local_fd, local_socket_path);
  i2c_close(&pd690xx);
  return service_exit_code;
}
