"""The play telemetry: the report (tools/tv_telemetry.py) on synthetic
records, the recorder's rotation and slot arithmetic
(port/macos/host/host_telemetry.c) compiled and run, the pull's choice of
files, and what the recorder must not do on the render thread."""
import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tv_telemetry as tele  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "port/macos/host"


def second(t, missed=0, **extra):
    record = {"k": "s", "t": float(t), "dur": 1.0, "game_ticks": int(t * 30), "map": "a10", "bsp": 1,
              "players": 2, "windows": 2, "views": 2, "cinematic": 0, "pos": [[t / 10.0, 5.0, 1.0], [0, 0, 0]],
              "frames": 60 - missed, "fps": 60.0 - missed, "wall_p50": 16.7, "wall_p95": 16.7 + missed * 5,
              "wall_max": 16.7 + missed * 10, "over16": missed, "over33": 0, "over50": 0, "missed": missed,
              "guest_ms": 8.0, "host_ms": 2.0, "gpu_ms": 11.0, "gpu_p50": 11.0, "gpu_p95": 12.0, "gpu_max": 13.0,
              "compile_ms": 0.0, "wait_drawable_ms": 1.0, "wait_inflight_ms": 0.0, "draws": 900, "passes": 9,
              "shadow_draws": 20, "fog_draws": 0, "transparent_draws": 40,
              "view": [{"draws": 450, "passes": 4, "shadow": 10, "fog": 0, "transparent": 20, "ms": 6.0},
                       {"draws": 440, "passes": 4, "shadow": 10, "fog": 0, "transparent": 20, "ms": 6.0},
                       {}, {}, {"draws": 10, "passes": 1, "shadow": 0, "fog": 0, "transparent": 0, "ms": 0.5}],
              "tex_converts": 0, "tex_convert_ms": 0.0, "tex_uploads": 0, "tex_deletes": 0, "stream_mb": 200.0,
              "ring_overflows": 0, "file_reads": 0, "file_ms": 0.0, "manifest_writes": 0,
              "footprint_mb": 900.0 + t * 0.1, "metal_mb": 400.0, "thermal": 0, "active": 1, "render": [3840, 2160],
              "drawable": [3840, 2160], "scale": 1.0, "self_ms": 0.05, "writer_ms": 0.01}
    record.update(extra)
    return record


def synthetic_session(path):
    """30 minutes of two-player a10: smooth for 15, then lag for 20 s of
    every 60, with texture conversions in the laggy seconds, and the thermal
    state going to fair at 20 minutes"""
    lines = [{"k": "session", "v": 1, "t": 0, "start": "20261023T020000Z", "device": "AppleTV11,1, test",
              "refresh_hz": 60.0}]
    for t in range(1800):
        thermal = 1 if t >= 1200 else 0
        if t >= 900 and t % 60 < 20:
            lines.append(second(t, missed=12, tex_converts=25, tex_convert_ms=180.0, guest_ms=9.0, thermal=thermal,
                                tex_deletes=40 if t % 60 == 0 else 0))
            if t % 60 == 5:
                lines.append({"k": "hitch", "t": t + 0.5, "serial": t * 60, "map": "a10", "bsp": 1, "views": 2,
                              "wall_ms": 120.0, "guest_ms": 10.0, "host_ms": 4.0, "gpu_ms": 12.0, "compile_ms": 0.0,
                              "tex_converts": 9, "tex_convert_ms": 95.0, "tex_uploads": 0, "file_ms": 0.0,
                              "manifest_ms": 0.0, "wait_drawable_ms": 1.0, "wait_inflight_ms": 0.0,
                              "event": "", "thermal": thermal})
        else:
            lines.append(second(t, thermal=thermal, gpu_p50=11.0 + thermal * 1.5))
    lines.append({"k": "end", "t": 1800.0, "seconds": 1800})
    path.write_text("\n".join(json.dumps(line) for line in lines) + "\n{\"k\":\"s\",\"t\":18")  # a cut last line
    return path


