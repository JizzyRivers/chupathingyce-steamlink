/*
STEAMLINK_CACHE_SCREEN.C

The screen while the first start makes the game's cache files
(HALO_CONSOLE_CACHE_SCREEN, cache_files_windows.c): on the stick's FAT32 a
file made long is written full of zeros, about three minutes of them, with
nothing else on the television. A ring in the stars lights up around its
length as they go, made here from nothing of the game's, so it needs nothing
but the code, and asks for no map: this is before any is loaded. The first
start's extraction (sdl_platform.c) and a map's download (browser_screen.c)
have the same ring.

The sound is the Xbox's loading screen's, as progress_bar.c plays it: its one
tone, compiled into the game, in four voices at four pitches, each joining
over its part of the load. The Xbox's load took seconds, and its voices start
near silence and end in it; these take minutes, so here each stays audible
from when it joins until the very end. This is before the game's mixer has
started, so these screens mix it themselves, to the output the game will take
over (steamlink_audio.c).
*/

#include "ui_overlay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

/* (d3d8_gl.c's: the first over the back buffer, the second the frame shown) */
void halo_draw_picture(const unsigned char *rgba, long width, long height);
void D3DDevice_Present(const void *source, const void *destination, void *unused, void *unused2);

enum
{
	SCREEN_WIDTH = 640,
	SCREEN_HEIGHT = 480,
};

#define RING_PI 3.14159265f

/* what each pixel is, worked out once: how much of the ring it is, how far
around it, its glow, the ring's surface there and the sky behind */
struct ring_pixel
{
	unsigned char band, around, glow, surface, sky;
};

static struct ring_pixel *ring_pixels;
static unsigned char *ring_frame;

