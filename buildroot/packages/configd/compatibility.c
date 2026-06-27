#include "compatibility.h"
#include "configd.h"
#include "hardware.h"
#include "release.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACK_PATH "/config/postmerkos/compatibility-ack.json"

static void set_error(char *error,size_t size,const char *message){if(error&&size)snprintf(error,size,"%s",message?message:"error");}
static const char *json_string(struct json_object *o,const char *key,const char *fallback){struct json_object*v=NULL;return o&&json_object_object_get_ex(o,key,&v)&&json_object_is_type(v,json_type_string)?json_object_get_string(v):fallback;}
static bool acknowledged(void){
  struct json_object *ack=json_object_from_file(ACK_PATH);if(!ack)return false;
  const char *model=json_string(ack,"model","");const char *version=json_string(ack,"firmware","");
  bool match=!strcmp(model,hardware.model)&&!strcmp(version,release_version());json_object_put(ack);return match;
}
struct json_object *compatibility_report_json(void){
  struct json_object *report=json_object_new_object();
  json_object_object_add(report,"firmware",json_object_new_string(release_version()));
  json_object_object_add(report,"model",json_object_new_string(hardware.model));
  json_object_object_add(report,"family",json_object_new_string(hardware.family));
  json_object_object_add(report,"compatibility",json_object_new_string(hardware_compatibility_name(hardware.compatibility)));
  json_object_object_add(report,"port_count",json_object_new_int((int)hardware.port_count));
  json_object_object_add(report,"copper_ports",json_object_new_int((int)hardware.copper_port_count));
  json_object_object_add(report,"uplink_ports",json_object_new_int((int)hardware.uplink_port_count));
  json_object_object_add(report,"poe_supported",json_object_new_boolean(hardware.poe_supported));
  json_object_object_add(report,"poe_available",json_object_new_boolean(hardware.poe_available));
  json_object_object_add(report,"poe_controllers",json_object_new_int((int)hardware.poe_controller_count));
  json_object_object_add(report,"switch_instances",json_object_new_int((int)hardware.switch_instances));
  char masked[18]="unknown";
  if(strlen(meraki_mac)==17)snprintf(masked,sizeof(masked),"%c%c:%c%c:%c%c:xx:xx:xx",meraki_mac[0],meraki_mac[1],meraki_mac[3],meraki_mac[4],meraki_mac[6],meraki_mac[7]);
  json_object_object_add(report,"mac_oui",json_object_new_string(masked));
  json_object_object_add(report,"project_repo",json_object_new_string(release_project_repo()));
  return report;
}
struct json_object *compatibility_notice_json(void){
  struct json_object *notice=json_object_new_object();
  bool incompatible=hardware.compatibility==COMPATIBILITY_INCOMPATIBLE;
  bool show=incompatible || (hardware.compatibility==COMPATIBILITY_UNTESTED&&!acknowledged());
  json_object_object_add(notice,"required",json_object_new_boolean(show));
  json_object_object_add(notice,"acknowledgeable",json_object_new_boolean(!incompatible));
  json_object_object_add(notice,"acknowledged",json_object_new_boolean(!show));
  json_object_object_add(notice,"compatibility",json_object_new_string(hardware_compatibility_name(hardware.compatibility)));
  json_object_object_add(notice,"model",json_object_new_string(hardware.model));
  json_object_object_add(notice,"firmware",json_object_new_string(release_version()));
  if(show)json_object_object_add(notice,"message",json_object_new_string(
      incompatible
        ? "This firmware is marked known-incompatible with the detected model. Do not flash or rely on switch hardware operation."
        : "This model is currently untested. Please submit a compatibility report to the project issue tracker after checking ports, PoE, LEDs and management access."));
  return notice;
}
int compatibility_acknowledge(char *error,size_t error_size){
  if(hardware.compatibility==COMPATIBILITY_INCOMPATIBLE){
    set_error(error,error_size,"known-incompatible firmware cannot be acknowledged");
    return -EPERM;
  }
  mkdir("/config",0755);mkdir("/config/postmerkos",0700);
  struct json_object *ack=json_object_new_object();json_object_object_add(ack,"model",json_object_new_string(hardware.model));json_object_object_add(ack,"firmware",json_object_new_string(release_version()));
  int rc=json_object_to_file_ext(ACK_PATH,ack,JSON_C_TO_STRING_PRETTY);json_object_put(ack);
  if(rc!=0){set_error(error,error_size,"unable to save compatibility acknowledgement");return -EIO;}chmod(ACK_PATH,0600);return 0;
}