def test_report_finds_the_periodic_lag_and_its_cause(tmp_path):
    path = synthetic_session(tmp_path / "20261023T020000Z-1.jsonl")
    sessions = tele.load_sessions([tmp_path])
    assert len(sessions) == 1 and len(sessions[0]["seconds"]) == 1800  # the cut line skipped
    play = tele.gameplay(sessions[0]["seconds"])
    stretches = tele.lag_stretches(play)
    assert len(stretches) == 15
    repeat, strength = tele.period([min(s["missed"], 30) for s in play], [s["t"] for s in play])
    assert repeat == 60 and strength > 0.3
    text = tele.report(sessions)
    assert "**Periodic:** the missed slots repeat every ~60 s" in text
    assert "The first begins 15.0 min into the session (0 in its first half, 15 in its second)" in text
    # the verdict names the conversions, and the hitch's cause
    verdict = text[text.index("## Verdict"):text.index("## Where the time goes")]
    assert "**textures converted**" in verdict or "**conversion ms**" in verdict
    assert "texture conversion (DXT on the host) (15)" in verdict
    assert "fair" in verdict
    # split screen, per view; thermal by state; trends
    assert "### Split screen: per view" in text and "| 2 | 1800 | player 2 |" in text
    assert "| fair | 600 |" in text
    assert "Memory footprint:" in text and "+6.000 MB/min" in text
    assert "## The recorder's own cost" in text
    assert path.name[:-6] in text


def test_report_of_no_lag_and_no_sessions(tmp_path):
    lines = [second(t) for t in range(100)]
    (tmp_path / "a.jsonl").write_text("\n".join(json.dumps(line) for line in lines))
    text = tele.report(tele.load_sessions([tmp_path]))
    assert "No laggy seconds" in text and "No change: nominal throughout" in text
    assert tele.report([]).endswith("No sessions.\n")


def test_hitch_cause_is_the_largest_part():
    assert tele.hitch_cause({"compile_ms": 40, "guest_ms": 12})[0] == "shader compile"
    assert tele.hitch_cause({"guest_ms": 70, "file_ms": 3})[0] == "the game's own work (guest)"
    assert tele.hitch_cause({"wait_drawable_ms": 50, "guest_ms": 9})[0].startswith("waiting for a drawable")


def test_pull_copies_new_and_grown_files_only(tmp_path):
    (tmp_path / "old.jsonl").write_text("x" * 10)
    (tmp_path / "grown.jsonl").write_text("x" * 10)
    assert tele.to_pull({"old.jsonl": 10, "grown.jsonl": 20, "new.jsonl": 5}, tmp_path) == ["grown.jsonl", "new.jsonl"]


def test_pull_reads_from_the_app_container_only():
    source = (ROOT / "tools/tv_telemetry.py").read_text()
    assert 'copy_args("from"' in source and 'copy_args("to"' not in source
    assert tele.REMOTE == "Library/Caches/OpenCE/telemetry"


RECORDER_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include "host_telemetry.h"

void host_log(const char *f, ...) { (void)f; }
void host_telemetry_platform_sample(struct host_telemetry_platform *p) { memset(p, 0, sizeof(*p)); }
void host_telemetry_platform_start(void) {}
double host_telemetry_platform_refresh_hz(void) { return 60.0; }
void host_telemetry_platform_describe(char *t, unsigned int s) { snprintf(t, s, "test"); }
uint64_t host_metal_allocated_bytes(void) { return 0; }

static void make(const char *directory, const char *name, int bytes)
{
    char path[1024];
    FILE *file;

    snprintf(path, sizeof(path), "%s/%s", directory, name);
    file = fopen(path, "w");
    for (int i = 0; i < bytes; i++)
        fputc('x', file);
    fclose(file);
}

static int exists(const char *directory, const char *name)
{
    char path[1024];
    struct stat status;

    snprintf(path, sizeof(path), "%s/%s", directory, name);
    return stat(path, &status) == 0;
}

