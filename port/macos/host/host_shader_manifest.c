/*
HOST_SHADER_MANIFEST.C

The file of host_shader_manifest.h. Every value is little-endian, as the
host (arm64) writes them:

	header:  "OCESHM01", the key's length (4 bytes) and the key, the build's
	         length (4) and the build
	records: the kind (4), the payload's length (4), the checksum (8: FNV-1a
	         of the kind, the length and the payload), the payload

A shader's payload is its source (no terminating zero); a pipeline's, a
struct shader_manifest_pipeline as this build lays it out (its length is
checked, and the indices of its shaders).
*/

#include "host_shader_manifest.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC "OCESHM01"
#define MAGIC_BYTES 8
/* (a longer file, or record, is not one this code wrote) */
#define FILE_LIMIT (64u << 20)
#define RECORD_LIMIT (1u << 20)
/* no more records are written past this (a shader's source is a few KiB;
a10's and b30's together are 1.5 MiB) */
#define WRITE_LIMIT (32u << 20)
#define RECORD_HEADER 16

enum
{
	_record_vertex_shader = 1,
	_record_fragment_shader,
	_record_pipeline,
};

static uint64_t checksum(uint32_t kind, uint32_t length, const void *payload)
{
	const unsigned char *bytes = payload;
	uint64_t hash = 1469598103934665603ULL;
	uint32_t index;

	for (index = 0; index < 4; index++)
		hash = (hash ^ ((kind >> (index * 8)) & 0xff)) * 1099511628211ULL;
	for (index = 0; index < 4; index++)
		hash = (hash ^ ((length >> (index * 8)) & 0xff)) * 1099511628211ULL;
	for (index = 0; index < length; index++)
		hash = (hash ^ bytes[index]) * 1099511628211ULL;
	return hash;
}

static int write_all(int descriptor, const void *data, size_t size)
{
	const unsigned char *bytes = data;

	while (size)
	{
		ssize_t written = write(descriptor, bytes, size);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return 0;
		bytes += written;
		size -= (size_t)written;
	}
	return 1;
}

static uint64_t header_bytes(const char *key, const char *build)
{
	return MAGIC_BYTES + 4 + strlen(key) + 4 + strlen(build);
}

/* the file emptied and given this key's and build's header */
static int start_again(struct shader_manifest *manifest, const char *key, const char *build)
{
	uint32_t key_length = (uint32_t)strlen(key), build_length = (uint32_t)strlen(build);

	manifest->shaders = manifest->pipelines = 0;
	manifest->kept = 0;
	if (ftruncate(manifest->descriptor, 0) || lseek(manifest->descriptor, 0, SEEK_SET) != 0 ||
		!write_all(manifest->descriptor, MAGIC, MAGIC_BYTES) || !write_all(manifest->descriptor, &key_length, 4) ||
		!write_all(manifest->descriptor, key, key_length) || !write_all(manifest->descriptor, &build_length, 4) ||
		!write_all(manifest->descriptor, build, build_length))
	{
		return 0;
	}
	manifest->bytes = header_bytes(key, build);
	return 1;
}

/* the records after the header, handed on; the length of what was good */
static uint64_t read_records(struct shader_manifest *manifest, const unsigned char *data, uint64_t size,
	uint64_t at, const struct shader_manifest_reader *reader)
{
	while (at < size)
	{
		uint32_t kind, length;
		uint64_t sum;
		const unsigned char *payload;

		if (size - at < RECORD_HEADER)
		{
			snprintf(manifest->discarded, sizeof(manifest->discarded), "a partly written record at byte %llu",
				(unsigned long long)at);
			break;
		}
		memcpy(&kind, data + at, 4);
		memcpy(&length, data + at + 4, 4);
		memcpy(&sum, data + at + 8, 8);
		payload = data + at + RECORD_HEADER;
		if (length > RECORD_LIMIT || length > size - at - RECORD_HEADER || checksum(kind, length, payload) != sum)
		{
			snprintf(manifest->discarded, sizeof(manifest->discarded), "a damaged record at byte %llu",
				(unsigned long long)at);
			break;
		}
		if (kind == _record_vertex_shader || kind == _record_fragment_shader)
		{
			if (reader && reader->shader)
				reader->shader(reader->context, manifest->shaders, kind == _record_fragment_shader,
					(const char *)payload, length);
			manifest->shaders++;
		}
		else if (kind == _record_pipeline && length == sizeof(struct shader_manifest_pipeline))
		{
			struct shader_manifest_pipeline pipeline;

			memcpy(&pipeline, payload, sizeof(pipeline));
			/* (its shaders are recorded before it, as they are made first) */
			if (pipeline.vertex_shader >= manifest->shaders || pipeline.fragment_shader >= manifest->shaders)
			{
				snprintf(manifest->discarded, sizeof(manifest->discarded),
					"a pipeline of shaders not recorded at byte %llu", (unsigned long long)at);
				break;
			}
			if (reader && reader->pipeline)
				reader->pipeline(reader->context, &pipeline);
			manifest->pipelines++;
		}
		else
		{
			snprintf(manifest->discarded, sizeof(manifest->discarded), "a record of kind %u at byte %llu", kind,
				(unsigned long long)at);
			break;
		}
		at += RECORD_HEADER + (uint64_t)length;
	}
	return at;
}

