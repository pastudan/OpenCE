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

The file: a header (a magic number, the key: the GPU and the compile
options, and the build that wrote it) and records, each with its length and
a checksum. A file of another key is not read. One of another build is read
(its shaders are most likely this build's too) and then started again, so
that it holds only what this build makes. A damaged or partly written
record ends what is read, and the file is cut there. The file is locked
while open: a second copy of the app keeps no record.
*/

#ifndef __HALO_MACOS_HOST_SHADER_MANIFEST_H
#define __HALO_MACOS_HOST_SHADER_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "metal_protocol.h"

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
	/* records in the file */
	uint32_t shaders, pipelines;
	uint64_t bytes;
	/* the file's records are this build's, and stay (else they were read and
	the file started again): set before the reader is called */
	int kept;
	/* why a file's contents were not all read, for the log ("" if they were) */
	char discarded[96];
};

struct shader_manifest_reader
{
	void *context;
	void (*shader)(void *context, uint32_t index, int fragment, const char *source, uint32_t length);
	void (*pipeline)(void *context, const struct shader_manifest_pipeline *pipeline);
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
void shader_manifest_close(struct shader_manifest *manifest);

#endif