int main(int argc, char **argv)
{
    const char *directory = argv[1];
    const uint64_t slot = 16666667;

    assert(host_telemetry_missed_slots(16000000, slot) == 0);
    assert(host_telemetry_missed_slots(20000000, slot) == 0);
    assert(host_telemetry_missed_slots(33333333, slot) == 1);
    assert(host_telemetry_missed_slots(50000000, slot) == 2);
    assert(host_telemetry_missed_slots(10, 0) == 0);

    make(directory, "20261001T000000Z-1.jsonl", 100);
    make(directory, "20261002T000000Z-1.jsonl", 100);
    make(directory, "20261003T000000Z-1.jsonl", 100);
    make(directory, "20261004T000000Z-1.jsonl", 100);
    make(directory, "keep.txt", 5000);
    /* (fewer than keep stay, room for the new one: two of four go) */
    assert(host_telemetry_rotate(directory, 3, 1 << 20) == 2);
    assert(!exists(directory, "20261001T000000Z-1.jsonl") && !exists(directory, "20261002T000000Z-1.jsonl"));
    assert(exists(directory, "20261003T000000Z-1.jsonl") && exists(directory, "keep.txt"));
    /* by bytes: at most 150 of them */
    assert(host_telemetry_rotate(directory, 40, 150) == 1);
    assert(!exists(directory, "20261003T000000Z-1.jsonl") && exists(directory, "20261004T000000Z-1.jsonl"));
    assert(host_telemetry_rotate(directory, 40, 1 << 20) == 0);
    /* a session's parts count as one session, and go oldest part first */
    make(directory, "20261005T000000Z-2.p001.jsonl", 100);
    make(directory, "20261005T000000Z-2.p002.jsonl", 100);
    make(directory, "20261005T000000Z-2.p003.jsonl", 100);
    make(directory, "20261006T000000Z-3.p001.jsonl", 100);
    assert(host_telemetry_rotate(directory, 3, 1 << 20) == 1);
    assert(!exists(directory, "20261004T000000Z-1.jsonl") && exists(directory, "20261005T000000Z-2.p001.jsonl"));
    assert(host_telemetry_rotate(directory, 2, 1 << 20) == 3);
    assert(!exists(directory, "20261005T000000Z-2.p003.jsonl") && exists(directory, "20261006T000000Z-3.p001.jsonl"));
    make(directory, "20261006T000000Z-3.p002.jsonl", 100);
    make(directory, "20261006T000000Z-3.p003.jsonl", 100);
    /* by bytes, a session's oldest parts first */
    assert(host_telemetry_rotate(directory, 40, 150) == 2);
    assert(!exists(directory, "20261006T000000Z-3.p002.jsonl") && exists(directory, "20261006T000000Z-3.p003.jsonl"));
    puts("ok");
    return 0;
}
'''

# a session recorded through the recorder itself, in parts of 4 KB
PARTS_HARNESS = r'''
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "host_frame_log.h"
#include "host_telemetry.h"

void host_log(const char *f, ...) { (void)f; }
void host_telemetry_platform_sample(struct host_telemetry_platform *p) { memset(p, 0, sizeof(*p)); }
void host_telemetry_platform_start(void) {}
double host_telemetry_platform_refresh_hz(void) { return 60.0; }
void host_telemetry_platform_describe(char *t, unsigned int s) { snprintf(t, s, "test"); }
uint64_t host_metal_allocated_bytes(void) { return 0; }

