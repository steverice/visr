/* The Settings app's texture pane (Settings.bundle/Root.plist): the "Upscale textures" switch, the size of the
   upscaled textures and a switch that deletes them. Only this file pair and Root.plist know about the pane, so an
   in-game menu can replace them. */
#ifndef HOST_TEXTURE_SETTINGS_H
#define HOST_TEXTURE_SETTINGS_H

/* writes the "Upscale textures" switch into config.toml when the player changed it, acts on the delete switch (and
   turns it back off), then publishes the cache's size */
void host_texture_settings_apply(const char *data_root);
/* applies now, and again every time the app returns to the foreground; also refreshes the size (not the delete)
   when the app enters the background, which is when the player opens Settings */
void host_texture_settings_observe(const char *data_root);

#endif
