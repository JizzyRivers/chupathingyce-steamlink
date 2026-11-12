/*
STEAMLINK_SETUP.C

The Steam Link's start-up screens (HALO_CONSOLE_SETUP_SCREENS,
halo_console_features.h): the box has no dialogs and no file picker, and
is on a television. What the game asks of a console's port there:

	console_setup_start          right after SDL starts: the install screen
	                             when run.sh is unpacking a package
	                             (HALO_INSTALL_SCREEN)
	platform_offer_game_data     the first start, with no maps: the disc
	                             image on the stick's root, found by itself
	                             and copied with the ring up
	console_setup_after_data     once the maps are checked: Custom Edition's
	                             maps from its installer on the stick, once,
	                             and the update question (updater.c's
	                             updater_console_check)
	console_setup_update_question, console_setup_screen_*,
	console_setup_notice         the updater's question, its download's ring
	                             and a failure said on the screen

The words are drawn in the menus' faces (posix_ui_font.c), needing no game
and no GL of the game's; the ring is the cache screen's
(steamlink_cache_screen.c), with its sound. Until 0.9.0 this was the
Steam Link's part of the shared sdl_platform.c.
*/

#include "platform.h"
#include "sdl_platform.h"
#include "port_config.h"
#include "posix.h"
#include "xiso.h"
#include "ce_installer.h"
#include "ui_font.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* (steamlink_cache_screen.c) */
const unsigned char *console_ring_frame(float fraction, int with_sound);
/* (updater.c) */
void updater_console_check(void);

void console_setup_start(void);
void console_setup_after_data(const char *data_root);
void *console_setup_screen_open(void);
void console_setup_screen_progress(void *screen, float fraction, const char *title, const char *line);
void console_setup_screen_close(void *screen);
void console_setup_notice(const char *first, const char *second);
int console_setup_update_question(const char *latest, const char *current);

/* ---------- the screen */

/* the screen's whole picture, and a 640x360 one to draw in: a window smaller
than the display stays that small in its middle, on a television */
static SDL_Window *screen_window(SDL_Renderer **renderer)
{
	const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
	SDL_Window *window = SDL_CreateWindow("Chupathingy", mode ? mode->w : 1280, mode ? mode->h : 720,
		SDL_WINDOW_FULLSCREEN);

	*renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
	if (*renderer)
	{
		SDL_SetRenderLogicalPresentation(*renderer, 640, 360, SDL_LOGICAL_PRESENTATION_LETTERBOX);
		SDL_SetRenderVSync(*renderer, 1);
		platform_log("screen: %s, %s", SDL_GetRendererName(*renderer), SDL_GetCurrentVideoDriver());
	}
	else
	{
		platform_log("screen: no renderer: %s", SDL_GetError());
	}
	return window;
}

/* the screens' words. A string is drawn once to a texture and kept, as a
television's screen says the same few things for minutes. Sizes and places
are in the 640x360 the screens lay out in; the glyphs are made at the 720
lines the display has */
enum
{
	SCREEN_TEXTS = 24,
	SCREEN_TEXT_SCALE = 2,
};

static struct
{
	SDL_Texture *texture;
	int font;
	float size;
	char text[200];
	int width, height;
} screen_texts[SCREEN_TEXTS];
static int screen_text_next;

/* (before their renderer goes) */
static void screen_text_forget(void)
{
	int index;

	for (index = 0; index < SCREEN_TEXTS; index++)
	{
		if (screen_texts[index].texture)
			SDL_DestroyTexture(screen_texts[index].texture);
		screen_texts[index].texture = NULL;
	}
}

