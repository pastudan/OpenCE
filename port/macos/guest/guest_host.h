/*
GUEST_HOST.H

The host functions the macOS build's guest imports (port/macos/README.md):
the guest's view. Each is an import of the WebAssembly module "env", which
wasm2c declares for the host as w2c_env_<name>; tools/macos_imports.py reads
the prototypes here and writes the host's side (build/macos/host_imports.c),
turning each pointer (an offset into the guest's memory) into a host one and
calling the host function of the same name (port/macos/host).

The types are WebAssembly's: int and unsigned int (and pointers) are i32,
long long i64, float f32, double f64. Window, renderer, gamepad and audio
stream objects are small integer handles.
*/

#ifndef __HALO_MACOS_GUEST_HOST_H
#define __HALO_MACOS_GUEST_HOST_H

struct metal_vertex_layout;
struct metal_clear;
struct metal_draw;

/* ---------- process */

/* errno, as the guest numbers it, after the last posix_* call on this
thread (port/macos/guest/guest_posix.c) */
int host_errno(void);

/* ---------- SDL (port/macos/guest/guest_sdl.c, port/macos/host/host_sdl.c) */

int host_sdl_init(unsigned int flags);
int host_sdl_set_hint(const char *name, const char *value);
int host_sdl_get_hint_boolean(const char *name, int default_value);
void host_sdl_get_error(char *buffer, unsigned int size);
long long host_sdl_ticks(void);
long long host_sdl_thread_id(void);
void host_sdl_scancode_name(int scancode, char *buffer, unsigned int size);
int host_sdl_scancode_from_name(const char *name);
int host_sdl_set_clipboard_text(const char *text);
void host_sdl_get_clipboard_text(char *buffer, unsigned int size);
int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message);
/* button flags, ids and texts in arrays of count; the id pressed into *button */
int host_sdl_show_message_box(unsigned int flags, const char *title, const char *message, int count,
	const unsigned int *button_flags, const int *button_ids, const char *const *button_texts, int *button);
/* filter names and patterns in arrays of count */
void host_sdl_show_open_file_dialog(int count, const char *const *names, const char *const *patterns,
	const char *default_location);
/* 0 while the dialog is open; 1 with the path chosen, -1 for none */
int host_sdl_open_file_dialog_result(char *path, unsigned int size);
/* start (1), stop (0) or ask (-1): whether text input is on */
int host_sdl_text_input(unsigned int window, int start);

unsigned int host_sdl_primary_display(void);
/* the display's desktop (0) or current (1) mode, an SDL_DisplayMode */
int host_sdl_display_mode(unsigned int display, int current, void *mode);
int host_sdl_display_usable_bounds(unsigned int display, void *rect);
int host_sdl_closest_fullscreen_display_mode(unsigned int display, int width, int height, float refresh_rate,
	int include_high_density_modes, void *mode);
/* up to capacity SDL_DisplayModes into modes; how many */
int host_sdl_fullscreen_display_modes(unsigned int display, void *modes, int capacity);

unsigned int host_sdl_create_window(const char *title, int width, int height, long long flags);
void host_sdl_destroy_window(unsigned int window);
unsigned int host_sdl_display_for_window(unsigned int window);
long long host_sdl_window_flags(unsigned int window);
/* in points (0) or pixels (1) */
int host_sdl_window_size(unsigned int window, int pixels, int *width, int *height);
int host_sdl_set_window_size(unsigned int window, int width, int height);
int host_sdl_set_window_fullscreen(unsigned int window, int fullscreen);
/* an SDL_DisplayMode, or NULL for the desktop's */
int host_sdl_set_window_fullscreen_mode(unsigned int window, const void *mode);
int host_sdl_set_relative_mouse(unsigned int window, int enabled);
void host_sdl_warp_mouse(unsigned int window, float x, float y);

unsigned int host_sdl_create_renderer(unsigned int window);
void host_sdl_destroy_renderer(unsigned int renderer);
int host_sdl_render_vsync(unsigned int renderer, int vsync);
int host_sdl_render_color(unsigned int renderer, int r, int g, int b, int a);
int host_sdl_render_scale(unsigned int renderer, float x, float y);
int host_sdl_render_clear(unsigned int renderer);
/* an SDL_FRect */
int host_sdl_render_fill_rect(unsigned int renderer, const void *rect);
int host_sdl_render_debug_text(unsigned int renderer, float x, float y, const char *text);
int host_sdl_render_present(unsigned int renderer);

