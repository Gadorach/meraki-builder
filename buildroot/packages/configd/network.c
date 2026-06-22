#include "network.h"
#include "configd.h"

#include <libpostmerkos.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_FALLBACK "169.254.0.10/16"
#define DEFAULT_MTU 1500U
#define MIN_POLL_SECONDS 5U
#define MAX_POLL_SECONDS 300U
#define ERROR_POLL_SECONDS 10U

struct dhcp_lease {
  bool valid;
  struct ipv4_runtime ipv4;
  unsigned int vlan;
  unsigned int renew_in;
  unsigned int expires_in;
};

static struct network_runtime runtime_state;
static struct json_object *active_config;
static struct json_object *pending_config;
static time_t pending_apply_at;
static unsigned int next_poll_seconds = MIN_POLL_SECONDS;

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value && *value ? value : fallback;
}

static void set_error(char *error, size_t error_size, const char *message) {
  if (error && error_size)
    snprintf(error, error_size, "%s", message ? message : "error");
}

static void runtime_set_error(const char *message) {
  snprintf(runtime_state.last_error, sizeof(runtime_state.last_error), "%s",
           message ? message : "");
  if (message && *message)
    snprintf(runtime_state.state, sizeof(runtime_state.state), "error");
}

static bool ipv4_equal(const struct ipv4_runtime *a,
                       const struct ipv4_runtime *b) {
  return a && b && !strcmp(a->address, b->address) &&
         a->prefix == b->prefix && !strcmp(a->gateway, b->gateway) &&
         !strcmp(a->broadcast, b->broadcast) && a->mtu == b->mtu;
}

static int parse_ipv4(const char *text, struct in_addr *address) {
  if (!text || !*text || !address) return -EINVAL;
  return inet_pton(AF_INET, text, address) == 1 ? 0 : -EINVAL;
}

static int copy_ipv4(char destination[16], const char *source) {
  struct in_addr parsed;
  if (!destination || parse_ipv4(source, &parsed) != 0) return -EINVAL;
  return inet_ntop(AF_INET, &parsed, destination, 16) ? 0 : -errno;
}

static void address_to_string(uint32_t host_order, char *buffer, size_t size) {
  struct in_addr address = { .s_addr = htonl(host_order) };
  if (!inet_ntop(AF_INET, &address, buffer, size))
    snprintf(buffer, size, "0.0.0.0");
}

int network_parse_cidr(const char *cidr, struct ipv4_runtime *value,
                       char *error, size_t error_size) {
  if (!cidr || !value) {
    set_error(error, error_size, "IPv4 CIDR is required");
    return -EINVAL;
  }

  char copy[64];
  int copied = snprintf(copy, sizeof(copy), "%s", cidr);
  if (copied < 0 || (size_t)copied >= sizeof(copy)) {
    set_error(error, error_size, "IPv4 CIDR is too long");
    return -EINVAL;
  }
  char *slash = strchr(copy, '/');
  if (!slash || strchr(slash + 1, '/')) {
    set_error(error, error_size, "IPv4 address must use CIDR notation");
    return -EINVAL;
  }
  *slash++ = '\0';
  char *end = NULL;
  unsigned long prefix = strtoul(slash, &end, 10);
  if (!*slash || !end || *end || prefix > 32) {
    set_error(error, error_size, "IPv4 prefix must be between 0 and 32");
    return -EINVAL;
  }

  struct in_addr address;
  if (parse_ipv4(copy, &address) != 0) {
    set_error(error, error_size, "invalid IPv4 address");
    return -EINVAL;
  }
  uint32_t host = ntohl(address.s_addr);
  if ((host & 0xf0000000U) == 0xe0000000U || host == 0 ||
      host == 0xffffffffU) {
    set_error(error, error_size, "IPv4 address is not usable unicast");
    return -EINVAL;
  }

  memset(value, 0, sizeof(*value));
  if (!inet_ntop(AF_INET, &address, value->address, sizeof(value->address))) {
    set_error(error, error_size, "unable to format IPv4 address");
    return -EINVAL;
  }
  value->prefix = (unsigned int)prefix;
  value->mtu = DEFAULT_MTU;
  uint32_t mask = prefix == 0 ? 0 : 0xffffffffU << (32 - prefix);
  uint32_t network = host & mask;
  uint32_t broadcast = network | ~mask;
  if (prefix <= 30 && (host == network || host == broadcast)) {
    set_error(error, error_size, "IPv4 address cannot be the network or broadcast address");
    return -EINVAL;
  }
  address_to_string(broadcast, value->broadcast, sizeof(value->broadcast));
  return 0;
}

