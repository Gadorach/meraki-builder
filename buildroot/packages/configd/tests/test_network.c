#include "../configd.h"
#include "../network.h"
#include "../result.h"
#include <json-c/json.h>
#include <libpd690xx.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool dry_run = true;
const char *config_file = "/tmp/configd-network-test.json";
char meraki_mac[18] = "00:11:22:33:44:55";
struct hardware_info hardware;
struct pd690xx_cfg pd690xx;

static struct json_object *parse(const char *text) {
  struct json_object *value = json_tokener_parse(text);
  assert(value);
  return value;
}

int main(void) {
  const char *dir = "/tmp/configd-network-test";
  char command[256];
  snprintf(command, sizeof(command), "rm -rf %s && mkdir -p %s", dir, dir);
  assert(system(command) == 0);
  char state[256], brain[256];
  snprintf(state, sizeof(state), "%s/dhcp_state", dir);
  snprintf(brain, sizeof(brain), "%s/brain", dir);
  setenv("CONFIGD_DHCP_STATE", state, 1);
  setenv("CONFIGD_DHCP_BRAIN", brain, 1);

  struct json_object *dhcp = parse(
      "{\"network\":{\"ipv4\":{\"mode\":\"dhcp\","
      "\"fallback_address\":\"169.254.0.10/16\",\"mtu\":1500}}}");
  struct apply_result result;
  apply_result_init(&result);
  assert(network_manager_init(dhcp, &result) == 0);
  const struct network_runtime *runtime = network_manager_runtime();
  assert(strcmp(runtime->source, "fallback") == 0);
  assert(strcmp(runtime->applied.address, "169.254.0.10") == 0);
  apply_result_cleanup(&result);

  /* Older donor graphs may expose only dhcpc_state_for_brain. A dotted
   * netmask must be normalized to the numeric prefix expected by set_host_ip. */
  FILE *file = fopen(brain, "w");
  assert(file);
  fputs("ip=192.168.4.15\n"
        "subnet=255.255.252.0\n"
        "router=192.168.4.1\n"
        "broadcast=192.168.7.255\n"
        "dns=192.168.4.1 1.1.1.1\n", file);
  fclose(file);
  apply_result_init(&result);
  assert(network_manager_poll(&result));
  runtime = network_manager_runtime();
  assert(strcmp(runtime->source, "dhcp") == 0);
  assert(strcmp(runtime->applied.address, "192.168.4.15") == 0);
  assert(runtime->applied.prefix == 22);
  assert(strcmp(runtime->applied.dns[0], "192.168.4.1") == 0);
  assert(strcmp(runtime->applied.dns[1], "1.1.1.1") == 0);
  apply_result_cleanup(&result);

  file = fopen(state, "w");
  assert(file);
  fputs("vlan added_by active state disc_ago offer_ago req_ago ack_delay renew_at exp_at ip gw bcast dns dns mtu\n", file);
  fputs("1 C|T true bound 10 9 8 0.0049 120 3600 192.168.4.15/22 192.168.4.1 192.168.7.255 192.168.4.1 0.0.0.0 1500\n", file);
  fclose(file);
  file = fopen(brain, "w");
  assert(file);
  fputs("ip=192.168.4.15\nsubnet=22\nrouter=192.168.4.1\nbroadcast=192.168.7.255\n", file);
  fclose(file);

  apply_result_init(&result);
  network_manager_poll(&result);
  runtime = network_manager_runtime();
  assert(strcmp(runtime->source, "dhcp") == 0);
  assert(strcmp(runtime->applied.address, "192.168.4.15") == 0);
  assert(runtime->applied.prefix == 22);
  assert(network_manager_next_poll_seconds() == 120);
  apply_result_cleanup(&result);

  struct json_object *statik = parse(
      "{\"network\":{\"ipv4\":{\"mode\":\"static\","
      "\"address\":\"10.20.30.40/24\",\"gateway\":\"10.20.30.1\","
      "\"mtu\":1500}}}");
  apply_result_init(&result);
  assert(network_manager_configure(statik, &result, false) == 0);
  runtime = network_manager_runtime();
  assert(strcmp(runtime->source, "static") == 0);
  assert(strcmp(runtime->applied.address, "10.20.30.40") == 0);
  assert(strcmp(runtime->applied.broadcast, "10.20.30.255") == 0);
  apply_result_cleanup(&result);

  /* Switching from static to DHCP without a lease must retain the reachable
   * static address until a valid lease arrives. */
  assert(unlink(state) == 0);
  assert(unlink(brain) == 0);
  apply_result_init(&result);
  assert(network_manager_configure(dhcp, &result, false) == 0);
  runtime = network_manager_runtime();
  assert(strcmp(runtime->source, "static") == 0);
  assert(strcmp(runtime->state, "waiting") == 0);
  assert(strcmp(runtime->applied.address, "10.20.30.40") == 0);
  apply_result_cleanup(&result);

  /* Moving from static to DHCP is make-before-break. Without a lease the
   * static address remains applied and polling continues. */
  unlink(state);
  unlink(brain);
  apply_result_init(&result);
  assert(network_manager_configure(dhcp, &result, false) == 0);
  runtime = network_manager_runtime();
  assert(strcmp(runtime->configured_mode, "dhcp") == 0);
  assert(strcmp(runtime->source, "static") == 0);
  assert(strcmp(runtime->state, "waiting") == 0);
  assert(strcmp(runtime->applied.address, "10.20.30.40") == 0);
  assert(network_manager_next_poll_seconds() == 5);
  apply_result_cleanup(&result);

  char error[128];
  struct json_object *invalid = parse(
      "{\"network\":{\"ipv4\":{\"mode\":\"static\","
      "\"address\":\"10.20.30.40/24\",\"gateway\":\"10.21.0.1\","
      "\"mtu\":1500}}}");
  assert(network_validate_config(invalid, error, sizeof(error)) != 0);

  json_object_put(invalid);
  json_object_put(statik);
  json_object_put(dhcp);

  /* /etc/resolv.conf rendering from the active DNS servers. */
  char resolv[256];
  snprintf(resolv, sizeof(resolv), "%s/resolv.conf", dir);
  setenv("CONFIGD_RESOLV_CONF", resolv, 1);
  unlink(resolv);

  struct ipv4_runtime dns_value = {0};
  snprintf(dns_value.dns[0], sizeof(dns_value.dns[0]), "192.168.4.1");
  snprintf(dns_value.dns[1], sizeof(dns_value.dns[1]), "1.1.1.1");
  assert(network_render_resolv_conf(&dns_value) == 0);
  FILE *rf = fopen(resolv, "r");
  assert(rf);
  char body[256];
  size_t got = fread(body, 1, sizeof(body) - 1, rf);
  fclose(rf);
  body[got] = '\0';
  assert(strstr(body, "nameserver 192.168.4.1\n"));
  assert(strstr(body, "nameserver 1.1.1.1\n"));

  /* A value with no DNS servers must not wipe the existing resolver. */
  struct ipv4_runtime empty_value = {0};
  assert(network_render_resolv_conf(&empty_value) == 0);
  rf = fopen(resolv, "r");
  assert(rf);
  got = fread(body, 1, sizeof(body) - 1, rf);
  fclose(rf);
  body[got] = '\0';
  assert(strstr(body, "nameserver 192.168.4.1\n"));

  puts("network tests passed");
  return 0;
}
