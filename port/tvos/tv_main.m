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
        setenv("HALO_RESOLUTION_SCALING", "720", 0);
        setenv("HALO_CLIPBOARD_JOIN", "false", 1);
        fprintf(stderr, "TV game entry: data at %s (purgeable cache)\n", getenv("HALO_DATA_ROOT"));
        fflush(stderr);
        return opence_guest_main(argc, argv);
    }
}
