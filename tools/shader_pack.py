#!/usr/bin/env python3
"""The bundled shader pack: every Metal shader (its source, as the guest's
translators write it) and pipeline every campaign map draws, recorded on a
Mac and shipped in the Apple TV app (metal_shaders.pack), so that even the
first launch after an install prepares a map's shaders while it loads
(port/macos/host/host_metal.m, "the shader cache"; the file's format is
host_shader_manifest.h's).

    python3 tools/shader_pack.py generate [--maps DIR] [--only a10,b30]   # ~30 min, takes the heavy-job lock
    python3 tools/shader_pack.py status                                   # is the pack this build's?
    python3 tools/shader_pack.py bundle APP                               # (tools/tvos_build.py's step)
    python3 tools/shader_pack.py coverage MANIFEST... [--pack P]          # how much of a real run it has
    python3 tools/shader_pack.py dump PACK                                # maps, shaders, pipelines

generate runs the macOS app (build/macos/OpenCE.app, so `ninja macos`
first) once per map with the shader tour on (port/linux/game/shader_tour.c:
the camera in every cluster of every BSP, every object and effect of the map
made in front of it, the screen effects, then every weapon fired in first
person by the scripted input), the textures taken as the TV takes
them (HALO_METAL_NO_BC=1; the shaders' text does not depend on it, which
`coverage` of a TV's own record shows), each run's record kept in
build/shader_pack/<translators>/<map>.manifest, and merges them into
build/shader_pack/metal_shaders.pack (zlib), the maps in the campaign's
order (the host prepares the next map's from it).

The pack's header names the hash of the sources that write the shaders'
text (TRANSLATORS); `bundle` copies it into the app only if that is this
tree's, so a pack that is out of date is left out (and says so) rather than
shipped; it could cost only time anyway, since a prepared shader is used only
for exactly the text a draw compiles. A run already recorded for this hash
is not run again: generate is a separate, cached step, not part of the
build.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import tvos_deploy as deploy  # noqa: E402

OUT = ROOT / "build" / "shader_pack"
PACK = OUT / "metal_shaders.pack"
APP = ROOT / "build/macos/OpenCE.app/Contents/MacOS/OpenCE"
# the campaign, in order, and the menus first
MAPS = ["ui", "a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40"]
# what decides a shader's text: the translators, the device's keys, the
# prologue (a change elsewhere can change which shaders a map makes, not the
# text of one)
TRANSLATORS = ["port/linux/src/nv2a_psh.c", "port/linux/src/nv2a_vsh.c", "port/linux/src/xgpu.h",
               "port/linux/src/xgpu_text.c", "port/macos/guest/d3d8_metal.c", "port/macos/guest/metal_protocol.h"]
# the compile options' part of the host's key (host_metal.m pack_key): MSL
# 2.2, invariance, sizeof(struct shader_manifest_pipeline)
PACK_KEY = "msl 20002|invariance 1|layout 364"
MAGIC = b"OCESHM01"
PACK_MAGIC = b"OCESHPZ1"
KIND_VERTEX, KIND_FRAGMENT, KIND_PIPELINE, KIND_MAP, KIND_USE = 1, 2, 3, 4, 5
PIPELINE_BYTES = 364
# a map's tour at most (a run that ends sooner logs "shader tour: done")
TOUR_SECONDS = 900
UI_SECONDS = 25


def translators_hash(root=ROOT):
    digest = hashlib.sha256()
    for name in TRANSLATORS:
        digest.update(name.encode() + b"\0" + (Path(root) / name).read_bytes() + b"\0")
    return digest.hexdigest()[:16]


def pack_build(root=ROOT):
    return f"translators {translators_hash(root)}"


# ---------- the file (host_shader_manifest.c)

def checksum(kind, payload):
    value = 1469598103934665603
    for byte in struct.pack("<II", kind, len(payload)) + payload:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def record(kind, payload):
    return struct.pack("<IIQ", kind, len(payload), checksum(kind, payload)) + payload


def header(key, build):
    return MAGIC + struct.pack("<I", len(key)) + key.encode() + struct.pack("<I", len(build)) + build.encode()


def parse(data):
    """a manifest's bytes (or a pack's): key, build, and its records in
    order as (kind, payload); a damaged end ends them, as the host's reader"""
    if data[:8] == PACK_MAGIC:
        size, = struct.unpack_from("<Q", data, 8)
        data = zlib.decompress(data[16:])
        if len(data) != size:
            raise ValueError("a pack that does not expand to its size")
    if data[:8] != MAGIC:
        raise ValueError("not a shader manifest")
    key_length, = struct.unpack_from("<I", data, 8)
    key = data[12:12 + key_length].decode()
    at = 12 + key_length
    build_length, = struct.unpack_from("<I", data, at)
    build = data[at + 4:at + 4 + build_length].decode()
    at += 4 + build_length
    records = []
    while at + 16 <= len(data):
        kind, length, value = struct.unpack_from("<IIQ", data, at)
        payload = data[at + 16:at + 16 + length]
        if len(payload) != length or checksum(kind, payload) != value:
            break
        records.append((kind, payload))
        at += 16 + length
    return key, build, records


