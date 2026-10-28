/*
STEAMLINK_PERFORMANCE_SWITCH.C

Performance mode's switch (steamlink_performance.c): Back held and down on
the d-pad pressed on any controller, or F9 pressed, once for each press.
*/

#include <SDL3/SDL.h>

void console_performance_update(void);
void steamlink_performance_update(int switched);

static int performance_switch_pressed(void)
{
	static int was_down;
	const bool *keys = SDL_GetKeyboardState(NULL);
	int down = keys && keys[SDL_SCANCODE_F9];
	SDL_JoystickID *pads;
	int count = 0, index, pressed;

	pads = SDL_GetGamepads(&count);
	for (index = 0; pads && index < count && !down; index++)
	{
		/* (the controllers xinput_sdl.c opened) */
		SDL_Gamepad *pad = SDL_GetGamepadFromID(pads[index]);

		if (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
			SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
		{
			down = 1;
		}
	}
	SDL_free(pads);
	pressed = down && !was_down;
	was_down = down;
	return pressed;
}

void console_performance_update(void)
{
	steamlink_performance_update(performance_switch_pressed());
}
