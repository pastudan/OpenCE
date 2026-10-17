/*
GUEST_SDL.C

The SDL3 functions the platform layer calls (sdl_platform.c, xinput_sdl.c,
dsound_sdl.c, voice_audio.c, menu_files.c), for the macOS build's guest
(port/macos/README.md). SDL runs in the host; what it hands back that is a
pointer there (a window, a gamepad, an audio stream, a renderer) is a small
handle here, which the host maps back (port/macos/host/host_sdl.c). Its
events have the same 128-byte layout on both sides (every field the
platform layer reads: checked by the build, tools/macos_build.py), so they
are copied as they are.

Strings and lists SDL returns are copied into guest memory: into a buffer
of the calling thread's where SDL keeps them (an error, a name), or into an
allocation the caller frees with SDL_free.

Each host_sdl_* function is an import of the guest (wasm2c's w2c_env_*),
whose host side is generated from these prototypes
(port/macos/guest/guest_host.h, tools/macos_imports.py).
*/

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "guest_host.h"

/* ---------- general */

bool SDL_Init(SDL_InitFlags flags)
{
	return host_sdl_init(flags) != 0;
}

bool SDL_SetHint(const char *name, const char *value)
{
	return host_sdl_set_hint(name, value) != 0;
}

bool SDL_GetHintBoolean(const char *name, bool default_value)
{
	return host_sdl_get_hint_boolean(name, default_value ? 1 : 0) != 0;
}

const char *SDL_GetError(void)
{
	static __thread char buffer[512];

	host_sdl_get_error(buffer, sizeof(buffer));
	return buffer;
}

void SDL_free(void *memory)
{
	free(memory);
}

Uint64 SDL_GetTicks(void)
{
	return (Uint64)host_sdl_ticks();
}

SDL_ThreadID SDL_GetCurrentThreadID(void)
{
	return (SDL_ThreadID)host_sdl_thread_id();
}

void SDL_Delay(Uint32 milliseconds)
{
	struct timespec duration;

	duration.tv_sec = milliseconds / 1000;
	duration.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
	nanosleep(&duration, NULL);
}

/* (the guest's own atomics: the memory is shared) */
int SDL_GetAtomicInt(SDL_AtomicInt *a)
{
	return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST);
}

int SDL_SetAtomicInt(SDL_AtomicInt *a, int v)
{
	return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST);
}

int SDL_AddAtomicInt(SDL_AtomicInt *a, int v)
{
	return __atomic_fetch_add(&a->value, v, __ATOMIC_SEQ_CST);
}

/* ---------- the keyboard's names */

const char *SDL_GetScancodeName(SDL_Scancode scancode)
{
	static __thread char buffer[64];

	host_sdl_scancode_name((int)scancode, buffer, sizeof(buffer));
	return buffer;
}

SDL_Scancode SDL_GetScancodeFromName(const char *name)
{
	return (SDL_Scancode)host_sdl_scancode_from_name(name);
}

/* ---------- the clipboard */

bool SDL_SetClipboardText(const char *text)
{
	return host_sdl_set_clipboard_text(text) != 0;
}

char *SDL_GetClipboardText(void)
{
	char *buffer = malloc(4096);

	if (!buffer)
		return NULL;
	host_sdl_get_clipboard_text(buffer, 4096);
	return buffer;
}

/* ---------- message boxes and dialogs */

bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *window)
{
	(void)window;
	return host_sdl_show_simple_message_box((unsigned int)flags, title, message) != 0;
}

/* the box's buttons as their texts (the host makes SDL's), with their ids
and flags in arrays of their own */
bool SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *button)
{
	unsigned int flags[8];
	int ids[8];
	const char *texts[8];
	int count = data->numbuttons < 8 ? data->numbuttons : 8;
	int index;

	for (index = 0; index < count; index++)
	{
		flags[index] = data->buttons[index].flags;
		ids[index] = data->buttons[index].buttonID;
		texts[index] = data->buttons[index].text;
	}
	return host_sdl_show_message_box((unsigned int)data->flags, data->title, data->message, count, flags, ids,
		texts, button) != 0;
}

