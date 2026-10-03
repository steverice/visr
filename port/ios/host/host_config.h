/* settings the host reads itself from config.toml (host_config.c) */
#pragma once
#include <stddef.h>

/* section.key as a number, or fallback when it isn't in the file */
double host_config_real(const char *name, double fallback);
/* section.key as a string, unquoted, or fallback */
void host_config_string(const char *name, const char *fallback, char *out, size_t size);
