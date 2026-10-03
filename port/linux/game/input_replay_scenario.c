/*
INPUT_REPLAY_SCENARIO.C

Whether the main menu's scenario (ui.map) is loaded, for input_replay.c,
which can't include the game's headers: a recording or replay starts with
the first level, not the menu that loads before it.
*/

#include "cseries.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"

unsigned char input_replay_main_menu_loaded(void)
{
	struct scenario *scenario = global_scenario_try_and_get();

	return scenario && scenario->type == _scenario_type_main_menu;
}
