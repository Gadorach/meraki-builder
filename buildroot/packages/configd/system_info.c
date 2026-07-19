#define _GNU_SOURCE
#include "system_info.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value && *value ? value : fallback;
}

static void trim(char *value) {
  if (!value) return;
  char *start = value;
  while (*start && isspace((unsigned char)*start)) start++;
  if (start != value) memmove(value, start, strlen(start) + 1);
  size_t length = strlen(value);
  while (length && isspace((unsigned char)value[length - 1]))
    value[--length] = '\0';
}

static int read_first_line(const char *path, char *buffer, size_t size) {
  if (!path || !buffer || size < 2) return -EINVAL;
  FILE *file = fopen(path, "r");
  if (!file) return -errno;
  if (!fgets(buffer, (int)size, file)) {
    int rc = ferror(file) ? -errno : -ENODATA;
    fclose(file);
    return rc;
  }
  fclose(file);
  trim(buffer);
  return 0;
}

static int read_key_value(const char *path, const char *wanted,
                          char *buffer, size_t size) {
  FILE *file = fopen(path, "r");
  if (!file) return -errno;
  char line[512];
  int rc = -ENOENT;
  while (fgets(line, sizeof(line), file)) {
    char *equals = strchr(line, '=');
    if (!equals) continue;
    *equals = '\0';
    trim(line);
    if (strcmp(line, wanted)) continue;
    snprintf(buffer, size, "%s", equals + 1);
    trim(buffer);
    rc = 0;
    break;
  }
  fclose(file);
  return rc;
}

int system_info_serial(char *buffer, size_t size) {
  if (!buffer || size == 0) return -EINVAL;
  buffer[0] = '\0';
  const char *boardinfo = env_or_default("CONFIGD_BOARDINFO",
                                         "/run/postmerkos/boardinfo");
  return read_key_value(boardinfo, "SERIAL", buffer, size);
}

static void add_string_if(struct json_object *object, const char *key,
                          const char *value) {
  if (value && *value)
    json_object_object_add(object, key, json_object_new_string(value));
}

static struct json_object *identity_json(void) {
  struct json_object *identity = json_object_new_object();
  const char *boardinfo = env_or_default("CONFIGD_BOARDINFO",
                                         "/run/postmerkos/boardinfo");
  char value[256] = {0};
  if (read_key_value(boardinfo, "SERIAL", value, sizeof(value)) == 0)
    add_string_if(identity, "serial_number", value);
  if (read_key_value(boardinfo, "PRODUCT_NUMBER", value, sizeof(value)) == 0)
    add_string_if(identity, "product_number", value);
  if (read_key_value(boardinfo, "MAC", value, sizeof(value)) == 0)
    add_string_if(identity, "base_mac", value);
  if (read_key_value(boardinfo, "MODEL", value, sizeof(value)) == 0)
    add_string_if(identity, "exact_model", value);

  char hostname[256] = {0};
  if (gethostname(hostname, sizeof(hostname) - 1) == 0)
    add_string_if(identity, "hostname", hostname);
  return identity;
}

static void cpu_add_field(struct json_object *cpu, const char *key,
                          const char *value) {
  if (!value || !*value) return;
  if (!strcmp(key, "system type")) add_string_if(cpu, "system_type", value);
  else if (!strcmp(key, "machine")) add_string_if(cpu, "machine", value);
  else if (!strcmp(key, "cpu model") || !strcmp(key, "model name"))
    add_string_if(cpu, "model", value);
  else if (!strcmp(key, "hardware")) add_string_if(cpu, "hardware", value);
  else if (!strcmp(key, "revision")) add_string_if(cpu, "revision", value);
  else if (!strcmp(key, "bogomips")) {
    struct json_object *existing = NULL;
    if (!json_object_object_get_ex(cpu, "bogomips", &existing))
      json_object_object_add(cpu, "bogomips", json_object_new_double(atof(value)));
  } else if (!strcmp(key, "ases implemented"))
    add_string_if(cpu, "ases", value);
  else if (!strcmp(key, "wait instruction"))
    add_string_if(cpu, "wait_instruction", value);
  else if (!strcmp(key, "tlb_entries"))
    json_object_object_add(cpu, "tlb_entries", json_object_new_int(atoi(value)));
}

static struct json_object *processor_json(void) {
  struct json_object *cpu = json_object_new_object();
  const char *path = env_or_default("CONFIGD_CPUINFO_FILE", "/proc/cpuinfo");
  FILE *file = fopen(path, "r");
  unsigned int processors = 0;
  if (file) {
    char line[512];
    while (fgets(line, sizeof(line), file)) {
      char *colon = strchr(line, ':');
      if (!colon) continue;
      *colon = '\0';
      char *value = colon + 1;
      trim(line); trim(value);
      for (char *p = line; *p; ++p) *p = (char)tolower((unsigned char)*p);
      if (!strcmp(line, "processor")) processors++;
      cpu_add_field(cpu, line, value);
    }
    fclose(file);
  }
  if (!processors) {
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0) processors = (unsigned int)online;
  }
  if (processors)
    json_object_object_add(cpu, "logical_processors",
                           json_object_new_int((int)processors));
  return cpu;
}

