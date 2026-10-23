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

static char maps_seen[8][72];
static unsigned map_count, uses_seen[16], use_count;

static void on_map(void *context, const char *name)
{
    (void)context;
    assert(map_count < 8);
    strcpy(maps_seen[map_count++], name);
}

static void on_use(void *context, uint32_t index)
{
    (void)context;
    assert(use_count < 16);
    uses_seen[use_count++] = index;
}

static const struct shader_manifest_reader reader = { NULL, on_shader, on_pipeline, on_map, on_use };

static void reopen(struct shader_manifest *manifest, const char *path, const char *key, const char *build)
{
    shaders_seen = pipelines_seen = map_count = use_count = 0;
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

    /* maps: the records after a map record are its; a shader of an
    earlier map's that this one draws too, a use of its index */
    unlink(path);
    reopen(&manifest, path, "gpu B|msl 2.2", "build 3");
    assert(shader_manifest_add_map(&manifest, "a10"));
    assert(shader_manifest_add_shader(&manifest, 0, "vertex a10", 10) == 0);
    assert(shader_manifest_add_map(&manifest, "b30"));
    assert(shader_manifest_add_shader_use(&manifest, 0));
    assert(!shader_manifest_add_shader_use(&manifest, 5));
    assert(shader_manifest_add_shader(&manifest, 1, "pixel b30", 9) == 1);
    /* (a name not of letters, digits, '_' and '-' is not written) */
    assert(!shader_manifest_add_map(&manifest, "levels\\a10") && !shader_manifest_add_map(&manifest, ""));
    assert(manifest.maps == 2 && !strcmp(manifest.map, "b30"));
    shader_manifest_close(&manifest);
    reopen(&manifest, path, "gpu B|msl 2.2", "build 3");
    assert(manifest.kept && shaders_seen == 2 && map_count == 2 && use_count == 1 && uses_seen[0] == 0);
    assert(!strcmp(maps_seen[0], "a10") && !strcmp(maps_seen[1], "b30") && !strcmp(manifest.map, "b30"));
    shader_manifest_close(&manifest);

    /* read only (a pack's): of this key and build, or of any build */
    {
        struct shader_manifest info;
        FILE *file = fopen(path, "rb");
        static unsigned char bytes[4096];
        size_t size = fread(bytes, 1, sizeof(bytes), file);

        fclose(file);
        shaders_seen = map_count = use_count = 0;
        assert(shader_manifest_read(bytes, size, "gpu B|msl 2.2", "build 3", &reader, &info));
        assert(shaders_seen == 2 && map_count == 2 && use_count == 1 && !strcmp(info.build, "build 3"));
        shaders_seen = 0;
        assert(!shader_manifest_read(bytes, size, "gpu B|msl 2.2", "build 4", &reader, &info));
        assert(shaders_seen == 0 && strstr(info.discarded, "another build"));
        assert(!shader_manifest_read(bytes, size, "gpu C|msl 2.2", NULL, &reader, &info) && shaders_seen == 0);
        assert(shader_manifest_read(bytes, size, "gpu B|msl 2.2", NULL, &reader, &info) && shaders_seen == 2);
        /* (the file as a pack, as it is and compressed by the test) */
        shaders_seen = 0;
        assert(shader_manifest_read_pack(path, "gpu B|msl 2.2", "build 3", &reader, &info) && shaders_seen == 2);
        assert(!shader_manifest_read_pack("/nonexistent-pack", "k", NULL, &reader, &info));
        if (argc > 2)
        {
            shaders_seen = map_count = 0;
            assert(shader_manifest_read_pack(argv[2], "gpu B|msl 2.2", "build 3", &reader, &info));
            assert(shaders_seen == 2 && map_count == 2);
            /* (a damaged compressed pack: nothing) */
            assert(argc > 3);
            shaders_seen = 0;
            assert(!shader_manifest_read_pack(argv[3], "gpu B|msl 2.2", "build 3", &reader, &info));
            assert(shaders_seen == 0 && strstr(info.discarded, "does not expand"));
        }
    }

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
                    "-lz", "-o", str(executable)], check=True)
    # (a compressed pack of the same records, as tools/shader_pack.py writes:
    # made from the file the harness writes, so the harness runs twice)
    result = subprocess.run([str(executable), str(tmp_path / "manifest")], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"
    raw = (tmp_path / "manifest").read_bytes()
    # (the harness ends on the unwritable folder: the file is the maps' one)
    import struct
    import zlib
    (tmp_path / "pack").write_bytes(b"OCESHPZ1" + struct.pack("<Q", len(raw)) + zlib.compress(raw))
    (tmp_path / "bad").write_bytes(b"OCESHPZ1" + struct.pack("<Q", len(raw)) + zlib.compress(raw)[:-9] + b"x" * 9)
    result = subprocess.run([str(executable), str(tmp_path / "manifest2"), str(tmp_path / "pack"), str(tmp_path / "bad")],
                            capture_output=True, text=True)
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
    assert "prepared.by_text[key]" in take and "key = prepared_key(text, fragment);" in take
    # (the prepared texts are made by the same function, from the recorded source)
    text = metal[metal.index("static NSString *prepared_text(PreparedShader *shader)"):]
    text = text[:text.index("\n}\n")]
    assert "shader_text(copy, shader.fragment)" in text
    # the record is the main thread's: the threads only compile
    prepare = metal[metal.index("static void prepare_map(NSString *name, uint32_t generation)\n{"):
                    metal.index("static NSString *build_identifier(")]
    assert "shader_manifest_add" not in prepare and "shader_cache.sources" not in prepare
    assert "shader_cache.known" not in prepare
    assert 'getenv("HALO_METAL_SHADER_CACHE")' in metal


def test_maps_are_prepared_when_they_load_not_at_launch():
    """nothing of a map's is compiled until the guest says it loads (its
    game_load), and a map's loading ends the last one's preparation"""
    metal = (ROOT / "port/macos/host/host_metal.m").read_text()
    start = metal[metal.index("static void shader_cache_start(void)\n{"):metal.index("void host_metal_map(const char *scenario)\n{")]
    # (at the start only the records of no map: an earlier build's)
    assert 'prepare_map(@"", atomic_load(&prepared.generation));' in start
    assert "prepare_map(name" not in start
    loading = metal[metal.index("void host_metal_map(const char *scenario)\n{"):]
    loading = loading[:loading.index("\n}\n")]
    assert "atomic_fetch_add(&prepared.generation, 1)" in loading and "prepare_map(name, generation)" in loading
    assert "shader_manifest_add_map(" in loading
    game = (ROOT / "source/game/game.c").read_text()
    load = game[game.index("boolean game_load(\n"):]
    assert load.index("metal_device_map(options->map_name);") < load.index("scenario_load(options->map_name)")
    guest = (ROOT / "port/macos/guest/guest_host.h").read_text()
    assert "void host_metal_map(const char *scenario);" in guest
    # the bundled pack is read on the preparation's queue, never compiled whole
    assert 'pathForResource:@"metal_shaders" ofType:@"pack"' in metal
    assert 'getenv("HALO_METAL_SHADER_PACK")' in metal


def test_shader_tour_is_off_by_default():
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    assert '{ "debug.shader_tour", _config_string, "\\"\\"", "HALO_SHADER_TOUR"' in config
    tour = (ROOT / "port/linux/game/shader_tour.c").read_text()
    assert 'config_string("debug.shader_tour")' in tour
    main = (ROOT / "source/main/main.c").read_text()
    assert "shader_tour_update(main_globals.main_menu_scenario_loaded);" in main


def test_the_memory_warning_trim_leaves_dropped_shaders_preparable():
    """host_metal_trim (tv/lifecycle) drops prepared shaders no draw has
    taken back to idle, under the lock, never one compiling, so the next
    map's load (or the low-priority pass) prepares it again"""
    metal = (ROOT / "port/macos/host/host_metal.m").read_text()
    trim = metal[metal.index("int64_t host_metal_trim(int step, int stopped)\n{"):]
    trim = trim[:trim.index("\n}\n")]
    drop = trim[trim.index("[prepared.condition lock];"):trim.index("[prepared.condition unlock];")]
    assert "shader.state == _prepared_compiling" in drop and "shader.state = _prepared_idle;" in drop
    prepare = metal[metal.index("static void prepare_map(NSString *name, uint32_t generation)\n{"):]
    prepare = prepare[:prepare.index("\n}\n")]
    # (a compile starts only for one still queued, checked under the lock)
    assert "if (!prepared_current(generation) || shader.state != _prepared_queued)" in prepare
