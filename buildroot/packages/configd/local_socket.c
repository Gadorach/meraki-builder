#define _GNU_SOURCE
#include "local_socket.h"
#include "auth.h"
#include "config_file.h"
#include "compatibility.h"
#include "config_apply.h"
#include "configd.h"
#include "result.h"
#include "network.h"
#include "port_clone.h"
#include "validation.h"
#include "console_cli.h"
#include "roles.h"
#include "status.h"
#include "service_ops.h"
#include "telemetry.h"
#include "time_ops.h"
#include "json_util.h"
#include "socket_io.h"

#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/reboot.h>

#define LOCAL_REQUEST_MAX 65536

static int send_json(int fd, const char *type, struct json_object *data) {
  struct json_object *reply = json_object_new_object();
  json_object_object_add(reply, "type", json_object_new_string(type));
  json_object_object_add(reply, "data", data ? json_object_get(data) : json_object_new_null());
  const char *text = json_object_to_json_string_ext(reply, JSON_C_TO_STRING_PLAIN);
  int rc = socket_write_line(fd, text, strlen(text));
  json_object_put(reply);
  return rc;
}

static int send_error(int fd, int status, const char *message) {
  struct json_object *data = json_object_new_object();
  json_object_object_add(data, "status", json_object_new_int(status));
  json_object_object_add(data, "message", json_object_new_string(message ? message : "error"));
  int rc = send_json(fd, "error", data);
  json_object_put(data);
  return rc;
}

static const char *request_type(struct json_object *request) {
  struct json_object *type = NULL;
  return request && json_object_object_get_ex(request, "type", &type) &&
      json_object_is_type(type, json_type_string) ? json_object_get_string(type) : NULL;
}

static struct json_object *request_data(struct json_object *request) {
  struct json_object *data = NULL;
  return request && json_object_object_get_ex(request, "data", &data) &&
      json_object_is_type(data, json_type_object) ? data : NULL;
}

static const char *object_string(struct json_object *object, const char *key) {
  struct json_object *value = NULL;
  return object && json_object_object_get_ex(object, key, &value) &&
      json_object_is_type(value, json_type_string) ? json_object_get_string(value) : NULL;
}

static const char *path_capability(const char *path) {
  if (!path) return "network.write";
  if (!strncmp(path, "ports.", 6) || !strncmp(path, "stp.", 4) ||
      !strncmp(path, "lacp.", 5) || !strncmp(path, "multicast.", 10))
    return "switching.write";
  return "network.write";
}

static bool delta_operator_safe(struct json_object *delta) {
  if (!delta || !json_object_is_type(delta, json_type_object)) return false;
  json_object_object_foreach(delta, key, value) {
    (void)value;
    if (strcmp(key, "ports") && strcmp(key, "stp") && strcmp(key, "lacp") &&
        strcmp(key, "multicast")) return false;
  }
  return true;
}

static int save_delta_reply(int fd, struct json_object *delta) {
  struct apply_result result;
  apply_result_init(&result);
  struct json_object *saved = NULL;
  char error[256] = {0};
  int rc = config_merge_validate_save_apply(delta, &saved, &result, false,
                                             error, sizeof(error));
  if (rc != 0) send_error(fd, 400, error[0] ? error : "configuration rejected");
  else {
    if (saved) {
      const struct network_runtime *net_rt = network_manager_runtime();
      const char *mgmt_addr = (net_rt && net_rt->applied.address[0]) ? net_rt->applied.address : NULL;
      telemetry_apply(saved, mgmt_addr);
    }
    struct json_object *ack = apply_result_json(&result, "Configuration accepted");
    send_json(fd, "ack", ack);
    json_object_put(ack);
  }
  if (saved) json_object_put(saved);
  apply_result_cleanup(&result);
  return rc;
}

