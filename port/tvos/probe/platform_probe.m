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

static void stage(const char *name)
{
    fprintf(stderr, "TV probe: %s\n", name);
    fflush(stderr);
}

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
    stage("Metal");
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
    // The Mac's 8 GiB guard reservation is optional on TV: the game target
    // uses explicit bounds checks and reserves only its 2.5 GiB guest size.
    // Neither test measures peak physical memory or installs a fault handler.
    stage("virtual address reservation");
    size_t guard_size = (size_t)8 * 1024 * 1024 * 1024;
    void *guard = mmap(NULL, guard_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (guard == MAP_FAILED) {
        [report appendFormat:@"Mac guard reservation: unavailable (%d); explicit bounds required\n", errno];
    } else {
        [report appendString:@"Mac guard reservation: available (not used by TV target)\n"];
        munmap(guard, guard_size);
    }
    size_t reserved = 0xA0000000ull;
    void *memory = mmap(NULL, reserved, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) {
        [report appendFormat:@"2.5 GiB guest reservation: FAILED (%d)\n", errno];
    } else {
        // Match wasm2c's commitment, then touch just one Xbox-window page.
        int result = mprotect(memory, reserved, PROT_READ | PROT_WRITE);
        if (!result) *((volatile unsigned char *)memory + 0x80000000ull) = 0x42;
        [report appendFormat:@"2.5 GiB guest / Xbox offset page: %@\n", result ? @"FAILED" : @"passed"];
        munmap(memory, reserved);
    }
    stage("atomic wait/wake");
    pthread_t thread;
    atomic_store(&wait_word, 0);
    if (pthread_create(&thread, NULL, wake_thread, NULL) == 0) {
        errno = 0;
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
    stage("loopback TCP");
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
@end
@implementation TVProbeDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
    (void)application; (void)options;
    stage("UIKit launch delegate");
    return YES;
}
@end

@interface TVProbeSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation TVProbeSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
    (void)session; (void)options;
    stage("UIKit scene connected");
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
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
        stage("worker started");
        BOOL passed = NO;
        NSString *report = probe(&passed);
        fprintf(stderr, "%s\n", report.UTF8String);
        fflush(stderr);
        dispatch_async(dispatch_get_main_queue(), ^{
            stage("main-queue completion");
            label.text = report;
            if (getenv("OPENCE_PROBE_AUTOMATED")) {
                fprintf(stderr, "%s: automatic TV platform probes\n", passed ? "PASS" : "FAIL");
                fflush(stderr);
                exit(passed ? 0 : 1);
            }
        });
    });
}
@end

int main(int argc, char **argv)
{
    stage("process main");
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(TVProbeDelegate.class));
    }
}