static int derive_fallback(const char *cidr, unsigned int mtu,
                           struct ipv4_runtime *value,
                           char *error, size_t error_size) {
  int rc = network_parse_cidr(cidr, value, error, error_size);
  if (rc != 0) return rc;
  snprintf(value->gateway, sizeof(value->gateway), "0.0.0.0");
  value->mtu = mtu;
  return 0;
}

struct json_object *network_default_config(void) {
  struct json_object *network = json_object_new_object();
  struct json_object *ipv4 = json_object_new_object();
  json_object_object_add(ipv4, "mode", json_object_new_string("dhcp"));
  json_object_object_add(ipv4, "fallback_address",
                         json_object_new_string(DEFAULT_FALLBACK));
  json_object_object_add(ipv4, "mtu", json_object_new_int(DEFAULT_MTU));
  json_object_object_add(network, "ipv4", ipv4);
  return network;
}

static int get_ipv4_config(struct json_object *config,
                           struct json_object **ipv4,
                           char *error, size_t error_size) {
  struct json_object *network = NULL;
  if (!config || !json_object_is_type(config, json_type_object) ||
      !json_object_object_get_ex(config, "network", &network) ||
      !json_object_is_type(network, json_type_object) ||
      !json_object_object_get_ex(network, "ipv4", ipv4) ||
      !json_object_is_type(*ipv4, json_type_object)) {
    set_error(error, error_size, "network.ipv4 must be an object");
    return -EINVAL;
  }
  return 0;
}

int network_validate_config(struct json_object *config,
                            char *error, size_t error_size) {
  struct json_object *ipv4 = NULL;
  if (get_ipv4_config(config, &ipv4, error, error_size) != 0) return -EINVAL;

  struct json_object *mode_obj = NULL;
  if (!json_object_object_get_ex(ipv4, "mode", &mode_obj) ||
      !json_object_is_type(mode_obj, json_type_string)) {
    set_error(error, error_size, "network.ipv4.mode must be a string");
    return -EINVAL;
  }
  const char *mode = json_object_get_string(mode_obj);
  if (strcmp(mode, "dhcp") && strcmp(mode, "static")) {
    set_error(error, error_size, "network.ipv4.mode must be dhcp or static");
    return -EINVAL;
  }

  struct json_object *mtu_obj = NULL;
  if (!json_object_object_get_ex(ipv4, "mtu", &mtu_obj) ||
      !json_object_is_type(mtu_obj, json_type_int)) {
    set_error(error, error_size, "network.ipv4.mtu must be an integer");
    return -EINVAL;
  }
  int mtu = json_object_get_int(mtu_obj);
  if (mtu < 576 || mtu > 9216) {
    set_error(error, error_size, "network.ipv4.mtu must be between 576 and 9216");
    return -EINVAL;
  }

  struct ipv4_runtime parsed;
  struct json_object *address_obj = NULL;
  if (!strcmp(mode, "static")) {
    if (!json_object_object_get_ex(ipv4, "address", &address_obj) ||
        !json_object_is_type(address_obj, json_type_string)) {
      set_error(error, error_size,
                "network.ipv4.address is required in static mode");
      return -EINVAL;
    }
    if (network_parse_cidr(json_object_get_string(address_obj), &parsed,
                           error, error_size) != 0) return -EINVAL;

    struct json_object *gateway_obj = NULL;
    if (json_object_object_get_ex(ipv4, "gateway", &gateway_obj)) {
      if (!json_object_is_type(gateway_obj, json_type_string)) {
        set_error(error, error_size, "network.ipv4.gateway must be a string");
        return -EINVAL;
      }
      const char *gateway_text = json_object_get_string(gateway_obj);
      if (gateway_text && *gateway_text && strcmp(gateway_text, "0.0.0.0")) {
        struct in_addr gateway;
        if (parse_ipv4(gateway_text, &gateway) != 0) {
          set_error(error, error_size, "network.ipv4.gateway is invalid");
          return -EINVAL;
        }
        struct in_addr address;
        parse_ipv4(parsed.address, &address);
        uint32_t mask = parsed.prefix == 0 ? 0 :
                        0xffffffffU << (32 - parsed.prefix);
        if ((ntohl(address.s_addr) & mask) != (ntohl(gateway.s_addr) & mask)) {
          set_error(error, error_size,
                    "network.ipv4.gateway must be in the configured subnet");
          return -EINVAL;
        }
      }
    }
  } else {
    if (!json_object_object_get_ex(ipv4, "fallback_address", &address_obj) ||
        !json_object_is_type(address_obj, json_type_string)) {
      set_error(error, error_size,
                "network.ipv4.fallback_address is required in DHCP mode");
      return -EINVAL;
    }
    if (network_parse_cidr(json_object_get_string(address_obj), &parsed,
                           error, error_size) != 0) return -EINVAL;
  }
  return 0;
}