static void handle_client(int fd) {
  struct ucred credentials;
  socklen_t credentials_length = sizeof(credentials);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_length) != 0) {
    send_error(fd, 500, "unable to identify local client"); return;
  }
  char username[128] = {0};
  if (account_username_for_uid(credentials.uid, username, sizeof(username)) != 0) {
    send_error(fd, 403, "local account could not be resolved");
    return;
  }
  enum postmerkos_role role = credentials.uid == 0
      ? POSTMERKOS_ROLE_ADMIN : role_for_username(username);
  if (role == POSTMERKOS_ROLE_NONE) {
    send_error(fd, 403, "account has no management role");
    return;
  }

  char buffer[LOCAL_REQUEST_MAX + 2];
  ssize_t got = socket_read_line(fd, buffer, sizeof(buffer), 5000);
  if (got == 0) return;
  if (got < 0) {
    if (got == -EMSGSIZE) send_error(fd, 413, "request exceeds local protocol limit");
    else if (got != -ECONNRESET && got != -EPIPE)
      send_error(fd, 400, got == -ETIMEDOUT ? "request timed out" : "request read failed");
    return;
  }
  struct json_object *request = json_tokener_parse(buffer);
  const char *type = request_type(request);
  if (!request || !type) { send_error(fd, 400, "malformed request"); goto done; }

  if (!strcmp(type, "ping")) {
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, "message", json_object_new_string("pong"));
    send_json(fd, "ack", data); json_object_put(data);
  } else if (!strcmp(type, "session")) {
    struct json_object *identity = role_identity_json_for_uid(credentials.uid);
    send_json(fd, "session", identity); json_object_put(identity);
  } else if (!strcmp(type, "status.get") && role_has_capability(role, "status.read")) {
    struct json_object *status = get_status();
    send_json(fd, "status", status); json_object_put(status);
  } else if (!strcmp(type, "config.get") && role_has_capability(role, "config.read")) {
    struct json_object *config = load_config_file();
    if (!config) send_error(fd, 404, "configuration unavailable");
    else { send_json(fd, "config", config); json_object_put(config); }
  } else if (!strcmp(type, "config.path.get") && role_has_capability(role, "config.read")) {
    struct json_object *data = request_data(request), *path = NULL;
    if (!data || !json_object_object_get_ex(data, "path", &path) ||
        !json_object_is_type(path, json_type_string)) send_error(fd, 400, "path is required");
    else {
      struct json_object *config = load_config_file();
      struct json_object *value = config ? console_path_get(config, json_object_get_string(path)) : NULL;
      if (!value) send_error(fd, 404, "configuration path not found");
      else send_json(fd, "value", value);
      if (config) json_object_put(config);
    }
  } else if (!strcmp(type, "config.path.set")) {
    struct json_object *data = request_data(request);
    const char *path = object_string(data, "path");
    const char *value = object_string(data, "value");
    struct json_object *literal = NULL;
    bool string_value = data && json_object_object_get_ex(data, "string", &literal) &&
                        json_object_get_boolean(literal);
    const char *capability = path_capability(path);
    if (!role_has_capability(role, capability)) send_error(fd, 403, "operation is not permitted for this role");
    else if (!path || value == NULL) send_error(fd, 400, "path and value are required");
    else {
      char error[256] = {0};
      struct json_object *delta = string_value
          ? console_delta_from_string_path(path, value, error, sizeof(error))
          : console_delta_from_path(path, value, error, sizeof(error));
      if (!delta) send_error(fd, 400, error[0] ? error : "invalid configuration value");
      else { save_delta_reply(fd, delta); json_object_put(delta); }
    }
  } else if (!strcmp(type, "config.delta")) {
    struct json_object *data = request_data(request), *delta = NULL;
    if (!data || !json_object_object_get_ex(data, "delta", &delta) ||
        !json_object_is_type(delta, json_type_object)) send_error(fd, 400, "configuration delta is required");
    else {
      const char *capability = delta_operator_safe(delta) ? "switching.write" : "network.write";
      if (!role_has_capability(role, capability)) send_error(fd, 403, "operation is not permitted for this role");
      else save_delta_reply(fd, delta);
    }
  } else if (!strcmp(type, "config.validate")) {
    struct json_object *data = request_data(request), *candidate = NULL;
    if (!role_has_capability(role, "config.restore")) send_error(fd, 403, "configuration validation requires administrator access");
    else if (!data || !json_object_object_get_ex(data, "config", &candidate) ||
             !json_object_is_type(candidate, json_type_object)) send_error(fd, 400, "complete configuration is required");
    else {
      char error[256] = {0};
      if (validate_configuration(candidate, error, sizeof(error)) != 0)
        send_error(fd, 400, error[0] ? error : "configuration is invalid");
      else {
        struct json_object *ack = json_object_new_object();
        json_object_object_add(ack, "message", json_object_new_string("Configuration is valid"));
        send_json(fd, "ack", ack); json_object_put(ack);
      }
    }
  } else if (!strcmp(type, "config.replace")) {
    struct json_object *data = request_data(request), *candidate = NULL;
    if (!role_has_capability(role, "config.restore")) send_error(fd, 403, "configuration restore requires administrator access");
    else if (!data || !json_object_object_get_ex(data, "config", &candidate) ||
             !json_object_is_type(candidate, json_type_object)) send_error(fd, 400, "complete configuration is required");
    else {
      char error[256] = {0};
      struct apply_result result;
      apply_result_init(&result);
      int rc = config_replace_validate_save_apply(candidate, &result,
                                                  error, sizeof(error));
      if (rc != 0) send_error(fd, 400, error[0] ? error : "configuration restore failed");
      else {
        struct json_object *ack = apply_result_json(&result, "Configuration restored");
        send_json(fd, "ack", ack); json_object_put(ack);
      }
      apply_result_cleanup(&result);
    }
  } else if (!strcmp(type, "compatibility.notice") && role_has_capability(role, "status.read")) {
    struct json_object *notice=compatibility_notice_json();send_json(fd,"compatibility_notice",notice);json_object_put(notice);
  } else if (!strcmp(type, "compatibility.report") && role_has_capability(role, "status.read")) {
    struct json_object *report=compatibility_report_json();send_json(fd,"compatibility_report",report);json_object_put(report);
  } else if (!strcmp(type, "compatibility.ack") && role_has_capability(role, "status.read")) {
    char error[256]={0};if(compatibility_acknowledge(error,sizeof(error))!=0)send_error(fd,400,error);else{struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Compatibility notice dismissed for this firmware"));send_json(fd,"ack",ack);json_object_put(ack);}
  } else if (!strcmp(type, "users.get") && role_has_capability(role, "users.manage")) {
    struct json_object *users=auth_list_users();send_json(fd,"users",users);json_object_put(users);
  } else if (!strcmp(type, "users.create")) {
    struct json_object *data=request_data(request);const char *target=object_string(data,"username"),*password=object_string(data,"password"),*new_role=object_string(data,"role");
    if(!role_has_capability(role,"users.manage"))send_error(fd,403,"account management requires administrator access");else{char error[256]={0};if(auth_create_user(target,password,new_role,error,sizeof(error))!=0)send_error(fd,400,error);else{struct json_object *users=auth_list_users();send_json(fd,"users",users);json_object_put(users);}}
  } else if (!strcmp(type, "users.delete")) {
    struct json_object *data=request_data(request);const char *target=object_string(data,"username");
    if(!role_has_capability(role,"users.manage"))send_error(fd,403,"account management requires administrator access");else{char error[256]={0};if(auth_delete_user(target,error,sizeof(error))!=0)send_error(fd,400,error);else{struct json_object *users=auth_list_users();send_json(fd,"users",users);json_object_put(users);}}
  } else if (!strcmp(type, "users.role")) {
    struct json_object *data=request_data(request);const char *target=object_string(data,"username"),*new_role=object_string(data,"role");
    if(!role_has_capability(role,"users.manage"))send_error(fd,403,"account management requires administrator access");else{char error[256]={0};if(auth_set_role(target,new_role,error,sizeof(error))!=0)send_error(fd,400,error);else{struct json_object *users=auth_list_users();send_json(fd,"users",users);json_object_put(users);}}
  } else if (!strcmp(type, "services.get") && role_has_capability(role, "status.read")) {
    struct json_object *status = service_status_json();
    send_json(fd, "services", status); json_object_put(status);
  } else if (!strcmp(type, "services.path.get") && role_has_capability(role, "status.read")) {
    struct json_object *data=request_data(request);const char *path=object_string(data,"path");struct json_object *policy=service_policy_load();struct json_object *value=policy?console_path_get(policy,path):NULL;
    if(!value)send_error(fd,404,"service policy path not found");else send_json(fd,"value",value);if(policy)json_object_put(policy);
  } else if (!strcmp(type, "services.path.set")) {
    struct json_object *data=request_data(request);const char *path=object_string(data,"path"),*value=object_string(data,"value");
    if(!role_has_capability(role,"services.manage"))send_error(fd,403,"service configuration requires administrator access");
    else if(!path||value==NULL)send_error(fd,400,"path and value are required");
    else {char error[256]={0};struct json_object *policy=service_policy_load();struct json_object *delta=console_delta_from_path(path,value,error,sizeof(error));if(!policy||!delta)send_error(fd,400,error[0]?error:"invalid service value");else{json_deep_merge(policy,delta);if(service_policy_save(policy,error,sizeof(error))!=0)send_error(fd,400,error);else{struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Service policy updated"));send_json(fd,"ack",ack);json_object_put(ack);}}if(delta)json_object_put(delta);if(policy)json_object_put(policy);}
  } else if (!strcmp(type, "services.set")) {
    struct json_object *data = request_data(request);
    if (!role_has_capability(role, "services.manage")) send_error(fd, 403, "service configuration requires administrator access");
    else {
      char error[256] = {0};
      if (service_policy_save(data, error, sizeof(error)) != 0) send_error(fd, 400, error[0] ? error : "service policy rejected");
      else { struct json_object *ack=json_object_new_object(); json_object_object_add(ack,"message",json_object_new_string("Service policy saved")); send_json(fd,"ack",ack); json_object_put(ack); }
    }
  } else if (!strcmp(type, "services.action")) {
    struct json_object *data=request_data(request);
    const char *service=object_string(data,"service"), *action=object_string(data,"action");
    if (!role_has_capability(role, "services.manage")) send_error(fd,403,"service control requires administrator access");
    else { char error[256]={0}; if(service_action(service,action,error,sizeof(error))!=0) send_error(fd,400,error[0]?error:"service action failed"); else {struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Service action completed"));send_json(fd,"ack",ack);json_object_put(ack);} }
  } else if (!strcmp(type, "time.get") && role_has_capability(role, "status.read")) {
    struct json_object *status=time_status_json(); send_json(fd,"time",status); json_object_put(status);
  } else if (!strcmp(type, "time.path.get") && role_has_capability(role, "status.read")) {
    struct json_object *data=request_data(request);const char *path=object_string(data,"path");struct json_object *policy=time_policy_load();struct json_object *value=policy?console_path_get(policy,path):NULL;
    if(!value)send_error(fd,404,"time policy path not found");else send_json(fd,"value",value);if(policy)json_object_put(policy);
  } else if (!strcmp(type, "time.path.set")) {
    struct json_object *data=request_data(request);const char *path=object_string(data,"path"),*value=object_string(data,"value");
    if(!role_has_capability(role,"services.manage"))send_error(fd,403,"time configuration requires administrator access");
    else if(!path||value==NULL)send_error(fd,400,"path and value are required");
    else {char error[256]={0};struct json_object *policy=time_policy_load();struct json_object *delta=console_delta_from_path(path,value,error,sizeof(error));if(!policy||!delta)send_error(fd,400,error[0]?error:"invalid time value");else{json_deep_merge(policy,delta);if(time_policy_save(policy,error,sizeof(error))!=0)send_error(fd,400,error);else{struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Time policy updated"));send_json(fd,"ack",ack);json_object_put(ack);}}if(delta)json_object_put(delta);if(policy)json_object_put(policy);}
  } else if (!strcmp(type, "time.set")) {
    struct json_object *data=request_data(request);
    if(!role_has_capability(role,"services.manage")) send_error(fd,403,"time configuration requires administrator access");
    else {char error[256]={0};if(time_policy_save(data,error,sizeof(error))!=0)send_error(fd,400,error[0]?error:"time policy rejected");else{struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Time policy saved"));send_json(fd,"ack",ack);json_object_put(ack);}}
  } else if (!strcmp(type, "time.set_clock")) {
    struct json_object *data=request_data(request), *epoch=NULL;
    if(!role_has_capability(role,"services.manage")) send_error(fd,403,"setting the clock requires administrator access");
    else if(!data || !json_object_object_get_ex(data,"epoch",&epoch) || !json_object_is_type(epoch,json_type_int)) send_error(fd,400,"epoch is required");
    else {char error[256]={0};if(time_set_epoch((time_t)json_object_get_int64(epoch),error,sizeof(error))!=0)send_error(fd,400,error);else{struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("System clock updated"));send_json(fd,"ack",ack);json_object_put(ack);}}
  } else if (!strcmp(type, "time.sync")) {
    if(!role_has_capability(role,"services.manage")) send_error(fd,403,"time synchronization requires administrator access");
    else {char error[256]={0};if(time_force_sync(error,sizeof(error))!=0)send_error(fd,400,error);else{struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Time synchronization requested"));send_json(fd,"ack",ack);json_object_put(ack);}}
  } else if (!strcmp(type, "services.apply")) {
    if(!role_has_capability(role,"services.manage"))send_error(fd,403,"service configuration requires administrator access");
    else{char error[256]={0};if(service_policy_apply(error,sizeof(error))!=0)send_error(fd,400,error[0]?error:"service policy apply failed");else{struct json_object *ack=json_object_new_object();json_object_object_add(ack,"message",json_object_new_string("Service policy applied"));send_json(fd,"ack",ack);json_object_put(ack);}}
  } else if (!strcmp(type, "ports.clone")) {
    struct json_object *data=request_data(request),*result=NULL;
    if(!role_has_capability(role,"switching.write"))send_error(fd,403,"port cloning requires operator access");else{char error[256]={0};if(port_clone_apply(data,&result,error,sizeof(error))!=0)send_error(fd,400,error);else{send_json(fd,"ports_cloned",result);json_object_put(result);}}
  } else if (!strcmp(type, "snapshot.get") && role_has_capability(role, "status.read")) {
    struct json_object *snapshot = json_object_new_object();
    struct json_object *status = get_status();
    struct json_object *config = load_config_file();
    json_object_object_add(snapshot, "status", status);
    json_object_object_add(snapshot, "config", config ? config : json_object_new_null());
    send_json(fd, "snapshot", snapshot);
    json_object_put(snapshot);
  } else if (!strcmp(type, "system.reboot") && role_has_capability(role, "system.reboot")) {
    struct json_object *ack = json_object_new_object();
    json_object_object_add(ack, "message", json_object_new_string("Reboot scheduled"));
    send_json(fd, "ack", ack); json_object_put(ack);
    if (!dry_run && !getenv("CONFIGD_DISABLE_REBOOT")) {
      pid_t child = fork();
      if (child == 0) { sleep(1); sync(); reboot(RB_AUTOBOOT); _exit(1); }
    }
  } else send_error(fd, 403, "operation is unavailable or not permitted");

done:
  if (request) json_object_put(request);
}

int local_socket_init(const char *path) {
  if (!path || !*path) return -EINVAL;
  char directory[256];
  snprintf(directory, sizeof(directory), "%s", path);
  char *slash = strrchr(directory, '/');
  if (slash) { *slash = '\0'; if (*directory) mkdir(directory, 0755); }
  unlink(path);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -errno;
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", path) >= (int)sizeof(address.sun_path)) {
    close(fd); return -ENAMETOOLONG;
  }
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 8) != 0) {
    int rc = -errno; close(fd); unlink(path); return rc;
  }
  chmod(path, 0666);
  return fd;
}

int local_socket_service_once(int listen_fd, int timeout_ms) {
  struct pollfd pollfd = { .fd = listen_fd, .events = POLLIN };
  int ready = poll(&pollfd, 1, timeout_ms);
  if (ready < 0) return errno == EINTR ? 0 : -errno;
  if (ready == 0 || !(pollfd.revents & POLLIN)) return 0;
  int client = accept(listen_fd, NULL, NULL);
  if (client < 0) return errno == EINTR ? 0 : -errno;
  handle_client(client);
  close(client);
  return 1;
}

void local_socket_shutdown(int listen_fd, const char *path) {
  if (listen_fd >= 0) close(listen_fd);
  if (path) unlink(path);
}