static int screen_text_index(SDL_Renderer *renderer, int font, float size, const char *text)
{
	float pixels = size * SCREEN_TEXT_SCALE, pen = 0.0f, ascent = 0.0f, descent = 0.0f;
	unsigned char *rgba;
	const char *c;
	int index, width, height, baseline;
	unsigned int previous = 0;

	for (index = 0; index < SCREEN_TEXTS; index++)
	{
		if (screen_texts[index].texture && screen_texts[index].font == font && screen_texts[index].size == size &&
			!strcmp(screen_texts[index].text, text))
		{
			return index;
		}
	}
	if (!posix_ui_font_metrics(font, pixels, &ascent, &descent))
		return -1;
	for (c = text; *c; c++)
	{
		pen += posix_ui_font_advance(font, pixels, (unsigned char)*c, previous);
		previous = (unsigned char)*c;
	}
	width = (int)pen + 4;
	/* (a face's capitals can stand taller than it says: the titles' do) */
	baseline = (int)(ascent + pixels * 0.35f);
	height = baseline + (int)(descent + pixels * 0.2f);
	rgba = calloc((size_t)width * height, 4);
	if (!rgba)
		return -1;
	pen = 1.0f;
	previous = 0;
	for (c = text; *c; c++)
	{
		int glyph_width, glyph_height, x_offset, y_offset, x, y;
		unsigned char *glyph;

		pen += previous ? posix_ui_font_advance(font, pixels, (unsigned char)*c, previous) -
			posix_ui_font_advance(font, pixels, (unsigned char)*c, 0) : 0.0f;
		glyph = posix_ui_font_glyph(font, pixels, (unsigned char)*c, &glyph_width, &glyph_height, &x_offset, &y_offset);
		for (y = 0; glyph && y < glyph_height; y++)
		{
			for (x = 0; x < glyph_width; x++)
			{
				int to_x = (int)pen + x_offset + x, to_y = baseline + y_offset + y;

				if (to_x >= 0 && to_x < width && to_y >= 0 && to_y < height)
				{
					unsigned char *to = rgba + ((size_t)to_y * width + to_x) * 4;
					unsigned char coverage = glyph[y * glyph_width + x];

					to[0] = to[1] = to[2] = 255;
					if (coverage > to[3])
						to[3] = coverage;
				}
			}
		}
		if (glyph)
			posix_ui_font_free(glyph);
		pen += posix_ui_font_advance(font, pixels, (unsigned char)*c, 0);
		previous = (unsigned char)*c;
	}
	index = screen_text_next;
	screen_text_next = (screen_text_next + 1) % SCREEN_TEXTS;
	if (screen_texts[index].texture)
		SDL_DestroyTexture(screen_texts[index].texture);
	screen_texts[index].texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width,
		height);
	if (screen_texts[index].texture)
	{
		SDL_UpdateTexture(screen_texts[index].texture, NULL, rgba, width * 4);
		SDL_SetTextureBlendMode(screen_texts[index].texture, SDL_BLENDMODE_BLEND);
	}
	free(rgba);
	screen_texts[index].font = font;
	screen_texts[index].size = size;
	snprintf(screen_texts[index].text, sizeof(screen_texts[index].text), "%s", text);
	screen_texts[index].width = width;
	screen_texts[index].height = height;
	return screen_texts[index].texture ? index : -1;
}

/* colors 0xRRGGBB. align: -1 from x, 0 about x, 1 to x. Too wide for the
screen, smaller. Its width back */
static float screen_text(SDL_Renderer *renderer, int font, float size, float x, float y, int align, unsigned int color,
	int glow, const char *text)
{
	int index = screen_text_index(renderer, font, size, text);
	SDL_FRect place;
	float fit = 1.0f;

	if (index < 0)
		return 0.0f;
	place.w = (float)screen_texts[index].width / SCREEN_TEXT_SCALE;
	place.h = (float)screen_texts[index].height / SCREEN_TEXT_SCALE;
	if (place.w > 600.0f)
		fit = 600.0f / place.w;
	place.w *= fit;
	place.h *= fit;
	place.x = align < 0 ? x : align > 0 ? x - place.w : x - place.w * 0.5f;
	place.y = y;
	if (glow)
	{
		static const float ring[4][2] = { { 1.5f, 0.0f }, { -1.5f, 0.0f }, { 0.0f, 1.5f }, { 0.0f, -1.5f } };
		int offset;

		SDL_SetTextureColorMod(screen_texts[index].texture, 60, 120, 255);
		SDL_SetTextureAlphaMod(screen_texts[index].texture, 70);
		for (offset = 0; offset < 4; offset++)
		{
			SDL_FRect spread = place;

			spread.x += ring[offset][0];
			spread.y += ring[offset][1];
			SDL_RenderTexture(renderer, screen_texts[index].texture, NULL, &spread);
		}
		SDL_SetTextureAlphaMod(screen_texts[index].texture, 255);
	}
	SDL_SetTextureColorMod(screen_texts[index].texture, (color >> 16) & 255, (color >> 8) & 255, color & 255);
	SDL_RenderTexture(renderer, screen_texts[index].texture, NULL, &place);
	return place.w;
}

