#!/usr/bin/env python3
"""Fixed-scene frame-time benchmark of the TV game (port/tvos/PERFORMANCE.md).

    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    # the saved game states, made once on the Mac build (same guest as the TV's):
    python3 tools/tvos_bench.py make-core --maps DIR [--scene b30|cryo]
    # runs (each target takes the heavy-job lock for its whole session; --no-lock if held):
    python3 tools/tvos_bench.py run --target simulator --maps DIR --out OUT
    python3 tools/tvos_bench.py run --target device --out OUT          # the Apple TV: ask its owner first
    python3 tools/tvos_bench.py run --target mac --maps DIR --out OUT  # validation only: not a TV number
    # one frame log, or a results folder's tables again:
    python3 tools/tvos_bench.py analyze frame_log.csv --scene opening
    python3 tools/tvos_bench.py report OUT

Scenes (no controller input: the most repeatable):
- opening ("a10 cutscene (partial)"): a fresh launch into a10 (init.txt
  "map_name a10"), the first part of the opening cinematic. NOT gameplay; kept
  for comparison, no longer run by default. Measured: the 30 s from the first frame of the first stretch of
  >= 5 s in which every frame has >= 20 fog-plane draws (the fog sequence;
  found from the frame log's counts, not from times, so a slower load does
  not shift it; the moment of fog the first frames flash is skipped).
- bridge: a launch that loads a saved game state at the bridge (BSP 1)
  (init.txt "map_name a10" + "core_load_name_at_startup tvbench_bridge.bin";
  the state is made by make-core: a10's opening run to its end, the player
  out of the cryo tube and put at the scenario's cheat_bridge_flag). Measured:
  the 35 s from the second frame after the state is read (the frame that
  reads it is the load).
- a10play ("a10 cryo bay"): a fresh launch into a10 left to run ~285 s: the
  whole opening cinematic, then the cryo bay's first playable (tutorial)
  section. Measured: 50 s from the first 5 s stretch of >= 40 fog-plane draws
  a frame (only the cryo bay has that many; the cinematic has <= 32).
- split ("a10 split screen (2 players)"): a fresh launch into a10 with two
  local players (init.txt "player_spawn_count 2" before "map_name
  levels\a10\a10": the console sets the players the next map is made for, as
  the co-op menu does; no controller needed, the second player stands
  still). The opening cinematic is one view; the measured window is the
  50 s from the first frame drawn as two views (the frame log's "views 2"
  event, from the play telemetry's game state, host_telemetry.h), the cryo
  bay's tutorial with both players. A saved state cannot start it: one
  saved with one player does not load into two.
- b30 ("b30 beach"): a launch that loads a saved state on The Silent
  Cartographer's beach at the mission's start (made 55 s into a fresh b30
  launch: the insertion cinematic over, the marines' firefight on). Measured:
  60 s from the second frame after the state is read.

Cold and warm. cold: the first launch with a new HALO_METAL_SHADER_SALT (a
comment added to every Metal shader and its name in the shader's function:
nothing is found in the system's compiled-shader caches of earlier launches,
which otherwise survive app updates), and the Metal host's own shader cache
emptied; its first-use compile stalls are in the numbers. warm: the next
launch with the same salt (the system's caches now hold every shader the
scene compiles). primed (--modes cold,warm,primed): a launch with a salt of
its own again, so the system's caches miss, but with the host's shader cache
as the cold launch left it: what a launch after a system or app update costs
once the game has been played (the host compiles the recorded shaders on
other threads from the start). The "steady" window of a run also leaves out its first
WARMUP_SECONDS (5 s) of the scene.

Every result is labelled with its target: "A12 device", "tvOS simulator" or
"Mac (not a TV)". Simulator and Mac numbers are not Apple TV numbers.
"""
import argparse
import csv
import datetime
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tvos_deploy as deploy  # noqa: E402

ROOT = deploy.ROOT
SCRATCH = deploy.SCRATCH
CORE_NAME = "tvbench_bridge.bin"
WARMUP_SECONDS = 5.0
SCENES = {
    # init.txt lines, seconds to run (HALO_EXIT_AFTER), the measured window
    "opening": {"init": ["map_name a10"], "seconds": 80, "window_seconds": 30.0},
    "bridge": {"init": ["map_name a10", f"core_load_name_at_startup {CORE_NAME}"], "seconds": 55,
               "window_seconds": 35.0},
    # b30 (The Silent Cartographer): the beach at the mission's start (the
    # Pelican has landed, the marines are out, the AI's firefight on the
    # beach), a saved state made 55 s into a fresh b30 launch; no input. (A
    # test_input bot was not repeatable: its motion is timed from the app's
    # launch, so a slower load shifts it; with no input, draws per 10 s agree
    # within about 5% between runs on the Mac.)
    "b30": {"init": ["map_name b30", "core_load_name_at_startup tvbench_b30.bin"], "seconds": 80,
            "window_seconds": 60.0, "core": "tvbench_b30.bin", "map": "b30.map",
            "env": {}},
    # a10 played from a fresh launch to the first playable section after the
    # opening cinematic (the cryo bay's tutorial, ~215 s in), measured there
    "a10play": {"init": ["map_name a10"], "seconds": 285, "window_seconds": 50.0},
    # the same cryo bay from a saved state (make-core --scene cryo: a fresh a10
    # launch saved as the bay's fog comes up, the tutorial's screen blur about
    # to start): ~60 s a run instead of ~285 s
    "cryo": {"init": ["map_name a10", "core_load_name_at_startup tvbench_cryo.bin"], "seconds": 70,
             "window_seconds": 50.0, "core": "tvbench_cryo.bin"},
    # two-player split screen in a10, from a fresh launch (scene_window)
    "split": {"init": ["player_spawn_count 2", "map_name levels\\a10\\a10"], "seconds": 290,
              "window_seconds": 50.0},
}
SCENE_LABEL = {"opening": "a10 cutscene (partial)", "bridge": "a10 bridge (gameplay)",
               "b30": "b30 beach (gameplay)", "a10play": "a10 cryo bay (first gameplay)",
               "cryo": "a10 cryo bay (saved state)", "split": "a10 split screen (2 players)"}
