/* A TV feasibility app, NOT the game. Reserve address space without
   touching gigabytes, exercise Metal work and atomic waits, and transfer a
   generated file between two loopback-only torrent sessions. */
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <os/os_sync_wait_on_address.h>
#import <dispatch/dispatch.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include "swarm_test.h"

static _Atomic uint32_t wait_word;
static void *wake_thread(void *unused)
{
    (void)unused;
    usleep(20000);
    atomic_store(&wait_word, 1);
    os_sync_wake_by_address_all(&wait_word, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    return NULL;
}

static NSString *probe(BOOL *passed)
{
    *passed = YES;
    NSMutableString *report = [NSMutableString stringWithString:@"OpenCE Apple TV feasibility probe\n\n"];
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    [report appendFormat:@"Metal: %@\n", device ? device.name : @"FAILED"];
    if (device) {
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
        [encoder fillBuffer:buffer range:NSMakeRange(0, 4096) value:0x5a];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        [report appendFormat:@"GPU buffer fill: %@\n",
            command.status == MTLCommandBufferStatusCompleted && ((unsigned char *)buffer.contents)[0] == 0x5a
                ? @"passed" : @"FAILED"];
        [report appendFormat:@"BC textures: %@; max buffer: %llu MiB\n",
            device.supportsBCTextureCompression ? @"yes" : @"decode fallback required",
            (unsigned long long)(device.maxBufferLength / (1024 * 1024))];
    }
    // Same reservation size as wasm2c uses on the Mac. Do not interpret
    // this as a physical-memory stress test or install a signal handler.
    size_t reserved = (size_t)8 * 1024 * 1024 * 1024;
    void *memory = mmap(NULL, reserved, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) {
        [report appendFormat:@"8 GiB address reservation: FAILED (%d)\n", errno];
    } else {
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        int result = mprotect((char *)memory + 0x80000000ull, page, PROT_READ | PROT_WRITE);
        if (!result) *((volatile unsigned char *)memory + 0x80000000ull) = 0x42;
        [report appendFormat:@"8 GiB reservation / Xbox offset page: %@\n", result ? @"FAILED" : @"passed"];
        munmap(memory, reserved);
    }
    pthread_t thread;
    atomic_store(&wait_word, 0);
    if (pthread_create(&thread, NULL, wake_thread, NULL) == 0) {
        int status = os_sync_wait_on_address_with_timeout(&wait_word, 0, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE,
            OS_CLOCK_MACH_ABSOLUTE_TIME, 1000000000ull);
        int saved_error = errno;
        uint32_t observed = atomic_load(&wait_word);
        pthread_join(thread, NULL);
        [report appendFormat:@"Atomic wait/wake: %@ (return %d, errno %d)\n",
            status >= 0 && observed == 1 ? @"passed" : @"FAILED or inconclusive", status, saved_error];
    } else {
        [report appendString:@"Atomic wait/wake: FAILED to start thread\n"];
    }
    NSString *temporary = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    char result[1024] = {0};
    int tcp = opence_probe_tcp(temporary.fileSystemRepresentation, result, sizeof(result));
    [report appendFormat:@"\nTCP result %d: %s\n", tcp, result];
    [NSFileManager.defaultManager removeItemAtPath:temporary error:NULL];
    [report appendString:@"\nNOT TESTED: gameplay, SDL lifecycle, wasm2c faults, peak RAM, WebRTC peers.\n"];
    *passed = tcp == 0 && [report rangeOfString:@"FAILED"].location == NSNotFound;
    return report;
}

@interface TVProbeDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation TVProbeDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
    (void)application; (void)options;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    UIViewController *controller = [UIViewController new];
    controller.view.backgroundColor = UIColor.blackColor;
    UILabel *label = [[UILabel alloc] initWithFrame:CGRectInset(self.window.bounds, 80, 60)];
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    label.numberOfLines = 0;
    label.font = [UIFont monospacedSystemFontOfSize:25 weight:UIFontWeightRegular];
    label.textColor = UIColor.whiteColor;
    label.text = @"Running asset-free Metal, memory, thread and loopback network probes…";
    [controller.view addSubview:label];
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL passed = NO;
        NSString *report = probe(&passed);
        NSLog(@"%@", report);
        dispatch_async(dispatch_get_main_queue(), ^{
            label.text = report;
            if (getenv("OPENCE_PROBE_AUTOMATED")) {
                fprintf(stderr, "%s: automatic TV platform probes\n", passed ? "PASS" : "FAIL");
                fflush(stderr);
                exit(passed ? 0 : 1);
            }
        });
    });
    return YES;
}
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(TVProbeDelegate.class));
    }
}
