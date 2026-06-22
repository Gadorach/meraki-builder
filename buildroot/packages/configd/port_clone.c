#include "port_clone.h"
#include "config_apply.h"
#include "config_file.h"
#include "configd.h"
#include "hardware.h"
#include "result.h"
#include "validation.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void set_error(char *error,size_t size,const char *message){if(error&&size)snprintf(error,size,"%s",message?message:"error");}
static struct json_object *member(struct json_object *o,const char *k){struct json_object *v=NULL;return o&&json_object_is_type(o,json_type_object)&&json_object_object_get_ex(o,k,&v)?v:NULL;}
static bool selected(struct json_object *fields,const char *name){
  if(!fields||!json_object_is_type(fields,json_type_array))return true;
  for(size_t i=0;i<json_object_array_length(fields);i++){struct json_object *v=json_object_array_get_idx(fields,i);if(json_object_is_type(v,json_type_string)&&!strcmp(json_object_get_string(v),name))return true;}
  return false;
}
static void copy_key(struct json_object *source,struct json_object *target,const char *key){struct json_object *v=member(source,key);if(v)json_object_object_add(target,key,json_object_get(v));}

int port_clone_apply(struct json_object *request,struct json_object **result,
                     char *error,size_t error_size){
  if(result)*result=NULL;
  if(!request||!json_object_is_type(request,json_type_object)){set_error(error,error_size,"clone request must be an object");return -EINVAL;}
  struct json_object *source_value=member(request,"source"),*targets=member(request,"targets"),*fields=member(request,"fields");
  if(!source_value||!json_object_is_type(source_value,json_type_int)||!targets||!json_object_is_type(targets,json_type_array)){set_error(error,error_size,"source and target ports are required");return -EINVAL;}
  unsigned int source_port=(unsigned int)json_object_get_int(source_value);
  if(!hardware_port_valid(&hardware,source_port)){set_error(error,error_size,"source port is invalid");return -EINVAL;}
  struct json_object *config=load_config_file();
  struct json_object *ports=member(config,"ports");char source_key[16];snprintf(source_key,sizeof(source_key),"%u",source_port);
  struct json_object *source=member(ports,source_key);
  if(!config||!ports||!source){if(config)json_object_put(config);set_error(error,error_size,"source port configuration is unavailable");return -ENOENT;}
  struct json_object *warnings=json_object_new_array();
  struct json_object *applied=json_object_new_array();
  for(size_t i=0;i<json_object_array_length(targets);i++){
    struct json_object *entry=json_object_array_get_idx(targets,i);
    if(!json_object_is_type(entry,json_type_int))continue;
    unsigned int port=(unsigned int)json_object_get_int(entry);
    if(!hardware_port_valid(&hardware,port)||port==source_port)continue;
    char key[16];snprintf(key,sizeof(key),"%u",port);struct json_object *target=member(ports,key);if(!target)continue;
    if(selected(fields,"administrative"))copy_key(source,target,"enabled");
    if(selected(fields,"name"))copy_key(source,target,"name");
    if(selected(fields,"phy")){copy_key(source,target,"speed");copy_key(source,target,"flow_control");copy_key(source,target,"eee");}
    if(selected(fields,"storm_control"))copy_key(source,target,"storm_control");
    if(selected(fields,"vlan"))copy_key(source,target,"vlan");
    if(selected(fields,"stp"))copy_key(source,target,"stp");
    if(selected(fields,"poe")){
      if(hardware_port_supports_poe(&hardware,port))copy_key(source,target,"poe");
      else {char message[96];snprintf(message,sizeof(message),"Port %u does not support PoE; PoE settings skipped",port);json_object_array_add(warnings,json_object_new_string(message));}
    }
    json_object_array_add(applied,json_object_new_int((int)port));
  }
  char validation_error[256]={0};
  if(validate_configuration(config,validation_error,sizeof(validation_error))!=0){json_object_put(config);json_object_put(warnings);json_object_put(applied);set_error(error,error_size,validation_error);return -EINVAL;}
  struct apply_result apply;apply_result_init(&apply);int rc=config_replace_validate_save_apply(config,&apply,error,error_size);
  if(rc==0){struct json_object *reply=json_object_new_object();json_object_object_add(reply,"source",json_object_new_int((int)source_port));json_object_object_add(reply,"targets",applied);json_object_object_add(reply,"warnings",warnings);json_object_object_add(reply,"apply_warnings",json_object_get(apply.warnings));if(result)*result=reply;else json_object_put(reply);}else{json_object_put(applied);json_object_put(warnings);}
  apply_result_cleanup(&apply);json_object_put(config);return rc;
}
