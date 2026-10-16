/*
HOST_SDL.C

SDL3 on behalf of the macOS build's guest (port/macos/guest/guest_sdl.c;
port/macos/guest/guest_host.h has each function's contract). SDL's objects
are 64-bit pointers, which the guest cannot hold: it gets small handles into
the table here instead. The guest's pointers have been made host ones
already (the generated build/macos/host_imports.c).

The host never calls the guest. An audio stream's callback (SDL's audio
thread asking for more) is answered by a guest thread that waits here
(host_sdl_audio_wait) and says when it is done (host_sdl_audio_done); what
it puts into the stream meanwhile is kept and put in by the callback, which
holds the stream's lock throughout.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <string.h>

#define HANDLE_COUNT 256

enum handle_type
{
	_handle_free,
	_handle_window,
	_handle_renderer,
	_handle_gamepad,
	_handle_audio,
};

struct handle
{
	int type;
	void *object;
	/* an audio stream with a callback: its binding (below) */
	void *binding;
};

static struct handle handles[HANDLE_COUNT];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t handle_new(int type, void *object)
{
	uint32_t index;

	if (!object)
		return 0;
	pthread_mutex_lock(&handle_lock);
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == type && handles[index].object == object)
		{
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == _handle_free)
		{
			handles[index].type = type;
			handles[index].object = object;
			handles[index].binding = NULL;
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	pthread_mutex_unlock(&handle_lock);
	host_log("out of SDL handles");
	return 0;
}

static void *handle_get(uint32_t handle, int type)
{
	void *object = NULL;

	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
		object = handles[handle].object;
	pthread_mutex_unlock(&handle_lock);
	return object;
}

static void *handle_release(uint32_t handle, int type, void **binding)
{
	void *object = NULL;

	if (binding)
		*binding = NULL;
	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
	{
		object = handles[handle].object;
		if (binding)
			*binding = handles[handle].binding;
		handles[handle].type = _handle_free;
		handles[handle].object = NULL;
		handles[handle].binding = NULL;
	}
	pthread_mutex_unlock(&handle_lock);
	return object;
}

/* ---------- general */

void host_sdl_main_thread_set(void)
{
	/* (the game's main runs on the process's main thread: host_main.c) */
	SDL_SetHint(SDL_HINT_APP_NAME, "Halo");
}

int host_sdl_init(unsigned int flags)
{
	return SDL_Init((SDL_InitFlags)flags) ? 1 : 0;
}

int host_sdl_set_hint(const char *name, const char *value)
{
	return SDL_SetHint(name, value) ? 1 : 0;
}

int host_sdl_get_hint_boolean(const char *name, int default_value)
{
	return SDL_GetHintBoolean(name, default_value != 0) ? 1 : 0;
}

void host_sdl_get_error(char *buffer, unsigned int size)
{
	SDL_strlcpy(buffer, SDL_GetError(), size);
}

long long host_sdl_ticks(void)
{
	return (long long)SDL_GetTicks();
}

long long host_sdl_thread_id(void)
{
	return (long long)SDL_GetCurrentThreadID();
}

void host_sdl_scancode_name(int scancode, char *buffer, unsigned int size)
{
	SDL_strlcpy(buffer, SDL_GetScancodeName((SDL_Scancode)scancode), size);
}

int host_sdl_scancode_from_name(const char *name)
{
	return (int)SDL_GetScancodeFromName(name);
}

int host_sdl_set_clipboard_text(const char *text)
{
	return SDL_SetClipboardText(text) ? 1 : 0;
}

void host_sdl_get_clipboard_text(char *buffer, unsigned int size)
{
	char *text = SDL_GetClipboardText();

	SDL_strlcpy(buffer, text ? text : "", size);
	SDL_free(text);
}

/* ---------- message boxes and dialogs */

int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message)
{
	return SDL_ShowSimpleMessageBox((SDL_MessageBoxFlags)flags, title, message, NULL) ? 1 : 0;
}

int host_sdl_show_message_box(unsigned int flags, const char *title, const char *message, int count,
	const unsigned int *button_flags, const int *button_ids, const uint32_t *button_texts, int *button)
{
	SDL_MessageBoxButtonData buttons[8];
	SDL_MessageBoxData data;
	int index;

	if (count > 8)
		count = 8;
	for (index = 0; index < count; index++)
	{
		buttons[index].flags = (SDL_MessageBoxButtonFlags)button_flags[index];
		buttons[index].buttonID = button_ids[index];
		buttons[index].text = HOST_POINTER(button_texts[index]);
	}
	memset(&data, 0, sizeof(data));
	data.flags = (SDL_MessageBoxFlags)flags;
	data.title = title;
	data.message = message;
	data.numbuttons = count;
	data.buttons = buttons;
	return SDL_ShowMessageBox(&data, button) ? 1 : 0;
}

