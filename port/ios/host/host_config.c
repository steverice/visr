/* The few settings the host itself needs (theater mode's, host_theater.m),
read from config.toml in the data folder (the working directory). The game
owns the file: it writes every setting with its default and its description
(port/linux/src/port_config.c), so the host only reads, and only the simple
"key = value" lines of one section. */
#include "host_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the raw value text of section.key (quotes kept), or 0 if it isn't there */
static int config_value(const char *name, char *value, size_t size)
{
	const char *dot = strchr(name, '.');
	char line[512], section[64] = "";
	FILE *file;
	int found = 0;

	if (!dot)
		return 0;
	file = fopen("config.toml", "r");
	if (!file)
		return 0;
	while (!found && fgets(line, sizeof(line), file))
	{
		char *start = line, *end;

		while (isspace((unsigned char)*start))
			start++;
		if (*start == '#' || !*start)
			continue;
		if (*start == '[')
		{
			end = strchr(start, ']');
			if (end)
				snprintf(section, sizeof(section), "%.*s", (int)(end - start - 1), start + 1);
			continue;
		}
		if (strlen(section) != (size_t)(dot - name) || strncmp(section, name, (size_t)(dot - name)))
			continue;
		end = strchr(start, '=');
		if (!end)
			continue;
		{
			char *key_end = end;

			while (key_end > start && isspace((unsigned char)key_end[-1]))
				key_end--;
			if ((size_t)(key_end - start) != strlen(dot + 1) || strncmp(start, dot + 1, (size_t)(key_end - start)))
				continue;
		}
		start = end + 1;
		while (isspace((unsigned char)*start))
			start++;
		/* up to a comment: a # outside a string (a hand-edited
		"online = true # on" is true, as the game reads it) */
		{
			char quote = 0;

			for (end = start; *end && (quote || *end != '#'); end++)
			{
				if (quote == '"' && *end == '\\' && end[1])
					end++;
				else if (*end == quote)
					quote = 0;
				else if (!quote && (*end == '"' || *end == '\''))
					quote = *end;
			}
		}
		while (end > start && isspace((unsigned char)end[-1]))
			end--;
		snprintf(value, size, "%.*s", (int)(end - start), start);
		found = 1;
	}
	fclose(file);
	return found;
}

double host_config_real(const char *name, double fallback)
{
	char value[64];

	return config_value(name, value, sizeof(value)) ? atof(value) : fallback;
}

void host_config_string(const char *name, const char *fallback, char *out, size_t size)
{
	char value[256];

	if (!config_value(name, value, sizeof(value)))
	{
		snprintf(out, size, "%s", fallback);
		return;
	}
	if (value[0] == '"')
	{
		char *close = strchr(value + 1, '"');

		snprintf(out, size, "%.*s", close ? (int)(close - value - 1) : (int)strlen(value + 1), value + 1);
	}
	else
		snprintf(out, size, "%s", value);
}
