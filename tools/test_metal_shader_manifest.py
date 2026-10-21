"""The Metal host's shader manifest (port/macos/host/host_shader_manifest.c),
compiled as it is and run on files of its own: what it keeps, and that a
damaged, foreign or locked file changes nothing but what is prepared."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent

HARNESS = r'''
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "host_shader_manifest.h"

static char seen[64][64];
static int seen_fragment[64];
static unsigned shaders_seen, pipelines_seen;
static struct shader_manifest_pipeline last_pipeline;

static void on_shader(void *context, uint32_t index, int fragment, const char *source, uint32_t length)
{
    (void)context;
    assert(index == shaders_seen && length < 64);
    memcpy(seen[index], source, length);
    seen[index][length] = 0;
    seen_fragment[index] = fragment;
    shaders_seen++;
}

static void on_pipeline(void *context, const struct shader_manifest_pipeline *pipeline)
{
    (void)context;
    last_pipeline = *pipeline;
    pipelines_seen++;
}

static const struct shader_manifest_reader reader = { NULL, on_shader, on_pipeline };

static void reopen(struct shader_manifest *manifest, const char *path, const char *key, const char *build)
{
    shaders_seen = pipelines_seen = 0;
    memset(seen, 0, sizeof(seen));
    assert(shader_manifest_open(manifest, path, key, build, &reader));
}

static long file_size(const char *path)
{
    struct stat status;
    assert(!stat(path, &status));
    return (long)status.st_size;
}

int main(int argc, char **argv)
{
    const char *path = argv[1];
    struct shader_manifest manifest, other;
    struct shader_manifest_pipeline pipeline;
    long good;
    int descriptor;

    (void)argc;
    /* a new file: nothing read, records kept */
    reopen(&manifest, path, "gpu A|msl 2.2", "build 1");
    assert(shaders_seen == 0 && !manifest.kept && !manifest.discarded[0]);
    assert(shader_manifest_add_shader(&manifest, 0, "vertex one", 10) == 0);
    assert(shader_manifest_add_shader(&manifest, 1, "pixel one", 9) == 1);
    memset(&pipeline, 0, sizeof(pipeline));
    pipeline.vertex_shader = 0;
    pipeline.fragment_shader = 1;
    pipeline.color_format = 80;
    pipeline.blend.write_mask = 15;
    pipeline.layout.attributes[3].format = 31;
    assert(shader_manifest_add_pipeline(&manifest, &pipeline));
    /* (a pipeline of shaders not recorded is refused) */
    pipeline.fragment_shader = 7;
    assert(!shader_manifest_add_pipeline(&manifest, &pipeline));
    pipeline.fragment_shader = 1;
    /* a second copy of the app keeps no record while this one has it */
    assert(!shader_manifest_open(&other, path, "gpu A|msl 2.2", "build 1", &reader) && other.descriptor < 0);
    shader_manifest_close(&manifest);

    /* read back in order, as written */
    reopen(&manifest, path, "gpu A|msl 2.2", "build 1");
    assert(manifest.kept && shaders_seen == 2 && pipelines_seen == 1 && !manifest.discarded[0]);
    assert(!strcmp(seen[0], "vertex one") && !seen_fragment[0]);
    assert(!strcmp(seen[1], "pixel one") && seen_fragment[1]);
    assert(!memcmp(&last_pipeline, &pipeline, sizeof(pipeline)));
    /* (appended after what was there: the indices go on) */
    assert(shader_manifest_add_shader(&manifest, 1, "pixel two", 9) == 2);
    shader_manifest_close(&manifest);
    good = file_size(path);

    /* a record cut short (a crash while writing): the good ones are read,
    the file cut back to them */
    descriptor = open(path, O_WRONLY | O_APPEND);
    assert(descriptor >= 0 && write(descriptor, "\x01\0\0\0\x40\0\0\0garbage", 15) == 15);
    close(descriptor);
    reopen(&manifest, path, "gpu A|msl 2.2", "build 1");
    assert(manifest.kept && shaders_seen == 3 && strstr(manifest.discarded, "record at byte"));
    shader_manifest_close(&manifest);
    assert(file_size(path) == good);

    /* a damaged byte in the last record: the records before it stay */
    descriptor = open(path, O_RDWR);
    assert(descriptor >= 0 && pwrite(descriptor, "X", 1, good - 3) == 1);
    close(descriptor);
    reopen(&manifest, path, "gpu A|msl 2.2", "build 1");
    assert(manifest.kept && shaders_seen == 2 && pipelines_seen == 1 && strstr(manifest.discarded, "damaged"));
    shader_manifest_close(&manifest);

    /* another build: read (to prepare), then started again with none */
    reopen(&manifest, path, "gpu A|msl 2.2", "build 2");
    assert(!manifest.kept && shaders_seen == 2 && manifest.shaders == 0);
    assert(shader_manifest_add_shader(&manifest, 0, "vertex b2", 9) == 0);
    shader_manifest_close(&manifest);
    reopen(&manifest, path, "gpu A|msl 2.2", "build 2");
    assert(manifest.kept && shaders_seen == 1 && !strcmp(seen[0], "vertex b2"));
    shader_manifest_close(&manifest);

    /* another GPU or compiler: nothing read, started again */
    reopen(&manifest, path, "gpu B|msl 2.2", "build 2");
    assert(!manifest.kept && shaders_seen == 0 && strstr(manifest.discarded, "another device"));
    shader_manifest_close(&manifest);

    /* not a manifest at all */
    descriptor = open(path, O_WRONLY | O_TRUNC);
    assert(descriptor >= 0 && write(descriptor, "hello, this is not one", 22) == 22);
    close(descriptor);
    reopen(&manifest, path, "gpu B|msl 2.2", "build 2");
    assert(!manifest.kept && shaders_seen == 0 && manifest.discarded[0]);
    assert(shader_manifest_add_shader(&manifest, 1, "pixel c", 7) == 0);
    shader_manifest_close(&manifest);
    reopen(&manifest, path, "gpu B|msl 2.2", "build 2");
    assert(manifest.kept && shaders_seen == 1);
    shader_manifest_close(&manifest);

    /* a folder that cannot be written: no record, no harm */
    assert(!shader_manifest_open(&manifest, "/nonexistent-folder/manifest", "k", "b", &reader));
    assert(manifest.descriptor < 0 && shader_manifest_add_shader(&manifest, 0, "x", 1) == ~0u);
    puts("ok");
    return 0;
}
'''


def test_shader_manifest_file(tmp_path):
    code = tmp_path / "manifest_test.c"
    code.write_text(HARNESS)
    executable = tmp_path / "manifest_test"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'port/macos/host'}",
                    f"-I{ROOT / 'port/macos/guest'}", str(code), str(ROOT / "port/macos/host/host_shader_manifest.c"),
                    "-o", str(executable)], check=True)
    result = subprocess.run([str(executable), str(tmp_path / "manifest")], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"


def test_a_draw_uses_only_what_its_own_text_compiles_to():
    """a prepared shader is taken only for exactly the text the draw would
    compile (shader_text of the same source and kind), else the draw
    compiles its own on the main thread, as before"""
    metal = (ROOT / "port/macos/host/host_metal.m").read_text()
    shader = metal[metal.index("unsigned int host_metal_shader("):metal.index("static MTLVertexDescriptor *vertex_descriptor_make(")]
    assert "NSString *text = shader_text(source, fragment);" in shader
    assert "function = prepared_take(text, fragment, &claimed);" in shader
    assert "newLibraryWithSource:text options:options error:&error" in shader
    take = metal[metal.index("static id<MTLFunction> prepared_take(NSString *text, int fragment, PreparedShader **claimed)\n{"):]
    take = take[:take.index("\n}\n")]
    assert "prepared.by_text[prepared_key(text, fragment)]" in take
    # (the prepared texts are made by the same function, from the recorded source)
    read = metal[metal.index("static void shader_cache_read_shader("):metal.index("static void shader_cache_read_pipeline(")]
    assert "shader_text(copy, fragment)" in read
    # the record is the main thread's: the threads only compile
    prepare = metal[metal.index("static void shader_cache_prepare("):metal.index("static NSString *build_identifier(")]
    assert "shader_manifest_add" not in prepare and "shader_cache.sources" not in prepare
    assert 'getenv("HALO_METAL_SHADER_CACHE")' in metal