/* the open file dialog's answer: 0 while it is open, 1 with path, -1 none */
static pthread_mutex_t dialog_lock = PTHREAD_MUTEX_INITIALIZER;
static int dialog_state;
static char dialog_path[1024];
static SDL_DialogFileFilter dialog_filters[8];
static char dialog_filter_text[8][2][128];

static void SDLCALL dialog_chosen(void *userdata, const char *const *files, int filter)
{
	(void)userdata;
	(void)filter;
	pthread_mutex_lock(&dialog_lock);
	if (files && files[0])
	{
		SDL_strlcpy(dialog_path, files[0], sizeof(dialog_path));
		dialog_state = 1;
	}
	else
	{
		dialog_state = -1;
	}
	pthread_mutex_unlock(&dialog_lock);
}

void host_sdl_show_open_file_dialog(int count, const uint32_t *names, const uint32_t *patterns,
	const char *default_location)
{
	int index;

	if (count > 8)
		count = 8;
	for (index = 0; index < count; index++)
	{
		SDL_strlcpy(dialog_filter_text[index][0], HOST_POINTER(names[index]), sizeof(dialog_filter_text[index][0]));
		SDL_strlcpy(dialog_filter_text[index][1], HOST_POINTER(patterns[index]),
			sizeof(dialog_filter_text[index][1]));
		dialog_filters[index].name = dialog_filter_text[index][0];
		dialog_filters[index].pattern = dialog_filter_text[index][1];
	}
	pthread_mutex_lock(&dialog_lock);
	dialog_state = 0;
	pthread_mutex_unlock(&dialog_lock);
	SDL_ShowOpenFileDialog(dialog_chosen, NULL, NULL, dialog_filters, count, default_location, false);
}

int host_sdl_open_file_dialog_result(char *path, unsigned int size)
{
	int state;

	pthread_mutex_lock(&dialog_lock);
	state = dialog_state;
	if (state > 0)
		SDL_strlcpy(path, dialog_path, size);
	if (state)
		dialog_state = 0;
	pthread_mutex_unlock(&dialog_lock);
	return state;
}

int host_sdl_text_input(unsigned int window, int start)
{
	SDL_Window *object = handle_get(window, _handle_window);

	if (!object)
		return 0;
	if (start < 0)
		return SDL_TextInputActive(object) ? 1 : 0;
	return (start ? SDL_StartTextInput(object) : SDL_StopTextInput(object)) ? 1 : 0;
}

/* ---------- displays */

unsigned int host_sdl_primary_display(void)
{
	return (unsigned int)SDL_GetPrimaryDisplay();
}

int host_sdl_display_mode(unsigned int display, int current, void *mode)
{
	const SDL_DisplayMode *found = current ? SDL_GetCurrentDisplayMode((SDL_DisplayID)display) :
		SDL_GetDesktopDisplayMode((SDL_DisplayID)display);

	if (!found)
		return 0;
	memcpy(mode, found, sizeof(*found));
	/* (its internal pointer means nothing to the guest) */
	((SDL_DisplayMode *)mode)->internal = NULL;
	return 1;
}

int host_sdl_display_usable_bounds(unsigned int display, void *rect)
{
	return SDL_GetDisplayUsableBounds((SDL_DisplayID)display, rect) ? 1 : 0;
}

int host_sdl_closest_fullscreen_display_mode(unsigned int display, int width, int height, float refresh_rate,
	int include_high_density_modes, void *mode)
{
	SDL_DisplayMode closest;

	if (!SDL_GetClosestFullscreenDisplayMode((SDL_DisplayID)display, width, height, refresh_rate,
		include_high_density_modes != 0, &closest))
	{
		return 0;
	}
	closest.internal = NULL;
	memcpy(mode, &closest, sizeof(closest));
	return 1;
}

int host_sdl_fullscreen_display_modes(unsigned int display, void *modes, int capacity)
{
	int count = 0, index;
	SDL_DisplayMode **list = SDL_GetFullscreenDisplayModes((SDL_DisplayID)display, &count);

	if (!list)
		return 0;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
	{
		SDL_DisplayMode mode = *list[index];

		mode.internal = NULL;
		memcpy((SDL_DisplayMode *)modes + index, &mode, sizeof(mode));
	}
	SDL_free(list);
	return count;
}

/* ---------- windows */

void host_metal_window_created(SDL_Window *window);
void host_metal_window_destroyed(SDL_Window *window);