int main(void)
{
    struct host_frame_accumulators frame;
    uint64_t now = host_frame_log_now();

    memset(&frame, 0, sizeof(frame));
    host_telemetry_start();
    for (uint64_t serial = 1; serial <= 60 * 20; serial++)
    {
        now += 16666667;
        host_telemetry_frame(serial, now, serial % 300 ? 16666667 : 80000000, &frame);
    }
    host_telemetry_finish();
    puts("ok");
    return 0;
}
'''


def test_recorder_rotation_and_slots(tmp_path):
    harness = tmp_path / "harness.c"
    harness.write_text(RECORDER_HARNESS)
    program = tmp_path / "harness"
    subprocess.run(["clang", "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", f"-I{HOST}",
                    f"-I{ROOT / 'port/macos/guest'}", str(harness), str(HOST / "host_telemetry.c"),
                    str(HOST / "host_frame_log.c"), "-o", str(program)], check=True)
    folder = tmp_path / "telemetry"
    folder.mkdir()
    assert subprocess.run([str(program), str(folder)], capture_output=True, text=True, check=True).stdout == "ok\n"


def test_recorder_writes_a_session_in_self_describing_parts(tmp_path):
    harness = tmp_path / "parts.c"
    harness.write_text(PARTS_HARNESS)
    program = tmp_path / "parts"
    subprocess.run(["clang", "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", f"-I{HOST}",
                    f"-I{ROOT / 'port/macos/guest'}", str(harness), str(HOST / "host_telemetry.c"),
                    str(HOST / "host_frame_log.c"), "-o", str(program)], check=True)
    folder = tmp_path / "telemetry"
    environment = dict(os.environ, HALO_TELEMETRY="on", HALO_TELEMETRY_DIR=str(folder), HALO_TELEMETRY_PART_KB="4")
    assert subprocess.run([str(program)], capture_output=True, text=True, check=True, env=environment,
                          cwd=tmp_path).stdout == "ok\n"
    parts = sorted(folder.glob("*.jsonl"))
    assert len(parts) >= 3
    stem = parts[0].name[:-len(".p001.jsonl")]
    assert [p.name for p in parts] == [f"{stem}.p{n:03d}.jsonl" for n in range(1, len(parts) + 1)]
    for number, part in enumerate(parts, 1):
        records = [json.loads(line) for line in part.read_text().splitlines()]
        assert records[0]["k"] == "session" and records[0]["part"] == number
        assert records[0]["start"] == stem.split("-")[0]
        # (a part ends at a whole line, and stays under its size but for one second's lines)
        assert part.read_text().endswith("\n") and part.stat().st_size < 4096 + 8192
    records = [json.loads(line) for part in parts for line in part.read_text().splitlines()]
    times = [r["t"] for r in records if r["k"] == "s"]
    assert times == sorted(times) and len(times) >= 18
    assert records[-1]["k"] == "end" and sum(r["k"] == "hitch" for r in records) == 4


def test_recorder_never_syncs_or_writes_on_the_render_thread():
    source = (HOST / "host_telemetry.c").read_text()
    assert "fsync" not in source.replace("no fsync", "")
    # (the file is written only by the writer's functions, on its queue)
    writes = [line for line in source.splitlines() if "fwrite(" in line or "fopen(" in line]
    assert len(writes) == 2
    assert "dispatch_async_f(telemetry.queue, chunk, writer_write)" in source
    assert "dispatch_queue_create(\"opence.telemetry\", DISPATCH_QUEUE_SERIAL)" in source
    # its GPU times come from the frame log's ring, a second late: no wait on the GPU
    assert "waitUntilCompleted" not in source and "host_frame_log_gpu_of(" in source


def test_recorder_defaults_and_settings():
    source = (HOST / "host_telemetry.c").read_text()
    wanted = source[source.index("int host_telemetry_wanted("):source.index("static void live_start(")]
    assert "#ifdef HALO_TVOS\n\treturn 1;\n#else\n\treturn 0;" in wanted
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    assert '{ "debug.telemetry", _config_string, "\\"auto\\"", "HALO_TELEMETRY"' in config
    assert '{ "debug.telemetry_live", _config_string, "\\"\\"", "HALO_TELEMETRY_LIVE"' in config
    # the benchmark's runs leave it off unless asked
    bench = (ROOT / "tools/tvos_bench.py").read_text()
    assert '"HALO_TELEMETRY": "off"' in bench


def test_telemetry_game_layout_matches_between_guest_and_host():
    protocol = (ROOT / "port/macos/guest/metal_protocol.h").read_text()
    struct = protocol[protocol.index("struct metal_telemetry_game"):]
    struct = struct[:struct.index("};")]
    assert "int game_ticks, local_players, windows, bsp, cinematic, reserved;" in struct
    assert "float positions[METAL_TELEMETRY_VIEWS][3];" in struct and "char map[32];" in struct
    main = (ROOT / "source/main/main.c").read_text()
    assert "metal_device_telemetry_game(" in main
    profile = (ROOT / "source/rasterizer/xbox/rasterizer_xbox_profile.c").read_text()
    assert "metal_device_telemetry_window(global_window_parameters.window_index)" in profile
