"""tools/shader_pack.py: the bundled shader pack's file (the host's format,
host_shader_manifest.c), its merge of the maps' records, its coverage of a
real run's record, and that only a pack of this tree's translators is
bundled."""
from pathlib import Path
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import shader_pack as pack  # noqa: E402


def pipeline(vertex, fragment, color=80):
    return struct.pack("<II", vertex, fragment) + struct.pack("<I", color) + bytes(pack.PIPELINE_BYTES - 12)


def manifest(records, key="gpu Apple A12 GPU|" + pack.PACK_KEY, build="b"):
    return pack.header(key, build) + b"".join(pack.record(kind, payload) for kind, payload in records)


def test_maps_of_reads_maps_uses_and_pipelines():
    data = manifest([(pack.KIND_MAP, b"a10"), (pack.KIND_VERTEX, b"v1"), (pack.KIND_FRAGMENT, b"p1"),
                     (pack.KIND_PIPELINE, pipeline(0, 1)), (pack.KIND_MAP, b"b30"), (pack.KIND_USE, struct.pack("<I", 0)),
                     (pack.KIND_FRAGMENT, b"p2"), (pack.KIND_PIPELINE, pipeline(0, 2))])
    key, build, records = pack.parse(data)
    assert key.startswith("gpu Apple A12") and build == "b"
    maps = pack.maps_of(records)
    assert list(maps) == ["a10", "b30"]
    assert maps["a10"][0] == [(False, b"v1"), (True, b"p1")]
    assert maps["b30"][0] == [(False, b"v1"), (True, b"p2")]
    assert maps["b30"][1][0][:2] == ((False, b"v1"), (True, b"p2"))
    # (a damaged end ends the records, as the host's reader)
    damaged = data[:-3] + b"xyz"
    assert len(pack.parse(damaged)[2]) == len(records) - 1


def test_write_pack_round_trip_shares_shaders_between_maps(tmp_path):
    a = [(False, b"v1"), (True, b"p1")]
    b = [(False, b"v1"), (True, b"p2")]
    distinct = pack.write_pack(tmp_path / "p", [("a10", a, [(a[0], a[1], bytes(356))]), ("b30", b, [])], "translators x")
    assert distinct == 3
    raw = (tmp_path / "p").read_bytes()
    assert raw[:8] == pack.PACK_MAGIC
    key, build, records = pack.parse(raw)
    assert key == pack.PACK_KEY and build == "translators x"
    maps = pack.maps_of(records)
    assert maps["a10"][0] == a and maps["b30"][0] == b and len(maps["a10"][1]) == 1
    # (the second map names the shared shader by a use record, not again)
    assert sum(1 for kind, _ in records if kind == pack.KIND_USE) == 1
    expanded = zlib.decompress(raw[16:])
    assert struct.unpack_from("<Q", raw, 8)[0] == len(expanded)


def test_the_host_reads_what_the_tool_writes(tmp_path):
    """the tool's pack read by host_shader_manifest.c's reader"""
    shaders = [(False, b"vertex one"), (True, b"pixel one")]
    pack.write_pack(tmp_path / "metal_shaders.pack", [("ui", shaders, [(shaders[0], shaders[1], bytes(356))]),
                                                      ("a10", shaders[:1], [])], "translators y")
    code = tmp_path / "read.c"
    code.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host_shader_manifest.h"
static unsigned shaders, pipelines, maps, uses;
static void s(void *c, uint32_t i, int f, const char *t, uint32_t l) { (void)c; (void)i; (void)f; (void)t; (void)l; shaders++; }
static void p(void *c, const struct shader_manifest_pipeline *x) { (void)c; assert(x->fragment_shader == 1); pipelines++; }
static void m(void *c, const char *n) { (void)c; (void)n; maps++; }
static void u(void *c, uint32_t i) { (void)c; assert(i == 0); uses++; }
int main(int argc, char **argv)
{
    struct shader_manifest_reader reader = { NULL, s, p, m, u };
    struct shader_manifest info;
    (void)argc;
    assert(shader_manifest_read_pack(argv[1], "msl 20002|invariance 1|layout 364", NULL, &reader, &info));
    assert(!strcmp(info.build, "translators y") && !info.discarded[0]);
    printf("%u %u %u %u\n", shaders, pipelines, maps, uses);
    return 0;
}
''')
    executable = tmp_path / "read"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", f"-I{ROOT / 'port/macos/host'}", f"-I{ROOT / 'port/macos/guest'}",
                    str(code), str(ROOT / "port/macos/host/host_shader_manifest.c"), "-lz", "-o", str(executable)],
                   check=True)
    result = subprocess.run([str(executable), str(tmp_path / "metal_shaders.pack")], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout.split() == ["2", "1", "2", "1"]


def test_the_host_key_matches_the_tool():
    metal = (ROOT / "port/macos/host/host_metal.m").read_text()
    assert 'pack_key = [NSString stringWithFormat:@"msl %lx|invariance %d|layout %zu"' in metal
    header = (ROOT / "port/macos/host/host_shader_manifest.c").read_text()
    assert '#define PACK_MAGIC "OCESHPZ1"' in header
    # (MSL 2.2, invariance on: compile_options)
    assert "unsigned int major = 2, minor = 2;" in metal and "options.preserveInvariance = YES;" in metal


def test_only_this_trees_pack_is_bundled(tmp_path):
    app = tmp_path / "OpenCETV.app"
    app.mkdir()
    shaders = [(False, b"v")]
    stale = tmp_path / "stale.pack"
    pack.write_pack(stale, [("a10", shaders, [])], "translators 0000000000000000")
    assert pack.bundle(app, stale, log=lambda *_: None) == 0
    assert not (app / "metal_shaders.pack").exists()
    current = tmp_path / "current.pack"
    pack.write_pack(current, [("a10", shaders, [])], pack.pack_build())
    assert pack.bundle(app, current, log=lambda *_: None) > 0
    assert (app / "metal_shaders.pack").read_bytes() == current.read_bytes()
    # (and a stale one later takes it out again)
    pack.bundle(app, stale, log=lambda *_: None)
    assert not (app / "metal_shaders.pack").exists()
    assert not pack.pack_state(tmp_path / "missing.pack")[0]


def test_coverage_counts_a_real_runs_shaders(tmp_path):
    shaders = [(False, b"v1"), (True, b"p1"), (True, b"p2")]
    pack.write_pack(tmp_path / "p", [("a10", shaders[:2], []), ("b30", shaders[2:], [])], "translators z")
    run = tmp_path / "run.manifest"
    run.write_bytes(manifest([(pack.KIND_MAP, b"a10"), (pack.KIND_VERTEX, b"v1"), (pack.KIND_FRAGMENT, b"p2"),
                              (pack.KIND_FRAGMENT, b"p9")]))
    result, = pack.coverage(tmp_path / "p", [run], log=lambda *_: None)
    assert result == {"record": str(run), "map": "a10", "shaders": 3, "in_map_set": 1, "in_pack": 2}


def test_translators_hash_follows_the_translators(tmp_path):
    for name in pack.TRANSLATORS:
        (tmp_path / name).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / name).write_text(name)
    first = pack.translators_hash(tmp_path)
    (tmp_path / pack.TRANSLATORS[0]).write_text("changed")
    assert pack.translators_hash(tmp_path) != first


def test_tvos_build_bundles_the_pack():
    build = (ROOT / "tools/tvos_build.py").read_text()
    assert build.index("add_icons(build / \"OpenCETV.app\", sdk)") < build.index("bundle_shader_pack(build / \"OpenCETV.app\")")
