/*
STEAMLINK_COMBOS.C

The Steam Link's button combos, read each pass of the event pump
(console_performance_update, sdl_platform.c), each once a press. Back is
held for all of them, as the old Xbox's dashboards and mod chips had their
in-game combos, and a key does each one too:

	Back + d-pad down (F9)   performance mode (steamlink_performance.c)
	Back + d-pad up (F8)     the game's framerate counter (display_framerate)
	Back + d-pad left (F7)   the box's memory, temperature and uptime, as
	                         EvolutionX's system info had them
	Back + d-pad right (F6)  a screenshot of the next frame to the stick
	                         (debug.screenshot_directory, run.sh sets it)
	Back + left stick (F5)   a line from the xbox-scene days
	Back + Start + both      in-game reset: back to the Steam Link's menu
	triggers

The lines go to the game's console (terminal.c), green as the scene's
dashboards were, and to the log. For a test with nobody at the pad,
HALO_COMBO_TEST presses them: its letters (p f i s l r, as the list above)
one every two seconds, from its first number of seconds after the start
("40:fislr").
*/

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "port_config.h"

void console_performance_update(void);
void steamlink_performance_update(int switched);

/* (the game's: main.c's counter, terminal.c's console, d3d8_gl.c's shot, the
menus' quit and the build's name) */
extern unsigned char display_framerate;
void terminal_printf(const void *color, const char *format, ...);
void halo_test_screenshot(void);
void platform_request_quit(void);
const char *build_identity_watermark(void);

enum
{
	COMBO_PERFORMANCE,
	COMBO_FRAMERATE,
	COMBO_SYSTEM_INFO,
	COMBO_SCREENSHOT,
	COMBO_SCENE,
	COMBO_RESET,
	NUMBER_OF_COMBOS
};

/* the console's color: alpha, red, green, blue */
static const float scene_green[4] = { 1.0f, 0.45f, 1.0f, 0.25f };

static const char *const scene_lines[] =
{
	"softmodded w the 007 agent under fire save. borrowed it off ur cousin, never gave it back",
	"xbins ftp: 3 slots, all full, u r 47th in the queue",
	"tsop flashed, m8plus on it. no going back now",
	"hotswapped the hdd w the box on. do not try this at home. we did tho",
	"x3 ce pinned to the lpc, steady hands, capisce",
	"evox ftp up on 192.168.0.whatever, flashfxp open, life is good",
	"the duke would never fit on this box",
	"unleashx skin done downloading. 4 hours on 56k",
	"xbmc playing a 700mb avi off a smb share. it stutters. we dont care",
	"eeprom backed up? no? good luck",
	"check ur 1.0's clock capacitor, its been leaking since 2001",
	"system link over xlink kai, 300 ping bloodgulch, best night of ur life",
	"sparkedit: warthog on top of red base. nobody touches the warthog",
	"error 16. reformat c, e, f. pray",
	"mechassault, splinter cell or 007. pick ur exploit",
	"f: is where the good stuff lives",
	"hold A for the ms dash, hold nothing for evox, hold ur breath for the tsop",
	"this box has 263 mb of ram. the og xbox had 64. we r living large",
};

#define SCENE_LINE_COUNT ((int)(sizeof(scene_lines) / sizeof(scene_lines[0])))

/* the in-game reset's line shows for this long before the game quits */
#define RESET_DELAY_MS 1200

static void scene_printf(const char *format, ...)
{
	char line[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	platform_log("combo: %s", line);
	terminal_printf(scene_green, "%s", line);
}

/* HALO_COMBO_TEST's combo held now, as a bit */
static unsigned test_combos(void)
{
	static const char letters[] = "pfislr";
	static const char *script;
	static Uint64 start_ms;
	const char *letter;
	Uint64 now = SDL_GetTicks();
	unsigned long step;

	if (!script)
	{
		const char *given = getenv("HALO_COMBO_TEST");

		script = given && strchr(given, ':') ? given : "";
		start_ms = now + (Uint64)strtoul(script, NULL, 10) * 1000;
		if (*script)
			script = strchr(script, ':') + 1;
	}
	if (!*script || now < start_ms || (now - start_ms) % 2000 >= 200)
		return 0;
	step = (unsigned long)((now - start_ms) / 2000);
	if (step >= strlen(script) || !(letter = strchr(letters, script[step])))
		return 0;
	return 1u << (letter - letters);
}

/* which combos are held now, a bit each */
static unsigned held_combos(void)
{
	const bool *keys = SDL_GetKeyboardState(NULL);
	unsigned held = 0;
	SDL_JoystickID *pads;
	int count = 0, index;

	if (keys)
	{
		held |= keys[SDL_SCANCODE_F9] ? 1u << COMBO_PERFORMANCE : 0;
		held |= keys[SDL_SCANCODE_F8] ? 1u << COMBO_FRAMERATE : 0;
		held |= keys[SDL_SCANCODE_F7] ? 1u << COMBO_SYSTEM_INFO : 0;
		held |= keys[SDL_SCANCODE_F6] ? 1u << COMBO_SCREENSHOT : 0;
		held |= keys[SDL_SCANCODE_F5] ? 1u << COMBO_SCENE : 0;
	}
	pads = SDL_GetGamepads(&count);
	for (index = 0; pads && index < count; index++)
	{
		/* (the controllers xinput_sdl.c opened) */
		SDL_Gamepad *pad = SDL_GetGamepadFromID(pads[index]);

		if (!pad || !SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK))
			continue;
		if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START))
		{
			/* (Start is in no other combo: the reset needs it, so the
			others cannot go off on the way to it) */
			if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384 &&
				SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384)
			{
				held |= 1u << COMBO_RESET;
			}
			continue;
		}
		held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? 1u << COMBO_PERFORMANCE : 0;
		held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) ? 1u << COMBO_FRAMERATE : 0;
		held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? 1u << COMBO_SYSTEM_INFO : 0;
		held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? 1u << COMBO_SCREENSHOT : 0;
		held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK) ? 1u << COMBO_SCENE : 0;
	}
	SDL_free(pads);
	return held | test_combos();
}