/* a pad's button, its color and letter as on the pad, and what it does; the
width back */
static float screen_button(SDL_Renderer *renderer, float x, float y, const char *letter, unsigned int color,
	const char *label)
{
	enum { POINTS = 28 };
	const float radius = 9.0f, center_x = x + radius, center_y = y + radius;
	SDL_Vertex vertices[POINTS + 2];
	int indices[POINTS * 3];
	int index;
	float width;

	vertices[0].position.x = center_x;
	vertices[0].position.y = center_y;
	vertices[0].color.r = (float)((color >> 16) & 255) / 255.0f;
	vertices[0].color.g = (float)((color >> 8) & 255) / 255.0f;
	vertices[0].color.b = (float)(color & 255) / 255.0f;
	vertices[0].color.a = 1.0f;
	vertices[0].tex_coord.x = vertices[0].tex_coord.y = 0.0f;
	for (index = 0; index <= POINTS; index++)
	{
		float angle = (float)index * 6.2831853f / POINTS;

		vertices[index + 1] = vertices[0];
		vertices[index + 1].position.x = center_x + cosf(angle) * radius;
		vertices[index + 1].position.y = center_y + sinf(angle) * radius;
		/* (darker at its edge, as a pad's button is) */
		vertices[index + 1].color.r *= 0.55f;
		vertices[index + 1].color.g *= 0.55f;
		vertices[index + 1].color.b *= 0.55f;
		if (index < POINTS)
		{
			indices[index * 3] = 0;
			indices[index * 3 + 1] = index + 1;
			indices[index * 3 + 2] = index + 2;
		}
	}
	SDL_RenderGeometry(renderer, NULL, vertices, POINTS + 2, indices, POINTS * 3);
	screen_text(renderer, POSIX_UI_FONT_BOLD, 11.0f, center_x, center_y - 10.5f, 0, 0xffffff, 0, letter);
	width = screen_text(renderer, POSIX_UI_FONT_MENU_SMALL, 11.0f, x + radius * 2.0f + 6.0f, y - 1.5f, -1, 0xa8ccff, 0, label);
	return radius * 2.0f + 6.0f + width;
}

static void screen_ring(SDL_Renderer *renderer, SDL_Texture **ring, float fraction, int with_sound)
{
	const unsigned char *frame = console_ring_frame(fraction, with_sound);

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	if (!*ring)
		*ring = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 640, 480);
	if (*ring && frame)
	{
		/* (the picture's 4:3 in the 16:9 the screen lays out in) */
		SDL_FRect place = { 80.0f, 0.0f, 480.0f, 360.0f };

		SDL_UpdateTexture(*ring, NULL, frame, 640 * 4);
		SDL_RenderTexture(renderer, *ring, NULL, &place);
	}
}