/* (the host shows the dialog and answers through the callback, on this
thread, the next time SDL_PumpEvents or SDL_PollEvent is called) */
struct open_file_dialog
{
	SDL_DialogFileCallback callback;
	void *userdata;
};

static struct open_file_dialog open_file_dialog;

void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
	const SDL_DialogFileFilter *filters, int nfilters, const char *default_location, bool allow_many)
{
	const char *names[8], *patterns[8];
	int count = nfilters < 8 ? nfilters : 8;
	int index;

	(void)window;
	(void)allow_many;
	for (index = 0; index < count; index++)
	{
		names[index] = filters[index].name;
		patterns[index] = filters[index].pattern;
	}
	open_file_dialog.callback = callback;
	open_file_dialog.userdata = userdata;
	host_sdl_show_open_file_dialog(count, names, patterns, default_location);
}

/* the dialog's answer, once it has one */
static void open_file_dialog_poll(void)
{
	char path[1024];
	int state;
	const char *files[2];

	if (!open_file_dialog.callback)
		return;
	state = host_sdl_open_file_dialog_result(path, sizeof(path));
	if (!state)
		return;
	files[0] = state > 0 ? path : NULL;
	files[1] = NULL;
	{
		SDL_DialogFileCallback callback = open_file_dialog.callback;

		open_file_dialog.callback = NULL;
		callback(open_file_dialog.userdata, files, -1);
	}
}

/* ---------- text input (the on-screen keyboard's: Steam's) */

bool SDL_HasScreenKeyboardSupport(void)
{
	return false;
}

SDL_PropertiesID SDL_CreateProperties(void)
{
	return 1;
}

void SDL_DestroyProperties(SDL_PropertiesID props)
{
	(void)props;
}

bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value)
{
	(void)props; (void)name; (void)value;
	return true;
}

bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value)
{
	(void)props; (void)name; (void)value;
	return true;
}

bool SDL_StartTextInputWithProperties(SDL_Window *window, SDL_PropertiesID props)
{
	(void)props;
	return host_sdl_text_input((unsigned int)(uintptr_t)window, 1) != 0;
}

bool SDL_StopTextInput(SDL_Window *window)
{
	return host_sdl_text_input((unsigned int)(uintptr_t)window, 0) != 0;
}

bool SDL_TextInputActive(SDL_Window *window)
{
	return host_sdl_text_input((unsigned int)(uintptr_t)window, -1) != 0;
}

/* ---------- displays */

/* a display's modes, kept for the display's mode functions to return (as
SDL keeps its own) */
static __thread SDL_DisplayMode display_mode_scratch[4];

SDL_DisplayID SDL_GetPrimaryDisplay(void)
{
	return (SDL_DisplayID)host_sdl_primary_display();
}

const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID display)
{
	SDL_DisplayMode *mode = &display_mode_scratch[0];

	return host_sdl_display_mode(display, 0, mode) ? mode : NULL;
}

const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID display)
{
	SDL_DisplayMode *mode = &display_mode_scratch[1];

	return host_sdl_display_mode(display, 1, mode) ? mode : NULL;
}

bool SDL_GetDisplayUsableBounds(SDL_DisplayID display, SDL_Rect *rect)
{
	return host_sdl_display_usable_bounds(display, rect) != 0;
}

bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID display, int w, int h, float refresh_rate,
	bool include_high_density_modes, SDL_DisplayMode *closest)
{
	return host_sdl_closest_fullscreen_display_mode(display, w, h, refresh_rate,
		include_high_density_modes ? 1 : 0, closest) != 0;
}

