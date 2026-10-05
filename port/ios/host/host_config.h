/* settings the host reads itself from config.toml (host_config.c) */
#pragma once
#include <stddef.h>

/* section.key as a number, or fallback when it isn't in the file */
double host_config_real(const char *name, double fallback);
/* section.key as a string, unquoted, or fallback */
void host_config_string(const char *name, const char *fallback, char *out, size_t size);
/* section.key set to true or false in config.toml, every other line kept (the section added when missing);
   1 on success */
int host_config_write_boolean(const char *name, int value);