FOG_START_DRAWS = 20
FOG_SUSTAIN_SECONDS = 5.0
SERIES = ("wall_ms", "gpu_ms", "guest_ms", "host_ms", "compile_ms", "wait_drawable_ms", "wait_inflight_ms",
          "wait_readback_ms", "sleep_ms", "present_interval_ms")
THRESHOLDS = (16.7, 33.3, 50.0)
TARGET_LABEL = {"device": "A12 device", "simulator": "tvOS simulator", "mac": "Mac (not a TV)"}


# ---------- the frame log (port/macos/host/host_frame_log.c)

def parse_frame_log(text):
    """(events, rows): events are dicts of t_ms, serial, kind, detail; rows
    the frames' columns as numbers"""
    events, lines = [], []
    for line in text.splitlines():
        if line.startswith("# event,"):
            _, t_ms, serial, kind, detail = line.split(",", 4)
            events.append({"t_ms": float(t_ms), "serial": int(serial), "kind": kind, "detail": detail})
        elif line and not line.startswith("#"):
            lines.append(line)
    rows = []
    for row in csv.DictReader(io.StringIO("\n".join(lines))):
        rows.append({key: (int(value) if key in ("serial", "gpu_buffers", "gpu_failed", "draws", "passes", "copies",
                                                    "compiles", "fog_draws", "transparent_draws",
                                                    "hoisted_buffers")
                           else float(value)) for key, value in row.items()})
    # (when each frame reached the display: the interval between two that did)
    previous = None
    for row in rows:
        presented = row.get("presented_ms", 0.0)
        row["present_interval_ms"] = presented - previous if presented and previous else None
        previous = presented or None
    return events, rows


def scene_window(scene, events, rows, window_seconds=None):
    """the rows of the measured window (scenes above), and a description;
    ([], reason) when the run never reached it"""
    window_seconds = window_seconds or SCENES[scene]["window_seconds"]
    if scene == "opening":
        opened = [e for e in events if e["kind"] == "map-open" and e["detail"].lower() == "a10.map"]
        after = opened[0]["serial"] if opened else 0
        # (the fog held for FOG_SUSTAIN_SECONDS: the opening's first frames
        # flash it for a moment before a black interval)
        start, run_start = None, None
        for index, row in enumerate(rows):
            if row["serial"] <= after or row.get("fog_draws", 0) < FOG_START_DRAWS:
                run_start = None
                continue
            if run_start is None:
                run_start = index
            if row["t_ms"] - rows[run_start]["t_ms"] >= FOG_SUSTAIN_SECONDS * 1000.0:
                start = run_start
                break
        if start is None:
            return [], "no frame with fog-plane draws (the opening's fog never appeared)"
        description = f"opening fog: {window_seconds:g} s from frame {rows[start]['serial']}"
    elif scene == "a10play":
        # (the first stretch of 5 s whose frames all have >= 40 fog-plane
        # draws: the cryo bay, after the cinematic; see PERFORMANCE.md)
        start, run_start = None, None
        for index, row in enumerate(rows):
            if row.get("fog_draws", 0) < 40:
                run_start = None
                continue
            if run_start is None:
                run_start = index
            if row["t_ms"] - rows[run_start]["t_ms"] >= 5000.0:
                start = run_start
                break
        if start is None:
            return [], "the cryo bay (>= 40 fog-plane draws) was never reached"
        description = f"a10 cryo bay: {window_seconds:g} s from frame {rows[start]['serial']}"
    elif scene == "split":
        # (the first frame drawn as two player views, after the cinematic)
        views = [e for e in events if e["kind"] == "views" and e["detail"] == "2"]
        split = None
        for event in views:
            start = next((index for index, row in enumerate(rows) if row["serial"] >= event["serial"] + 2), None)
            # (a moment of two views between the cinematic's shots is not the
            # game: the views must hold for FOG_SUSTAIN_SECONDS)
            later = [e for e in events if e["kind"] == "views" and e["serial"] > event["serial"]]
            if start is not None and (not later or later[0]["t_ms"] - event["t_ms"] >= FOG_SUSTAIN_SECONDS * 1000.0):
                split = start
                break
        if split is None:
            return [], "two player views were never drawn (player_spawn_count 2 did not take)"
        start = split
        description = f"a10 split screen: {window_seconds:g} s from frame {rows[start]['serial']}"
    elif scene in ("bridge", "b30", "cryo"):
        core = SCENES[scene].get("core", CORE_NAME)
        loaded = [e for e in events if e["kind"] == "core-open" and e["detail"] == core]
        if not loaded:
            return [], f"the saved state {core} was never read"
        start = next((index for index, row in enumerate(rows) if row["serial"] >= loaded[0]["serial"] + 2), None)
        if start is None:
            return [], "no frames after the saved state was read"
        description = f"{scene}: {window_seconds:g} s from frame {rows[start]['serial']}"
    else:
        raise ValueError(scene)
    begin = rows[start]["t_ms"]
    window = [row for row in rows[start:] if row["t_ms"] - begin <= window_seconds * 1000.0]
    if window and window[-1]["t_ms"] - begin < window_seconds * 1000.0 * 0.9:
        description += f" (cut short: the run ended {(window[-1]['t_ms'] - begin) / 1000:.1f} s in)"
    return window, description


def percentile(values, fraction):
    """nearest rank, as host_frame_log.c's"""
    if not values:
        return None
    ordered = sorted(values)
    rank = int(fraction * len(ordered) + 0.999999)
    return ordered[min(max(rank, 1), len(ordered)) - 1]


