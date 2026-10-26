/*
 * the probe: what the steam link gives SDL3, before any of the game. its
 * video drivers, a full screen OpenGL ES 2.0 window, the GL driver and its
 * extensions, the screen cycling colour, and every controller and button.
 * start + back (or a minute) quits
 *
 * the log: probe.log next to the program, and standard output
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdarg.h>
#include <stdio.h>

/* the few GL ES 2.0 names it needs, without the headers (SDL loads GL) */
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_COLOR_BUFFER_BIT 0x00004000

typedef const unsigned char *(*gl_get_string_t)(unsigned int name);
typedef void (*gl_get_integerv_t)(unsigned int name, int *value);
typedef void (*gl_clear_color_t)(float r, float g, float b, float a);
typedef void (*gl_clear_t)(unsigned int mask);
typedef void (*gl_viewport_t)(int x, int y, int w, int h);

static FILE *probe_log_file;

static void probe_log(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	putchar('\n');
	fflush(stdout);
	if (probe_log_file) {
		va_start(args, format);
		vfprintf(probe_log_file, format, args);
		va_end(args);
		fputc('\n', probe_log_file);
		fflush(probe_log_file);
	}
}

/* next to the program, or /tmp when the app's folder can't be written */
static void probe_open_log(void)
{
	char path[1024];
	const char *base = SDL_GetBasePath();
	SDL_snprintf(path, sizeof(path), "%sprobe.log", base ? base : "");
	probe_log_file = fopen(path, "w");
	if (!probe_log_file)
		probe_log_file = fopen("/tmp/chupathingyce-probe.log", "w");
}

static void *probe_gl(const char *name)
{
	void *function = (void *)SDL_GL_GetProcAddress(name);
	if (!function)
		probe_log("gl: no %s", name);
	return function;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	probe_open_log();

	int version = SDL_GetVersion();
	probe_log("sdl %d.%d.%d (%s)", SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version),
		SDL_VERSIONNUM_MICRO(version), SDL_GetRevision());
	probe_log("platform %s, %d cpu, %d MB ram, neon %s", SDL_GetPlatform(), SDL_GetNumLogicalCPUCores(),
		SDL_GetSystemRAM(), SDL_HasNEON() ? "yes" : "no");

	for (int i = 0; i < SDL_GetNumVideoDrivers(); i++)
		probe_log("video driver available: %s", SDL_GetVideoDriver(i));

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
		probe_log("SDL_Init failed: %s", SDL_GetError());
		return 1;
	}
	probe_log("video driver in use: %s", SDL_GetCurrentVideoDriver());

	/* ES 2.0: all the box's GPU has (the game's own GL path is ES 3.2) */
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

	SDL_Window *window = SDL_CreateWindow("ChupathingyCE probe", 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
	if (!window) {
		probe_log("SDL_CreateWindow failed: %s", SDL_GetError());
		SDL_Quit();
		return 2;
	}
	SDL_GLContext context = SDL_GL_CreateContext(window);
	if (!context) {
		probe_log("SDL_GL_CreateContext failed: %s", SDL_GetError());
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 3;
	}
	SDL_GL_SetSwapInterval(1);

	gl_get_string_t gl_get_string = (gl_get_string_t)probe_gl("glGetString");
	gl_get_integerv_t gl_get_integerv = (gl_get_integerv_t)probe_gl("glGetIntegerv");
	gl_clear_color_t gl_clear_color = (gl_clear_color_t)probe_gl("glClearColor");
	gl_clear_t gl_clear = (gl_clear_t)probe_gl("glClear");
	gl_viewport_t gl_viewport = (gl_viewport_t)probe_gl("glViewport");
	if (!gl_get_string || !gl_get_integerv || !gl_clear_color || !gl_clear || !gl_viewport) {
		SDL_Quit();
		return 4;
	}

	/* what a GLES2 renderer for the game would have to work with */
	probe_log("gl vendor: %s", gl_get_string(GL_VENDOR));
	probe_log("gl renderer: %s", gl_get_string(GL_RENDERER));
	probe_log("gl version: %s", gl_get_string(GL_VERSION));
	probe_log("glsl version: %s", gl_get_string(GL_SHADING_LANGUAGE_VERSION));
	int max_texture_size = 0;
	gl_get_integerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
	probe_log("gl max texture size: %d", max_texture_size);
	probe_log("gl extensions: %s", gl_get_string(GL_EXTENSIONS));

	int width = 0, height = 0;
	SDL_GetWindowSizeInPixels(window, &width, &height);
	probe_log("window: %dx%d pixels", width, height);
	gl_viewport(0, 0, width, height);

	Uint64 start = SDL_GetTicks();
	Uint64 frames = 0;
	bool running = true;
	while (running && SDL_GetTicks() - start < 60000) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			switch (event.type) {
			case SDL_EVENT_QUIT:
				running = false;
				break;
			case SDL_EVENT_GAMEPAD_ADDED: {
				SDL_Gamepad *gamepad = SDL_OpenGamepad(event.gdevice.which);
				probe_log("controller added: %s", gamepad ? SDL_GetGamepadName(gamepad) : SDL_GetError());
				break;
			}
			case SDL_EVENT_GAMEPAD_REMOVED:
				probe_log("controller removed");
				break;
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
				probe_log("button: %s", SDL_GetGamepadStringForButton((SDL_GamepadButton)event.gbutton.button));
				SDL_Gamepad *gamepad = SDL_GetGamepadFromID(event.gbutton.which);
				if (gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START) &&
					SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK))
					running = false;
				break;
			}
			}
		}

		/* the colour cycling: a frame the eye can see is a working swap */
		float t = (float)(SDL_GetTicks() - start) / 1000.0f;
		gl_clear_color(0.5f + 0.5f * SDL_sinf(t), 0.5f + 0.5f * SDL_sinf(t + 2.1f), 0.5f + 0.5f * SDL_sinf(t + 4.2f), 1.0f);
		gl_clear(GL_COLOR_BUFFER_BIT);
		SDL_GL_SwapWindow(window);
		frames++;
	}

	Uint64 elapsed = SDL_GetTicks() - start;
	probe_log("%llu frames in %llu ms (%.1f fps)", (unsigned long long)frames, (unsigned long long)elapsed,
		elapsed ? frames * 1000.0 / elapsed : 0.0);

	SDL_GL_DestroyContext(context);
	SDL_DestroyWindow(window);
	SDL_Quit();
	if (probe_log_file)
		fclose(probe_log_file);
	return 0;
}