/* a NULL-terminated array of pointers to modes, in one allocation */
SDL_DisplayMode **SDL_GetFullscreenDisplayModes(SDL_DisplayID display, int *count)
{
	SDL_DisplayMode modes[64];
	int found = host_sdl_fullscreen_display_modes(display, modes, 64);
	SDL_DisplayMode **result;
	SDL_DisplayMode *copies;
	int index;

	if (found < 0)
		found = 0;
	result = malloc((size_t)(found + 1) * sizeof(*result) + (size_t)found * sizeof(*copies));
	if (!result)
		return NULL;
	copies = (SDL_DisplayMode *)(result + found + 1);
	for (index = 0; index < found; index++)
	{
		copies[index] = modes[index];
		result[index] = &copies[index];
	}
	result[found] = NULL;
	if (count)
		*count = found;
	return result;
}

/* ---------- windows */

SDL_Window *SDL_CreateWindow(const char *title, int width, int height, SDL_WindowFlags flags)
{
	return (SDL_Window *)(uintptr_t)host_sdl_create_window(title, width, height, (long long)flags);
}

void SDL_DestroyWindow(SDL_Window *window)
{
	host_sdl_destroy_window((unsigned int)(uintptr_t)window);
}

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window)
{
	return (SDL_DisplayID)host_sdl_display_for_window((unsigned int)(uintptr_t)window);
}

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window)
{
	return (SDL_WindowFlags)host_sdl_window_flags((unsigned int)(uintptr_t)window);
}

bool SDL_GetWindowSize(SDL_Window *window, int *width, int *height)
{
	int w = 0, h = 0;
	int result = host_sdl_window_size((unsigned int)(uintptr_t)window, 0, &w, &h);

	if (width)
		*width = w;
	if (height)
		*height = h;
	return result != 0;
}

bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *width, int *height)
{
	int w = 0, h = 0;
	int result = host_sdl_window_size((unsigned int)(uintptr_t)window, 1, &w, &h);

	if (width)
		*width = w;
	if (height)
		*height = h;
	return result != 0;
}

bool SDL_SetWindowSize(SDL_Window *window, int width, int height)
{
	return host_sdl_set_window_size((unsigned int)(uintptr_t)window, width, height) != 0;
}

bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen)
{
	return host_sdl_set_window_fullscreen((unsigned int)(uintptr_t)window, fullscreen ? 1 : 0) != 0;
}

bool SDL_SetWindowFullscreenMode(SDL_Window *window, const SDL_DisplayMode *mode)
{
	return host_sdl_set_window_fullscreen_mode((unsigned int)(uintptr_t)window, mode) != 0;
}

bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled)
{
	return host_sdl_set_relative_mouse((unsigned int)(uintptr_t)window, enabled ? 1 : 0) != 0;
}

void SDL_WarpMouseInWindow(SDL_Window *window, float x, float y)
{
	host_sdl_warp_mouse((unsigned int)(uintptr_t)window, x, y);
}

/* (the renderer's own: the Metal device presents, port/macos/host/host_metal.m) */
bool SDL_GL_SetSwapInterval(int interval)
{
	host_metal_set_vsync(interval);
	return true;
}

/* ---------- the extraction's progress window (sdl_platform.c) */

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, const char *name)
{
	(void)name;
	return (SDL_Renderer *)(uintptr_t)host_sdl_create_renderer((unsigned int)(uintptr_t)window);
}

void SDL_DestroyRenderer(SDL_Renderer *renderer)
{
	host_sdl_destroy_renderer((unsigned int)(uintptr_t)renderer);
}

bool SDL_SetRenderVSync(SDL_Renderer *renderer, int vsync)
{
	return host_sdl_render_vsync((unsigned int)(uintptr_t)renderer, vsync) != 0;
}

bool SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	return host_sdl_render_color((unsigned int)(uintptr_t)renderer, r, g, b, a) != 0;
}

bool SDL_SetRenderScale(SDL_Renderer *renderer, float x, float y)
{
	return host_sdl_render_scale((unsigned int)(uintptr_t)renderer, x, y) != 0;
}