static struct json_object *kernel_json(void) {
  struct json_object *kernel = json_object_new_object();
  struct utsname value;
  if (uname(&value) == 0) {
    add_string_if(kernel, "system", value.sysname);
    add_string_if(kernel, "node", value.nodename);
    add_string_if(kernel, "release", value.release);
    add_string_if(kernel, "version", value.version);
    add_string_if(kernel, "architecture", value.machine);
  }
  char command_line[1024] = {0};
  if (read_first_line(env_or_default("CONFIGD_CMDLINE_FILE", "/proc/cmdline"),
                      command_line, sizeof(command_line)) == 0)
    add_string_if(kernel, "command_line", command_line);
  return kernel;
}

static unsigned int process_count(const char *proc_root) {
  DIR *directory = opendir(proc_root);
  if (!directory) return 0;
  unsigned int count = 0;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    const char *name = entry->d_name;
    if (!*name) continue;
    bool numeric = true;
    for (const char *p = name; *p; ++p)
      if (!isdigit((unsigned char)*p)) { numeric = false; break; }
    if (numeric) count++;
  }
  closedir(directory);
  return count;
}

static struct json_object *runtime_json(void) {
  struct json_object *runtime = json_object_new_object();
  char line[256] = {0};
  double uptime = 0.0;
  if (read_first_line(env_or_default("CONFIGD_UPTIME_FILE", "/proc/uptime"),
                      line, sizeof(line)) == 0 && sscanf(line, "%lf", &uptime) == 1) {
    json_object_object_add(runtime, "uptime_seconds",
                           json_object_new_int64((int64_t)uptime));
    time_t now = time(NULL);
    if (now > (time_t)uptime)
      json_object_object_add(runtime, "boot_time_epoch",
                             json_object_new_int64((int64_t)(now - (time_t)uptime)));
  }

  double load1 = 0.0, load5 = 0.0, load15 = 0.0;
  unsigned int running = 0, total = 0;
  if (read_first_line(env_or_default("CONFIGD_LOADAVG_FILE", "/proc/loadavg"),
                      line, sizeof(line)) == 0 &&
      sscanf(line, "%lf %lf %lf %u/%u", &load1, &load5, &load15,
             &running, &total) >= 3) {
    json_object_object_add(runtime, "load_1", json_object_new_double(load1));
    json_object_object_add(runtime, "load_5", json_object_new_double(load5));
    json_object_object_add(runtime, "load_15", json_object_new_double(load15));
    if (total) {
      json_object_object_add(runtime, "running_processes",
                             json_object_new_int((int)running));
      json_object_object_add(runtime, "scheduled_processes",
                             json_object_new_int((int)total));
    }
  }
  unsigned int processes = process_count(env_or_default("CONFIGD_PROC_ROOT", "/proc"));
  if (processes)
    json_object_object_add(runtime, "process_count",
                           json_object_new_int((int)processes));
  return runtime;
}

static void memory_add(struct json_object *memory, const char *key,
                       unsigned long long kib) {
  json_object_object_add(memory, key,
                         json_object_new_int64((int64_t)(kib * 1024ULL)));
}

static struct json_object *memory_json(void) {
  struct json_object *memory = json_object_new_object();
  const char *path = env_or_default("CONFIGD_MEMINFO_FILE", "/proc/meminfo");
  FILE *file = fopen(path, "r");
  if (!file) return memory;
  char key[64], unit[16], line[256];
  unsigned long long value;
  while (fgets(line, sizeof(line), file)) {
    key[0] = unit[0] = '\0'; value = 0;
    if (sscanf(line, "%63[^:]: %llu %15s", key, &value, unit) < 2) continue;
    if (!strcmp(key, "MemTotal")) memory_add(memory, "total_bytes", value);
    else if (!strcmp(key, "MemFree")) memory_add(memory, "free_bytes", value);
    else if (!strcmp(key, "MemAvailable")) memory_add(memory, "available_bytes", value);
    else if (!strcmp(key, "Buffers")) memory_add(memory, "buffers_bytes", value);
    else if (!strcmp(key, "Cached")) memory_add(memory, "cached_bytes", value);
    else if (!strcmp(key, "SReclaimable")) memory_add(memory, "reclaimable_bytes", value);
    else if (!strcmp(key, "Slab")) memory_add(memory, "slab_bytes", value);
    else if (!strcmp(key, "SwapTotal")) memory_add(memory, "swap_total_bytes", value);
    else if (!strcmp(key, "SwapFree")) memory_add(memory, "swap_free_bytes", value);
  }
  fclose(file);
  return memory;
}

