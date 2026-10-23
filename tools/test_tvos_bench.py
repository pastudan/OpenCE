"""tools/tvos_bench.py's frame-log statistics and scene windows, and that the
host's frame log (HALO_FRAME_LOG) is off by default and cheap."""
from pathlib import Path

import pytest

import tvos_bench as bench

ROOT = Path(__file__).resolve().parent.parent


# ---------- the frame log and its statistics

HEADER = ("serial,t_ms,wall_ms,host_ms,compile_ms,wait_inflight_ms,wait_drawable_ms,wait_readback_ms,sleep_ms,"
          "guest_ms,gpu_ms,gpu_buffers,gpu_failed,gpu_start_ms,gpu_end_ms,presented_ms,draws,passes,copies,compiles,"
          "fog_draws,transparent_draws")


def frame_log(frames, events=()):
    lines = ["# OpenCE frame log, level 2, %d frames" % len(frames)]
    lines += [f"# event,{t},{serial},{kind},{detail}" for t, serial, kind, detail in events]
    lines.append(HEADER)
    for serial, (t, wall, gpu, fog, compile_ms) in enumerate(frames, 1):
        lines.append(f"{serial},{t},{wall},0.5,{compile_ms},0,1,0,0,{wall - 1.5},{gpu},1,0,{t},{t + gpu},{t + 20},"
                     f"200,4,0,{1 if compile_ms else 0},{fog},10")
    return "\n".join(lines) + "\n"


def test_parse_frame_log():
    events, rows = bench.parse_frame_log(frame_log([(10.0, 16.7, 5.0, 0, 0), (26.7, 16.7, 5.0, 0, 0)],
                                                   [(1.0, 1, "map-open", "a10.map")]))
    assert events == [{"t_ms": 1.0, "serial": 1, "kind": "map-open", "detail": "a10.map"}]
    assert rows[1]["serial"] == 2 and rows[1]["fog_draws"] == 0 and rows[1]["gpu_ms"] == 5.0
    assert rows[0]["present_interval_ms"] is None and rows[1]["present_interval_ms"] == pytest.approx(16.7)


def test_percentiles_are_nearest_rank():
    values = list(range(1, 101))
    assert bench.percentile(values, 0.50) == 50
    assert bench.percentile(values, 0.95) == 95
    assert bench.percentile(values, 0.99) == 99
    assert bench.percentile([7.0], 0.99) == 7.0
    assert bench.percentile([], 0.5) is None
    stats = bench.series_stats([10.0, 20.0, 40.0, 60.0, None])
    assert stats["n"] == 4 and stats["max"] == 60.0
    assert (stats["over_16.7"], stats["over_33.3"], stats["over_50"]) == (3, 2, 1)


def test_opening_window_starts_at_the_fog_and_counts_frames():
    # 5 s of no fog, then 40 s of fog at 60 fps with one 120 ms stall
    frames, t = [], 0.0
    for index in range(300):
        t += 16.7
        frames.append((t, 16.7, 2.0, 0, 0))
    for index in range(2400):
        wall = 120.0 if index == 100 else 16.7
        t += wall
        frames.append((t, wall, 9.0, 32, 110.0 if index == 100 else 0))
    analysis = bench.analyze("opening", frame_log(frames, [(5.0, 1, "map-open", "a10.map")]))
    full = analysis["full"]
    assert "frame 301" in analysis["window"]
    assert full["series"]["wall_ms"]["over_50"] == 1 and full["series"]["wall_ms"]["max"] == 120.0
    assert full["seconds"] == pytest.approx(30.0, abs=0.2)
    assert full["mean_fog_draws"] == 32
    assert full["stalls"][0]["compile_ms"] == 110.0
    # the steady window leaves out the scene's first 5 s (the stall is in them)
    assert analysis["steady"]["series"]["wall_ms"]["over_50"] == 0


def test_bridge_window_starts_after_the_saved_state():
    frames = [(16.7 * (i + 1), 16.7, 3.0, 0, 0) for i in range(3000)]
    analysis = bench.analyze("bridge", frame_log(frames, [(1.0, 1, "map-open", "a10.map"),
                                                          (900.0, 50, "core-open", bench.CORE_NAME)]))
    assert "frame 52" in analysis["window"]
    assert not bench.analyze("bridge", frame_log(frames))["full"]["frames"]
    assert not bench.analyze("opening", frame_log(frames))["full"]["frames"]


