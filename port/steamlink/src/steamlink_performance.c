/*
STEAMLINK_PERFORMANCE.C

Performance mode (HALO_CONSOLE_PERFORMANCE_MODE, display.performance_mode):
the passes the Steam Link's frame can least afford left out while it is on.
Its GPU's driver costs about half a millisecond a draw whatever the draw,
so what pays is a pass's draws: on a10's bridge, frozen on one picture
(HALO_PERF_AB, gles2_layer.c), the level's decals, dynamic lights,
reflections and specular highlights together took a frame from 261 to 147
ms, and the lens flares another 33. The level's fog, shadows, detail
objects and the models' level of detail cost nothing there and are kept.

Holding Back and pressing down on the d-pad (or F9) switches it, with a
line on the screen (steamlink_performance_switch.c reads them: the game's
headers and the system's do not meet in one file). The game's own flags
(rasterizer_debug_options, the script's rasterizer_environment_decals and
the rest) are set each frame while it is on, and put back as they were when
it goes off.
*/

#include "cseries.h"
#include "interface/terminal.h"
#include "rasterizer/rasterizer_console_vars.h"

/* (port_config.c's, as the game's own sources declare it) */
int config_boolean(const char *name);
void steamlink_performance_update(int switched);

static struct
{
	boolean *flag;
	boolean kept;
} performance_flags[] =
{
	{ &rasterizer_debug_options.draw_environment_decals },
	{ &rasterizer_debug_options.draw_environment_diffuse_lights },
	{ &rasterizer_debug_options.draw_environment_reflections },
	{ &rasterizer_debug_options.draw_environment_specular_lights },
	{ &rasterizer_debug_options.draw_lens_flares },
};

#define PERFORMANCE_FLAG_COUNT ((int)(sizeof(performance_flags) / sizeof(performance_flags[0])))

static int performance_on = -1;

static void performance_keep(void)
{
	int index;

	for (index = 0; index < PERFORMANCE_FLAG_COUNT; index++)
		performance_flags[index].kept = *performance_flags[index].flag;
}

static void performance_restore(void)
{
	int index;

	for (index = 0; index < PERFORMANCE_FLAG_COUNT; index++)
		*performance_flags[index].flag = performance_flags[index].kept;
}

/* each pass of the event pump, switched when its switch was pressed */
void steamlink_performance_update(int switched)
{
	int index;

	if (performance_on < 0)
	{
		performance_on = config_boolean("display.performance_mode") != 0;
		performance_keep();
	}
	if (switched)
	{
		performance_on = !performance_on;
		if (performance_on)
			performance_keep();
		else
			performance_restore();
		terminal_printf(NULL, "performance mode %s", performance_on ? "on" : "off");
	}
	if (!performance_on)
		return;
	for (index = 0; index < PERFORMANCE_FLAG_COUNT; index++)
		*performance_flags[index].flag = FALSE;
}