def maps_of(records):
    """{map: (shaders as (fragment, source), pipelines as (vertex source,
    fragment source, the rest's bytes))}, the maps in order ("" the records
    before any map's)"""
    shaders, maps, current = [], {}, ""

    def entry(name):
        return maps.setdefault(name, ([], []))
    for kind, payload in records:
        if kind in (KIND_VERTEX, KIND_FRAGMENT):
            shaders.append((kind == KIND_FRAGMENT, payload))
            if shaders[-1] not in entry(current)[0]:
                entry(current)[0].append(shaders[-1])
        elif kind == KIND_USE:
            index, = struct.unpack("<I", payload)
            if index < len(shaders) and shaders[index] not in entry(current)[0]:
                entry(current)[0].append(shaders[index])
        elif kind == KIND_MAP:
            current = payload.decode()
            entry(current)
        elif kind == KIND_PIPELINE and len(payload) == PIPELINE_BYTES:
            vertex, fragment = struct.unpack_from("<II", payload)
            if vertex < len(shaders) and fragment < len(shaders):
                pipeline = (shaders[vertex], shaders[fragment], payload[8:])
                if pipeline not in entry(current)[1]:
                    entry(current)[1].append(pipeline)
    return maps


def write_pack(path, maps, build, compress=True):
    """maps: [(name, shaders, pipelines)] as maps_of gives them, in order"""
    body = bytearray(header(PACK_KEY, build))
    index = {}
    for name, shaders, pipelines in maps:
        body += record(KIND_MAP, name.encode())
        for shader in shaders:
            if shader in index:
                body += record(KIND_USE, struct.pack("<I", index[shader]))
            else:
                index[shader] = len(index)
                body += record(KIND_FRAGMENT if shader[0] else KIND_VERTEX, shader[1])
        for vertex, fragment, rest in pipelines:
            for shader in (vertex, fragment):
                if shader not in index:
                    index[shader] = len(index)
                    body += record(KIND_FRAGMENT if shader[0] else KIND_VERTEX, shader[1])
            body += record(KIND_PIPELINE, struct.pack("<II", index[vertex], index[fragment]) + rest)
    data = bytes(body)
    if compress:
        data = PACK_MAGIC + struct.pack("<Q", len(data)) + zlib.compress(data, 9)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(data)
    return len(index)


# ---------- generate

def map_data_root(scratch, name, maps):
    root = Path(scratch) / name
    (root / "maps").mkdir(parents=True, exist_ok=True)
    for file in ("ui.map", f"{name}.map"):
        link = root / "maps" / file
        if link.is_symlink() or link.exists():
            link.unlink()
        if (Path(maps) / file).is_file():
            link.symlink_to((Path(maps) / file).resolve())
    init = root / "init.txt"
    if name == "ui":
        init.unlink(missing_ok=True)
    else:
        init.write_text(f"map_name levels\\{name}\\{name}\r\n")
    return root