bool SDL_RenderClear(SDL_Renderer *renderer)
{
	return host_sdl_render_clear((unsigned int)(uintptr_t)renderer) != 0;
}

bool SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_FRect *rect)
{
	return host_sdl_render_fill_rect((unsigned int)(uintptr_t)renderer, rect) != 0;
}

bool SDL_RenderDebugText(SDL_Renderer *renderer, float x, float y, const char *text)
{
	return host_sdl_render_debug_text((unsigned int)(uintptr_t)renderer, x, y, text) != 0;
}

bool SDL_RenderPresent(SDL_Renderer *renderer)
{
	return host_sdl_render_present((unsigned int)(uintptr_t)renderer) != 0;
}

/* ---------- events */

bool SDL_PollEvent(SDL_Event *event)
{
	SDL_Event scratch;

	open_file_dialog_poll();
	return host_sdl_poll_event(event ? event : &scratch) != 0;
}

void SDL_PumpEvents(void)
{
	open_file_dialog_poll();
	host_sdl_pump_events();
}

bool SDL_PushEvent(SDL_Event *event)
{
	return host_sdl_push_event(event) != 0;
}

/* ---------- gamepads */

SDL_JoystickID *SDL_GetGamepads(int *count)
{
	unsigned int ids[16];
	int found = host_sdl_get_gamepads(ids, 16);
	SDL_JoystickID *result = malloc((size_t)(found + 1) * sizeof(SDL_JoystickID));
	int index;

	if (!result)
		return NULL;
	for (index = 0; index < found; index++)
		result[index] = ids[index];
	result[found] = 0;
	if (count)
		*count = found;
	return result;
}

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id)
{
	return (SDL_Gamepad *)(uintptr_t)host_sdl_open_gamepad(id);
}

SDL_Gamepad *SDL_GetGamepadFromID(SDL_JoystickID id)
{
	return (SDL_Gamepad *)(uintptr_t)host_sdl_gamepad_from_id(id);
}

void SDL_CloseGamepad(SDL_Gamepad *gamepad)
{
	host_sdl_close_gamepad((unsigned int)(uintptr_t)gamepad);
}

const char *SDL_GetGamepadName(SDL_Gamepad *gamepad)
{
	static __thread char buffer[128];

	host_sdl_gamepad_name((unsigned int)(uintptr_t)gamepad, buffer, sizeof(buffer));
	return buffer;
}

Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
	return (Sint16)host_sdl_gamepad_axis((unsigned int)(uintptr_t)gamepad, (int)axis);
}

bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
	return host_sdl_gamepad_button((unsigned int)(uintptr_t)gamepad, (int)button) != 0;
}

SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad)
{
	return (SDL_GamepadType)host_sdl_gamepad_type((unsigned int)(uintptr_t)gamepad);
}

bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low, Uint16 high, Uint32 milliseconds)
{
	return host_sdl_rumble_gamepad((unsigned int)(uintptr_t)gamepad, low, high, milliseconds) != 0;
}

/* ---------- audio

A stream with a callback (dsound_sdl.c's) has a guest thread of its own
here, which waits in the host for SDL's audio thread to ask for more, calls
the callback, and tells the host it is done: the host never calls into the
guest (port/macos/host/host_sdl.c). */

struct audio_binding
{
	SDL_AudioStream *stream;
	SDL_AudioStreamCallback callback;
	void *userdata;
};

