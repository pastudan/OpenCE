/*
HOST_SHADER_MANIFEST.H

The renderer's record of the shaders and pipelines it has made
(host_metal.m), kept in a file in the app's caches so that the next launch
can compile them again on other threads before the game asks for them: a
shader's first draw then finds it built. It holds only what the compiles
were made from (each shader's source as the game gave it, and what each
pipeline was made of), never anything compiled, so a stale or foreign
record can cost time but never change a picture: a draw uses only what its
own source compiles to, and a recorded shader is used only for exactly its
source.

The records are grouped by the map that made them: a map record names the
map the records after it are of, and a shader one map recorded that another
uses too is named again by its index (a use record), so that a map's set
can be prepared when it loads, not every map's at launch.

The file: a header (a magic number, the key: the GPU and the compile
options, and the build that wrote it) and records, each with its length and
a checksum. A file of another key is not read. One of another build is read
(its shaders are most likely this build's too) and then started again, so
that it holds only what this build makes. A damaged or partly written
record ends what is read, and the file is cut there. The file is locked
while open: a second copy of the app keeps no record.

A pack (tools/shader_pack.py) is the same records, made at build time from
scripted runs of every campaign map and shipped in the app's bundle: read
only (shader_manifest_read_pack), perhaps compressed (zlib), and keyed by
the compile options alone (its shaders' text is the same on every GPU).
*/

#ifndef __HALO_MACOS_HOST_SHADER_MANIFEST_H
#define __HALO_MACOS_HOST_SHADER_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "metal_protocol.h"

/* (a map record's name is no longer) */
#define SHADER_MANIFEST_MAP_NAME_LIMIT 64

/* what a pipeline was made of: its shaders (their records' indices, in the
order the file has them) and the state a pipeline_key of host_metal.m has */
struct shader_manifest_pipeline
{
	uint32_t vertex_shader, fragment_shader;
	uint32_t color_format, depth_format;
	struct metal_blend blend;
	struct metal_vertex_layout layout;
};

struct shader_manifest
{
	int descriptor;
	/* records in the file (a use record is not a shader) */
	uint32_t shaders, pipelines, maps;
	uint64_t bytes;
	/* the file's records are this build's, and stay (else they were read and
	the file started again): set before the reader is called */
	int kept;
	/* why a file's contents were not all read, for the log ("" if they were) */
	char discarded[96];
	/* the build that wrote what was read (shader_manifest_read's) */
	char build[160];
	/* the map the last map record read or written names ("" none) */
	char map[SHADER_MANIFEST_MAP_NAME_LIMIT + 1];
};

struct shader_manifest_reader
{
	void *context;
	void (*shader)(void *context, uint32_t index, int fragment, const char *source, uint32_t length);
	void (*pipeline)(void *context, const struct shader_manifest_pipeline *pipeline);
	/* (either may be NULL) the records after this are of this map; a
	shader recorded before (its index) is used by the current map too */
	void (*map)(void *context, const char *name);
	void (*shader_used)(void *context, uint32_t index);
};

/* the file at path opened (made if it is not there), its records of this key
handed to reader in order, and the file left ready for this build's; 0 if it
cannot be opened, locked or written (manifest->descriptor -1: no record is
kept) */
int shader_manifest_open(struct shader_manifest *manifest, const char *path, const char *key, const char *build,
	const struct shader_manifest_reader *reader);
/* a shader's source, kept: its index (the shader_manifest_pipeline's), or ~0
if it was not written */
uint32_t shader_manifest_add_shader(struct shader_manifest *manifest, int fragment, const char *source,
	uint32_t length);
/* 0 if it was not written */
int shader_manifest_add_pipeline(struct shader_manifest *manifest, const struct shader_manifest_pipeline *pipeline);
/* the records after this are of this map (a name of letters, digits and
'_' or '-', at most SHADER_MANIFEST_MAP_NAME_LIMIT); 0 if not written */
int shader_manifest_add_map(struct shader_manifest *manifest, const char *name);
/* a shader recorded before used by the current map too; 0 if not written */
int shader_manifest_add_shader_use(struct shader_manifest *manifest, uint32_t index);
void shader_manifest_close(struct shader_manifest *manifest);

/* a whole file's bytes, read only: its records handed to reader if its key
is key and (unless build is NULL) it is that build's (info: what was read,
the build it says, and why not all); 1 if it was read */
int shader_manifest_read(const void *data, size_t size, const char *key, const char *build,
	const struct shader_manifest_reader *reader, struct shader_manifest *info);
/* a pack's file read as shader_manifest_read: a manifest file, or one
compressed ("OCESHPZ1", its size (8), and the file compressed with zlib); 0
if there is none, it cannot be read, or it is not of this key or build
(info->discarded says why) */
int shader_manifest_read_pack(const char *path, const char *key, const char *build,
	const struct shader_manifest_reader *reader, struct shader_manifest *info);

#endif