def run_environment(record_path, data_root, tour, seconds):
    environment = dict(os.environ)
    environment.update({
        "HALO_DATA_ROOT": str(data_root), "HALO_SAVE_ROOT": str(Path(data_root) / "save"),
        "HALO_HIDDEN_WINDOW": "1", "HALO_EXIT_AFTER": str(seconds), "HALO_NO_VSYNC": "1",
        "HALO_VOLUME": "0", "HALO_NET_ONLINE": "0", "HALO_TELEMETRY": "off",
        # (as the TV: its textures, its HUD; neither changes a shader's text)
        "HALO_METAL_NO_BC": "1", "HALO_HIGH_RES_HUD": "true", "HALO_HIGH_RES_TEXT": "true",
        "HALO_RESOLUTION_SCALING": "720",
        "HALO_METAL_SHADER_CACHE": str(record_path), "HALO_METAL_SHADER_CACHE_RESET": "1",
        "HALO_METAL_SHADER_PACK": "0",
    })
    for name in ("HALO_METAL_SHADER_SALT", "HALO_METAL_PROBE", "HALO_METAL_PROBE_EARLY", "HALO_METAL_LANGUAGE_VERSION"):
        environment.pop(name, None)
    if tour:
        # (the tour's play: the scripted player fires, throws and jumps)
        environment["HALO_SHADER_TOUR"] = "on"
        environment["HALO_TEST_INPUT"] = "bot:1"
    return environment


def generate(maps_dir, only=None, force=False, lock=True, log=print):
    if not APP.is_file():
        raise SystemExit(f"no {APP}: run `ninja macos` first")
    hash_ = translators_hash()
    runs = OUT / hash_
    runs.mkdir(parents=True, exist_ok=True)
    scratch = deploy.SCRATCH / "shader-pack-data"
    names = [name for name in MAPS if not only or name in only]
    todo = [name for name in names if force or not (runs / f"{name}.manifest").is_file()]
    if todo:
        with deploy.HeavyLock(f"shader_pack.py generate ({len(todo)} maps)", enabled=lock, wait_seconds=6 * 3600,
                              poll=20):
            for name in todo:
                if name != "ui" and not (Path(maps_dir) / f"{name}.map").is_file():
                    log(f"{name}: no {name}.map in {maps_dir}; skipped")
                    continue
                data_root = map_data_root(scratch, name, maps_dir)
                record_path = runs / f"{name}.manifest.part"
                log_path = runs / f"{name}.log"
                seconds = UI_SECONDS if name == "ui" else TOUR_SECONDS
                started = time.monotonic()
                with open(log_path, "w") as output:
                    result = subprocess.run([str(APP)], env=run_environment(record_path, data_root, name != "ui",
                                                                            seconds),
                                            stdout=output, stderr=subprocess.STDOUT, timeout=seconds + 300)
                text = log_path.read_text(errors="replace")
                done = name == "ui" or "shader tour: done" in text
                shaders = len([1 for kind, _ in parse(record_path.read_bytes())[2] if kind in (1, 2)]) \
                    if record_path.is_file() else 0
                log(f"{name}: exit {result.returncode}, {time.monotonic() - started:.0f} s, {shaders} shaders, "
                    f"tour {'done' if done else 'NOT finished'} ({log_path})")
                if record_path.is_file() and shaders:
                    record_path.replace(runs / f"{name}.manifest")
    merge(runs, names, hash_, log)


def merge(runs, names, hash_, log=print):
    """the runs' records into the pack: each map's set (a run's records of
    other maps, the menus' it starts in, are left to their own run)"""
    merged = []
    for name in names:
        path = Path(runs) / f"{name}.manifest"
        if not path.is_file():
            log(f"{name}: not recorded")
            continue
        maps = maps_of(parse(path.read_bytes())[2])
        shaders, pipelines = maps.get(name, ([], []))
        if name == "ui":
            # (the menus' run has no map but the menus)
            for other in maps.values():
                shaders = shaders + [s for s in other[0] if s not in shaders]
                pipelines = pipelines + [p for p in other[1] if p not in pipelines]
        merged.append((name, shaders, pipelines))
        log(f"{name}: {len(shaders)} shaders, {len(pipelines)} pipelines")
    distinct = write_pack(PACK, merged, f"translators {hash_}")
    log(f"{PACK}: {len(merged)} maps, {distinct} distinct shaders, {PACK.stat().st_size / 1024:.0f} KiB "
        f"(expanded {sum(len(s[1]) for _, shaders, _ in merged for s in shaders) / 1024:.0f} KiB of sources)")