/* an SDL_Event (128 bytes) */
int host_sdl_poll_event(void *event);
void host_sdl_pump_events(void);
int host_sdl_push_event(const void *event);

int host_sdl_get_gamepads(unsigned int *ids, int capacity);
unsigned int host_sdl_open_gamepad(unsigned int id);
unsigned int host_sdl_gamepad_from_id(unsigned int id);
void host_sdl_close_gamepad(unsigned int gamepad);
void host_sdl_gamepad_name(unsigned int gamepad, char *buffer, unsigned int size);
int host_sdl_gamepad_axis(unsigned int gamepad, int axis);
int host_sdl_gamepad_button(unsigned int gamepad, int button);
int host_sdl_gamepad_type(unsigned int gamepad);
int host_sdl_rumble_gamepad(unsigned int gamepad, unsigned int low, unsigned int high, unsigned int milliseconds);

/* a stream on the device (an SDL_AudioSpec); with_callback: its guest
thread waits for SDL's requests (host_sdl_audio_wait) */
unsigned int host_sdl_open_audio_stream(unsigned int device, const void *spec, int with_callback);
/* waits for the stream's next request (the bytes wanted, and in all);
0 once the stream is destroyed */
int host_sdl_audio_wait(unsigned int stream, int *additional, int *total);
/* the request answered (SDL_PutAudioStreamData meanwhile is kept for it) */
void host_sdl_audio_done(unsigned int stream);
int host_sdl_put_audio_stream_data(unsigned int stream, const void *data, int length);
int host_sdl_resume_audio_stream_device(unsigned int stream);
int host_sdl_get_audio_stream_data(unsigned int stream, void *data, int length);
int host_sdl_get_audio_stream_available(unsigned int stream);
void host_sdl_destroy_audio_stream(unsigned int stream);
int host_sdl_audio_devices(int recording, unsigned int *ids, int capacity);
int host_sdl_audio_device_name(unsigned int device, char *buffer, unsigned int size);

/* the names (NUL-separated) that match, into names; how many, or -1 */
int host_sdl_glob_directory(const char *path, const char *pattern, unsigned int flags, char *names,
	unsigned int size);

/* ---------- the renderer (port/macos/host/host_metal.m) */

/* the window's CAMetalLayer made, and the device's state with it */
int host_metal_attach(unsigned int window);
void host_metal_set_vsync(int interval);
/* the frame drawn so far, shown: the picture last given to
host_metal_present_target, letterboxed in the window */
void host_metal_present(void);
void host_metal_present_target(unsigned int texture, unsigned int width, unsigned int height);

/* textures (port/macos/guest/metal_protocol.h's formats, types and usage):
a handle, its storage (made again each time: a frame on the GPU keeps the
old), pixels into a level, a level of one from another's first, and
mipmaps made from a level down */
unsigned int host_metal_texture_new(void);
void host_metal_texture_define(unsigned int texture, int type, unsigned int format, unsigned int width,
	unsigned int height, unsigned int depth, unsigned int levels, int usage, unsigned int swizzle);
void host_metal_texture_upload(unsigned int texture, unsigned int face, unsigned int level, unsigned int x,
	unsigned int y, unsigned int z, unsigned int width, unsigned int height, unsigned int depth, const void *data,
	unsigned int bytes_per_row, unsigned int bytes_per_image);
void host_metal_texture_delete(unsigned int texture);
void host_metal_texture_copy_level(unsigned int source, unsigned int destination, unsigned int level,
	unsigned int width, unsigned int height);
void host_metal_texture_generate_mipmaps(unsigned int texture, unsigned int first);
/* a colour target's pixels (BGRA, rows from the top) once the GPU has drawn
them; 1 if read */
int host_metal_texture_read(unsigned int texture, unsigned int width, unsigned int height, void *data);

/* a shader of the translators' program text (0 if it does not build), and
a vertex layout */
unsigned int host_metal_shader(const char *source, int fragment);
unsigned int host_metal_vertex_layout(const struct metal_vertex_layout *layout);

/* drawing: the targets (textures, 0 for none), a clear and a draw */
void host_metal_set_targets(unsigned int color, unsigned int depth);
void host_metal_clear(const struct metal_clear *clear);
void host_metal_draw(const struct metal_draw *draw);

/* visibility tests: one begun (its number, or ~0), ended with the game's
index of it, and the latest count the GPU has for an index (0 if none) */
unsigned int host_metal_visibility_begin(void);
void host_metal_visibility_end(unsigned int test, unsigned int index);
int host_metal_visibility_result(unsigned int index, unsigned int *count);

#endif