static int read_key_value(const char *path, const char *key,
                          char *value, size_t value_size) {
  FILE *file = fopen(path, "r");
  if (!file) return -errno;
  char line[256];
  int rc = -ENOENT;
  size_t key_len = strlen(key);
  while (fgets(line, sizeof(line), file)) {
    if (!strncmp(line, key, key_len) && line[key_len] == '=') {
      char *start = line + key_len + 1;
      start[strcspn(start, "\r\n")] = '\0';
      int copied = snprintf(value, value_size, "%s", start);
      rc = copied < 0 || (size_t)copied >= value_size ? -ENOSPC : 0;
      break;
    }
  }
  fclose(file);
  return rc;
}

static int parse_prefix(const char *text, unsigned int *prefix) {
  if (!text || !*text || !prefix) return -EINVAL;

  if (!strchr(text, '.')) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *end || value > 32) return -EINVAL;
    *prefix = (unsigned int)value;
    return 0;
  }

  struct in_addr address;
  if (parse_ipv4(text, &address) != 0) return -EINVAL;
  uint32_t mask = ntohl(address.s_addr);
  bool saw_zero = false;
  unsigned int bits = 0;
  for (int bit = 31; bit >= 0; bit--) {
    bool set = (mask & (1U << bit)) != 0;
    if (set && saw_zero) return -EINVAL;
    if (set) bits++;
    else saw_zero = true;
  }
  *prefix = bits;
  return 0;
}

static void parse_dns_values(const char *text, struct ipv4_runtime *value) {
  if (!text || !value) return;
  char copy[96];
  if (snprintf(copy, sizeof(copy), "%s", text) >= (int)sizeof(copy)) return;
  char *save = NULL;
  unsigned int index = 0;
  for (char *token = strtok_r(copy, " ,\t\r\n", &save);
       token && index < 2;
       token = strtok_r(NULL, " ,\t\r\n", &save)) {
    struct in_addr parsed;
    if (parse_ipv4(token, &parsed) != 0 || !strcmp(token, "0.0.0.0"))
      continue;
    snprintf(value->dns[index], sizeof(value->dns[index]), "%s", token);
    index++;
  }
}

static int parse_dhcp_brain(struct dhcp_lease *lease,
                            char *error, size_t error_size) {
  const char *path = env_or_default(
      "CONFIGD_DHCP_BRAIN", "/click/uplinkstate/dhcpc_state_for_brain");
  char address[32] = "";
  char subnet[32] = "";
  char gateway[32] = "";
  char broadcast[32] = "";
  char dns[96] = "";

  int rc = read_key_value(path, "ip", address, sizeof(address));
  if (rc != 0 || !strcmp(address, "0.0.0.0")) {
    set_error(error, error_size,
              rc == -ENOENT ? "no DHCP address in brain state" :
              rc < 0 ? strerror(-rc) : "no DHCP address in brain state");
    return rc != 0 ? rc : -ENOENT;
  }
  if (read_key_value(path, "subnet", subnet, sizeof(subnet)) != 0)
    subnet[0] = '\0';
  if (read_key_value(path, "router", gateway, sizeof(gateway)) != 0)
    gateway[0] = '\0';
  if (read_key_value(path, "broadcast", broadcast, sizeof(broadcast)) != 0)
    broadcast[0] = '\0';
  if (read_key_value(path, "dns", dns, sizeof(dns)) != 0)
    dns[0] = '\0';

  unsigned int prefix = 0;
  if (parse_prefix(subnet, &prefix) != 0) {
    set_error(error, error_size,
              "DHCP brain state is missing a valid subnet/prefix");
    return -EINVAL;
  }

  char cidr[64];
  if (snprintf(cidr, sizeof(cidr), "%s/%u", address, prefix) >=
      (int)sizeof(cidr) ||
      network_parse_cidr(cidr, &lease->ipv4, error, error_size) != 0)
    return -EINVAL;

  struct in_addr parsed;
  if (gateway[0] && parse_ipv4(gateway, &parsed) == 0)
    copy_ipv4(lease->ipv4.gateway, gateway);
  else
    snprintf(lease->ipv4.gateway, sizeof(lease->ipv4.gateway), "0.0.0.0");
  if (broadcast[0] && parse_ipv4(broadcast, &parsed) == 0)
    copy_ipv4(lease->ipv4.broadcast, broadcast);
  parse_dns_values(dns, &lease->ipv4);
  lease->ipv4.mtu = DEFAULT_MTU;
  lease->vlan = 1;
  lease->renew_in = MIN_POLL_SECONDS;
  lease->expires_in = 0;
  lease->valid = true;
  return 0;
}