/* (host_metal.m) */
SDL_Window *host_sdl_window(unsigned int window)
{
	return handle_get(window, _handle_window);
}

unsigned int host_sdl_create_window(const char *title, int width, int height, long long flags)
{
	/* (the game asks for OpenGL's window; Metal draws in this one) */
	SDL_WindowFlags window_flags = ((SDL_WindowFlags)flags & ~SDL_WINDOW_OPENGL) | SDL_WINDOW_METAL;
	SDL_Window *window = SDL_CreateWindow(title, width, height, window_flags);

	if (window)
		host_metal_window_created(window);
	return handle_new(_handle_window, window);
}

void host_sdl_destroy_window(unsigned int window)
{
	SDL_Window *object = handle_release(window, _handle_window, NULL);

	if (object)
	{
		host_metal_window_destroyed(object);
		SDL_DestroyWindow(object);
	}
}

unsigned int host_sdl_display_for_window(unsigned int window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? (unsigned int)SDL_GetDisplayForWindow(object) : 0;
}

long long host_sdl_window_flags(unsigned int window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? (long long)SDL_GetWindowFlags(object) : 0;
}

int host_sdl_window_size(unsigned int window, int pixels, int *width, int *height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	*width = 0;
	*height = 0;
	if (!object)
		return 0;
	return (pixels ? SDL_GetWindowSizeInPixels(object, width, height) : SDL_GetWindowSize(object, width, height)) ?
		1 : 0;
}

int host_sdl_set_window_size(unsigned int window, int width, int height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object && SDL_SetWindowSize(object, width, height) ? 1 : 0;
}

int host_sdl_set_window_fullscreen(unsigned int window, int fullscreen)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object && SDL_SetWindowFullscreen(object, fullscreen != 0) ? 1 : 0;
}

int host_sdl_set_window_fullscreen_mode(unsigned int window, const void *mode)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_DisplayMode closest;
	const SDL_DisplayMode *wanted = mode;

	if (!object)
		return 0;
	if (!wanted)
		return SDL_SetWindowFullscreenMode(object, NULL) ? 1 : 0;
	/* (the guest's copy has no internal pointer: SDL's own mode of that
	size and rate) */
	if (!SDL_GetClosestFullscreenDisplayMode(wanted->displayID, wanted->w, wanted->h, wanted->refresh_rate,
		wanted->pixel_density > 1.0f, &closest))
	{
		return 0;
	}
	return SDL_SetWindowFullscreenMode(object, &closest) ? 1 : 0;
}

int host_sdl_set_relative_mouse(unsigned int window, int enabled)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object && SDL_SetWindowRelativeMouseMode(object, enabled != 0) ? 1 : 0;
}

void host_sdl_warp_mouse(unsigned int window, float x, float y)
{
	SDL_Window *object = handle_get(window, _handle_window);

	if (object)
		SDL_WarpMouseInWindow(object, x, y);
}

/* ---------- the extraction's progress window */

unsigned int host_sdl_create_renderer(unsigned int window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? handle_new(_handle_renderer, SDL_CreateRenderer(object, NULL)) : 0;
}

void host_sdl_destroy_renderer(unsigned int renderer)
{
	SDL_Renderer *object = handle_release(renderer, _handle_renderer, NULL);

	if (object)
		SDL_DestroyRenderer(object);
}

int host_sdl_render_vsync(unsigned int renderer, int vsync)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object && SDL_SetRenderVSync(object, vsync) ? 1 : 0;
}

int host_sdl_render_color(unsigned int renderer, int r, int g, int b, int a)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object && SDL_SetRenderDrawColor(object, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a) ? 1 : 0;
}

int host_sdl_render_scale(unsigned int renderer, float x, float y)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object && SDL_SetRenderScale(object, x, y) ? 1 : 0;
}

int host_sdl_render_clear(unsigned int renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object && SDL_RenderClear(object) ? 1 : 0;
}

int host_sdl_render_fill_rect(unsigned int renderer, const void *rect)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object && SDL_RenderFillRect(object, rect) ? 1 : 0;
}

int host_sdl_render_debug_text(unsigned int renderer, float x, float y, const char *text)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object && SDL_RenderDebugText(object, x, y, text) ? 1 : 0;
}

int host_sdl_render_present(unsigned int renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object && SDL_RenderPresent(object) ? 1 : 0;
}

/* ---------- events */

/* the event, with its window as the guest knows it: the layouts agree,
and the platform layer reads no pointer of one (text, drop and user events
carry them; the guest does not read those) */
int host_sdl_poll_event(void *event)
{
	SDL_Event host_event;

	if (!SDL_PollEvent(&host_event))
		return 0;
	memcpy(event, &host_event, sizeof(host_event));
	return 1;
}

