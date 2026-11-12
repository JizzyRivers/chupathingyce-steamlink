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

It also draws fewer pixels and sees less far, as the old consoles' ports of
big games did: the picture drawn with display.performance_lines' lines (240,
a quarter of the pixels; d3d8_gl.c's screen_mode_choose asks
console_performance_lines), scaled up at the present, and the view fogged
to opaque at display.performance_view_distance and clipped there (render.c
asks console_performance_view_distance), so that the level's far clusters
and objects are not drawn at all (not in a cutscene, framed for what is far
away). Models count as half their size on the screen when their detail is
chosen (display.performance_model_detail; models.c asks
console_performance_model_detail, as often as it draws one).

Past that, what a frame at Bloodgulch's spawn gives up: the sky, which the
fog's color fills in (render_sky.c), the transparent parts of the level's
objects (render_objects.c), scenery nothing can touch, as plants (the same)
and the HUD's frames and backgrounds (hud_draw.c), each a setting of its own
and all kept in a cutscene and at the main menu. The driver's own share is launch.sh's: the shader
compiler's load-time constants, worked out again at every draw, are off.

Holding Back and pressing down on the d-pad (or F9) switches it, with a
line on the screen (steamlink_combos.c reads them, with the other combos: the game's
headers and the system's do not meet in one file). The game's own flags
(rasterizer_debug_options, the script's rasterizer_environment_decals and
the rest) are set each frame while it is on, and put back as they were when
it goes off.
*/

#include "cseries.h"
#include "cutscene/cinematics.h"
#include "interface/terminal.h"
#include "rasterizer/rasterizer_console_vars.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"

/* (port_config.c's, as the game's own sources declare it) */
int config_boolean(const char *name);
long config_integer(const char *name);
double config_real(const char *name);
unsigned long config_changes(void);
void steamlink_performance_update(int switched);
long console_performance_lines(void);
real console_performance_view_distance(void);
real console_performance_model_detail(void);
boolean console_performance_object_transparents(void);
boolean console_performance_sky(void);
real console_performance_decoration_distance(void);
boolean console_performance_hud_frames(void);

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

static void performance_start(void)
{
	if (performance_on >= 0)
		return;
	performance_on = config_boolean("display.performance_mode") != 0;
	performance_keep();
}

/* whether what is left out for the frame rate is left out now: not in a
cutscene, framed for what is far away and close, nor at the main menu, whose
picture is its sky */
static boolean performance_cutting(void)
{
	performance_start();
	return performance_on && !cinematic_in_progress() &&
		global_scenario_get()->type != _scenario_type_main_menu;
}

/* the lines the picture is drawn with, 0 for the game's own (asked between
frames: a switch shows from the next one) */
long console_performance_lines(void)
{
	performance_start();
	return performance_on ? config_integer("display.performance_lines") : 0;
}

/* how far the camera sees, 0 for as far as the level does */
real console_performance_view_distance(void)
{
	return performance_cutting() ? (real)config_real("display.performance_view_distance") : 0.0f;
}

/* what a model's size on the screen is multiplied by when its detail is
chosen, 1 for the game's own */
real console_performance_model_detail(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static real detail = 1.0f;

	performance_start();
	if (!performance_on)
		return 1.0f;
	if (read_at != config_changes())
	{
		read_at = config_changes();
		detail = (real)config_real("display.performance_model_detail");
	}
	return detail;
}

/* a setting read once a change of the settings */
static boolean performance_setting(const char *name, unsigned long *read_at, boolean *value)
{
	if (*read_at != config_changes())
	{
		*read_at = config_changes();
		*value = config_boolean(name) != 0;
	}
	return *value;
}

/* whether the level's objects draw their transparent parts (a cutscene
keeps them, framed for what is close) */
boolean console_performance_object_transparents(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static boolean value = TRUE;

	return !performance_cutting() ||
		performance_setting("display.performance_object_transparents", &read_at, &value);
}

boolean console_performance_sky(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static boolean value = TRUE;

	return !performance_cutting() || performance_setting("display.performance_sky", &read_at, &value);
}

boolean console_performance_hud_frames(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static boolean value = TRUE;

	return !performance_cutting() || performance_setting("display.performance_hud_frames", &read_at, &value);
}

/* how far scenery nothing can touch is drawn, 0 as far as everything else */
real console_performance_decoration_distance(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static real distance = 0.0f;

	if (!performance_cutting())
		return 0.0f;
	if (read_at != config_changes())
	{
		read_at = config_changes();
		distance = (real)config_real("display.performance_decoration_distance");
	}
	return distance;
}

/* each pass of the event pump, switched when its switch was pressed */
void steamlink_performance_update(int switched)
{
	int index;

	performance_start();
	if (switched)
	{
		performance_on = !performance_on;
		if (performance_on)
			performance_keep();
		else
			performance_restore();
		terminal_printf(NULL, "did haguero vibe code this shit?");
	}
	if (!performance_on)
		return;
	for (index = 0; index < PERFORMANCE_FLAG_COUNT; index++)
		*performance_flags[index].flag = FALSE;
}