/* a frame of the ring as far as fraction, with what it is for under it */
static void screen_ring_present(SDL_Renderer *renderer, SDL_Texture **ring, float fraction, const char *title,
	const char *line)
{
	char percent[16], heading[160];
	SDL_FRect bar = { 170.0f, 322.0f, 300.0f, 3.0f };
	int index;

	screen_ring(renderer, ring, fraction, 1);
	screen_text(renderer, POSIX_UI_FONT_TITLE, 15.0f, 320.0f, 241.0f, 0, 0xa8ccff, 1, "CHUPATHINGYCE");
	snprintf(percent, sizeof(percent), "%d%%", (int)(fraction * 100.0f));
	screen_text(renderer, POSIX_UI_FONT_MENU, 13.0f, 320.0f, 268.0f, 0, 0xffffff, 0, percent);
	for (index = 0; title[index] && index < (int)sizeof(heading) - 1; index++)
		heading[index] = (char)SDL_toupper((unsigned char)title[index]);
	heading[index] = 0;
	screen_text(renderer, POSIX_UI_FONT_MENU_SMALL, 10.0f, 320.0f, 292.0f, 0, 0x8ca8d0, 0, heading);
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(renderer, 30, 40, 60, 255);
	SDL_RenderFillRect(renderer, &bar);
	bar.w *= fraction;
	SDL_SetRenderDrawColor(renderer, 168, 204, 255, 255);
	SDL_RenderFillRect(renderer, &bar);
	screen_text(renderer, POSIX_UI_FONT_REGULAR, 8.0f, 320.0f, 330.0f, 0, 0x6c84a8, 0, line);
	SDL_RenderPresent(renderer);
}

/* a screen with the ring on it, for as long as something takes */
struct setup_screen
{
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *ring;
};

void *console_setup_screen_open(void)
{
	struct setup_screen *screen = calloc(1, sizeof(*screen));

	if (screen)
		screen->window = screen_window(&screen->renderer);
	return screen;
}

void console_setup_screen_progress(void *opened, float fraction, const char *title, const char *line)
{
	struct setup_screen *screen = opened;
	SDL_Event event;

	if (!screen || !screen->renderer)
		return;
	while (SDL_PollEvent(&event))
	{
		/* (the game starts in this process after: a pad seen now is not
		seen again) */
		if (event.type == SDL_EVENT_GAMEPAD_ADDED)
			SDL_OpenGamepad(event.gdevice.which);
	}
	screen_ring_present(screen->renderer, &screen->ring, fraction, title ? title : "", line ? line : "");
}

/* (the ring's texture and the words' are the renderer's, and go first) */
void console_setup_screen_close(void *opened)
{
	struct setup_screen *screen = opened;

	if (!screen)
		return;
	screen_text_forget();
	if (screen->ring)
		SDL_DestroyTexture(screen->ring);
	if (screen->renderer)
		SDL_DestroyRenderer(screen->renderer);
	if (screen->window)
		SDL_DestroyWindow(screen->window);
	free(screen);
}

/* ---------- what the screens say */

/* said on the screen for as long as it takes to read, or until a button is
pressed */
void console_setup_notice(const char *first, const char *second)
{
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *ring = NULL;
	Uint64 start = SDL_GetTicks();
	BOOL finished = FALSE;

	platform_log("%s %s", first, second);
	SDL_InitSubSystem(SDL_INIT_GAMEPAD);
	window = screen_window(&renderer);
	if (!renderer)
		return;
	while (!finished && SDL_GetTicks() - start < 60000)
	{
		SDL_Event event;

		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_GAMEPAD_ADDED)
				SDL_OpenGamepad(event.gdevice.which);
			else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_KEY_DOWN ||
				event.type == SDL_EVENT_QUIT)
				finished = TRUE;
		}
		screen_ring(renderer, &ring, 0.0f, 0);
		screen_text(renderer, POSIX_UI_FONT_MENU, 12.0f, 320.0f, 250.0f, 0, 0xffffff, 0, first);
		screen_text(renderer, POSIX_UI_FONT_MENU_SMALL, 10.0f, 320.0f, 274.0f, 0, 0xa8ccff, 0, second);
		screen_button(renderer, 280.0f, 308.0f, "A", 0x5cc24a, "CONTINUE");
		SDL_RenderPresent(renderer);
		SDL_Delay(50);
	}
	screen_text_forget();
	if (ring)
		SDL_DestroyTexture(ring);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
}

