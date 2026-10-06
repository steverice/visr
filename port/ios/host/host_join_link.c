/* Join links the app is opened with (CFBundleURLTypes' halo scheme). UIKit
gives the URL to SDL's scene delegate, at launch (its connection options) or
while the app runs (scene:openURLContexts:), and SDL posts it as a drop event;
an event watch takes it here. A valid link is written to join_link.txt in the
data folder, which internet play's thread looks for every second
(poll_invite_file, p2p.c, as upstream's Android activity writes it) and joins
as Direct Link's PASTE LINK does (p2p_invite_received). So a link is taken
whatever the game is doing: in a menu, loading or mid-campaign, internet play
reaches the invite's host in the background, and the host's game is then in
Join Game > Direct Link's list, as on the desktop. On a cold launch the file
waits until internet play starts. */
#include "host_join_link.h"
#include "host_config.h"
#include "ios_host.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static char link_path[1100], temporary_path[1100];

static bool SDLCALL join_link_event(void *userdata, SDL_Event *event)
{
	(void)userdata;
	if (event->type == SDL_EVENT_DROP_FILE)
		host_join_link_open(event->drop.data);
	return true;
}

void host_join_link_install(const char *data_root)
{
	snprintf(link_path, sizeof(link_path), "%s/join_link.txt", data_root);
	snprintf(temporary_path, sizeof(temporary_path), "%s/join_link.new", data_root);
	/* a link from a run that ended before internet play took it is not one
	the player just opened */
	if (remove(link_path) == 0)
		host_logf(HOST_LOG_INFO, "join link: removed one a previous run did not take");
	/* SDL drops a URL that arrives before its event queue is up, and the XISO
	import screen spins the run loop before SDL_Init */
	if (!SDL_InitSubSystem(SDL_INIT_EVENTS) || !SDL_AddEventWatch(join_link_event, NULL))
		host_logf(HOST_LOG_ERROR, "join link: cannot watch SDL's events (%s); links will not open games",
			SDL_GetError());
}

void host_join_link_open(const char *url)
{
	char invite[HOST_JOIN_LINK_SIZE], online[16];
	FILE *file;
	int parsed = host_join_link_parse(url, invite);
	int written;

	if (!parsed)
		return;
	if (parsed < 0)
	{
		host_logf(HOST_LOG_WARN, "join link: ignored a halo: link that is not a join link (%.80s)", url);
		return;
	}
	/* (off by default on iOS; written to the file only when it is on, so that
	the link does not wait there to be joined whenever it is turned on) */
	host_config_string("network.online", "false", online, sizeof(online));
	if (strcmp(online, "true"))
	{
		host_logf(HOST_LOG_WARN, "join link: internet play is off (network.online in config.toml); ignored %s",
			invite);
		return;
	}
	/* written whole, then renamed into place: the game takes the file by
	renaming it, and must never read half a link */
	file = fopen(temporary_path, "wb");
	written = file && fputs(invite, file) >= 0;
	if (file && fclose(file) != 0)
		written = 0;
	if (!written || rename(temporary_path, link_path) != 0)
	{
		remove(temporary_path);
		host_logf(HOST_LOG_ERROR, "join link: cannot write %s for the game", link_path);
		return;
	}
	host_logf(HOST_LOG_INFO, "join link: opened with %s; internet play joins it", invite);
}
