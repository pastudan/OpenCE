/*
IMAGE_SOURCE.C

Discovers the maps in a bounded, transport-independent Xbox disc image.
Does not copy or publish files. Follows the desktop reader's format handling.

The image's file system is XDVDFS, read as extract-xiso does
(https://github.com/XboxDev/extract-xiso, extract-xiso.c, whose format
handling this follows; its license is below and in
port/third_party/extract-xiso/LICENSE.TXT): 2048-byte sectors; a
volume descriptor at 0x10000 that starts and ends with
"MICROSOFT*XBOX*MEDIA" and gives the root directory's sector and size; and
directories whose entries form a binary tree (each entry: left and right
subtree offsets in 4-byte units, the start sector, the size, attributes, the
name's length and the name, 4-byte aligned). Images made from a whole disc
put the game partition further in, at one of the offsets below.

Some parts of this code are copyright in@fishtank.com. This product
includes software developed by in <in@fishtank.com>.

 * Copyright (c) 2003 in <in@fishtank.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *
 *    This product includes software developed by in <in@fishtank.com>.
 *
 * 4. Neither the name of "in" nor the email address "in@fishtank.com"
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED `AS IS' AND ANY EXPRESS OR IMPLIED WARRANTIES
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * AUTHOR OR ANY CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Transport-independent, bounded discovery for the TV importer. The format
   interpretation follows xiso.c and retains its extract-xiso notice above. */
#include "image_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048u
#define MAX_DIRECTORY (4u * 1024u * 1024u)
#define MAX_IMAGE (16ull * 1024ull * 1024ull * 1024ull)
#define MAX_OUTPUT (8ull * 1024ull * 1024ull * 1024ull)
static const char magic[] = "MICROSOFT*XBOX*MEDIA";

static int fail(char *error, size_t size, const char *reason)
{
    if (error && size) snprintf(error, size, "%s", reason);
    return 0;
}
static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static unsigned int u16(const unsigned char *p) { return p[0] | (unsigned int)p[1] << 8; }

int opence_image_read_exact(const struct opence_image_source *source, uint64_t offset,
    void *bytes, size_t size, char *error, size_t error_size)
{
    if (!source || !source->read || (!bytes && size)) return fail(error, error_size, "Invalid image source");
    if (offset > source->size || size > source->size - offset)
        return fail(error, error_size, "Image range is outside the source");
    if (source->cancelled && source->cancelled(source->context)) return fail(error, error_size, "Import cancelled");
    if (!size) return 1;
    if (!source->read(source->context, offset, bytes, size)) return fail(error, error_size, "Image read failed");
    return 1;
}

struct entry { char name[256]; uint32_t sector, size; unsigned char directory; };
struct walk {
    const unsigned char *bytes;
    unsigned int size, count, visits;
    unsigned char *seen;
    struct entry *entries;
};
static int walk_entry(struct walk *w, unsigned int offset, unsigned int depth)
{
    const unsigned char *p;
    unsigned int i, length, left, right;
    struct entry *out;
    if (depth > 64 || ++w->visits > 4096 || offset > w->size || w->size - offset < 14 || (offset & 3)) return 0;
    p = w->bytes + offset;
    left = u16(p); right = u16(p + 2);
    if (left == 0xffff) return depth == 0 && w->count == 0;
    length = p[13];
    if (!length || length > w->size - offset - 14 || w->count == OPENCE_IMAGE_MAX_FILES) return 0;
    /* Claim the whole record, not just its start: a child must not hide
       inside another node's header or name. */
    for (i = offset / 4; i < (offset + 14 + length + 3) / 4; ++i) {
        if (w->seen[i]) return 0;
        w->seen[i] = 1;
    }
    out = &w->entries[w->count++];
    for (i = 0; i < length; ++i) {
        unsigned char c = p[14 + i];
        if (c < 0x20 || c > 0x7e || strchr("/\\:*?\"<>|", c)) return 0;
        out->name[i] = c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : (char)c;
    }
    out->name[length] = 0;
    if (!strcmp(out->name, ".") || !strcmp(out->name, "..") || out->name[length - 1] == ' ' ||
        out->name[length - 1] == '.') return 0;
    for (i = 0; i + 1 < w->count; ++i) if (!strcmp(w->entries[i].name, out->name)) return 0;
    out->sector = u32(p + 4); out->size = u32(p + 8); out->directory = (p[12] & 0x10) != 0;
    if (left && !walk_entry(w, left * 4u, depth + 1)) return 0;
    if (right && !walk_entry(w, right * 4u, depth + 1)) return 0;
    return 1;
}

static int directory(const struct opence_image_source *source, uint64_t partition, uint32_t sector,
    uint32_t size, struct entry *entries, unsigned int *count, char *error, size_t error_size)
{
    struct walk w;
    int result;
    if (!size || size > MAX_DIRECTORY) return fail(error, error_size, "Invalid image directory size");
    memset(&w, 0, sizeof(w));
    w.size = size; w.entries = entries;
    w.bytes = malloc(size); w.seen = calloc((size + 3u) / 4u, 1);
    if (!w.bytes || !w.seen) { free((void *)w.bytes); free(w.seen); return fail(error, error_size, "Out of memory"); }
    result = opence_image_read_exact(source, partition + (uint64_t)sector * SECTOR,
        (void *)w.bytes, size, error, error_size);
    if (result && !walk_entry(&w, 0, 0)) result = fail(error, error_size, "Malformed image directory");
    *count = result ? w.count : 0;
    free((void *)w.bytes); free(w.seen);
    return result;
}