static void *audio_thread(void *context)
{
	struct audio_binding *binding = context;
	unsigned int handle = (unsigned int)(uintptr_t)binding->stream;

	for (;;)
	{
		int additional = 0, total = 0;

		if (!host_sdl_audio_wait(handle, &additional, &total))
			break;
		binding->callback(binding->userdata, binding->stream, additional, total);
		host_sdl_audio_done(handle);
	}
	free(binding);
	return NULL;
}

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec *spec,
	SDL_AudioStreamCallback callback, void *userdata)
{
	unsigned int handle = host_sdl_open_audio_stream(device, spec, callback ? 1 : 0);
	struct audio_binding *binding;
	pthread_t thread;

	if (!handle || !callback)
		return (SDL_AudioStream *)(uintptr_t)handle;
	binding = malloc(sizeof(*binding));
	if (!binding)
	{
		host_sdl_destroy_audio_stream(handle);
		return NULL;
	}
	binding->stream = (SDL_AudioStream *)(uintptr_t)handle;
	binding->callback = callback;
	binding->userdata = userdata;
	if (pthread_create(&thread, NULL, audio_thread, binding) != 0)
	{
		free(binding);
		host_sdl_destroy_audio_stream(handle);
		return NULL;
	}
	pthread_detach(thread);
	return binding->stream;
}

bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *data, int length)
{
	return host_sdl_put_audio_stream_data((unsigned int)(uintptr_t)stream, data, length) != 0;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream)
{
	return host_sdl_resume_audio_stream_device((unsigned int)(uintptr_t)stream) != 0;
}

int SDL_GetAudioStreamData(SDL_AudioStream *stream, void *data, int length)
{
	return host_sdl_get_audio_stream_data((unsigned int)(uintptr_t)stream, data, length);
}

int SDL_GetAudioStreamAvailable(SDL_AudioStream *stream)
{
	return host_sdl_get_audio_stream_available((unsigned int)(uintptr_t)stream);
}

void SDL_DestroyAudioStream(SDL_AudioStream *stream)
{
	host_sdl_destroy_audio_stream((unsigned int)(uintptr_t)stream);
}

static SDL_AudioDeviceID *audio_devices(int recording, int *count)
{
	unsigned int ids[32];
	int found = host_sdl_audio_devices(recording, ids, 32);
	SDL_AudioDeviceID *result = malloc((size_t)(found + 1) * sizeof(SDL_AudioDeviceID));
	int index;

	if (!result)
		return NULL;
	for (index = 0; index < found; index++)
		result[index] = ids[index];
	result[found] = 0;
	if (count)
		*count = found;
	return result;
}

SDL_AudioDeviceID *SDL_GetAudioPlaybackDevices(int *count)
{
	return audio_devices(0, count);
}

SDL_AudioDeviceID *SDL_GetAudioRecordingDevices(int *count)
{
	return audio_devices(1, count);
}

const char *SDL_GetAudioDeviceName(SDL_AudioDeviceID device)
{
	static __thread char buffer[8][256];
	static __thread int next;
	char *name = buffer[next++ % 8];

	if (!host_sdl_audio_device_name(device, name, 256))
		return NULL;
	return name;
}

/* ---------- precise timing (sdl_platform.c's frame limiter) */

Uint64 SDL_GetTicksNS(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (Uint64)now.tv_sec * 1000000000ull + (Uint64)now.tv_nsec;
}

void SDL_DelayPrecise(Uint64 nanoseconds)
{
	struct timespec duration;

	duration.tv_sec = (time_t)(nanoseconds / 1000000000ull);
	duration.tv_nsec = (long)(nanoseconds % 1000000000ull);
	nanosleep(&duration, NULL);
}

/* ---------- what the macOS build has none of yet

The window's icon is the application bundle's (port/macos/Info.plist), and
the device's screenshots are its own (d3d8_gl.c's, for OpenGL; the Metal
device's are to come): these calls fail, as they would without the file. */

bool SDL_SetError(const char *format, ...)
{
	(void)format;
	return false;
}

SDL_IOStream *SDL_IOFromConstMem(const void *memory, size_t size)
{
	(void)memory;
	(void)size;
	return NULL;
}

SDL_Surface *SDL_LoadPNG_IO(SDL_IOStream *stream, bool close)
{
	(void)stream;
	(void)close;
	return NULL;
}

