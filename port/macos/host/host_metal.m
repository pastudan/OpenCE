/*
HOST_METAL.M

The macOS build's renderer (port/macos/README.md): Metal, drawing into the
game's window's CAMetalLayer.

This first version presents a cleared frame each time the game presents:
it is what the game boots with on macOS while the Direct3D device is moved
to Metal (port/linux/src/d3d8_metal.c, to come), with the game running in
full behind it (debug.null_renderer's device: d3d8_null.c).
*/

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include "host.h"

SDL_Window *host_sdl_window(unsigned int window);

static struct
{
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	SDL_MetalView view;
	CAMetalLayer *layer;
	SDL_Window *window;
	int vsync;
	unsigned long frame;
} metal;

void host_metal_window_created(SDL_Window *window)
{
	(void)window;
}

void host_metal_window_destroyed(SDL_Window *window)
{
	if (window != metal.window)
		return;
	if (metal.view)
		SDL_Metal_DestroyView(metal.view);
	metal.view = NULL;
	metal.layer = nil;
	metal.window = NULL;
}

int host_metal_attach(unsigned int window)
{
	SDL_Window *object = host_sdl_window(window);

	if (!object)
		return 0;
	if (!metal.device)
	{
		metal.device = MTLCreateSystemDefaultDevice();
		if (!metal.device)
		{
			SDL_SetError("no Metal device");
			return 0;
		}
		metal.queue = [metal.device newCommandQueue];
		host_log("Metal: %s", metal.device.name.UTF8String);
	}
	metal.view = SDL_Metal_CreateView(object);
	if (!metal.view)
		return 0;
	metal.layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(metal.view);
	metal.layer.device = metal.device;
	metal.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	metal.layer.framebufferOnly = YES;
	metal.layer.maximumDrawableCount = 3;
	metal.layer.displaySyncEnabled = metal.vsync ? YES : NO;
	metal.window = object;
	return 1;
}

void host_metal_set_vsync(int interval)
{
	metal.vsync = interval != 0;
	if (metal.layer)
		metal.layer.displaySyncEnabled = metal.vsync ? YES : NO;
}

void host_metal_present(void)
{
	@autoreleasepool
	{
		int width = 0, height = 0;
		id<CAMetalDrawable> drawable;
		MTLRenderPassDescriptor *pass;
		id<MTLCommandBuffer> commands;
		id<MTLRenderCommandEncoder> encoder;

		if (!metal.layer)
			return;
		SDL_GetWindowSizeInPixels(metal.window, &width, &height);
		if (width > 0 && height > 0 &&
			(metal.layer.drawableSize.width != width || metal.layer.drawableSize.height != height))
		{
			metal.layer.drawableSize = CGSizeMake(width, height);
		}
		drawable = [metal.layer nextDrawable];
		if (!drawable)
			return;
		pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = drawable.texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		/* (a slow pulse: the game is presenting) */
		{
			double phase = (double)(metal.frame++ % 240) / 240.0;

			pass.colorAttachments[0].clearColor = MTLClearColorMake(0.05, 0.08 + 0.06 * phase, 0.12, 1.0);
		}
		commands = [metal.queue commandBuffer];
		encoder = [commands renderCommandEncoderWithDescriptor:pass];
		[encoder endEncoding];
		[commands presentDrawable:drawable];
		[commands commit];
	}
}