static int parse_dhcp_state(struct dhcp_lease *lease,
                            char *error, size_t error_size) {
  memset(lease, 0, sizeof(*lease));
  const char *path = env_or_default("CONFIGD_DHCP_STATE",
                                    "/click/uplinkstate/dhcp_state");
  FILE *file = fopen(path, "r");
  if (!file) {
    int state_error = -errno;
    int brain_rc = parse_dhcp_brain(lease, error, error_size);
    if (brain_rc == 0) return 0;
    if (state_error != -ENOENT)
      set_error(error, error_size, strerror(-state_error));
    return brain_rc != -ENOENT ? brain_rc : state_error;
  }

  char line[512];
  bool header = true;
  while (fgets(line, sizeof(line), file)) {
    if (header) { header = false; continue; }
    char *tokens[20] = {0};
    size_t count = 0;
    char *save = NULL;
    for (char *token = strtok_r(line, " \t\r\n", &save);
         token && count < 20;
         token = strtok_r(NULL, " \t\r\n", &save)) {
      tokens[count++] = token;
    }
    if (count < 16 || strcmp(tokens[2], "true") ||
        strcmp(tokens[3], "bound")) continue;

    char *end = NULL;
    lease->vlan = (unsigned int)strtoul(tokens[0], &end, 10);
    if (!end || *end) continue;
    unsigned long renew = strtoul(tokens[8], &end, 10);
    if (!end || *end) continue;
    unsigned long expires = strtoul(tokens[9], &end, 10);
    if (!end || *end) continue;
    unsigned long mtu = strtoul(tokens[15], &end, 10);
    if (!end || *end || mtu < 576 || mtu > 9216) mtu = DEFAULT_MTU;

    if (network_parse_cidr(tokens[10], &lease->ipv4,
                           error, error_size) != 0) continue;
    struct in_addr parsed;
    if (parse_ipv4(tokens[11], &parsed) != 0 ||
        parse_ipv4(tokens[12], &parsed) != 0) continue;
    snprintf(lease->ipv4.gateway, sizeof(lease->ipv4.gateway), "%s",
             tokens[11]);
    snprintf(lease->ipv4.broadcast, sizeof(lease->ipv4.broadcast), "%s",
             tokens[12]);
    if (parse_ipv4(tokens[13], &parsed) == 0)
      snprintf(lease->ipv4.dns[0], sizeof(lease->ipv4.dns[0]), "%s",
               tokens[13]);
    if (parse_ipv4(tokens[14], &parsed) == 0)
      snprintf(lease->ipv4.dns[1], sizeof(lease->ipv4.dns[1]), "%s",
               tokens[14]);
    lease->ipv4.mtu = (unsigned int)mtu;
    lease->renew_in = (unsigned int)renew;
    lease->expires_in = (unsigned int)expires;
    lease->valid = true;
    break;
  }
  fclose(file);

  if (!lease->valid) {
    int brain_rc = parse_dhcp_brain(lease, error, error_size);
    if (brain_rc == 0) return 0;
    set_error(error, error_size, "no active bound DHCP lease");
    return -ENOENT;
  }

  const char *brain = env_or_default(
      "CONFIGD_DHCP_BRAIN", "/click/uplinkstate/dhcpc_state_for_brain");
  struct in_addr parsed;
  char brain_value[96];
  if (read_key_value(brain, "ip", brain_value, sizeof(brain_value)) == 0 &&
      parse_ipv4(brain_value, &parsed) == 0)
    copy_ipv4(lease->ipv4.address, brain_value);
  if (read_key_value(brain, "router", brain_value, sizeof(brain_value)) == 0 &&
      parse_ipv4(brain_value, &parsed) == 0)
    copy_ipv4(lease->ipv4.gateway, brain_value);
  if (read_key_value(brain, "broadcast", brain_value,
                     sizeof(brain_value)) == 0 &&
      parse_ipv4(brain_value, &parsed) == 0)
    copy_ipv4(lease->ipv4.broadcast, brain_value);
  if (read_key_value(brain, "subnet", brain_value,
                     sizeof(brain_value)) == 0) {
    unsigned int prefix = 0;
    if (parse_prefix(brain_value, &prefix) == 0)
      lease->ipv4.prefix = prefix;
  }
  if (read_key_value(brain, "dns", brain_value, sizeof(brain_value)) == 0)
    parse_dns_values(brain_value, &lease->ipv4);
  return 0;
}