void host_sdl_pump_events(void)
{
	SDL_PumpEvents();
}

int host_sdl_push_event(const void *event)
{
	SDL_Event host_event;

	memcpy(&host_event, event, sizeof(host_event));
	return SDL_PushEvent(&host_event) ? 1 : 0;
}

/* ---------- gamepads */

int host_sdl_get_gamepads(unsigned int *ids, int capacity)
{
	int count = 0, index;
	SDL_JoystickID *list = SDL_GetGamepads(&count);

	if (!list)
		return 0;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		ids[index] = list[index];
	SDL_free(list);
	return count;
}

unsigned int host_sdl_open_gamepad(unsigned int id)
{
	return handle_new(_handle_gamepad, SDL_OpenGamepad((SDL_JoystickID)id));
}

unsigned int host_sdl_gamepad_from_id(unsigned int id)
{
	return handle_new(_handle_gamepad, SDL_GetGamepadFromID((SDL_JoystickID)id));
}

void host_sdl_close_gamepad(unsigned int gamepad)
{
	SDL_Gamepad *object = handle_release(gamepad, _handle_gamepad, NULL);

	if (object)
		SDL_CloseGamepad(object);
}

void host_sdl_gamepad_name(unsigned int gamepad, char *buffer, unsigned int size)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);
	const char *name = object ? SDL_GetGamepadName(object) : NULL;

	SDL_strlcpy(buffer, name ? name : "", size);
}

int host_sdl_gamepad_axis(unsigned int gamepad, int axis)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadAxis(object, (SDL_GamepadAxis)axis) : 0;
}

int host_sdl_gamepad_button(unsigned int gamepad, int button)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object && SDL_GetGamepadButton(object, (SDL_GamepadButton)button) ? 1 : 0;
}

int host_sdl_gamepad_type(unsigned int gamepad)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? (int)SDL_GetGamepadType(object) : (int)SDL_GAMEPAD_TYPE_UNKNOWN;
}

int host_sdl_rumble_gamepad(unsigned int gamepad, unsigned int low, unsigned int high, unsigned int milliseconds)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object && SDL_RumbleGamepad(object, (Uint16)low, (Uint16)high, milliseconds) ? 1 : 0;
}

/* ---------- audio */

struct audio_binding
{
	pthread_mutex_t lock;
	pthread_cond_t requested;
	pthread_cond_t done;
	int pending;
	int quit;
	int additional;
	int total;
	/* what the guest put into the stream during the callback */
	unsigned char *buffer;
	int buffer_length;
	int buffer_size;
};

/* the binding whose callback this thread is answering, if any */
static _Thread_local struct audio_binding *answering;

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
	struct audio_binding *binding = userdata;

	pthread_mutex_lock(&binding->lock);
	if (binding->quit)
	{
		pthread_mutex_unlock(&binding->lock);
		return;
	}
	binding->additional = additional;
	binding->total = total;
	binding->pending = 1;
	pthread_cond_signal(&binding->requested);
	while (binding->pending && !binding->quit)
		pthread_cond_wait(&binding->done, &binding->lock);
	if (binding->buffer_length)
	{
		SDL_PutAudioStreamData(stream, binding->buffer, binding->buffer_length);
		binding->buffer_length = 0;
	}
	pthread_mutex_unlock(&binding->lock);
}

unsigned int host_sdl_open_audio_stream(unsigned int device, const void *spec, int with_callback)
{
	struct audio_binding *binding;
	SDL_AudioStream *stream;
	uint32_t handle;

	if (!with_callback)
	{
		stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, spec, NULL, NULL);
		return stream ? handle_new(_handle_audio, stream) : 0;
	}
	binding = SDL_calloc(1, sizeof(*binding));
	if (!binding)
		return 0;
	pthread_mutex_init(&binding->lock, NULL);
	pthread_cond_init(&binding->requested, NULL);
	pthread_cond_init(&binding->done, NULL);
	/* (the device starts paused: no callback before the guest's thread
	waits for one) */
	stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, spec, audio_callback, binding);
	if (!stream)
	{
		SDL_free(binding);
		return 0;
	}
	handle = handle_new(_handle_audio, stream);
	if (!handle)
	{
		SDL_DestroyAudioStream(stream);
		SDL_free(binding);
		return 0;
	}
	pthread_mutex_lock(&handle_lock);
	handles[handle].binding = binding;
	pthread_mutex_unlock(&handle_lock);
	return handle;
}