static int is_map(const char *name)
{
    size_t size = strlen(name);
    return size > 4 && !strcmp(name + size - 4, ".map");
}

int opence_image_discover(const struct opence_image_source *source,
    struct opence_image_layout *layout, char *error, size_t error_size)
{
    static const uint64_t partitions[] = {0, 0x0fd90000ull, 0x02080000ull, 0x18300000ull};
    struct entry *entries;
    unsigned char descriptor[SECTOR], header[SECTOR];
    uint64_t partition = 0;
    uint32_t root_sector = 0, root_size = 0, maps_sector = 0, maps_size = 0;
    unsigned int i, count = 0;
    int found = 0, has_ui = 0, result = 0;
    if (error && error_size) error[0] = 0;
    if (layout) memset(layout, 0, sizeof(*layout));
    if (!source || !source->read || !layout || source->size < 0x10800 || source->size > MAX_IMAGE)
        return fail(error, error_size, "Invalid disc image size or source");
    for (i = 0; i < sizeof(partitions) / sizeof(partitions[0]); ++i) {
        uint64_t offset = partitions[i] + 0x10000ull;
        if (offset > source->size || SECTOR > source->size - offset) continue;
        // A source error or cancellation is not a failed volume signature.
        if (!opence_image_read_exact(source, offset, descriptor, SECTOR, error, error_size)) return 0;
        if (!memcmp(descriptor, magic, 20) && !memcmp(descriptor + 0x7ec, magic, 20)) {
            partition = partitions[i]; root_sector = u32(descriptor + 20); root_size = u32(descriptor + 24);
            found = 1; break;
        }
    }
    if (!found) return fail(error, error_size, "Not a supported Xbox disc image");
    entries = calloc(OPENCE_IMAGE_MAX_FILES, sizeof(*entries));
    if (!entries) return fail(error, error_size, "Out of memory");
    if (!directory(source, partition, root_sector, root_size, entries, &count, error, error_size)) goto done;
    for (i = 0; i < count; ++i) if (entries[i].directory && !strcmp(entries[i].name, "maps")) {
        maps_sector = entries[i].sector; maps_size = entries[i].size; break;
    }
    if (!maps_size) { fail(error, error_size, "Disc image has no maps directory"); goto done; }
    {
        uint64_t root_offset = (uint64_t)root_sector * SECTOR;
        uint64_t maps_offset = (uint64_t)maps_sector * SECTOR;
        if (maps_offset < root_offset + root_size && root_offset < maps_offset + maps_size) {
            fail(error, error_size, "Overlapping image directories"); goto done;
        }
    }
    if (!directory(source, partition, maps_sector, maps_size, entries, &count, error, error_size)) goto done;
    for (i = 0; i < count; ++i) {
        struct entry *entry = &entries[i];
        struct opence_image_file *file;
        unsigned int j;
        uint64_t offset = partition + (uint64_t)entry->sector * SECTOR;
        if (entry->directory || (!is_map(entry->name) && strcmp(entry->name, "loading.tga"))) continue;
        if (!entry->size || offset > source->size || entry->size > source->size - offset ||
            entry->size > MAX_OUTPUT - layout->total_size) { fail(error, error_size, "Invalid map file extent"); goto done; }
        for (j = 0; j < layout->count; ++j) {
            const struct opence_image_file *other = &layout->files[j];
            if (offset < other->offset + other->size && other->offset < offset + entry->size) {
                fail(error, error_size, "Overlapping map file extents"); goto done;
            }
        }
        if (is_map(entry->name)) {
            if (entry->size < SECTOR) { fail(error, error_size, "Truncated map header"); goto done; }
            if (!opence_image_read_exact(source, offset, header, SECTOR, error, error_size)) goto done;
            if (memcmp(header, "daeh", 4) || memcmp(header + 0x7fc, "toof", 4) || u32(header + 4) != 5 ||
                (memcmp(header + 0x40, "01.10.12.2276", sizeof("01.10.12.2276")) &&
                 memcmp(header + 0x40, "01.01.14.2342", sizeof("01.01.14.2342")))) {
                fail(error, error_size, "Unsupported or damaged Xbox map header"); goto done;
            }
        }
        file = &layout->files[layout->count++];
        memcpy(file->name, entry->name, sizeof(file->name)); file->offset = offset; file->size = entry->size;
        layout->total_size += file->size;
        has_ui |= !strcmp(file->name, "ui.map");
    }
    if (!has_ui) { fail(error, error_size, "Disc image has no ui.map"); goto done; }
    result = 1;
done:
    free(entries);
    if (!result) memset(layout, 0, sizeof(*layout));
    return result;
}