/* configd is the authoritative management-IP owner, but historically it pushed
 * the DNS resolver only into the Click datapath and never populated the Linux
 * /etc/resolv.conf. Linux userspace clients (chrony, etc.) therefore had no
 * resolver until the first DHCP lease renewal, leaving the clock stuck at the
 * epoch. Render resolv.conf from the active DNS servers so userspace resolution
 * works from boot, for both DHCP leases and static configuration. */
int network_render_resolv_conf(const struct ipv4_runtime *value) {
  if (!value) return -EINVAL;

  char desired[160];
  size_t len = 0;
  int n = snprintf(desired, sizeof(desired),
                   "# Generated by configd from the active network configuration.\n");
  if (n < 0 || (size_t)n >= sizeof(desired)) return -EOVERFLOW;
  len = (size_t)n;
  size_t servers = 0;
  for (size_t i = 0; i < 2; i++) {
    if (!value->dns[i][0]) continue;
    n = snprintf(desired + len, sizeof(desired) - len, "nameserver %s\n",
                 value->dns[i]);
    if (n < 0 || (size_t)n >= sizeof(desired) - len) return -EOVERFLOW;
    len += (size_t)n;
    servers++;
  }
  /* Never wipe a working resolver because a transient lease carried no DNS. */
  if (servers == 0) return 0;

  const char *path = env_or_default("CONFIGD_RESOLV_CONF", "/etc/resolv.conf");

  /* Skip the rewrite when the file already matches, to avoid NOR flash wear. */
  FILE *existing = fopen(path, "r");
  if (existing) {
    char current[160];
    size_t got = fread(current, 1, sizeof(current), existing);
    fclose(existing);
    if (got == len && memcmp(current, desired, len) == 0) return 0;
  }

  char temp[280];
  n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
  if (n < 0 || (size_t)n >= sizeof(temp)) return -EOVERFLOW;
  FILE *out = fopen(temp, "w");
  if (!out) return -errno;
  int rc = 0;
  if (fwrite(desired, 1, len, out) != len) rc = -EIO;
  if (rc == 0 && fflush(out) != 0) rc = -errno;
  if (rc == 0 && fsync(fileno(out)) != 0) rc = -errno;
  if (fclose(out) != 0 && rc == 0) rc = -errno;
  if (rc == 0 && rename(temp, path) != 0) rc = -errno;
  if (rc != 0) unlink(temp);
  return rc;
}

