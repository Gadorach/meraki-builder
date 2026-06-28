#include "../configd.h"
#include "../telemetry.h"
#include <json-c/json.h>
#include <libpd690xx.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Required globals for linking telemetry.c (mirroring test_network.c) */
bool dry_run = true;
const char *config_file = "/tmp/configd-telemetry-test.json";
char meraki_mac[18] = "00:11:22:33:44:55";
struct hardware_info hardware;
struct pd690xx_cfg pd690xx;

/* link stub: telemetry_apply (which calls service_action) is not exercised by these unit tests */
int service_action(const char *service, const char *action, char *error, size_t error_size) {
  (void)service; (void)action; (void)error; (void)error_size; return 0;
}

static struct json_object *parse(const char *t) {
  struct json_object *v = json_tokener_parse(t);
  assert(v);
  return v;
}

int main(void) {
  char err[256];

  /* absent telemetry section is valid */
  struct json_object *none = parse("{\"network\":{}}");
  assert(telemetry_validate(none, err, sizeof(err)) == 0);
  json_object_put(none);

  /* defaults: both disabled, port 9100, empty community */
  struct json_object *def = telemetry_default_config();
  struct json_object *snmp, *prom, *v;
  assert(json_object_object_get_ex(def, "snmp", &snmp));
  assert(json_object_object_get_ex(snmp, "enabled", &v) && !json_object_get_boolean(v));
  assert(json_object_object_get_ex(snmp, "community", &v) &&
         strcmp(json_object_get_string(v), "") == 0);
  assert(json_object_object_get_ex(def, "prometheus", &prom));
  assert(json_object_object_get_ex(prom, "port", &v) && json_object_get_int(v) == 9100);
  json_object_put(def);

  /* enabling SNMP with empty community is rejected */
  struct json_object *bad = parse(
    "{\"telemetry\":{\"snmp\":{\"enabled\":true,\"community\":\"\"}}}");
  assert(telemetry_validate(bad, err, sizeof(err)) == -EINVAL);
  json_object_put(bad);

  /* enabling SNMP with a community is accepted */
  struct json_object *ok = parse(
    "{\"telemetry\":{\"snmp\":{\"enabled\":true,\"community\":\"s3cret\"}}}");
  assert(telemetry_validate(ok, err, sizeof(err)) == 0);
  json_object_put(ok);

  /* prometheus port out of range rejected */
  struct json_object *badport = parse(
    "{\"telemetry\":{\"prometheus\":{\"enabled\":true,\"port\":70000}}}");
  assert(telemetry_validate(badport, err, sizeof(err)) == -EINVAL);
  json_object_put(badport);

  /* denylisted port 161 rejected */
  struct json_object *deny = parse(
    "{\"telemetry\":{\"prometheus\":{\"enabled\":true,\"port\":161}}}");
  assert(telemetry_validate(deny, err, sizeof(err)) == -EINVAL);
  json_object_put(deny);

  /* unknown key rejected */
  struct json_object *unknown = parse(
    "{\"telemetry\":{\"bogus\":1}}");
  assert(telemetry_validate(unknown, err, sizeof(err)) == -EINVAL);
  json_object_put(unknown);

  /* snmpd.env render */
  {
    char path[256];
    snprintf(path, sizeof(path), "%s/snmpd.env",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    setenv("CONFIGD_SNMPD_ENV", path, 1);
    struct json_object *cfg = parse(
      "{\"telemetry\":{\"snmp\":{\"enabled\":true,\"community\":\"s3cret\","
      "\"location\":\"rack 4\",\"contact\":\"net@ex\"}}}");
    assert(telemetry_write_snmpd_env(cfg, "192.0.2.10") == 0);
    json_object_put(cfg);

    FILE *f = fopen(path, "r");
    assert(f);
    char c[1024]; size_t n = fread(c, 1, sizeof(c) - 1, f); fclose(f); c[n] = '\0';
    assert(strstr(c, "SNMP_COMMUNITY='s3cret'"));
    assert(strstr(c, "SNMP_LOCATION='rack 4'"));
    assert(strstr(c, "SNMP_CONTACT='net@ex'"));
    assert(strstr(c, "SNMP_BIND='192.0.2.10'"));
    unsetenv("CONFIGD_SNMPD_ENV");
  }

  puts("telemetry validation tests passed");
  return 0;
}