static struct audio_binding *audio_binding(uint32_t handle)
{
	struct audio_binding *binding = NULL;

	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == _handle_audio)
		binding = handles[handle].binding;
	pthread_mutex_unlock(&handle_lock);
	return binding;
}

int host_sdl_audio_wait(unsigned int stream, int *additional, int *total)
{
	struct audio_binding *binding = audio_binding(stream);

	if (!binding)
		return 0;
	pthread_mutex_lock(&binding->lock);
	while (!binding->pending && !binding->quit)
		pthread_cond_wait(&binding->requested, &binding->lock);
	if (binding->quit)
	{
		pthread_mutex_unlock(&binding->lock);
		return 0;
	}
	*additional = binding->additional;
	*total = binding->total;
	pthread_mutex_unlock(&binding->lock);
	answering = binding;
	return 1;
}

void host_sdl_audio_done(unsigned int stream)
{
	struct audio_binding *binding = audio_binding(stream);

	answering = NULL;
	if (!binding)
		return;
	pthread_mutex_lock(&binding->lock);
	binding->pending = 0;
	pthread_cond_signal(&binding->done);
	pthread_mutex_unlock(&binding->lock);
}

/* (on the answering thread, the stream's lock is held by SDL's callback:
kept for it) */
static int audio_keep(struct audio_binding *binding, const void *data, int length)
{
	if (length <= 0)
		return 1;
	if (binding->buffer_length + length > binding->buffer_size)
	{
		int size = (binding->buffer_length + length) * 2;
		unsigned char *buffer = SDL_realloc(binding->buffer, (size_t)size);

		if (!buffer)
			return 0;
		binding->buffer = buffer;
		binding->buffer_size = size;
	}
	memcpy(binding->buffer + binding->buffer_length, data, (size_t)length);
	binding->buffer_length += length;
	return 1;
}

int host_sdl_put_audio_stream_data(unsigned int stream, const void *data, int length)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);
	struct audio_binding *binding;

	if (!object)
		return 0;
	binding = audio_binding(stream);
	if (binding && answering == binding)
		return audio_keep(binding, data, length);
	return SDL_PutAudioStreamData(object, data, length) ? 1 : 0;
}

int host_sdl_resume_audio_stream_device(unsigned int stream)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object && SDL_ResumeAudioStreamDevice(object) ? 1 : 0;
}

int host_sdl_get_audio_stream_data(unsigned int stream, void *data, int length)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_GetAudioStreamData(object, data, length) : -1;
}

int host_sdl_get_audio_stream_available(unsigned int stream)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_GetAudioStreamAvailable(object) : -1;
}

void host_sdl_destroy_audio_stream(unsigned int stream)
{
	struct audio_binding *binding = audio_binding(stream);
	SDL_AudioStream *object;
	void *ignored;

	/* the guest's thread is let go first (a callback waiting on it gives
	up), then the stream: its destruction waits for a callback running */
	if (binding)
	{
		pthread_mutex_lock(&binding->lock);
		binding->quit = 1;
		pthread_cond_broadcast(&binding->requested);
		pthread_cond_broadcast(&binding->done);
		pthread_mutex_unlock(&binding->lock);
	}
	object = handle_release(stream, _handle_audio, &ignored);
	if (object)
		SDL_DestroyAudioStream(object);
	/* (the binding is left: the guest's thread may still be returning from
	host_sdl_audio_wait; a few bytes a stream, and the game opens one) */
}

int host_sdl_audio_devices(int recording, unsigned int *ids, int capacity)
{
	int count = 0, index;
	SDL_AudioDeviceID *list = recording ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);

	if (!list)
		return 0;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		ids[index] = list[index];
	SDL_free(list);
	return count;
}

int host_sdl_audio_device_name(unsigned int device, char *buffer, unsigned int size)
{
	const char *name = SDL_GetAudioDeviceName((SDL_AudioDeviceID)device);

	if (!name)
		return 0;
	SDL_strlcpy(buffer, name, size);
	return 1;
}

/* ---------- files */

int host_sdl_glob_directory(const char *path, const char *pattern, unsigned int flags, char *names,
	unsigned int size)
{
	int count = 0, index, written = 0;
	unsigned int used = 0;
	char **list = SDL_GlobDirectory(path, pattern, (SDL_GlobFlags)flags, &count);

	if (!list)
		return -1;
	for (index = 0; index < count; index++)
	{
		size_t length = strlen(list[index]) + 1;

		if (used + length > size)
			break;
		memcpy(names + used, list[index], length);
		used += (unsigned int)length;
		written++;
	}
	SDL_free(list);
	return written;
}
