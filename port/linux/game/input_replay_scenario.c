/*
INPUT_REPLAY_SCENARIO.C

The game state input_replay.c needs and can't reach without the game's
headers: whether the main menu's scenario (ui.map) is loaded, since a
recording or replay starts with the first level, not the menu that loads
before it; and skipping a cinematic the way the accept button does
(player_control.c), for a replay that has reached the point where the
recording skipped it.
*/

#include "cseries.h"
#include "cutscene/cinematics.h"
#include "main/main.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"

unsigned char input_replay_main_menu_loaded(void)
{
	struct scenario *scenario = global_scenario_try_and_get();

	return scenario && scenario->type == _scenario_type_main_menu;
}

void input_replay_skip_cinematic(void)
{
	if (cinematic_can_be_skipped())
		main_skip_cinematic();
}
