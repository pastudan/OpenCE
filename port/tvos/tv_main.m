/* Initial game-host entry. SDL owns UIKit's scene delegate and invokes
   SDL_main on the main thread. Import/source-selection UI comes next. */
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>

int opence_guest_main(int argc, char **argv);

int main(int argc, char **argv)
{
    @autoreleasepool {
        NSFileManager *files = NSFileManager.defaultManager;
        NSURL *cache = [files URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
        NSURL *data = [cache URLByAppendingPathComponent:@"OpenCE" isDirectory:YES];
        NSURL *save = [data URLByAppendingPathComponent:@"save" isDirectory:YES];
        NSError *error = nil;
        if (![files createDirectoryAtURL:save withIntermediateDirectories:YES attributes:nil error:&error]) {
            fprintf(stderr, "TV data folder failed: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        /* Source data and saves are purgeable on tvOS. Only a simulator
           developer run may override these roots via the process environment. */
#if TARGET_OS_SIMULATOR
        if (!getenv("HALO_DATA_ROOT"))
#endif
            setenv("HALO_DATA_ROOT", data.fileSystemRepresentation, 1);
        setenv("HALO_SAVE_ROOT", save.fileSystemRepresentation, 1);
        setenv("HALO_DISPLAY_MODE", "borderless", 1);
        setenv("HALO_FULLSCREEN", "true", 1);
        /* Keep display.resolution_scaling in config.toml authoritative;
           the initial 720p bring-up override hid the player's saved choice. */
        setenv("HALO_NET_JOIN_FROM_CLIPBOARD", "false", 1);
        /* The TV's default: the full-screen blurs' middle passes at half
           resolution (the scopes' zoom stays full; 44-51 dB, and the A12's
           cryo frames over 50 ms 28-39 -> 3-6: PERFORMANCE.md). Not
           overwritten, so a launch's own HALO_HALF_RES_CONVOLUTION=0 wins. */
        setenv("HALO_HALF_RES_CONVOLUTION", "true", 0);
        /* The game's texture cache: 96 MiB of the desktop's 256 (the Xbox's
           was 22; a frame of the largest Custom Edition maps draws ~64). Its
           pages join the app's footprint as they are first used, and over a
           long session all of them were: tvOS then reclaimed memory under the
           game (port/tvos/PERFORMANCE.md). An environment value set for a
           run (tvos_bench --env) is kept. */
        setenv("HALO_TEXTURE_CACHE_MB", "96", 0);
        fprintf(stderr, "TV game entry: data at %s (purgeable cache)\n", getenv("HALO_DATA_ROOT"));
        fflush(stderr);
        return opence_guest_main(argc, argv);
    }
}