/* the whole file, or NULL */
static unsigned char *read_file(int descriptor, uint64_t size)
{
	unsigned char *data = malloc(size ? (size_t)size : 1);
	uint64_t have = 0;

	if (!data)
		return NULL;
	while (have < size)
	{
		ssize_t got = pread(descriptor, data + have, (size_t)(size - have), (off_t)have);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		have += (uint64_t)got;
	}
	if (have != size)
	{
		free(data);
		return NULL;
	}
	return data;
}

int shader_manifest_open(struct shader_manifest *manifest, const char *path, const char *key, const char *build,
	const struct shader_manifest_reader *reader)
{
	uint32_t key_length = (uint32_t)strlen(key), build_length = (uint32_t)strlen(build), length;
	unsigned char *data = NULL;
	struct stat status;
	uint64_t good = 0;

	memset(manifest, 0, sizeof(*manifest));
	manifest->descriptor = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (manifest->descriptor < 0)
		return 0;
	/* (another copy of the app has it: none is kept by this one) */
	if (flock(manifest->descriptor, LOCK_EX | LOCK_NB) || fstat(manifest->descriptor, &status))
		goto fail;
	if (status.st_size == 0)
	{
		/* (new) */
	}
	else if ((uint64_t)status.st_size > FILE_LIMIT || (uint64_t)status.st_size < MAGIC_BYTES + 8)
	{
		snprintf(manifest->discarded, sizeof(manifest->discarded), "a file of %lld bytes",
			(long long)status.st_size);
	}
	else if (!(data = read_file(manifest->descriptor, (uint64_t)status.st_size)))
	{
		snprintf(manifest->discarded, sizeof(manifest->discarded), "a file that could not be read");
	}
	else if (memcmp(data, MAGIC, MAGIC_BYTES) || memcmp(data + MAGIC_BYTES, &key_length, 4) ||
		(uint64_t)status.st_size < MAGIC_BYTES + 4 + key_length + 4 ||
		memcmp(data + MAGIC_BYTES + 4, key, key_length))
	{
		snprintf(manifest->discarded, sizeof(manifest->discarded), "a file of another device or compiler");
	}
	else
	{
		uint64_t at = MAGIC_BYTES + 4 + key_length;

		memcpy(&length, data + at, 4);
		at += 4;
		if (length > (uint64_t)status.st_size - at)
		{
			snprintf(manifest->discarded, sizeof(manifest->discarded), "a damaged header");
		}
		else
		{
			/* (another build's shaders are read, and most will be this
			build's too, but it keeps only its own) */
			manifest->kept = length == build_length && !memcmp(data + at, build, build_length);
			good = read_records(manifest, data, (uint64_t)status.st_size, at + length, reader);
		}
	}
	free(data);
	if (!manifest->kept)
	{
		if (!start_again(manifest, key, build))
			goto fail;
	}
	else
	{
		/* (a damaged end cut off: what is written next follows the good
		records) */
		if (good < (uint64_t)status.st_size && ftruncate(manifest->descriptor, (off_t)good))
			goto fail;
		if (lseek(manifest->descriptor, (off_t)good, SEEK_SET) != (off_t)good)
			goto fail;
		manifest->bytes = good;
	}
	return 1;

fail:
	close(manifest->descriptor);
	manifest->descriptor = -1;
	manifest->kept = 0;
	return 0;
}

static int append(struct shader_manifest *manifest, uint32_t kind, const void *payload, uint32_t length)
{
	unsigned char *record;
	uint64_t sum;
	int written;

	if (manifest->descriptor < 0 || length > RECORD_LIMIT || manifest->bytes + RECORD_HEADER + length > WRITE_LIMIT)
		return 0;
	if (!(record = malloc(RECORD_HEADER + (size_t)length)))
		return 0;
	sum = checksum(kind, length, payload);
	memcpy(record, &kind, 4);
	memcpy(record + 4, &length, 4);
	memcpy(record + 8, &sum, 8);
	memcpy(record + RECORD_HEADER, payload, length);
	/* (one write: a crash leaves a whole record or a part of one, which the
	next open cuts off) */
	written = write_all(manifest->descriptor, record, RECORD_HEADER + (size_t)length);
	free(record);
	if (!written)
	{
		/* (none more: the file may end in a part of this one) */
		close(manifest->descriptor);
		manifest->descriptor = -1;
		return 0;
	}
	manifest->bytes += RECORD_HEADER + (uint64_t)length;
	return 1;
}

uint32_t shader_manifest_add_shader(struct shader_manifest *manifest, int fragment, const char *source,
	uint32_t length)
{
	if (!append(manifest, fragment ? _record_fragment_shader : _record_vertex_shader, source, length))
		return ~0u;
	return manifest->shaders++;
}

int shader_manifest_add_pipeline(struct shader_manifest *manifest, const struct shader_manifest_pipeline *pipeline)
{
	if (pipeline->vertex_shader >= manifest->shaders || pipeline->fragment_shader >= manifest->shaders ||
		!append(manifest, _record_pipeline, pipeline, sizeof(*pipeline)))
	{
		return 0;
	}
	manifest->pipelines++;
	return 1;
}

void shader_manifest_close(struct shader_manifest *manifest)
{
	if (manifest->descriptor >= 0)
		close(manifest->descriptor);
	manifest->descriptor = -1;
}
