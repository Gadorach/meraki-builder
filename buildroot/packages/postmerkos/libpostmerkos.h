#ifndef LIBPOSTMERKOS_H
#define LIBPOSTMERKOS_H

#include <stdbool.h>
#include <stddef.h>

#define DEVICE_FILE "/run/postmerkos/boardinfo"
#define PORTS_FILE "/click/switch_port_table/dump_pports"

/* Returns the current UTC time as an ISO-8601 string. */
const char *get_time(void);

bool starts_with(const char *str, const char *prefix);
bool ends_with(const char *str, const char *suffix);
char *itoa(int num, char *buffer, int base);

/* Copy the one-based, whitespace-delimited field into OUTPUT. */
int get_field_copy(const char *line, unsigned int field,
                   char *output, size_t output_size);

/* Click file helpers. All return 0 on success or a negative errno value. */
int click_write(const char *path, const char *value);
int click_read(const char *path, char *buffer, size_t buffer_size);
int write_switch_port_table(const char *handler, const char *value);
int read_switch_port_table(const char *handler, unsigned int port,
                           char *buffer, size_t buffer_size);

#endif