def test_plan_shares_a_salt_between_a_cold_run_and_its_warm_run():
    runs = bench.plan(["opening", "bridge"], [1080, 2160], ["cold", "warm"], 2)
    assert len(runs) == 16
    assert [r["mode"] for r in runs[:2]] == ["cold", "warm"] and runs[0]["salt"] == runs[1]["salt"]
    assert len({r["salt"] for r in runs}) == 8
    environment = bench.run_environment(runs[0])
    assert environment["HALO_FRAME_LOG"] == "1" and environment["HALO_METAL_SHADER_SALT"] == runs[0]["salt"]
    assert environment["HALO_RESOLUTION_SCALING"] == "1080"


def test_markdown_labels_targets():
    result = {"target": "simulator", "scene": "opening", "resolution": 2160, "mode": "cold", "repeat": 1,
              "valid": True, "analysis": bench.analyze("opening", frame_log(
                  [(16.7 * (i + 1), 16.7, 9.0, 32, 0) for i in range(100)], [(0.0, 0, "map-open", "a10.map")]))}
    table = bench.markdown([result])
    assert "| tvOS simulator | a10 cutscene (partial) | 2160 | cold | 1 |" in table
    assert "A12" not in table


# ---------- the instrumentation in the host

def test_frame_log_is_opt_in_and_cheap():
    source = (ROOT / "port/macos/host/host_frame_log.c").read_text()
    header = (ROOT / "port/macos/host/host_frame_log.h").read_text()
    metal = (ROOT / "port/macos/host/host_metal.m").read_text()
    assert 'getenv("HALO_FRAME_LOG")' in source
    # one record a frame in memory, written at exit: no line a frame
    assert "atexit(finish)" in source
    present = source[source.index("void host_frame_log_present("):source.index("static uint64_t ns_of_seconds")]
    assert "host_log(" not in present.replace("interval_line(serial)", "")
    # the breakdown's clock reads only with the breakdown on, on the main thread
    assert "host_frame_log_level >= 2 && pthread_main_np()" in header
    assert "waitUntilCompleted" not in source
    # every timed wait in the renderer is gated
    for wait in ("dispatch_semaphore_wait(frames_free", "[metal.layer nextDrawable]", "[submitted waitUntilCompleted]"):
        assert metal.count(wait) == 2, wait


def test_shader_salt_is_diagnostic_and_off_by_default():
    metal = (ROOT / "port/macos/host/host_metal.m").read_text()
    assert 'getenv("HALO_METAL_SHADER_SALT")' in metal
    assert "if (salt)" in metal


def test_shader_salt_changes_the_compiled_code_not_only_the_source():
    # (a comment alone leaves the AIR as it was: the system's cache of
    # pipelines still finds them, and a "cold" run is only half cold)
    metal = (ROOT / "port/macos/host/host_metal.m").read_text()
    text = metal[metal.index("static NSString *shader_text("):metal.index("unsigned int host_metal_shader(")]
    assert "withString:shader_entry_point(fragment)" in text
    shader = metal[metal.index("unsigned int host_metal_shader("):metal.index("static MTLVertexDescriptor *vertex_descriptor_make(")]
    assert "newFunctionWithName:shader_entry_point(fragment)" in shader
    settings = metal[metal.index("static void shader_settings_read("):metal.index("static const char *shader_salt(")]
    assert '@"fragment_main"' in settings and '@"vertex_main"' in settings


def test_cold_runs_empty_the_shader_cache_and_primed_runs_miss_the_system_cache():
    runs = bench.plan(["b30"], [2160], ["cold", "warm", "primed"], 1)
    cold, warm, primed = runs
    assert cold["salt"] == warm["salt"] and primed["salt"] != cold["salt"]
    environments = [bench.run_environment(run) for run in runs]
    assert {e["HALO_METAL_SHADER_CACHE"] for e in environments} == {bench.SHADER_CACHE}
    assert [e["HALO_METAL_SHADER_CACHE_RESET"] for e in environments] == ["1", "0", "0"]
    metal = (ROOT / "port/macos/host/host_metal.m").read_text()
    assert 'getenv("HALO_METAL_SHADER_CACHE_RESET")' in metal