static void add_mount_metadata(struct json_object *filesystem, const char *path) {
  const char *mounts_path = env_or_default("CONFIGD_MOUNTS_FILE", "/proc/mounts");
  FILE *file = fopen(mounts_path, "r");
  if (!file) return;
  char source[256], target[256], type[64], options[512];
  while (fscanf(file, "%255s %255s %63s %511s %*d %*d\n",
                source, target, type, options) == 4) {
    if (strcmp(target, path)) continue;
    add_string_if(filesystem, "source", source);
    add_string_if(filesystem, "type", type);
    add_string_if(filesystem, "options", options);
    break;
  }
  fclose(file);
}

static struct json_object *filesystem_json(const char *path) {
  struct json_object *filesystem = json_object_new_object();
  add_string_if(filesystem, "path", path);
  struct statvfs stats;
  if (statvfs(path, &stats) != 0) {
    json_object_object_add(filesystem, "available", json_object_new_boolean(false));
    return filesystem;
  }
  uint64_t unit = stats.f_frsize ? stats.f_frsize : stats.f_bsize;
  uint64_t total = (uint64_t)stats.f_blocks * unit;
  uint64_t free = (uint64_t)stats.f_bfree * unit;
  uint64_t available = (uint64_t)stats.f_bavail * unit;
  uint64_t used = total >= free ? total - free : 0;
  json_object_object_add(filesystem, "available", json_object_new_boolean(true));
  json_object_object_add(filesystem, "total_bytes", json_object_new_int64((int64_t)total));
  json_object_object_add(filesystem, "used_bytes", json_object_new_int64((int64_t)used));
  json_object_object_add(filesystem, "free_bytes", json_object_new_int64((int64_t)free));
  json_object_object_add(filesystem, "available_bytes", json_object_new_int64((int64_t)available));
  json_object_object_add(filesystem, "used_percent",
      json_object_new_double(total ? (double)used * 100.0 / (double)total : 0.0));
#ifdef ST_RDONLY
  json_object_object_add(filesystem, "read_only",
      json_object_new_boolean((stats.f_flag & ST_RDONLY) != 0));
#endif
  add_mount_metadata(filesystem, path);
  return filesystem;
}

static struct json_object *storage_json(void) {
  struct json_object *storage = json_object_new_object();
  json_object_object_add(storage, "root", filesystem_json(
      env_or_default("CONFIGD_ROOT_PATH", "/")));
  json_object_object_add(storage, "overlay", filesystem_json(
      env_or_default("CONFIGD_OVERLAY_PATH", "/overlay")));
  json_object_object_add(storage, "configuration", filesystem_json(
      env_or_default("CONFIGD_CONFIG_PATH", "/config")));
  json_object_object_add(storage, "temporary", filesystem_json(
      env_or_default("CONFIGD_TMP_PATH", "/tmp")));
  return storage;
}

static struct json_object *flash_json(void) {
  struct json_object *flash = json_object_new_object();
  struct json_object *partitions = json_object_new_array();
  json_object_object_add(flash, "partitions", partitions);
  const char *path = env_or_default("CONFIGD_MTD_FILE", "/proc/mtd");
  FILE *file = fopen(path, "r");
  if (!file) return flash;
  char line[512];
  uint64_t total = 0;
  while (fgets(line, sizeof(line), file)) {
    unsigned int index = 0;
    unsigned long long size = 0, erase = 0;
    char name[256] = {0};
    if (sscanf(line, "mtd%u: %llx %llx \"%255[^\"]\"",
               &index, &size, &erase, name) != 4) continue;
    struct json_object *partition = json_object_new_object();
    char device[32]; snprintf(device, sizeof(device), "mtd%u", index);
    json_object_object_add(partition, "device", json_object_new_string(device));
    json_object_object_add(partition, "name", json_object_new_string(name));
    json_object_object_add(partition, "size_bytes", json_object_new_int64((int64_t)size));
    json_object_object_add(partition, "erase_size_bytes", json_object_new_int64((int64_t)erase));
    json_object_array_add(partitions, partition);
    total += size;
  }
  fclose(file);
  json_object_object_add(flash, "total_bytes", json_object_new_int64((int64_t)total));
  return flash;
}

struct json_object *system_info_json(void) {
  struct json_object *system = json_object_new_object();
  json_object_object_add(system, "identity", identity_json());
  json_object_object_add(system, "processor", processor_json());
  json_object_object_add(system, "kernel", kernel_json());
  json_object_object_add(system, "runtime", runtime_json());
  json_object_object_add(system, "memory", memory_json());
  json_object_object_add(system, "storage", storage_json());
  json_object_object_add(system, "flash", flash_json());
  return system;
}