def series_stats(values):
    values = [value for value in values if value is not None]
    if not values:
        return None
    return {
        "n": len(values),
        "mean": round(sum(values) / len(values), 3),
        "p50": round(percentile(values, 0.50), 3),
        "p95": round(percentile(values, 0.95), 3),
        "p99": round(percentile(values, 0.99), 3),
        "max": round(max(values), 3),
        **{f"over_{threshold:g}": sum(1 for value in values if value > threshold) for threshold in THRESHOLDS},
    }


def window_stats(rows):
    """each series' statistics, and the window's totals"""
    result = {"frames": len(rows), "series": {}}
    if not rows:
        return result
    for name in SERIES:
        values = [row.get(name) for row in rows]
        # (a frame whose GPU time never came: no command buffer completed for it)
        if name == "gpu_ms":
            values = [row["gpu_ms"] for row in rows if row.get("gpu_buffers")]
        result["series"][name] = series_stats(values)
    result["seconds"] = round((rows[-1]["t_ms"] - rows[0]["t_ms"]) / 1000.0, 2)
    result["compile_total_ms"] = round(sum(row.get("compile_ms", 0.0) for row in rows), 1)
    result["compiles"] = sum(row.get("compiles", 0) for row in rows)
    result["gpu_failed"] = sum(row.get("gpu_failed", 0) for row in rows)
    result["mean_draws"] = round(sum(row.get("draws", 0) for row in rows) / len(rows), 1)
    result["mean_fog_draws"] = round(sum(row.get("fog_draws", 0) for row in rows) / len(rows), 1)
    result["mean_passes"] = round(sum(row.get("passes", 0) for row in rows) / len(rows), 2)
    result["mean_copies"] = round(sum(row.get("copies", 0) for row in rows) / len(rows), 2)
    result["stalls"] = sorted(({"serial": row["serial"], "wall_ms": row["wall_ms"],
                                "compile_ms": row.get("compile_ms", 0.0), "gpu_ms": row.get("gpu_ms", 0.0),
                                "guest_ms": row.get("guest_ms", 0.0), "wait_drawable_ms": row.get("wait_drawable_ms", 0.0)}
                               for row in rows if row["wall_ms"] > 50.0), key=lambda stall: -stall["wall_ms"])[:8]
    return result


def analyze(scene, text):
    events, rows = parse_frame_log(text)
    window, description = scene_window(scene, events, rows)
    analysis = {"scene": scene, "window": description, "events": events, "all_frames": len(rows),
                "full": window_stats(window)}
    if window:
        begin = window[0]["t_ms"]
        analysis["steady"] = window_stats([row for row in window if row["t_ms"] - begin >= WARMUP_SECONDS * 1000.0])
        analysis["run_compile_total_ms"] = round(sum(row.get("compile_ms", 0.0) for row in rows), 1)
        analysis["run_over_50"] = sum(1 for row in rows[1:] if row["wall_ms"] > 50.0)
    return analysis


# ---------- tables

def _fmt(value, digits=1):
    return "—" if value is None else f"{value:.{digits}f}"


