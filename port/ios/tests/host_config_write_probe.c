/* host_config_write_boolean: one key set, every other line kept, the section added when missing */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "host_config.h"

static char *slurp(const char *path)
{
	static char text[4096];
	FILE *file = fopen(path, "r");
	size_t n = fread(text, 1, sizeof(text) - 1, file);

	fclose(file);
	text[n] = 0;
	return text;
}

int main(void)
{
	char directory[] = "/tmp/host-config-XXXXXX";
	FILE *file;

	assert(mkdtemp(directory) && chdir(directory) == 0);   /* host_config reads config.toml in the working directory */
	file = fopen("config.toml", "w");
	fputs("# mine\n[display]\nhigh_res_hud = true\nupscaled_textures = true\n[debug]\ntexture_log = false\n", file);
	fclose(file);
	assert(host_config_write_boolean("display.upscaled_textures", 0) == 1);
	assert(strstr(slurp("config.toml"), "# mine\n[display]\nhigh_res_hud = true\nupscaled_textures = false\n[debug]\ntexture_log = false\n"));
	file = fopen("config.toml", "w");
	fputs("[debug]\ntexture_log = false\n", file);
	fclose(file);
	assert(host_config_write_boolean("display.upscaled_textures", 1) == 1);
	assert(strstr(slurp("config.toml"), "[debug]\ntexture_log = false\n\n[display]\nupscaled_textures = true\n"));
	file = fopen("config.toml", "w");
	fputs("[display]\nhigh_res_hud = false\n[debug]\n", file);
	fclose(file);
	assert(host_config_write_boolean("display.upscaled_textures", 0) == 1);
	assert(strstr(slurp("config.toml"), "[display]\nhigh_res_hud = false\nupscaled_textures = false\n[debug]\n"));
	file = fopen("config.toml", "w");   /* a last line without its newline keeps its value */
	fputs("[display]\nhigh_res_hud = true", file);
	fclose(file);
	assert(host_config_write_boolean("display.upscaled_textures", 0) == 1);
	assert(!strcmp(slurp("config.toml"), "[display]\nhigh_res_hud = true\nupscaled_textures = false\n"));
	unlink("config.toml");                /* no file yet: one with the section */
	assert(host_config_write_boolean("display.upscaled_textures", 0) == 1);
	assert(!strcmp(slurp("config.toml"), "\n[display]\nupscaled_textures = false\n"));
	assert(access("config.toml.tmp", F_OK) != 0);
	unlink("config.toml");
	rmdir(directory);
	puts("host_config_write_probe: ok");
	return 0;
}
