#!/usr/bin/env python3
"""Fixed-scene frame-time benchmark of the TV game (port/tvos/PERFORMANCE.md).

    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    # the bridge scene's saved game state, made once on the Mac build (same guest as the TV's):
    python3 tools/tvos_bench.py make-core --maps DIR
    # runs (each target takes the heavy-job lock for its whole session; --no-lock if held):
    python3 tools/tvos_bench.py run --target simulator --maps DIR --out OUT
    python3 tools/tvos_bench.py run --target device --out OUT          # the Apple TV: ask its owner first
    python3 tools/tvos_bench.py run --target mac --maps DIR --out OUT  # validation only: not a TV number
    # one frame log, or a results folder's tables again:
    python3 tools/tvos_bench.py analyze frame_log.csv --scene opening
    python3 tools/tvos_bench.py report OUT

Scenes (both a10, no controller input: the most repeatable):
- opening: a fresh launch into a10 (init.txt "map_name a10"), the opening
  cinematic. Measured: the 30 s from the first frame with >= 20 fog-plane
  draws (the fog sequence; frame counts from the frame log, not times, so a
  slower load does not shift it).
- bridge: a launch that loads a saved game state at the bridge (BSP 1)
  (init.txt "map_name a10" + "core_load_name_at_startup tvbench_bridge.bin";
  the state is made by make-core: a10's opening run to its end, the player
  out of the cryo tube and put at the scenario's cheat_bridge_flag). Measured:
  the 35 s from the second frame after the state is read (the frame that
  reads it is the load).

Cold and warm. cold: the first launch with a new HALO_METAL_SHADER_SALT (a
comment added to every Metal shader: nothing is found in the system's
compiled-shader caches of earlier launches, which otherwise survive app
updates); its first-use compile stalls are in the numbers. warm: the next
launch with the same salt (the system's caches now hold every shader the
scene compiles). The "steady" window of a run also leaves out its first
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
}
FOG_START_DRAWS = 20
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
                                                    "compiles", "fog_draws", "transparent_draws")
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
        start = next((index for index, row in enumerate(rows)
                      if row["serial"] > after and row.get("fog_draws", 0) >= FOG_START_DRAWS), None)
        if start is None:
            return [], "no frame with fog-plane draws (the opening's fog never appeared)"
        description = f"opening fog: {window_seconds:g} s from frame {rows[start]['serial']}"
    elif scene == "bridge":
        loaded = [e for e in events if e["kind"] == "core-open" and e["detail"] == CORE_NAME]
        if not loaded:
            return [], f"the saved state {CORE_NAME} was never read"
        start = next((index for index, row in enumerate(rows) if row["serial"] >= loaded[0]["serial"] + 2), None)
        if start is None:
            return [], "no frames after the saved state was read"
        description = f"bridge: {window_seconds:g} s from frame {rows[start]['serial']}"
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
            f"| {TARGET_LABEL.get(result['target'], result['target'])} | {result['scene']} | {result['resolution']} | "
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
            f"| {TARGET_LABEL.get(result['target'], result['target'])} | {result['scene']} | {result['resolution']} | "
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
    its cold launch, then its warm launches"""
    runs = []
    for repeat in range(1, repeats + 1):
        for scene in scenes:
            for resolution in resolutions:
                salt = uuid.uuid4().hex[:12]
                for mode in modes:
                    runs.append({"scene": scene, "resolution": resolution, "mode": mode, "repeat": repeat,
                                 "salt": salt})
    return runs


def run_environment(run, frame_log="1", extra=None):
    environment = {"HALO_FRAME_LOG": frame_log, "HALO_FRAME_LOG_INTERVAL": "0",
                   "HALO_RESOLUTION_SCALING": str(run["resolution"]), "HALO_METAL_SHADER_SALT": run["salt"],
                   "HALO_HIGH_RES_HUD": "true", "HALO_HIGH_RES_TEXT": "true"}
    environment.update(extra or {})
    return environment


def scene_data_root(scene, maps, core=None):
    """a scratch data folder for a scene (Mac/simulator): the maps linked,
    its init.txt, the saved state"""
    root = SCRATCH / "tvbench-data" / scene
    (root / "maps").mkdir(parents=True, exist_ok=True)
    for name in ("ui.map", "a10.map"):
        link = root / "maps" / name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(Path(maps).resolve() / name)
    (root / "init.txt").write_text("\r\n".join(SCENES[scene]["init"]) + "\r\n")
    if scene == "bridge":
        core = Path(core or SCRATCH / "tvbench" / CORE_NAME)
        if not core.is_file():
            raise deploy.DeployError(f"no saved state {core}: run tvos_bench.py make-core first")
        (root / "core").mkdir(exist_ok=True)
        shutil.copyfile(core, root / "core" / CORE_NAME)
    return root


