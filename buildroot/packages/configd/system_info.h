#ifndef CONFIGD_SYSTEM_INFO_H
#define CONFIGD_SYSTEM_INFO_H

#include <json-c/json.h>
#include <stddef.h>

struct json_object *system_info_json(void);

/* Read the device SERIAL from boardinfo into buffer. Returns 0 on success. */
int system_info_serial(char *buffer, size_t size);

#endif