static float ring_clamp(float value)
{
	return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

/* ---------- the sound */

enum
{
	TONE_VOICES = 4,
	TONE_RATE = 48000,
	TONE_CHANNELS = 2,
	TONE_BYTES = 18396,
};

/* (progress_bar.c's, and dsound_sdl.c's decoder of its Xbox ADPCM) */
extern unsigned char compressed_monk_tone[TONE_BYTES];
short *console_decode_adpcm(const unsigned char *source, unsigned long size, unsigned long *frames);
int console_audio_start(int rate, int channels, void (*mix)(float *output, unsigned long frames));

static const struct
{
	float first, last, volume, pitch;
} tone_voices[TONE_VOICES] =
{
	/* (progress_bar.c's ranges, volumes and pitches) */
	{ 0.0f, 1.0f, 0.60f, -4681.0f },
	{ 0.4f, 1.0f, 0.70f, -6386.0f },
	{ 0.5f, 1.0f, 0.60f, -3999.0f },
	{ 0.55f, 1.0f, 0.70f, -8773.0f },
};

static short *tone_samples;
static unsigned long tone_frames;
static double tone_positions[TONE_VOICES];
static double tone_steps[TONE_VOICES];
/* (each voice's gain, set by the screen, followed a little at a time by the
output's thread) */
static volatile float tone_targets[TONE_VOICES];
static float tone_gains[TONE_VOICES];
static int tone_started;
static float tone_scale = 1.0f;

static void tone_mix(float *output, unsigned long frames)
{
	unsigned long frame;
	int voice;

	for (frame = 0; frame < frames; frame++)
	{
		float sample = 0.0f;

		for (voice = 0; voice < TONE_VOICES; voice++)
		{
			unsigned long index = (unsigned long)tone_positions[voice];
			unsigned long next = index + 1 < tone_frames ? index + 1 : 0;
			float part = (float)(tone_positions[voice] - (double)index);

			tone_gains[voice] += (tone_targets[voice] - tone_gains[voice]) * 0.0002f;
			sample += ((float)tone_samples[index] * (1.0f - part) + (float)tone_samples[next] * part) *
				tone_gains[voice] * tone_scale / 32768.0f;
			tone_positions[voice] += tone_steps[voice];
			if (tone_positions[voice] >= (double)tone_frames)
				tone_positions[voice] -= (double)tone_frames;
		}
		output[frame * TONE_CHANNELS] = sample;
		output[frame * TONE_CHANNELS + 1] = sample;
	}
}

static void tone_progress(float fraction)
{
	int voice;

	if (!tone_started)
	{
		tone_started = -1;
		tone_samples = console_decode_adpcm(compressed_monk_tone, TONE_BYTES, &tone_frames);
		if (!tone_samples || !tone_frames)
			return;
		{
			unsigned long frame;
			int peak = 0;

			for (frame = 0; frame < tone_frames; frame++)
			{
				int level = abs((int)tone_samples[frame]);

				if (level > peak)
					peak = level;
			}
			/* (each voice to the volume in its table, however loud the tone) */
			tone_scale = peak ? 32768.0f / (float)peak : 1.0f;
			SDL_Log("cache screen: the tone's %lu frames, peak %d of 32768", tone_frames, peak);
		}
		for (voice = 0; voice < TONE_VOICES; voice++)
			tone_steps[voice] = 48000.0 * pow(2.0, (double)tone_voices[voice].pitch / 4096.0) / (double)TONE_RATE;
		if (console_audio_start(TONE_RATE, TONE_CHANNELS, tone_mix))
			tone_started = 1;
	}
	if (tone_started <= 0)
		return;
	for (voice = 0; voice < TONE_VOICES; voice++)
	{
		/* (in over a twentieth of the load, out over the last hundredth) */
		float in = ring_clamp((fraction - tone_voices[voice].first) * 20.0f + (voice ? 0.0f : 1.0f));
		float out = ring_clamp((1.0f - fraction) * 100.0f);

		tone_targets[voice] = tone_voices[voice].volume * 0.4f * in * out;
	}
}

/* ---------- the picture */

static void ring_make(void)
{
	const float center_x = 320.0f, center_y = 205.0f;
	const float radius_x = 250.0f, radius_y = 84.0f;
	const float cosine = 0.951f, sine = -0.309f;
	int x, y;

	ring_pixels = malloc(sizeof(struct ring_pixel) * SCREEN_WIDTH * SCREEN_HEIGHT);
	ring_frame = malloc(SCREEN_WIDTH * SCREEN_HEIGHT * 4);
	if (!ring_pixels || !ring_frame)
		return;
	for (y = 0; y < SCREEN_HEIGHT; y++)
	{
		for (x = 0; x < SCREEN_WIDTH; x++)
		{
			struct ring_pixel *pixel = &ring_pixels[y * SCREEN_WIDTH + x];
			float dx = (float)x - center_x, dy = (float)y - center_y;
			float u = (dx * cosine + dy * sine) / radius_x;
			float v = (dy * cosine - dx * sine) / radius_y;
			float radius = sqrtf(u * u + v * v);
			float around = (atan2f(v, u) + RING_PI) / (2.0f * RING_PI);
			/* (nearer the viewer, below, it is wider) */
			float half_width = 0.05f + 0.05f * ring_clamp(v);
			float distance = fabsf(radius - 1.0f);
			float surface = 0.5f + 0.25f * sinf(around * 2.0f * RING_PI * 37.0f) +
				0.25f * sinf(around * 2.0f * RING_PI * 91.0f + radius * 40.0f);
			unsigned int hash = (unsigned int)x * 73856093u ^ (unsigned int)y * 19349663u;
			float nebula = 0.5f + 0.5f * sinf((float)x * 0.013f + (float)y * 0.021f);

			hash ^= hash >> 13;
			hash *= 1274126177u;
			hash ^= hash >> 16;
			pixel->band = (unsigned char)(255.0f * ring_clamp((half_width - distance) * 90.0f));
			pixel->around = (unsigned char)(255.0f * ring_clamp(around));
			pixel->glow = (unsigned char)(255.0f * ring_clamp(1.0f - (distance - half_width) / 0.18f));
			pixel->surface = (unsigned char)(255.0f * ring_clamp(surface));
			pixel->sky = (hash & 511u) == 0u ? (unsigned char)(150u + (hash >> 25)) : (unsigned char)(nebula * 18.0f);
		}
	}
}

static void ring_draw(float fraction, float pulse)
{
	unsigned char *to = ring_frame;
	int edge = (int)(fraction * 255.0f);
	int index;

	for (index = 0; index < SCREEN_WIDTH * SCREEN_HEIGHT; index++, to += 4)
	{
		const struct ring_pixel *pixel = &ring_pixels[index];
		int lit = pixel->around <= edge;
		int red = pixel->sky / 3 + 2, green = pixel->sky / 2 + 4, blue = pixel->sky + 10;

		if (pixel->glow && lit)
		{
			int glow = pixel->glow * pixel->glow / 255;

			glow = (int)((float)glow * (0.55f + 0.2f * pulse));
			red += glow / 8;
			green += glow / 3;
			blue += glow * 3 / 4;
		}
		if (pixel->band)
		{
			int surface_red, surface_green, surface_blue;

			if (lit)
			{
				int front = edge - pixel->around;

				surface_red = 40 + pixel->surface * 3 / 4;
				surface_green = 90 + pixel->surface * 5 / 8;
				surface_blue = 170 + pixel->surface / 3;
				/* (its leading edge, as hot as it gets) */
				if (front < 4 && edge < 255)
					surface_red = surface_green = surface_blue = 255;
			}
			else
			{
				surface_red = 20 + pixel->surface / 12;
				surface_green = 26 + pixel->surface / 10;
				surface_blue = 40 + pixel->surface / 8;
			}
			red += (surface_red - red) * pixel->band / 255;
			green += (surface_green - green) * pixel->band / 255;
			blue += (surface_blue - blue) * pixel->band / 255;
		}
		to[0] = (unsigned char)(red > 255 ? 255 : red);
		to[1] = (unsigned char)(green > 255 ? 255 : green);
		to[2] = (unsigned char)(blue > 255 ? 255 : blue);
		to[3] = 255;
	}
}

/* whether a screen drawn between slow steps is due again: three a second, as
ten made the box's shaders take noticeably longer */
int console_ring_due(void)
{
	static Uint64 shown;
	Uint64 now = SDL_GetTicks();

	if (shown && now - shown < 300)
		return 0;
	shown = now;
	return 1;
}

/* the ring as far as fraction, 640x480 RGBA, for a screen to show */
const unsigned char *console_ring_frame(float fraction, int with_sound)
{
	float pulse = 0.5f + 0.5f * sinf((float)SDL_GetTicks() * 0.003f);

	if (!ring_pixels)
		ring_make();
	if (!ring_pixels || !ring_frame)
		return NULL;
	if (with_sound)
		tone_progress(fraction);
	ring_draw(fraction, pulse);
	return ring_frame;
}

/* behind what the game draws next (a map's download, browser_screen.c) */
void console_ring_picture(float fraction)
{
	const unsigned char *frame = console_ring_frame(fraction, 0);

	if (frame)
		halo_draw_picture(frame, SCREEN_WIDTH, SCREEN_HEIGHT);
}

void console_cache_screen(unsigned long long done, unsigned long long total)
{
	char percent[16];
	float fraction = total ? (float)((double)done / (double)total) : 1.0f;
	const unsigned char *frame = console_ring_frame(fraction, 1);

	if (!frame)
		return;
	halo_draw_picture(frame, SCREEN_WIDTH, SCREEN_HEIGHT);
	if (ui_overlay_available())
	{
		snprintf(percent, sizeof(percent), "%d%%", (int)(fraction * 100.0f));
		ui_overlay_text_glow(UI_FONT_TITLE, 21.0f, 320.0f, 338.0f, UI_ALIGN_CENTER, 0xa8ccffffu, 0x3c78ff80u, "CHUPATHINGYCE");
		ui_overlay_text(UI_FONT_MENU, 24.0f, 320.0f, 376.0f, UI_ALIGN_CENTER, 0xffffffffu, percent);
		ui_overlay_text(UI_FONT_MENU_SMALL, 15.0f, 320.0f, 420.0f, UI_ALIGN_CENTER, 0x8ca8d0ffu,
			"PREPARING THE CACHE. THIS ONLY HAPPENS ONCE.");
	}
	D3DDevice_Present(NULL, NULL, NULL, NULL);
}

/* the ring's memory back, for the game; its tone silent, to the game's mixer
taking the output over (and its samples kept for that moment's last buffers) */
void console_cache_screen_done(void)
{
	int voice;

	for (voice = 0; voice < TONE_VOICES; voice++)
		tone_targets[voice] = 0.0f;
	free(ring_pixels);
	free(ring_frame);
	ring_pixels = NULL;
	ring_frame = NULL;
}
