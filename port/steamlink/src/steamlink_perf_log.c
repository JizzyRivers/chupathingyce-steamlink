/*
STEAMLINK_PERF_LOG.C

The game's frame timing hooks (halo_perf_log.h) on the Steam Link, for where
a frame goes: with HALO_PERF_LOG set, every 30 frames a line with the time
of each part of the main loop and the render's parts, and the draws each of
the render's parts made (the GLES 2 layer's count, gles2_layer.c).
*/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_perf_log.h"
#include "platform.h"

#define PERF_FRAMES 30

int halo_perf_log_on;
unsigned long halo_perf_counters[NUMBER_OF_HALO_PERF_COUNTERS];

unsigned long console_gles2_draw_count(void);

static const char *const phase_names[NUMBER_OF_HALO_PERF_PHASES] =
{
	"tick", "render", "world", "objects", "transparent", "effects", "hud", "sound", "network", "present", "vsync",
	"begin", "setup", "flares", "screen effect", "end", "render sound", "occlusion wait", "texture wait",
};

static Uint64 started[NUMBER_OF_HALO_PERF_PHASES];
static Uint64 spent[NUMBER_OF_HALO_PERF_PHASES];
static unsigned long part_draws[NUMBER_OF_HALO_PERF_PHASES];
static int part = -1;
static unsigned long part_draws_start;
static Uint64 frames_start;
static int frames;

__attribute__((constructor)) static void perf_log_setting(void)
{
	halo_perf_log_on = getenv("HALO_PERF_LOG") != NULL;
}

void halo_perf_begin(int phase)
{
	started[phase] = SDL_GetTicksNS();
}

void halo_perf_end(int phase)
{
	if (started[phase])
		spent[phase] += SDL_GetTicksNS() - started[phase];
	started[phase] = 0;
}

void halo_perf_render_part(int phase)
{
	unsigned long draws = console_gles2_draw_count();

	if (part >= 0)
	{
		halo_perf_end(part);
		part_draws[part] += draws - part_draws_start;
	}
	part = phase;
	part_draws_start = draws;
	if (part >= 0)
		halo_perf_begin(part);
}

void halo_perf_frame(void)
{
	char line[1024];
	size_t length = 0;
	Uint64 now = SDL_GetTicksNS();
	int phase;

	if (!frames_start)
		frames_start = now;
	if (++frames < PERF_FRAMES)
		return;
	length += (size_t)snprintf(line + length, sizeof(line) - length, "perf: %.1f ms a frame;",
		(double)(now - frames_start) / 1e6 / frames);
	for (phase = 0; phase < NUMBER_OF_HALO_PERF_PHASES && length < sizeof(line); phase++)
	{
		if (!spent[phase] && !part_draws[phase])
			continue;
		length += (size_t)snprintf(line + length, sizeof(line) - length, " %s %.1f", phase_names[phase],
			(double)spent[phase] / 1e6 / frames);
		if (part_draws[phase] && length < sizeof(line))
			length += (size_t)snprintf(line + length, sizeof(line) - length, " (%lu draws)", part_draws[phase] / frames);
	}
	platform_log("%s", line);
	memset(spent, 0, sizeof(spent));
	memset(part_draws, 0, sizeof(part_draws));
	memset(halo_perf_counters, 0, sizeof(halo_perf_counters));
	frames = 0;
	frames_start = now;
}