def test_new_level_scene_starts_after_its_map_opens():
    frames = [(16.7 * (i + 1), 16.7, 5.0, 0, 0) for i in range(4000)]
    events = [(10.0, 1, "map-open", "ui.map"), (20.0, 3, "map-open", "b40.map")]
    window, description = bench.scene_window("b40", *bench.parse_frame_log(frame_log(frames, events)))
    assert window[0]["serial"] == 4 and description.startswith("b40 (a new level): 60 s")
    _, reason = bench.scene_window("a30", *bench.parse_frame_log(frame_log(frames, events)))
    assert "a30.map was never opened" in reason


def test_nopack_runs_are_cold_without_the_bundled_pack():
    cold, nopack = bench.plan(["b30"], [2160], ["cold", "nopack"], 1)
    assert cold["salt"] != nopack["salt"]
    environments = [bench.run_environment(run) for run in (cold, nopack)]
    assert [e["HALO_METAL_SHADER_CACHE_RESET"] for e in environments] == ["1", "1"]
    assert "HALO_METAL_SHADER_PACK" not in environments[0] and environments[1]["HALO_METAL_SHADER_PACK"] == "0"
    # (a pack named for every run, as on the Mac, is left out of nopack's)
    extra = {"HALO_METAL_SHADER_PACK": "/x/metal_shaders.pack"}
    assert bench.run_environment(cold, extra=extra)["HALO_METAL_SHADER_PACK"] == "/x/metal_shaders.pack"
    assert bench.run_environment(nopack, extra=extra)["HALO_METAL_SHADER_PACK"] == "0"


def test_split_scene_starts_at_two_views_that_hold():
    frames = [(16.7 * (i + 1), 16.7, 5.0, 0, 0) for i in range(4000)]
    events = [(10.0, 1, "map-open", "a10.map"), (100.0, 5, "views", "2"), (150.0, 8, "views", "1"),
              (1000.0, 60, "views", "2")]
    window, description = bench.scene_window("split", *bench.parse_frame_log(frame_log(frames, events)))
    assert window[0]["serial"] == 62 and description.startswith("a10 split screen: 50 s")
    assert bench.SCENES["split"]["init"] == ["player_spawn_count 2", "map_name levels\\a10\\a10"]
    _, reason = bench.scene_window("split", *bench.parse_frame_log(frame_log(frames[:10])))
    assert "two player views were never drawn" in reason


def test_latelag_scene_moves_both_players_between_two_areas():
    scene = bench.SCENES["latelag"]
    assert scene["init"] == bench.SCENES["split"]["init"] and scene["seconds"] >= 600
    commands = scene["env"]["HALO_TEST_COMMANDS"].split(";")
    assert commands[0] == "1 (game_speed 8)" and "226 (game_speed 1)" in commands
    moves = [c for c in commands if "switch_bsp" in c]
    assert len(moves) >= 14 and {c.split()[-1] for c in moves} == {"0)", "1)"}
    # (both players each time: two teleports for each switch)
    assert sum("object_teleport (player1)" in c for c in commands) == len(moves)
    # (each area unseen for longer than the old 30 s idle sweep)
    times = [float(c.split()[0]) for c in moves]
    assert min(b - a for a, b in zip(times, times[1:])) > 30
    frames = [(16.7 * (i + 1), 16.7, 5.0, 0, 0) for i in range(4000)]
    window, description = bench.scene_window("latelag", *bench.parse_frame_log(
        frame_log(frames, [(100.0, 5, "views", "2")])), window_seconds=20)
    assert window and "latelag" in description
    game = (ROOT / "port/linux/game/network_test.c").read_text()
    assert 'config_string("debug.test_commands")' in game and "hs_compile_and_evaluate(command)" in game
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    assert '{ "debug.test_commands", _config_string, "\\"\\"", "HALO_TEST_COMMANDS"' in config


def test_multimap_scene_changes_maps_and_pushes_them():
    scene = bench.SCENES["multimap"]
    commands = scene["env"]["HALO_TEST_COMMANDS"].split(";")
    assert [c for c in commands if "map_name" in c] == [
        "300 (map_name levels\\b30\\b30)", "300 (map_name levels\\b40\\b40)", "300 (map_name levels\\c10\\c10)",
        "300 (map_name levels\\b30\\b30)"]
    assert set(scene["maps"]) == {"b30.map", "b40.map", "c10.map"}
    source = (ROOT / "tools/tvos_bench.py").read_text()
    assert 'SCENES[run["scene"]].get("maps", ())' in source