/* a number from a /proc or /sys file's line that starts with `key` (the
first line for none), or -1 */
static long long file_number(const char *path, const char *key)
{
	char line[256];
	long long value = -1;
	size_t key_length = key ? strlen(key) : 0;
	FILE *file = fopen(path, "r");

	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file))
	{
		if (key && strncmp(line, key, key_length) != 0)
			continue;
		value = strtoll(line + key_length, NULL, 10);
		break;
	}
	fclose(file);
	return value;
}

static void system_info(void)
{
	/* (the box's kernel is 3.8, before MemAvailable: free and the page
	cache together) */
	long long total = file_number("/proc/meminfo", "MemTotal:");
	long long free_kb = file_number("/proc/meminfo", "MemFree:");
	long long cached = file_number("/proc/meminfo", "Cached:");
	long long buffers = file_number("/proc/meminfo", "Buffers:");
	long long game = file_number("/proc/self/status", "VmRSS:");
	long long uptime = file_number("/proc/uptime", NULL);
	long long temperature = file_number("/sys/class/thermal/thermal_zone0/temp", NULL);
	/* (the 2015 box has none: /sys/class/thermal is not there) */
	char temperature_text[32] = "temp ? (no sensor, touch it)";

	if (temperature > 1000)
		snprintf(temperature_text, sizeof(temperature_text), "cpu %lld c", temperature / 1000);
	else if (temperature > 0)
		snprintf(temperature_text, sizeof(temperature_text), "cpu %lld c", temperature);
	scene_printf("sysinfo: %s", build_identity_watermark());
	scene_printf("ram %lld mb free of %lld, the game %lld mb. %s. up %lld min",
		(free_kb + cached + buffers) / 1024, total / 1024, game / 1024, temperature_text, uptime / 60);
}

static void screenshot(void)
{
	if (!*config_string("debug.screenshot_directory"))
	{
		scene_printf("no screenshot folder (debug.screenshot_directory)");
		return;
	}
	halo_test_screenshot();
}

void console_performance_update(void)
{
	static unsigned was_held;
	static Uint64 reset_at, screenshot_note_at;
	static int scene_line = -1;
	unsigned held = held_combos();
	unsigned pressed = held & ~was_held;
	Uint64 now = SDL_GetTicks();

	was_held = held;
	steamlink_performance_update((pressed & (1u << COMBO_PERFORMANCE)) != 0);
	if (pressed & (1u << COMBO_FRAMERATE))
	{
		display_framerate = !display_framerate;
		scene_printf(display_framerate ? "framerate counter on. debug bios not required" : "framerate counter off");
	}
	if (pressed & (1u << COMBO_SYSTEM_INFO))
		system_info();
	if (pressed & (1u << COMBO_SCREENSHOT))
	{
		screenshot();
		/* (the line after the shot, which is the next frame's: not in it) */
		screenshot_note_at = now + 100;
	}
	if (screenshot_note_at && now >= screenshot_note_at)
	{
		screenshot_note_at = 0;
		if (*config_string("debug.screenshot_directory"))
			scene_printf("screenshot on the stick. ftp it off like its 2004");
	}
	if (pressed & (1u << COMBO_SCENE))
	{
		/* (a different line each time, from a random start) */
		if (scene_line < 0)
			scene_line = (int)(SDL_GetPerformanceCounter() % SCENE_LINE_COUNT);
		scene_printf("%s", scene_lines[scene_line]);
		scene_line = (scene_line + 1) % SCENE_LINE_COUNT;
	}
	if ((pressed & (1u << COMBO_RESET)) && !reset_at)
	{
		scene_printf("igr: back to the dash");
		reset_at = now + RESET_DELAY_MS;
	}
	if (reset_at && now >= reset_at)
	{
		reset_at = 0;
		platform_request_quit();
	}
}