static int apply_ipv4(const struct ipv4_runtime *value, const char *source,
                      struct apply_result *result) {
  if (!value || !source) return -EINVAL;
  if (ipv4_equal(&runtime_state.applied, value) &&
      !strcmp(runtime_state.source, source)) return 0;

  char command[128];
  snprintf(command, sizeof(command), "%s %u %s %u %s 1",
           value->address, value->prefix, value->gateway,
           value->mtu, value->broadcast);
  if (!getenv("POSTMERKOS_BOOT_OUTPUT"))
    printf("%s network source=%s set_host_ip=%s%s\n", get_time(), source,
           command, dry_run ? " dry_run=true" : "");

  int rc = dry_run ? 0 : click_write("/click/set_host_ip/run", command);
  if (rc != 0) {
    char message[192];
    snprintf(message, sizeof(message), "set_host_ip failed: %s",
             strerror(-rc));
    runtime_set_error(message);
    apply_result_warn(result, "%s", message);
    return rc;
  }

  if (!dry_run) {
    int secondary = click_write("/click/syslog_event_log/src_ip",
                                value->address);
    if (secondary != 0)
      apply_result_warn(result, "syslog source update failed: %s",
                        strerror(-secondary));
    secondary = click_write("/click/wan0_resolver/dst", value->gateway);
    if (secondary != 0)
      apply_result_warn(result, "resolver gateway update failed: %s",
                        strerror(-secondary));
    secondary = network_render_resolv_conf(value);
    if (secondary != 0)
      apply_result_warn(result, "resolv.conf update failed: %s",
                        strerror(-secondary));
    secondary = write_switch_port_table("have_network_connection", "true");
    if (secondary != 0)
      apply_result_warn(result, "network connection flag update failed: %s",
                        strerror(-secondary));
  }

  bool new_dhcp_lease = !strcmp(source, "dhcp") &&
      (strcmp(runtime_state.source, "dhcp") ||
       !ipv4_equal(&runtime_state.applied, value));
  runtime_state.applied = *value;
  snprintf(runtime_state.source, sizeof(runtime_state.source), "%s", source);
  if (new_dhcp_lease) runtime_state.lease_acquired_at = (long)time(NULL);
  else if (strcmp(source, "dhcp")) runtime_state.lease_acquired_at = 0;
  snprintf(runtime_state.state, sizeof(runtime_state.state), "%s",
           !strcmp(source, "dhcp") ? "bound" : "applied");
  runtime_state.last_error[0] = '\0';
  runtime_state.last_change = (long)time(NULL);
  apply_result_applied(result);
  return 0;
}

static int parse_desired(struct json_object *config,
                         char *mode, size_t mode_size,
                         struct ipv4_runtime *static_value,
                         struct ipv4_runtime *fallback_value,
                         char *error, size_t error_size) {
  struct json_object *ipv4 = NULL;
  if (get_ipv4_config(config, &ipv4, error, error_size) != 0) return -EINVAL;
  const char *mode_text = json_object_get_string(
      json_object_object_get(ipv4, "mode"));
  snprintf(mode, mode_size, "%s", mode_text);
  struct json_object *mtu_obj = json_object_object_get(ipv4, "mtu");
  unsigned int mtu = (unsigned int)json_object_get_int(mtu_obj);

  if (!strcmp(mode, "static")) {
    const char *address = json_object_get_string(
        json_object_object_get(ipv4, "address"));
    if (network_parse_cidr(address, static_value,
                           error, error_size) != 0) return -EINVAL;
    static_value->mtu = mtu;
    const char *gateway = json_object_get_string(
        json_object_object_get(ipv4, "gateway"));
    snprintf(static_value->gateway, sizeof(static_value->gateway), "%s",
             gateway);
  } else {
    const char *fallback = json_object_get_string(
        json_object_object_get(ipv4, "fallback_address"));
    if (derive_fallback(fallback, mtu, fallback_value,
                        error, error_size) != 0) return -EINVAL;
  }
  return 0;
}