def markdown(results):
    """the results' tables: frame times, and where the frame's time goes"""
    lines = ["| target | scene | res | mode | rep | frames | wall p50 | p95 | p99 | max | >16.7 | >33.3 | >50 | "
             "GPU p50 | GPU p95 | GPU max | compile ms | valid |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for result in results:
        stats = (result.get("analysis") or {}).get("full") or {}
        wall = (stats.get("series") or {}).get("wall_ms") or {}
        gpu = (stats.get("series") or {}).get("gpu_ms") or {}
        lines.append(
            f"| {TARGET_LABEL.get(result['target'], result['target'])} | {SCENE_LABEL.get(result['scene'], result['scene'])} | {result['resolution']} | "
            f"{result['mode']} | {result['repeat']} | {stats.get('frames', 0)} | {_fmt(wall.get('p50'))} | "
            f"{_fmt(wall.get('p95'))} | {_fmt(wall.get('p99'))} | {_fmt(wall.get('max'))} | "
            f"{wall.get('over_16.7', '—')} | {wall.get('over_33.3', '—')} | {wall.get('over_50', '—')} | "
            f"{_fmt(gpu.get('p50'))} | {_fmt(gpu.get('p95'))} | {_fmt(gpu.get('max'))} | "
            f"{_fmt(stats.get('compile_total_ms'), 0)} | {'yes' if result.get('valid') else 'NO: ' + str(result.get('invalid_reason'))} |")
    lines += ["", "Mean per frame in the window, ms (guest = wall - host calls - main-thread sleeps):", "",
              "| target | scene | res | mode | rep | wall | guest | host | compile | wait drawable | wait in-flight | "
              "sleep | GPU | draws | fog draws | passes | copies |",
              "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for result in results:
        stats = (result.get("analysis") or {}).get("full") or {}
        series = stats.get("series") or {}

        def mean(name):
            return _fmt((series.get(name) or {}).get("mean"), 2)
        lines.append(
            f"| {TARGET_LABEL.get(result['target'], result['target'])} | {SCENE_LABEL.get(result['scene'], result['scene'])} | {result['resolution']} | "
            f"{result['mode']} | {result['repeat']} | {mean('wall_ms')} | {mean('guest_ms')} | {mean('host_ms')} | "
            f"{mean('compile_ms')} | {mean('wait_drawable_ms')} | {mean('wait_inflight_ms')} | {mean('sleep_ms')} | "
            f"{mean('gpu_ms')} | {stats.get('mean_draws', '—')} | {stats.get('mean_fog_draws', '—')} | "
            f"{stats.get('mean_passes', '—')} | {stats.get('mean_copies', '—')} |")
    return "\n".join(lines) + "\n"


def write_results(out, results, meta):
    out = Path(out)
    (out / "results.json").write_text(json.dumps({"meta": meta, "results": results}, indent=2, default=str))
    (out / "results.md").write_text(f"# {meta.get('label', 'tvos_bench')}\n\n" + json.dumps(meta, default=str) +
                                    "\n\n" + markdown(results))


# ---------- the runs

def plan(scenes, resolutions, modes, repeats):
    """the runs in order: for each repeat, scene and resolution a new salt,
    its cold launch, then its warm launches; a primed launch (after them)
    has a salt of its own, and the shader cache's record the cold one made"""
    runs = []
    for repeat in range(1, repeats + 1):
        for scene in scenes:
            for resolution in resolutions:
                salt = uuid.uuid4().hex[:12]
                for mode in modes:
                    runs.append({"scene": scene, "resolution": resolution, "mode": mode, "repeat": repeat,
                                 "salt": salt + "p" if mode == "primed" else salt})
    return runs


# the Metal host's shader cache (host_metal.m) of the runs, not the player's:
# emptied by each cold launch, so that it is cold too
SHADER_CACHE = "tvbench_shaders.manifest"


def run_environment(run, frame_log="1", extra=None):
    environment = {"HALO_FRAME_LOG": frame_log, "HALO_FRAME_LOG_INTERVAL": "0",
                   "HALO_RESOLUTION_SCALING": str(run["resolution"]), "HALO_METAL_SHADER_SALT": run["salt"],
                   "HALO_METAL_SHADER_CACHE": SHADER_CACHE,
                   "HALO_METAL_SHADER_CACHE_RESET": "1" if run["mode"] == "cold" else "0",
                   "HALO_HIGH_RES_HUD": "true", "HALO_HIGH_RES_TEXT": "true",
                   # (the play telemetry, on by default on the TV, off in a
                   # benchmark's runs unless --env asks: host_telemetry.h)
                   "HALO_TELEMETRY": "off"}
    environment.update(SCENES.get(run["scene"], {}).get("env", {}))
    environment.update(extra or {})
    return environment


def scene_core(scene):
    """the saved state a scene loads, or None"""
    return SCENES[scene].get("core", CORE_NAME) if any("core_load" in line for line in SCENES[scene]["init"]) else None


def scene_data_root(scene, maps, core=None):
    """a scratch data folder for a scene (Mac/simulator): the maps linked,
    its init.txt, the saved state"""
    root = SCRATCH / "tvbench-data" / scene
    (root / "maps").mkdir(parents=True, exist_ok=True)
    for name in ("ui.map", SCENES[scene].get("map", "a10.map")):
        link = root / "maps" / name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(Path(maps).resolve() / name)
    (root / "init.txt").write_text("\r\n".join(SCENES[scene]["init"]) + "\r\n")
    core_name = scene_core(scene)
    if core_name:
        source = Path(core if core and scene == "bridge" else SCRATCH / "tvbench" / core_name)
        if not source.is_file():
            raise deploy.DeployError(f"no saved state {source}: run tvos_bench.py make-core first")
        (root / "core").mkdir(exist_ok=True)
        shutil.copyfile(source, root / "core" / core_name)
    return root


def finish_run(run, target, log_path, csv_path, findings):
    result = dict(run, target=target, log=str(log_path), frame_log=str(csv_path), **{
        "valid": findings["valid"], "invalid_reason": findings["invalid_reason"], "findings": findings})
    text = Path(csv_path).read_text() if Path(csv_path).is_file() else ""
    # (a frame log copied off the TV is the run's own only if its frame count
    # is the one this launch's summary gave: else it is an earlier run's)
    summary = findings.get("frame_log_summary")
    match = re.match(r"# OpenCE frame log, level \d+, (\d+) frames", text)
    if text and summary and match and int(match.group(1)) not in (summary["frames"], summary["frames"] + 1):
        result["valid"], result["invalid_reason"] = False, "the frame log copied is not this run's"
        return result
    if text:
        result["analysis"] = analyze(run["scene"], text)
        if not result["analysis"]["full"]["frames"]:
            result["valid"] = False
            result["invalid_reason"] = result["analysis"]["window"]
    else:
        result["valid"] = False
        result["invalid_reason"] = result["invalid_reason"] or "no frame log written"
    return result


def run_mac(runs, maps, out, extra_env, core=None):
    app = ROOT / "build/macos/OpenCE.app/Contents/MacOS/OpenCE"
    results = []
    for index, run in enumerate(runs):
        root = scene_data_root(run["scene"], maps, core)
        name = f"{index:02d}-{run['scene']}-{run['resolution']}-{run['mode']}-{run['repeat']}"
        log_path, csv_path = out / f"{name}.log", out / f"{name}.csv"
        environment = dict(os.environ, HALO_DATA_ROOT=str(root), HALO_SAVE_ROOT=str(root / "save"),
                           HALO_HIDDEN_WINDOW="1", HALO_EXIT_AFTER=str(SCENES[run["scene"]]["seconds"]),
                           HALO_FRAME_LOG_PATH=str(csv_path), **run_environment(run, extra=extra_env))
        print(f"== {name}", flush=True)
        with log_path.open("w") as log:
            subprocess.run([str(app)], env=environment, stdout=log, stderr=subprocess.STDOUT,
                           timeout=SCENES[run["scene"]]["seconds"] + 120)
        results.append(finish_run(run, "mac", log_path, csv_path,
                                  deploy.log_findings(log_path.read_text(errors="replace"))))
        print(markdown(results[-1:]).splitlines()[2], flush=True)
    return results


def run_simulator(runs, maps, out, extra_env, app=None, core=None):
    from tvos_simcheck import capture, select_simulator
    app = Path(app or ROOT / "build/tvos/simulator/OpenCETV.app").resolve()
    runtime, device = select_simulator(json.loads(capture("xcrun", "simctl", "list", "runtimes", "--json"))["runtimes"],
                                       json.loads(capture("xcrun", "simctl", "list", "devicetypes", "--json"))["devicetypes"])
    identifier = capture("xcrun", "simctl", "create", "OpenCE bench", device["identifier"], runtime["identifier"]).strip()
    results = []
    try:
        deploy.run(["codesign", "--force", "--sign", "-", app])
        deploy.run(["xcrun", "simctl", "boot", identifier])
        deploy.run(["xcrun", "simctl", "bootstatus", identifier, "-b"], timeout=300)
        deploy.run(["xcrun", "simctl", "install", identifier, app], timeout=120)
        for index, run in enumerate(runs):
            root = scene_data_root(run["scene"], maps, core)
            name = f"{index:02d}-{run['scene']}-{run['resolution']}-{run['mode']}-{run['repeat']}"
            log_path, csv_path = out / f"{name}.log", out / f"{name}.csv"
            child = dict(HALO_DATA_ROOT=str(root), HALO_EXIT_AFTER=str(SCENES[run["scene"]]["seconds"]),
                         HALO_FRAME_LOG_PATH=str(csv_path), **run_environment(run, extra=extra_env))
            environment = dict(os.environ, **{f"SIMCTL_CHILD_{key}": value for key, value in child.items()})
            print(f"== {name}", flush=True)
            with log_path.open("w") as log:
                subprocess.run(["xcrun", "simctl", "launch", "--console", "--terminate-running-process", identifier,
                                deploy.BUNDLE], env=environment, stdout=log, stderr=subprocess.STDOUT,
                               timeout=SCENES[run["scene"]]["seconds"] + 180)
            results.append(finish_run(run, "simulator", log_path, csv_path,
                                      deploy.log_findings(log_path.read_text(errors="replace"))))
            print(markdown(results[-1:]).splitlines()[2], flush=True)
    finally:
        subprocess.run(["xcrun", "simctl", "shutdown", identifier], check=False)
        subprocess.run(["xcrun", "simctl", "delete", identifier], check=False)
    return results


def file_digest(path):
    import hashlib
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def record_trace(device, trace, out, extra_env):
    """an xctrace recording of a launch: devicectl launches the app (with
    its environment and console), xctrace attaches by process name after
    trace["attach_after"] seconds and records trace["seconds"]"""
    name = f"trace-{trace['template'].replace(' ', '_')}-{trace['scene']}-{trace['resolution']}"
    output, log_path = out / f"{name}.trace", out / f"{name}.log"
    if output.exists():
        shutil.rmtree(output)
    udid = deploy.device_udid(device)
    environment = run_environment(trace, extra=extra_env)
    environment["HALO_EXIT_AFTER"] = str(trace["attach_after"] + trace["seconds"] + 15)
    command = deploy.launch_args(device, environment)
    print(f"== {name}\n+ {' '.join(command)}", flush=True)
    with log_path.open("w") as log:
        launched = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, text=True,
                                    env=deploy.developer_environment())
        time.sleep(trace["attach_after"])
        # (by pid: xctrace did not find the process by its name on the TV)
        pid = deploy.device_pid(device, "OpenCETV.app/OpenCETV")
        print(f"OpenCETV pid on the TV: {pid}", flush=True)
        recorded = deploy.run(deploy.trace_args(trace["template"], output, trace["seconds"], udid, "attach",
                                                str(pid) if pid else "OpenCETV"),
                              check=False, capture=True, timeout=trace["seconds"] + 600)
        try:
            launched.wait(timeout=trace["attach_after"] + trace["seconds"] + 180)
        except subprocess.TimeoutExpired:
            launched.send_signal(2)
            launched.wait(timeout=30)
    (out / f"{name}.xctrace.txt").write_text(recorded.stdout or "")
    findings = deploy.log_findings(log_path.read_text(errors="replace"))
    print(f"xctrace exit {recorded.returncode}; trace {'written' if output.exists() else 'MISSING'}: "
          f"{(recorded.stdout or '')[-400:]}", flush=True)
    return dict(trace, name=name, trace=str(output), trace_written=output.exists(), xctrace_exit=recorded.returncode,
                xctrace_output=(recorded.stdout or "")[-2000:], findings=findings)


def run_device(runs, out, extra_env, device, core=None, traces=(), trace_results=None, maps=None, push=True):
    """the runs on the Apple TV: the owner's config.toml and init.txt backed
    up first and put back afterwards whatever happens (and checked)"""
    trace_results = [] if trace_results is None else trace_results
    backups = SCRATCH / "tv-config-backups"
    backups.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    config_backup, init_backup = backups / f"config-{stamp}.toml", backups / f"init-{stamp}.txt"
    container = "Library/Caches/OpenCE"
    deploy.run(deploy.copy_args("from", device, f"{container}/config.toml", config_backup), timeout=120)
    deploy.run(deploy.copy_args("from", device, f"{container}/init.txt", init_backup), timeout=120)
    if not config_backup.stat().st_size or not init_backup.stat().st_size:
        raise deploy.DeployError("a backup is empty: stopping before anything on the TV changes")
    print(f"backed up the TV's config.toml and init.txt: {config_backup}, {init_backup}", flush=True)
    results = []
    try:
        for map_name in sorted({SCENES[run["scene"]].get("map", "a10.map") for run in runs} - {"a10.map"}) if push else ():
            if not maps:
                raise deploy.DeployError(f"{map_name} is needed on the TV: pass --maps")
            deploy.run(deploy.copy_args("to", device, Path(maps) / map_name, f"{container}/maps/{map_name}"),
                       timeout=1800)
        for core_name in sorted({scene_core(run["scene"]) for run in runs} - {None}) if push else ():
            source = Path(core if core and core_name == CORE_NAME else SCRATCH / "tvbench" / core_name)
            deploy.run(deploy.copy_args("to", device, source, f"{container}/core/{core_name}"), timeout=300)
        current_init = None
        for index, run in enumerate(runs):
            name = f"{index:02d}-{run['scene']}-{run['resolution']}-{run['mode']}-{run['repeat']}"
            log_path, csv_path = out / f"{name}.log", out / f"{name}.csv"
            init = "\r\n".join(SCENES[run["scene"]]["init"]) + "\r\n"
            if init != current_init:
                init_path = out / f"init-{run['scene']}.txt"
                init_path.write_text(init)
                deploy.run(deploy.copy_args("to", device, init_path, f"{container}/init.txt"), timeout=120)
                current_init = init
            print(f"== {name}", flush=True)
            findings = deploy.launch(device, run_environment(run, extra=extra_env), log_path,
                                     SCENES[run["scene"]]["seconds"])
            try:
                deploy.run(deploy.copy_args("from", device, f"{container}/frame_log.csv", csv_path), timeout=300)
            except subprocess.SubprocessError as error:
                print(f"no frame log copied: {error}", flush=True)
            # (HALO_PASS_LOG: the pass log beside it, host_frame_log.c's name for it)
            if run_environment(run, extra=extra_env).get("HALO_PASS_LOG", "0") not in ("", "0"):
                try:
                    deploy.run(deploy.copy_args("from", device, f"{container}/frame_log-passes.csv",
                                                out / f"{name}-passes.csv"), timeout=600)
                except subprocess.SubprocessError as error:
                    print(f"no pass log copied: {error}", flush=True)
            results.append(finish_run(run, "device", log_path, csv_path, findings))
            print(markdown(results[-1:]).splitlines()[2], flush=True)
            write_results(out, results, {"label": "partial (device)"})
            if not findings["valid"] and findings["invalid_reason"] != "the game trapped":
                print(f"stopping the batch: {findings['invalid_reason']} (the TV may be in use or asleep)", flush=True)
                break
        if traces and all(result["valid"] for result in results):
            for trace in traces:
                # (attach when the opening's fog is up: the frame log's time of
                # the window's start in this session's first valid run of it,
                # plus the app's start-up before the frame log's clock, ~3 s)
                measured = [r for r in results if r["scene"] == trace["scene"] and r.get("analysis", {}).get("full", {})
                            .get("frames")]
                if trace.get("attach_after"):
                    pass
                elif measured:
                    events, rows = parse_frame_log(Path(measured[-1]["frame_log"]).read_text())
                    window, _ = scene_window(trace["scene"], events, rows)
                    trace["attach_after"] = int(window[0]["t_ms"] / 1000.0 + 3 + 5) if window else 30
                else:
                    trace["attach_after"] = 30
                init = "\r\n".join(SCENES[trace["scene"]]["init"]) + "\r\n"
                if init != current_init:
                    init_path = out / f"init-{trace['scene']}.txt"
                    init_path.write_text(init)
                    deploy.run(deploy.copy_args("to", device, init_path, f"{container}/init.txt"), timeout=120)
                    current_init = init
                results_trace = record_trace(device, trace, out, extra_env)
                trace_results.append(results_trace)
                if not results_trace["findings"]["valid"]:
                    print(f"stopping: trace run invalid ({results_trace['findings']['invalid_reason']})", flush=True)
                    break
    finally:
        deploy.run(deploy.copy_args("to", device, init_backup, f"{container}/init.txt"), timeout=120, check=False)
        deploy.run(deploy.copy_args("to", device, config_backup, f"{container}/config.toml"), timeout=120, check=False)
        check = out / "restored-check"
        check.mkdir(exist_ok=True)
        ok = True
        for remote, backup in (("config.toml", config_backup), ("init.txt", init_backup)):
            deploy.run(deploy.copy_args("from", device, f"{container}/{remote}", check / remote), timeout=120,
                       check=False)
            same = (check / remote).is_file() and file_digest(check / remote) == file_digest(backup)
            ok = ok and same
            print(f"restored {remote}: {'verified identical to the backup' if same else 'NOT VERIFIED'}", flush=True)
        if not ok:
            print(f"RESTORE NOT VERIFIED: put back {config_backup} and {init_backup} by hand "
                  "(tvos_deploy.py restore-config)", file=sys.stderr, flush=True)
    return results


def make_b30_core(maps, out_core):
    """b30's saved state: a fresh b30 launch, saved 55 s in (the insertion
    cinematic is over at about 45 s; global_mission_start is then true)"""
    import socket
    root = SCRATCH / "tvbench-data" / "make-core-b30"
    (root / "maps").mkdir(parents=True, exist_ok=True)
    for name in ("ui.map", "b30.map"):
        link = root / "maps" / name
        if not link.exists():
            link.symlink_to(Path(maps).resolve() / name)
    (root / "init.txt").write_text("map_name b30\r\n")
    shutil.rmtree(root / "core", ignore_errors=True)
    app = ROOT / "build/macos/OpenCE.app/Contents/MacOS/OpenCE"
    environment = dict(os.environ, HALO_DATA_ROOT=str(root), HALO_SAVE_ROOT=str(root / "save"), HALO_HIDDEN_WINDOW="1",
                       HALO_EXIT_AFTER="70", HALO_TELNET_CONSOLE="1", HALO_RESOLUTION_SCALING="480")
    process = subprocess.Popen([str(app)], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    started, connection, replies = time.monotonic(), None, ""
    try:
        for at, command in ((54, "global_mission_start"), (56, "(core_save_name tvbench_b30.bin)")):
            while time.monotonic() - started < at:
                time.sleep(0.1)
            while connection is None:
                try:
                    connection = socket.create_connection(("127.0.0.1", 2323), timeout=2)
                except OSError:
                    time.sleep(0.25)
            connection.sendall((command + "\r\n").encode("ascii"))
            time.sleep(1.0)
            connection.settimeout(1.0)
            try:
                replies += connection.recv(65536).decode("latin-1", "replace")
            except socket.timeout:
                pass
        process.wait(timeout=120)
    finally:
        if process.poll() is None:
            process.kill()
    saved = root / "core" / "tvbench_b30.bin"
    if not saved.is_file() or "saved 'tvbench_b30.bin'" not in replies or "true" not in replies:
        raise deploy.DeployError(f"b30's state was not saved after the mission started: {replies[-500:]}")
    Path(out_core).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(saved, out_core)
    print(f"saved b30's state: {out_core}")


def make_cryo_core(maps, out_core, at_seconds=224.0):
    """the cryo bay's saved state: a fresh a10 launch (no input) saved
    at_seconds in, as the bay's fog (>= 40 fog-plane draws) comes up and the
    tutorial's screen convolution starts; the frame log of the launch is
    checked for both, so a slower load that misses the moment is an error"""
    import socket
    root = SCRATCH / "tvbench-data" / "make-core-cryo"
    (root / "maps").mkdir(parents=True, exist_ok=True)
    for name in ("ui.map", "a10.map"):
        link = root / "maps" / name
        if not link.exists():
            link.symlink_to(Path(maps).resolve() / name)
    (root / "init.txt").write_text("map_name a10\r\n")
    shutil.rmtree(root / "core", ignore_errors=True)
    frame_log = root / "make-core-frames.csv"
    app = ROOT / "build/macos/OpenCE.app/Contents/MacOS/OpenCE"
    environment = dict(os.environ, HALO_DATA_ROOT=str(root), HALO_SAVE_ROOT=str(root / "save"), HALO_HIDDEN_WINDOW="1",
                       HALO_EXIT_AFTER=str(int(at_seconds) + 8), HALO_TELNET_CONSOLE="1",
                       HALO_RESOLUTION_SCALING="480", HALO_FRAME_LOG="1", HALO_FRAME_LOG_INTERVAL="0",
                       HALO_FRAME_LOG_PATH=str(frame_log))
    with socket.socket() as check:
        check.bind(("127.0.0.1", 2323))
    process = subprocess.Popen([str(app)], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    started, connection, replies = time.monotonic(), None, ""
    try:
        while time.monotonic() - started < at_seconds:
            time.sleep(0.1)
            if connection is None and time.monotonic() - started > 20:
                try:
                    connection = socket.create_connection(("127.0.0.1", 2323), timeout=2)
                except OSError:
                    pass
        while connection is None:
            try:
                connection = socket.create_connection(("127.0.0.1", 2323), timeout=2)
            except OSError:
                time.sleep(0.25)
        connection.sendall(b"(core_save_name tvbench_cryo.bin)\r\n")
        time.sleep(1.0)
        connection.settimeout(1.0)
        try:
            replies += connection.recv(65536).decode("latin-1", "replace")
        except socket.timeout:
            pass
        process.wait(timeout=120)
    finally:
        if process.poll() is None:
            process.kill()
    saved = root / "core" / "tvbench_cryo.bin"
    if not saved.is_file() or "saved 'tvbench_cryo.bin'" not in replies:
        raise deploy.DeployError(f"the cryo bay's state was not saved: {replies[-500:]}")
    _, rows = parse_frame_log(frame_log.read_text())
    last = rows[-12:] if rows else []
    fog = min((row["fog_draws"] for row in last), default=0)
    if fog < 40:
        raise deploy.DeployError(f"the state was saved before the cryo bay's fog ({fog} fog-plane draws a frame at the "
                                 f"end): try a later --at")
    Path(out_core).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(saved, out_core)
    print(f"saved the cryo bay's state: {out_core} ({saved.stat().st_size} bytes; {fog}+ fog-plane draws a frame)")


def make_core(maps, out_core):
    """the bridge scene's saved state, made with the Mac build (whose guest
    is the TV build's): a10's opening to its end, then the console puts the
    player at the bridge and saves"""
    import socket
    root = SCRATCH / "tvbench-data" / "make-core"
    (root / "maps").mkdir(parents=True, exist_ok=True)
    for name in ("ui.map", "a10.map"):
        link = root / "maps" / name
        if not link.exists():
            link.symlink_to(Path(maps).resolve() / name)
    (root / "init.txt").write_text("map_name a10\r\n")
    shutil.rmtree(root / "core", ignore_errors=True)
    app = ROOT / "build/macos/OpenCE.app/Contents/MacOS/OpenCE"
    environment = dict(os.environ, HALO_DATA_ROOT=str(root), HALO_SAVE_ROOT=str(root / "save"), HALO_HIDDEN_WINDOW="1",
                       HALO_EXIT_AFTER="75", HALO_TELNET_CONSOLE="1", HALO_RESOLUTION_SCALING="480")
    with socket.socket() as check:
        check.bind(("127.0.0.1", 2323))
    script = [(58, "(unit_exit_vehicle (player0))"), (59, "(object_teleport (player0) cheat_bridge_flag)"),
              (60, "(player_enable_input true)"), (64, f"(core_save_name {CORE_NAME})")]
    process = subprocess.Popen([str(app)], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    started = time.monotonic()
    replies = []
    try:
        connection = None
        for at, command in script:
            while time.monotonic() - started < at:
                time.sleep(0.1)
            while connection is None:
                try:
                    connection = socket.create_connection(("127.0.0.1", 2323), timeout=2)
                except OSError:
                    time.sleep(0.25)
            connection.sendall((command + "\r\n").encode("ascii"))
            time.sleep(1.0)
            connection.settimeout(1.0)
            try:
                replies.append(connection.recv(65536).decode("latin-1", "replace"))
            except socket.timeout:
                pass
        process.wait(timeout=120)
    finally:
        if process.poll() is None:
            process.kill()
    saved = root / "core" / CORE_NAME
    if not saved.is_file() or f"saved '{CORE_NAME}'" not in "".join(replies):
        raise deploy.DeployError(f"the state was not saved: {''.join(replies)[-500:]}")
    Path(out_core).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(saved, out_core)
    print(f"saved the bridge state: {out_core} ({saved.stat().st_size} bytes)")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("run")
    p.add_argument("--target", choices=("mac", "simulator", "device"), required=True)
    p.add_argument("--scenes", default="bridge,b30", help=f"of {', '.join(SCENES)}")
    p.add_argument("--plan", help="instead of --scenes/--resolutions/--repeats: SCENE/RES/REPEATS entries, "
                   "comma-separated, e.g. b30/1080/2,b30/2160/2,a10play/2160/1 (each cold then warm)")
    p.add_argument("--resolutions", default="1080,2160")
    p.add_argument("--modes", default="cold,warm")
    p.add_argument("--repeats", type=int, default=2)
    p.add_argument("--maps", help="folder holding ui.map and a10.map (mac/simulator)")
    p.add_argument("--core", help=f"the bridge state (default {SCRATCH / 'tvbench' / CORE_NAME})")
    p.add_argument("--app", help="the simulator app")
    p.add_argument("--out", required=True)
    p.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for every run")
    p.add_argument("--device", default=deploy.DEVICE)
    p.add_argument("--install", action="store_true", help="device: install the signed build first (in the session)")
    p.add_argument("--signed", default=str(deploy.SIGNED), help="device: the signed app to install (default the shared "
                   "tv-deploy copy; children working in parallel should sign to a path of their own)")
    p.add_argument("--traces", default="", help="device: xctrace templates to record after the runs, comma-separated "
                   "(the opening at 4K, warm), e.g. 'Metal System Trace,Time Profiler'")
    p.add_argument("--trace-seconds", type=int, default=20)
    p.add_argument("--trace-scene", default="opening", help="the scene (at 4K, warm) the traces record")
    p.add_argument("--trace-attach-after", type=int, help="seconds after launch (default: from the scene's window)")
    p.add_argument("--no-lock", action="store_true")
    p.add_argument("--no-push", action="store_true", help="device: the maps and saved states are already on the TV "
                   "(an earlier run of this session copied them)")
    p = sub.add_parser("analyze")
    p.add_argument("csv")
    p.add_argument("--scene", choices=tuple(SCENES), required=True)
    p = sub.add_parser("report")
    p.add_argument("out")
    p = sub.add_parser("make-core")
    p.add_argument("--maps", required=True)
    p.add_argument("--scene", choices=("bridge", "b30", "cryo"), default="bridge")
    p.add_argument("--at", type=float, default=224.0, help="cryo: seconds after launch to save")
    p.add_argument("--out")
    args = parser.parse_args(argv)

    if args.command == "analyze":
        print(json.dumps(analyze(args.scene, Path(args.csv).read_text()), indent=2))
        return 0
    if args.command == "report":
        data = json.loads((Path(args.out) / "results.json").read_text())
        print(markdown(data["results"]))
        return 0
    if args.command == "make-core":
        if args.scene == "b30":
            make_b30_core(args.maps, args.out or SCRATCH / "tvbench" / "tvbench_b30.bin")
        elif args.scene == "cryo":
            make_cryo_core(args.maps, args.out or SCRATCH / "tvbench" / "tvbench_cryo.bin", args.at)
        else:
            make_core(args.maps, args.out or SCRATCH / "tvbench" / CORE_NAME)
        return 0

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    if args.plan:
        runs = []
        entries = [entry.split("/") for entry in args.plan.split(",")]
        for repeat in range(1, max(int(e[2]) for e in entries) + 1):
            for scene, resolution, repeats in entries:
                if repeat <= int(repeats):
                    runs += [dict(run, repeat=repeat) for run in plan([scene], [int(resolution)], args.modes.split(","), 1)]
    else:
        runs = plan(args.scenes.split(","), [int(r) for r in args.resolutions.split(",")], args.modes.split(","),
                    args.repeats)
    extra = deploy.parse_env(args.env)
    commit = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short=8", "HEAD"], capture_output=True,
                            text=True).stdout.strip()
    meta = {"label": f"tvos_bench {TARGET_LABEL[args.target]}", "target": args.target, "commit": commit,
            "started": datetime.datetime.now(datetime.timezone.utc).isoformat(), "warmup_seconds": WARMUP_SECONDS,
            "scenes": SCENES, "extra_env": extra}
    if args.target == "device":
        build_info = Path(args.signed).parent / "build-info.json"
        meta["installed_build"] = json.loads(build_info.read_text()) if build_info.is_file() else None
    print(f"{len(runs)} runs:", ", ".join(f"{r['scene']}/{r['resolution']}/{r['mode']}/{r['repeat']}" for r in runs))
    task = f"tvos_bench {args.target}: {len(runs)} runs"
    with deploy.HeavyLock(task, enabled=not args.no_lock and args.target != "mac"):
        if args.target == "mac":
            results = run_mac(runs, args.maps, out, extra, args.core)
        elif args.target == "simulator":
            results = run_simulator(runs, args.maps, out, extra, args.app, args.core)
        else:
            if args.install:
                deploy.run(["xcrun", "devicectl", "device", "install", "app", "--device", args.device,
                            args.signed], timeout=900)
            # (the traces reuse the salt of the last 4K opening runs: warm)
            salts = [r["salt"] for r in runs if r["scene"] == args.trace_scene and r["resolution"] == 2160]
            traces = [{"template": template, "scene": args.trace_scene, "resolution": 2160, "mode": "warm",
                       "repeat": 0, "salt": salts[-1] if salts else "trace", "seconds": args.trace_seconds,
                       "attach_after": args.trace_attach_after} for template in args.traces.split(",") if template]
            trace_results = []
            results = run_device(runs, out, extra, args.device, args.core, traces, trace_results, args.maps,
                                 push=not args.no_push)
            meta["traces"] = trace_results
    meta["finished"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    write_results(out, results, meta)
    print(markdown(results))
    return 0 if all(result["valid"] for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