/* a new version's question, with the pad: 1 update, 2 never ask again, 0 not
now (nor if nobody answers: the game is not to wait for a player who is not
there) */
int console_setup_update_question(const char *latest, const char *current)
{
	SDL_Window *window;
	SDL_Renderer *renderer;
	Uint64 start = SDL_GetTicks();
	SDL_Texture *ring = NULL;
	char first[120];
	int answer = -1;
	float x;

	snprintf(first, sizeof(first), "VERSION %s IS OUT. YOU HAVE %s.", latest, current);
	window = screen_window(&renderer);
	if (!renderer)
		return 0;
	while (answer < 0 && SDL_GetTicks() - start < 60000)
	{
		SDL_Event event;

		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_GAMEPAD_ADDED)
				SDL_OpenGamepad(event.gdevice.which);
			else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
			{
				if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
					answer = 1;
				else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)
					answer = 0;
				else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH)
					answer = 2;
			}
			else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_QUIT)
				answer = 0;
		}
		screen_ring(renderer, &ring, 1.0f, 0);
		screen_text(renderer, POSIX_UI_FONT_TITLE, 15.0f, 320.0f, 241.0f, 0, 0xa8ccff, 1, "UPDATE AVAILABLE");
		screen_text(renderer, POSIX_UI_FONT_MENU, 11.0f, 320.0f, 277.0f, 0, 0xffffff, 0, first);
		x = 154.0f;
		x += screen_button(renderer, x, 308.0f, "A", 0x5cc24a, "UPDATE NOW") + 28.0f;
		x += screen_button(renderer, x, 308.0f, "B", 0xdc4438, "NOT NOW") + 28.0f;
		screen_button(renderer, x, 308.0f, "Y", 0xe8b92c, "DON'T ASK AGAIN");
		SDL_RenderPresent(renderer);
		SDL_Delay(30);
	}
	screen_text_forget();
	if (ring)
		SDL_DestroyTexture(ring);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	return answer < 0 ? 0 : answer;
}

/* ---------- start-up */

/* HALO_INSTALL_SCREEN: the app's run.sh, unpacking a package, with nothing else
to put on the television for that minute (which otherwise shows whatever video
memory holds, garbled). The ring goes round until the file it names is gone,
then this copy of the game quits; its length says nothing of the unpacking's */
static void install_screen(const char *running)
{
	struct setup_screen *screen = console_setup_screen_open();
	Uint64 start = SDL_GetTicks();

	while (screen && screen->renderer && !access(running, F_OK))
	{
		console_setup_screen_progress(screen, (float)((SDL_GetTicks() - start) % 20000) / 20000.0f,
			"Installing the new version...", "This takes about a minute.");
		SDL_Delay(30);
	}
	console_setup_screen_close(screen);
	SDL_Quit();
	exit(EXIT_SUCCESS);
}

void console_setup_start(void)
{
	const char *running = getenv("HALO_INSTALL_SCREEN");

	if (running && *running)
		install_screen(running);
}

/* the copy out of image into destination, the ring up meanwhile: TRUE once
it worked (why not in error), with the bytes it wrote in written */
static BOOL import_with_ring(int kind, const char *image, const char *destination, unsigned long long *written,
	char *error, int error_size)
{
	const char *title = kind == PLATFORM_IMPORT_CE_INSTALLER ? "Importing Custom Edition maps..." :
		"Extracting the maps folder...";
	struct setup_screen *screen;
	BOOL succeeded = FALSE;
	unsigned long long done = 0, total = 0;

	*written = 0;
	if (!platform_import_start(kind, image, destination, error, error_size))
		return FALSE;
	screen = console_setup_screen_open();
	for (;;)
	{
		char file[256] = "", line[320];
		BOOL finished = platform_import_poll(file, sizeof(file), &done, &total, &succeeded, error, error_size);

		snprintf(line, sizeof(line), "%s  (%llu of %llu MB)", file, done >> 20, total >> 20);
		console_setup_screen_progress(screen, total ? (float)((double)done / (double)total) : 0.0f, title, line);
		if (finished)
			break;
		SDL_Delay(16);
	}
	console_setup_screen_close(screen);
	*written = done;
	return succeeded;
}