static int configure_now(struct json_object *config,
                         struct apply_result *result) {
  char error[192];
  char mode[8];
  struct ipv4_runtime static_value;
  struct ipv4_runtime fallback_value;
  memset(&static_value, 0, sizeof(static_value));
  memset(&fallback_value, 0, sizeof(fallback_value));
  if (parse_desired(config, mode, sizeof(mode), &static_value,
                    &fallback_value, error, sizeof(error)) != 0) {
    runtime_set_error(error);
    apply_result_warn(result, "network configuration rejected internally: %s",
                      error);
    return -EINVAL;
  }
  snprintf(runtime_state.configured_mode,
           sizeof(runtime_state.configured_mode), "%s", mode);

  if (!strcmp(mode, "static")) {
    next_poll_seconds = 60;
    runtime_state.renew_in = 0;
    runtime_state.expires_in = 0;
    runtime_state.lease_expires_at = 0;
    runtime_state.lease_acquired_at = 0;
    runtime_state.consecutive_misses = 0;
    return apply_ipv4(&static_value, "static", result);
  }

  struct dhcp_lease lease;
  if (parse_dhcp_state(&lease, error, sizeof(error)) == 0) {
    runtime_state.renew_in = lease.renew_in;
    runtime_state.expires_in = lease.expires_in;
    runtime_state.lease_expires_at = (long)time(NULL) + lease.expires_in;
    runtime_state.consecutive_misses = 0;
    next_poll_seconds = lease.renew_in < MIN_POLL_SECONDS ? MIN_POLL_SECONDS :
                        lease.renew_in > MAX_POLL_SECONDS ? MAX_POLL_SECONDS :
                        lease.renew_in;
    return apply_ipv4(&lease.ipv4, "dhcp", result);
  }

  runtime_state.consecutive_misses++;
  runtime_state.renew_in = 0;
  next_poll_seconds = MIN_POLL_SECONDS;
  time_t now = time(NULL);

  /* A user changing from static to DHCP should not lose access merely because
   * no lease is available yet. Keep the already-applied static address until
   * Click reports a valid lease, then move directly to that lease. */
  if (!strcmp(runtime_state.source, "static") &&
      runtime_state.applied.address[0]) {
    runtime_state.expires_in = 0;
    snprintf(runtime_state.state, sizeof(runtime_state.state), "waiting");
    snprintf(runtime_state.last_error, sizeof(runtime_state.last_error), "%s",
             error);
    return 0;
  }

  if (!strcmp(runtime_state.source, "dhcp") &&
      runtime_state.lease_expires_at > (long)now) {
    runtime_state.expires_in =
        (unsigned int)(runtime_state.lease_expires_at - now);
    snprintf(runtime_state.state, sizeof(runtime_state.state), "renewing");
    snprintf(runtime_state.last_error, sizeof(runtime_state.last_error), "%s",
             error);
    return 0;
  }

  runtime_state.expires_in = 0;
  int rc = apply_ipv4(&fallback_value, "fallback", result);
  if (rc == 0) {
    snprintf(runtime_state.state, sizeof(runtime_state.state), "waiting");
    snprintf(runtime_state.last_error, sizeof(runtime_state.last_error), "%s",
             error);
  }
  return rc;
}

static void retain_active_config(struct json_object *config) {
  if (active_config) json_object_put(active_config);
  active_config = json_object_get(config);
}

int network_manager_init(struct json_object *config,
                         struct apply_result *result) {
  memset(&runtime_state, 0, sizeof(runtime_state));
  snprintf(runtime_state.source, sizeof(runtime_state.source), "none");
  snprintf(runtime_state.state, sizeof(runtime_state.state), "initializing");
  retain_active_config(config);
  return configure_now(config, result);
}

int network_manager_observe(struct json_object *config) {
  char error[192];
  char mode[8];
  struct ipv4_runtime static_value;
  struct ipv4_runtime fallback_value;
  memset(&runtime_state, 0, sizeof(runtime_state));
  memset(&static_value, 0, sizeof(static_value));
  memset(&fallback_value, 0, sizeof(fallback_value));
  snprintf(runtime_state.source, sizeof(runtime_state.source), "none");
  snprintf(runtime_state.state, sizeof(runtime_state.state), "unknown");
  retain_active_config(config);

  if (parse_desired(config, mode, sizeof(mode), &static_value,
                    &fallback_value, error, sizeof(error)) != 0) {
    runtime_set_error(error);
    return -EINVAL;
  }
  snprintf(runtime_state.configured_mode,
           sizeof(runtime_state.configured_mode), "%s", mode);

  if (!strcmp(mode, "static")) {
    runtime_state.applied = static_value;
    snprintf(runtime_state.source, sizeof(runtime_state.source), "static");
    snprintf(runtime_state.state, sizeof(runtime_state.state), "configured");
    return 0;
  }

  struct dhcp_lease lease;
  if (parse_dhcp_state(&lease, error, sizeof(error)) == 0) {
    runtime_state.applied = lease.ipv4;
    runtime_state.renew_in = lease.renew_in;
    runtime_state.expires_in = lease.expires_in;
    runtime_state.lease_expires_at = (long)time(NULL) + lease.expires_in;
    runtime_state.lease_acquired_at = (long)time(NULL);
    snprintf(runtime_state.source, sizeof(runtime_state.source), "dhcp");
    snprintf(runtime_state.state, sizeof(runtime_state.state), "bound");
    return 0;
  }

  runtime_state.applied = fallback_value;
  snprintf(runtime_state.source, sizeof(runtime_state.source), "fallback");
  snprintf(runtime_state.state, sizeof(runtime_state.state), "waiting");
  snprintf(runtime_state.last_error, sizeof(runtime_state.last_error), "%s",
           error);
  return 0;
}

