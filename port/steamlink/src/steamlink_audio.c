/*
STEAMLINK_AUDIO.C

The Steam Link's sound (HALO_CONSOLE_AUDIO_OUTPUT, console_audio_start): the
box has no ALSA card; its HDMI audio is the Marvell presentation engine's,
which only the firmware's own SDL plays to (its "marvell" audio driver).
The game's SDL3 is upstream's, without it, so the mixer's output goes to
the firmware's SDL2 (/usr/lib/libSDL2-2.0.so.0), loaded here for its audio
alone. The game's SDL3 is linked into the executable and exports nothing,
so the two never meet.

It plays only while the Steam Link's own interface (shell) is not running:
the box launches an app with it closed (/home/steam/shell.sh), and with it
open the output cannot be opened.
*/

#include "platform.h"

#include <dlfcn.h>
#include <string.h>

/* SDL2's, as SDL_audio.h has them */
#define SDL2_INIT_AUDIO 0x00000010u
#define SDL2_AUDIO_F32LSB 0x8120
#define SDL2_AUDIO_ALLOW_NONE 0

typedef void (*sdl2_audio_callback)(void *userdata, unsigned char *stream, int length);

struct sdl2_audio_spec
{
	int freq;
	unsigned short format;
	unsigned char channels;
	unsigned char silence;
	unsigned short samples;
	unsigned short padding;
	unsigned int size;
	sdl2_audio_callback callback;
	void *userdata;
};

/* the most the game's mixer mixes in a call (dsound_sdl.c's MIX_CHUNK_FRAMES) */
#define MIX_CHUNK_FRAMES 256
/* the output's buffer: 1024 frames, 21 ms at 48 kHz (the box's one core
mixes 256 frames in about 2 ms) */
#define OUTPUT_FRAMES 1024

static void (*audio_mix)(float *output, unsigned long frames);
static int audio_channels;

static void audio_callback(void *userdata, unsigned char *stream, int length)
{
	float *output = (float *)stream;
	unsigned long frames = (unsigned long)length / ((unsigned long)audio_channels * sizeof(float));

	(void)userdata;
	while (frames)
	{
		unsigned long chunk = frames > MIX_CHUNK_FRAMES ? MIX_CHUNK_FRAMES : frames;

		audio_mix(output, chunk);
		output += chunk * (unsigned long)audio_channels;
		frames -= chunk;
	}
}

int console_audio_start(int rate, int channels, void (*mix)(float *output, unsigned long frames))
{
	void *sdl2 = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
	int (*init_subsystem)(unsigned int);
	const char *(*get_error)(void);
	const char *(*current_driver)(void);
	unsigned int (*open_device)(const char *, int, const struct sdl2_audio_spec *, struct sdl2_audio_spec *, int);
	void (*pause_device)(unsigned int, int);
	struct sdl2_audio_spec wanted, given;
	unsigned int device;

	if (!sdl2)
	{
		platform_log("audio: no firmware SDL2 (%s)", dlerror());
		return 0;
	}
	init_subsystem = (__typeof__(init_subsystem))dlsym(sdl2, "SDL_InitSubSystem");
	get_error = (__typeof__(get_error))dlsym(sdl2, "SDL_GetError");
	current_driver = (__typeof__(current_driver))dlsym(sdl2, "SDL_GetCurrentAudioDriver");
	open_device = (__typeof__(open_device))dlsym(sdl2, "SDL_OpenAudioDevice");
	pause_device = (__typeof__(pause_device))dlsym(sdl2, "SDL_PauseAudioDevice");
	if (!init_subsystem || !get_error || !current_driver || !open_device || !pause_device)
	{
		platform_log("audio: the firmware's SDL2 lacks its audio functions");
		dlclose(sdl2);
		return 0;
	}
	if (init_subsystem(SDL2_INIT_AUDIO) < 0)
	{
		platform_log("audio: the firmware's SDL2 has no audio (%s)", get_error());
		return 0;
	}
	audio_mix = mix;
	audio_channels = channels;
	memset(&wanted, 0, sizeof(wanted));
	wanted.freq = rate;
	wanted.format = SDL2_AUDIO_F32LSB;
	wanted.channels = (unsigned char)channels;
	wanted.samples = OUTPUT_FRAMES;
	wanted.callback = audio_callback;
	/* (exactly as asked: SDL2 converts to what the output takes) */
	device = open_device(NULL, 0, &wanted, &given, SDL2_AUDIO_ALLOW_NONE);
	if (!device)
	{
		platform_log("audio: the firmware's %s output does not open (%s): the Steam Link's interface may hold it",
			current_driver(), get_error());
		return 0;
	}
	pause_device(device, 0);
	platform_log("audio: the firmware's %s output, %d Hz, %d channels, %d frames a buffer", current_driver(),
		given.freq, given.channels, given.samples);
	return 1;
}