/* a file in the folder above the data folder, the root of the player's USB
stick (the app's run.sh mounts it), whose name ends in one of the
extensions and which is what is_kind says it is */
static BOOL stick_find(const char *destination, const char *const *extensions, int (*is_kind)(const char *path),
	char *found_path, int size)
{
	char folder[1024];
	char *slash;
	DIR *directory;
	struct dirent *entry;
	BOOL found = FALSE;

	snprintf(folder, sizeof(folder), "%s", destination);
	slash = strrchr(folder, '/');
	if (slash && slash != folder)
		*slash = '\0';
	directory = opendir(folder);
	if (!directory)
		return FALSE;
	while (!found && (entry = readdir(directory)) != NULL)
	{
		const char *dot = strrchr(entry->d_name, '.');
		int index;

		for (index = 0; dot && extensions[index] && SDL_strcasecmp(dot, extensions[index]); index++)
		{
		}
		if (!dot || !extensions[index])
			continue;
		snprintf(found_path, (size_t)size, "%s/%s", folder, entry->d_name);
		found = is_kind(found_path) != 0;
	}
	closedir(directory);
	return found;
}

/* the first start, with no maps: the disc image on the stick, copied */
BOOL platform_offer_game_data(char *destination, unsigned long size)
{
	static const char *const image_extensions[] = { ".iso", ".xiso", NULL };
	char image[1100];
	char error[512];
	unsigned long long written = 0;

	(void)size;
	if (config_boolean("debug.hidden_window") || config_real("debug.exit_after") > 0.0)
		return FALSE;
	if (!SDL_Init(SDL_INIT_VIDEO))
		return FALSE;
	if (!stick_find(destination, image_extensions, xiso_is_image, image, sizeof(image)))
	{
		console_setup_notice("No Halo: Combat Evolved Xbox disc image found.",
			"Put your .iso on the root of a USB stick, plug it in, and start this again.");
		exit(EXIT_SUCCESS);
	}
	platform_log("importing maps from %s to %s", image, destination);
	if (!import_with_ring(PLATFORM_IMPORT_XBOX_DISC, image, destination, &written, error, sizeof(error)))
	{
		console_setup_notice("The maps could not be copied from the disc image:", error);
		exit(EXIT_SUCCESS);
	}
	platform_log("extracted the maps folder");
	return TRUE;
}

/* Custom Edition's maps, optional, from its installer on the stick beside the
disc image (whatever it is called: it is told by what it holds), once: they
are what its servers' maps are played with */
static void offer_custom_edition(const char *destination)
{
	static const char *const installer_extensions[] = { ".exe", NULL };
	char maps_ce[1100];
	char on_disk[256];
	char installer[1400];
	char error[512];
	unsigned long long written = 0;

	snprintf(maps_ce, sizeof(maps_ce), "%s/maps_ce", destination);
	if (posix_find_entry_case_insensitive(maps_ce, "bitmaps.map", on_disk, sizeof(on_disk)))
		return;
	if (!stick_find(destination, installer_extensions, ce_installer_is_installer, installer, sizeof(installer)))
		return;
	platform_log("importing Custom Edition's maps from %s to %s", installer, destination);
	if (import_with_ring(PLATFORM_IMPORT_CE_INSTALLER, installer, destination, &written, error, sizeof(error)))
		platform_log("imported %llu MB of maps into %s", written >> 20, maps_ce);
	else
		console_setup_notice("Custom Edition's maps could not be copied from its installer:", error);
}

void console_setup_after_data(const char *data_root)
{
	offer_custom_edition(data_root);
	updater_console_check();
}