int network_manager_configure(struct json_object *config,
                              struct apply_result *result,
                              bool defer_address_change) {
  if (!config) return -EINVAL;
  retain_active_config(config);
  if (!defer_address_change) return configure_now(config, result);

  if (pending_config) json_object_put(pending_config);
  pending_config = json_object_get(config);
  pending_apply_at = time(NULL) + 1;
  snprintf(runtime_state.state, sizeof(runtime_state.state), "applying");
  next_poll_seconds = 1;
  return 0;
}

bool network_manager_poll(struct apply_result *result) {
  struct ipv4_runtime before = runtime_state.applied;
  char source_before[sizeof(runtime_state.source)];
  char state_before[sizeof(runtime_state.state)];
  snprintf(source_before, sizeof(source_before), "%s", runtime_state.source);
  snprintf(state_before, sizeof(state_before), "%s", runtime_state.state);

  if (pending_config && time(NULL) >= pending_apply_at) {
    struct json_object *config = pending_config;
    pending_config = NULL;
    configure_now(config, result);
    json_object_put(config);
  } else if (!pending_config && active_config &&
             !strcmp(runtime_state.configured_mode, "dhcp")) {
    configure_now(active_config, result);
  } else if (!pending_config) {
    next_poll_seconds = 60;
  }

  return !ipv4_equal(&before, &runtime_state.applied) ||
         strcmp(source_before, runtime_state.source) ||
         strcmp(state_before, runtime_state.state);
}

unsigned int network_manager_next_poll_seconds(void) {
  return next_poll_seconds ? next_poll_seconds : ERROR_POLL_SECONDS;
}

const struct network_runtime *network_manager_runtime(void) {
  return &runtime_state;
}

struct json_object *network_manager_status_json(void) {
  struct json_object *network = json_object_new_object();
  struct json_object *ipv4 = json_object_new_object();
  json_object_object_add(ipv4, "configured_mode",
      json_object_new_string(runtime_state.configured_mode[0] ?
                             runtime_state.configured_mode : "dhcp"));
  json_object_object_add(ipv4, "source",
                         json_object_new_string(runtime_state.source));
  json_object_object_add(ipv4, "state",
                         json_object_new_string(runtime_state.state));
  if (runtime_state.applied.address[0]) {
    char cidr[32];
    snprintf(cidr, sizeof(cidr), "%s/%u", runtime_state.applied.address,
             runtime_state.applied.prefix);
    json_object_object_add(ipv4, "address", json_object_new_string(cidr));
    json_object_object_add(ipv4, "gateway",
        json_object_new_string(runtime_state.applied.gateway));
    json_object_object_add(ipv4, "broadcast",
        json_object_new_string(runtime_state.applied.broadcast));
    json_object_object_add(ipv4, "mtu",
        json_object_new_int((int)runtime_state.applied.mtu));
    struct json_object *dns = json_object_new_array();
    for (int i = 0; i < 2; i++) {
      if (runtime_state.applied.dns[i][0] &&
          strcmp(runtime_state.applied.dns[i], "0.0.0.0"))
        json_object_array_add(dns,
            json_object_new_string(runtime_state.applied.dns[i]));
    }
    json_object_object_add(ipv4, "dns", dns);
  }
  json_object_object_add(ipv4, "renew_in",
      json_object_new_int((int)runtime_state.renew_in));
  json_object_object_add(ipv4, "expires_in",
      json_object_new_int((int)runtime_state.expires_in));
  json_object_object_add(ipv4, "lease_acquired_at",
      runtime_state.lease_acquired_at
        ? json_object_new_int64(runtime_state.lease_acquired_at)
        : json_object_new_null());
  json_object_object_add(ipv4, "lease_expires_at",
      runtime_state.lease_expires_at
        ? json_object_new_int64(runtime_state.lease_expires_at)
        : json_object_new_null());
  json_object_object_add(ipv4, "last_change",
      json_object_new_int64(runtime_state.last_change));
  json_object_object_add(ipv4, "last_error",
      runtime_state.last_error[0]
        ? json_object_new_string(runtime_state.last_error)
        : json_object_new_null());
  json_object_object_add(network, "ipv4", ipv4);
  return network;
}
