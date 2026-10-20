/* Bounded, transport-independent Xbox disc discovery. No filesystem writes,
   allocation proportional to image size, networking or game dependencies. */
#ifndef OPENCE_IMAGE_SOURCE_H
#define OPENCE_IMAGE_SOURCE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef int (*opence_image_read)(void *context, uint64_t offset, void *bytes, size_t size);
typedef int (*opence_image_cancelled)(void *context);
struct opence_image_source {
    void *context;
    uint64_t size;
    /* Exact reads; return nonzero only for fully available, verified bytes.
       HTTP/torrent implementations own timeout and integrity checks. */
    opence_image_read read;
    opence_image_cancelled cancelled;
};
#define OPENCE_IMAGE_MAX_FILES 256
struct opence_image_file {
    char name[256];
    uint64_t offset;
    uint64_t size;
};
struct opence_image_layout {
    unsigned int count;
    uint64_t total_size;
    struct opence_image_file files[OPENCE_IMAGE_MAX_FILES];
};
/* Every invocation checks source bounds and cancellation before a callback.
   May run on a background thread; a source is used serially. */
int opence_image_read_exact(const struct opence_image_source *source, uint64_t offset,
    void *bytes, size_t size, char *error, size_t error_size);
/* Discover only supported .map files / loading.tga, require ui.map, validate
   each map header before returning an import layout. Does not publish files. */
int opence_image_discover(const struct opence_image_source *source,
    struct opence_image_layout *layout, char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
#endif