def finish_run(run, target, log_path, csv_path, findings):
    result = dict(run, target=target, log=str(log_path), frame_log=str(csv_path), **{
        "valid": findings["valid"], "invalid_reason": findings["invalid_reason"], "findings": findings})
    if Path(csv_path).is_file():
        result["analysis"] = analyze(run["scene"], Path(csv_path).read_text())
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


def run_device(runs, out, extra_env, device, core=None):
    """the runs on the Apple TV: the owner's config.toml and init.txt backed
    up first and put back afterwards whatever happens (and checked)"""
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
        if any(run["scene"] == "bridge" for run in runs):
            core = Path(core or SCRATCH / "tvbench" / CORE_NAME)
            deploy.run(deploy.copy_args("to", device, core, f"{container}/core/{CORE_NAME}"), timeout=300)
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
            results.append(finish_run(run, "device", log_path, csv_path, findings))
            print(markdown(results[-1:]).splitlines()[2], flush=True)
            write_results(out, results, {"label": "partial (device)"})
            if findings["background_gpu_errors"]:
                print("the TV is not showing the app (GPU work refused in the background): stopping", flush=True)
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
    p.add_argument("--scenes", default="opening,bridge")
    p.add_argument("--resolutions", default="1080,2160")
    p.add_argument("--modes", default="cold,warm")
    p.add_argument("--repeats", type=int, default=2)
    p.add_argument("--maps", help="folder holding ui.map and a10.map (mac/simulator)")
    p.add_argument("--core", help=f"the bridge state (default {SCRATCH / 'tvbench' / CORE_NAME})")
    p.add_argument("--app", help="the simulator app")
    p.add_argument("--out", required=True)
    p.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for every run")
    p.add_argument("--device", default=deploy.DEVICE)
    p.add_argument("--no-lock", action="store_true")
    p = sub.add_parser("analyze")
    p.add_argument("csv")
    p.add_argument("--scene", choices=tuple(SCENES), required=True)
    p = sub.add_parser("report")
    p.add_argument("out")
    p = sub.add_parser("make-core")
    p.add_argument("--maps", required=True)
    p.add_argument("--out", default=SCRATCH / "tvbench" / CORE_NAME)
    args = parser.parse_args(argv)

    if args.command == "analyze":
        print(json.dumps(analyze(args.scene, Path(args.csv).read_text()), indent=2))
        return 0
    if args.command == "report":
        data = json.loads((Path(args.out) / "results.json").read_text())
        print(markdown(data["results"]))
        return 0
    if args.command == "make-core":
        make_core(args.maps, args.out)
        return 0

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    runs = plan(args.scenes.split(","), [int(r) for r in args.resolutions.split(",")], args.modes.split(","),
                args.repeats)
    extra = deploy.parse_env(args.env)
    commit = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short=8", "HEAD"], capture_output=True,
                            text=True).stdout.strip()
    meta = {"label": f"tvos_bench {TARGET_LABEL[args.target]}", "target": args.target, "commit": commit,
            "started": datetime.datetime.now(datetime.timezone.utc).isoformat(), "warmup_seconds": WARMUP_SECONDS,
            "scenes": SCENES, "extra_env": extra}
    if args.target == "device":
        build_info = SCRATCH / "tv-deploy" / "build-info.json"
        meta["installed_build"] = json.loads(build_info.read_text()) if build_info.is_file() else None
    print(f"{len(runs)} runs:", ", ".join(f"{r['scene']}/{r['resolution']}/{r['mode']}/{r['repeat']}" for r in runs))
    task = f"tvos_bench {args.target}: {len(runs)} runs"
    with deploy.HeavyLock(task, enabled=not args.no_lock and args.target != "mac"):
        if args.target == "mac":
            results = run_mac(runs, args.maps, out, extra, args.core)
        elif args.target == "simulator":
            results = run_simulator(runs, args.maps, out, extra, args.app, args.core)
        else:
            results = run_device(runs, out, extra, args.device, args.core)
    meta["finished"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    write_results(out, results, meta)
    print(markdown(results))
    return 0 if all(result["valid"] for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