bool SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon)
{
	(void)window;
	(void)icon;
	return false;
}

SDL_Surface *SDL_CreateSurface(int width, int height, SDL_PixelFormat format)
{
	(void)width;
	(void)height;
	(void)format;
	return NULL;
}

void SDL_DestroySurface(SDL_Surface *surface)
{
	(void)surface;
}

bool SDL_SavePNG(SDL_Surface *surface, const char *file)
{
	(void)surface;
	(void)file;
	return false;
}

bool SDL_CreateDirectory(const char *path)
{
	(void)path;
	return false;
}

bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info)
{
	(void)path;
	(void)info;
	return false;
}

bool SDL_GetCurrentTime(SDL_Time *ticks)
{
	(void)ticks;
	return false;
}

bool SDL_TimeToDateTime(SDL_Time ticks, SDL_DateTime *date, bool local_time)
{
	(void)ticks;
	(void)date;
	(void)local_time;
	return false;
}

/* ---------- files read and written whole or by parts (the mods' and the
settings': port_mods.c, port_config.c), with the C library's stdio, which
wasi-libc gives the guest (paths are UTF-8 already) */

struct SDL_IOStream
{
	FILE *file;
};

SDL_IOStream *SDL_IOFromFile(const char *file, const char *mode)
{
	SDL_IOStream *stream;
	FILE *opened = file && mode ? fopen(file, mode) : NULL;

	if (!opened)
		return NULL;
	stream = malloc(sizeof(*stream));
	if (!stream)
	{
		fclose(opened);
		return NULL;
	}
	stream->file = opened;
	return stream;
}

Sint64 SDL_SeekIO(SDL_IOStream *context, Sint64 offset, SDL_IOWhence whence)
{
	int origin = whence == SDL_IO_SEEK_CUR ? SEEK_CUR : whence == SDL_IO_SEEK_END ? SEEK_END : SEEK_SET;

	if (!context || fseeko(context->file, (off_t)offset, origin) != 0)
		return -1;
	return (Sint64)ftello(context->file);
}

size_t SDL_ReadIO(SDL_IOStream *context, void *pointer, size_t size)
{
	return context ? fread(pointer, 1, size, context->file) : 0;
}

bool SDL_CloseIO(SDL_IOStream *context)
{
	int closed;

	if (!context)
		return false;
	closed = fclose(context->file);
	free(context);
	return closed == 0;
}

bool SDL_SaveFile(const char *file, const void *data, size_t datasize)
{
	FILE *out = fopen(file, "wb");
	bool written;

	if (!out)
		return false;
	written = fwrite(data, 1, datasize, out) == datasize;
	return fclose(out) == 0 && written;
}

bool SDL_RenamePath(const char *oldpath, const char *newpath)
{
	return rename(oldpath, newpath) == 0;
}

bool SDL_RemovePath(const char *path)
{
	return remove(path) == 0;
}

/* ---------- files (the menus folder's: menu_files.c) */

/* the names in a folder (and its folders, for a pattern with a "/") that
match a pattern, in one allocation: the array, NULL-terminated, then the
names */
char **SDL_GlobDirectory(const char *path, const char *pattern, SDL_GlobFlags flags, int *count)
{
	static char names[16384];
	int found = host_sdl_glob_directory(path, pattern, (unsigned int)flags, names, sizeof(names));
	char **result, *text;
	size_t length = 0;
	int index;

	if (found < 0)
		return NULL;
	for (index = 0, text = names; index < found; index++)
	{
		length += strlen(text) + 1;
		text += strlen(text) + 1;
	}
	result = malloc((size_t)(found + 1) * sizeof(char *) + length);
	if (!result)
		return NULL;
	text = (char *)(result + found + 1);
	memcpy(text, names, length);
	for (index = 0; index < found; index++)
	{
		result[index] = text;
		text += strlen(text) + 1;
	}
	result[found] = NULL;
	if (count)
		*count = found;
	return result;
}