# ---------- bundle, status

def pack_state(pack=PACK, root=ROOT):
    """(usable, why)"""
    if not Path(pack).is_file():
        return False, f"no pack ({pack}): run tools/shader_pack.py generate"
    try:
        key, build, _ = parse(Path(pack).read_bytes())
    except (ValueError, zlib.error) as error:
        return False, f"{pack} is damaged: {error}"
    if key != PACK_KEY:
        return False, f"{pack} is of another compiler setting ({key})"
    if build != pack_build(root):
        return False, f"{pack} is of other shader translators ({build}; this tree's {pack_build(root)}): " \
                      "run tools/shader_pack.py generate"
    return True, f"{pack} is this tree's ({build})"


def bundle(app, pack=PACK, log=print):
    """the pack into the app as metal_shaders.pack if it is this tree's;
    else none (the app prepares only from its own record). The bytes added"""
    target = Path(app) / "metal_shaders.pack"
    usable, why = pack_state(pack)
    if not usable:
        target.unlink(missing_ok=True)
        log(f"shader pack: not bundled: {why}")
        return 0
    shutil.copyfile(pack, target)
    log(f"shader pack: bundled ({target.stat().st_size / 1024:.0f} KiB): {why}")
    return target.stat().st_size


# ---------- coverage

def coverage(pack, manifests, log=print):
    """of the shaders each record has (a real run's: the TV's or a
    benchmark's own record), how many the pack has, by map"""
    _, _, records = parse(Path(pack).read_bytes())
    pack_maps = maps_of(records)
    every = {shader for shaders, _ in pack_maps.values() for shader in shaders}
    results = []
    for path in manifests:
        key, build, run_records = parse(Path(path).read_bytes())
        run_maps = maps_of(run_records)
        if set(run_maps) <= {""}:
            run_maps = {"(no map records)": ([s for s in maps_of(run_records).get("", ([], []))[0]], [])}
        for name, (shaders, pipelines) in run_maps.items():
            if not shaders:
                continue
            same_map = set(pack_maps.get(name, ([], []))[0])
            in_map = sum(shader in same_map for shader in shaders)
            anywhere = sum(shader in every for shader in shaders)
            results.append({"record": str(path), "map": name, "shaders": len(shaders), "in_map_set": in_map,
                            "in_pack": anywhere})
            log(f"{Path(path).name} [{key.split('|')[0]}] map {name or '(none)'}: {len(shaders)} shaders, "
                f"{anywhere} in the pack ({100.0 * anywhere / len(shaders):.0f}%), {in_map} in its map's set "
                f"({100.0 * in_map / len(shaders):.0f}%)")
    return results


def dump(pack, log=print):
    key, build, records = parse(Path(pack).read_bytes())
    log(f"{pack}: key {key!r}, build {build!r}")
    for name, (shaders, pipelines) in maps_of(records).items():
        log(f"  {name or '(none)'}: {len(shaders)} shaders "
            f"({sum(1 for s in shaders if s[0])} pixel), {len(pipelines)} pipelines")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("generate")
    p.add_argument("--maps", default=str(deploy.SCRATCH / "halo_data" / "maps"))
    p.add_argument("--only", help="comma-separated maps (default all)")
    p.add_argument("--force", action="store_true", help="run again maps already recorded for these translators")
    p.add_argument("--no-lock", action="store_true")
    sub.add_parser("status")
    p = sub.add_parser("bundle")
    p.add_argument("app")
    p = sub.add_parser("coverage")
    p.add_argument("manifests", nargs="+")
    p.add_argument("--pack", default=str(PACK))
    p = sub.add_parser("dump")
    p.add_argument("pack", nargs="?", default=str(PACK))
    args = parser.parse_args(argv)
    if args.command == "generate":
        generate(args.maps, set(args.only.split(",")) if args.only else None, args.force, not args.no_lock)
    elif args.command == "status":
        usable, why = pack_state()
        print(why)
        return 0 if usable else 1
    elif args.command == "bundle":
        bundle(args.app)
    elif args.command == "coverage":
        coverage(args.pack, args.manifests)
    elif args.command == "dump":
        dump(args.pack)
    return 0


if __name__ == "__main__":
    sys.exit(main())
