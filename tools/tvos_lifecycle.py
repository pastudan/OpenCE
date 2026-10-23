#!/usr/bin/env python3
"""Background and foreground the TV game, and check that it stops cleanly
and comes back (port/macos/host/host_lifecycle.h; port/tvos/PERFORMANCE.md,
"Lifecycle (A12)").

    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    python3 tools/tvos_lifecycle.py simulator --maps DIR [--app build/tvos/simulator/OpenCETV.app]
    python3 tools/tvos_lifecycle.py device [--memory-warning]   # the installed build on the A12 TV

A run launches the game into a map (init.txt "map_name a10", from a saved
state when there is one), lets it play, then puts another app in front
(the simulator: simctl launch of the Settings app; the TV: devicectl launch
of TVSettings, which is what the Home button does as far as the game can
tell: it resigns active and enters the background), waits, launches the
game again (which brings the running copy to the front: no relaunch), lets
it play, and does that CYCLES times. The game's console output is kept
(--console), with its frame log (HALO_FRAME_LOG) and lifecycle lines; the
result says:

- background GPU errors ("Insufficient Permission (to submit GPU work from
  background)"), GPU failures and traps: there must be none;
- the stops and resumes the game logged, and how long each stop was;
- the frames' wall time and waits (drawable, in flight, readback) in the
  seconds of play before the first stop and after the last resume, from
  the frame log: what they were before, and that they are normal after.

The TV is a person's: the device mode takes the heavy-job lock
(tools/tvos_deploy.py), backs up and restores its config.toml and
init.txt, and stops if the TV is asleep or the game was already running.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tvos_deploy as deploy  # noqa: E402
import tvos_bench as bench  # noqa: E402

SETTINGS = {"simulator": "com.apple.TVSettings", "device": "com.apple.TVSettings"}
STOP = re.compile(r"lifecycle: stopped: GPU idle, audio paused, ([\d.]+) MB released; footprint (\d+) MB")
RESUME = re.compile(r"lifecycle: going on after ([\d.]+) s")
DRAIN = re.compile(r"Metal: the GPU's work finished \((\d+) frames waited for, ([\d.]+) ms\)")


def frame_rows(csv_path):
    """the frame log's rows and events (tools/tvos_bench.py's parser)"""
    if not Path(csv_path).is_file():
        return [], []
    events, rows = bench.parse_frame_log(Path(csv_path).read_text())
    return events, rows


def window_stats(rows):
    if not rows:
        return None
    wall = sorted(r["wall_ms"] for r in rows)

    def pct(values, fraction):
        return values[min(len(values) - 1, int(fraction * len(values)))]
    return {"frames": len(rows), "wall_p50": round(pct(wall, 0.5), 2), "wall_p95": round(pct(wall, 0.95), 2),
            "wall_max": round(wall[-1], 2),
            "over33": sum(1 for r in rows if r["wall_ms"] > 33.3),
            "wait_drawable_ms": round(statistics.mean(r["wait_drawable_ms"] for r in rows), 3),
            "wait_inflight_ms": round(statistics.mean(r["wait_inflight_ms"] for r in rows), 3),
            "wait_readback_ms": round(statistics.mean(r["wait_readback_ms"] for r in rows), 3),
            "wait_inflight_max": round(max(r["wait_inflight_ms"] for r in rows), 2),
            "wait_readback_max": round(max(r["wait_readback_ms"] for r in rows), 2)}


def analyze(log_text, csv_path, play_seconds):
    """the run's findings: errors, stops, and the frames before and after"""
    findings = deploy.log_findings(log_text)
    stops = [{"released_mb": float(m.group(1)), "footprint_mb": int(m.group(2))} for m in STOP.finditer(log_text)]
    resumes = [float(m.group(1)) for m in RESUME.finditer(log_text)]
    drains = [{"frames": int(m.group(1)), "ms": float(m.group(2))} for m in DRAIN.finditer(log_text)]
    events, rows = frame_rows(csv_path)
    result = {"background_gpu_errors": findings["background_gpu_errors"], "gpu_errors": findings["gpu_errors"],
              "trap": findings["trap"], "metal_device": findings["metal_device"],
              "launched": re.findall(r"lifecycle: launched ([^\n]+)", log_text),
              "inactive": len(re.findall(r"lifecycle: inactive", log_text)),
              "stops": stops, "resumes_s": resumes, "drains": drains,
              "paused_menu": log_text.count("the app is active again: the game paused"),
              "memory": re.findall(r"lifecycle: memory [^\n]+", log_text),
              "lifecycle_events": [f"{e['t_ms'] / 1000.0:.1f}s {e['kind']} {e['detail']}" for e in events
                                   if e["kind"] in ("lifecycle", "memory")]}
    lifecycle = [e for e in events if e["kind"] == "lifecycle"]
    first_stop = next((e for e in lifecycle if e["detail"] in ("will-resign-active", "background")), None)
    last_resume = next((e for e in reversed(lifecycle) if e["detail"].startswith("resumed")), None)
    if rows and first_stop:
        # (the play_seconds before the first stop, its last 2 s left out: the
        # moment of the Home press; and after the last resume, its first 2 s)
        before = [r for r in rows if first_stop["t_ms"] - (play_seconds - 2) * 1000 <= r["t_ms"]
                  < first_stop["t_ms"] - 2000]
        result["before"] = window_stats(before)
    if rows and last_resume:
        after_rows = [r for r in rows if r["serial"] > last_resume["serial"]]
        result["first_frame_after_ms"] = round(after_rows[0]["wall_ms"], 2) if after_rows else None
        result["first_frames_after_ms"] = [round(r["wall_ms"], 1) for r in after_rows[:10]]
        if after_rows:
            start = after_rows[0]["t_ms"]
            result["after"] = window_stats([r for r in after_rows if start + 2000 <= r["t_ms"]
                                            < start + (play_seconds - 2) * 1000])
    result["valid"] = (result["background_gpu_errors"] == 0 and not result["trap"] and result["gpu_errors"] == 0
                       and findings["metal_device"] is not None)
    return result


# ---------- the simulator

def data_root(maps, root):
    """a data folder of this run's own: the maps linked, an init.txt into the
    bridge from its saved state (tools/tvos_bench.py make-core), or a10's
    start without one"""
    (root / "maps").mkdir(parents=True, exist_ok=True)
    for name in ("ui.map", "a10.map"):
        link = root / "maps" / name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(Path(maps).resolve() / name)
    core = deploy.SCRATCH / "tvbench" / bench.CORE_NAME
    init = ["map_name a10"]
    if core.is_file():
        (root / "core").mkdir(exist_ok=True)
        (root / "core" / bench.CORE_NAME).write_bytes(core.read_bytes())
        init.append(f"core_load_name_at_startup {bench.CORE_NAME}")
    (root / "init.txt").write_text("\r\n".join(init) + "\r\n")
    return root


def run_simulator(args):
    from tvos_simcheck import capture, select_simulator
    app = Path(args.app).resolve()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    root = data_root(args.maps, out / "data")
    runtime, device = select_simulator(json.loads(capture("xcrun", "simctl", "list", "runtimes", "--json"))["runtimes"],
                                       json.loads(capture("xcrun", "simctl", "list", "devicetypes", "--json"))["devicetypes"])
    identifier = capture("xcrun", "simctl", "create", "OpenCE lifecycle", device["identifier"], runtime["identifier"]).strip()
    log_path, csv_path = out / "simulator.log", out / "simulator-frame_log.csv"
    seconds = args.play * (args.cycles + 1) + args.away * args.cycles + 60
    try:
        deploy.run(["codesign", "--force", "--sign", "-", app])
        deploy.run(["xcrun", "simctl", "boot", identifier])
        deploy.run(["xcrun", "simctl", "bootstatus", identifier, "-b"], timeout=300)
        deploy.run(["xcrun", "simctl", "install", identifier, app], timeout=120)
        child = dict(HALO_DATA_ROOT=str(root), HALO_EXIT_AFTER=str(seconds), HALO_FRAME_LOG="1",
                     HALO_FRAME_LOG_INTERVAL="0", HALO_FRAME_LOG_PATH=str(csv_path), HALO_RESOLUTION_SCALING="720")
        if args.memory_warning:
            child["HALO_MEMORY_WARNING_AFTER"] = str(args.warmup + args.play)
        environment = dict(os.environ, **{f"SIMCTL_CHILD_{key}": value for key, value in child.items()})
        with log_path.open("w") as log:
            game = subprocess.Popen(["xcrun", "simctl", "launch", "--console", "--terminate-running-process",
                                     identifier, deploy.BUNDLE], env=environment, stdout=log, stderr=subprocess.STDOUT)
            time.sleep(args.warmup + args.play)
            if args.memory_warning:
                time.sleep(10)
            for cycle in range(args.cycles):
                print(f"cycle {cycle + 1}: another app in front", flush=True)
                subprocess.run(["xcrun", "simctl", "launch", identifier, SETTINGS["simulator"]], check=False)
                time.sleep(args.away)
                print(f"cycle {cycle + 1}: the game in front again", flush=True)
                # (the running copy brought to the front: launch without
                # --terminate-running-process, and no console of its own)
                subprocess.run(["xcrun", "simctl", "launch", identifier, deploy.BUNDLE], check=False)
                time.sleep(args.play)
            game.wait(timeout=seconds + 120)
    finally:
        subprocess.run(["xcrun", "simctl", "shutdown", identifier], check=False)
        subprocess.run(["xcrun", "simctl", "delete", identifier], check=False)
    return analyze(log_path.read_text(errors="replace"), csv_path, args.play)


# ---------- the Apple TV

def run_device(args):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    container = "Library/Caches/OpenCE"
    log_path, csv_path = out / "device.log", out / "device-frame_log.csv"
    seconds = args.warmup + args.play * (args.cycles + 1) + args.away * args.cycles + 30
    with deploy.HeavyLock(f"#115 lifecycle: device background/foreground ~{seconds // 60 + 3} min",
                          enabled=not args.no_lock):
        if deploy.device_pid(args.device, "OpenCETV"):
            raise deploy.DeployError("OpenCE is already running on the TV (its owner may be playing): try later")
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        backups = deploy.SCRATCH / "tv-config-backups"
        backups.mkdir(parents=True, exist_ok=True)
        init_backup = backups / f"init-lifecycle-{stamp}.txt"
        deploy.run(deploy.copy_args("from", args.device, f"{container}/init.txt", init_backup), timeout=120)
        if not init_backup.stat().st_size:
            raise deploy.DeployError("the init.txt backup is empty: stopping before anything on the TV changes")
        init = out / "init.txt"
        init.write_text("map_name a10\r\ncore_load_name_at_startup tvbench_bridge.bin\r\n")
        try:
            deploy.run(deploy.copy_args("to", args.device, init, f"{container}/init.txt"), timeout=120)
            environment = {"HALO_EXIT_AFTER": str(seconds), "HALO_FRAME_LOG": "1", "HALO_FRAME_LOG_INTERVAL": "0"}
            if args.memory_warning:
                # (devicectl's sendMemoryWarning is tried too; on this TV it
                # fails: ENOENT. The game's own diagnostic warning stands in)
                environment["HALO_MEMORY_WARNING_AFTER"] = str(args.warmup + args.play)
            command = deploy.launch_args(args.device, environment)
            print("+", " ".join(command), flush=True)
            with log_path.open("w") as log:
                game = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                        env=deploy.developer_environment())
                time.sleep(args.warmup + args.play)
                pid = deploy.device_pid(args.device, "OpenCETV")
                if args.memory_warning and pid:
                    print(f"a memory warning to pid {pid}", flush=True)
                    deploy.run(["xcrun", "devicectl", "device", "process", "sendMemoryWarning", "--device",
                                args.device, "--pid", str(pid)], check=False, timeout=60)
                    time.sleep(10)
                for cycle in range(args.cycles):
                    print(f"cycle {cycle + 1}: TVSettings in front (as Home)", flush=True)
                    deploy.run(["xcrun", "devicectl", "device", "process", "launch", "--device", args.device,
                                SETTINGS["device"]], check=False, timeout=60)
                    time.sleep(args.away)
                    print(f"cycle {cycle + 1}: OpenCE in front again", flush=True)
                    deploy.run(["xcrun", "devicectl", "device", "process", "launch", "--device", args.device,
                                deploy.BUNDLE], check=False, timeout=60)
                    time.sleep(args.play)
                try:
                    game.wait(timeout=seconds + 120)
                except subprocess.TimeoutExpired:
                    game.send_signal(2)
                    game.wait(timeout=60)
            deploy.run(deploy.copy_args("from", args.device, f"{container}/frame_log.csv", csv_path), timeout=300,
                       check=False)
        finally:
            deploy.run(deploy.copy_args("to", args.device, init_backup, f"{container}/init.txt"), timeout=120,
                       check=False)
            check = out / "init-restored.txt"
            deploy.run(deploy.copy_args("from", args.device, f"{container}/init.txt", check), timeout=120, check=False)
            same = check.is_file() and check.read_bytes() == init_backup.read_bytes()
            print(f"restored init.txt: {'verified identical to the backup' if same else 'NOT VERIFIED'}", flush=True)
    return analyze(log_path.read_text(errors="replace"), csv_path, args.play)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", choices=("simulator", "device"))
    parser.add_argument("--maps", help="simulator: the folder holding ui.map and a10.map")
    parser.add_argument("--app", default=str(deploy.ROOT / "build/tvos/simulator/OpenCETV.app"))
    parser.add_argument("--out", default=str(deploy.SCRATCH / "lifecycle" / "run"))
    parser.add_argument("--device", default=deploy.DEVICE)
    parser.add_argument("--cycles", type=int, default=2)
    parser.add_argument("--warmup", type=int, default=40, help="seconds for the launch and the map's load")
    parser.add_argument("--play", type=int, default=20, help="seconds of play before each stop and after each resume")
    parser.add_argument("--away", type=int, default=15, help="seconds another app is in front")
    parser.add_argument("--memory-warning", action="store_true",
                        help="a memory warning before the first stop (HALO_MEMORY_WARNING_AFTER, and on the TV "
                             "devicectl sendMemoryWarning too)")
    parser.add_argument("--no-lock", action="store_true")
    args = parser.parse_args()
    if args.target == "simulator":
        if not args.maps:
            parser.error("--maps is needed for the simulator")
        with deploy.HeavyLock("#115 lifecycle: simulator background/foreground", enabled=not args.no_lock):
            result = run_simulator(args)
    else:
        result = run_device(args)
    text = json.dumps(result, indent=2)
    print(text)
    (Path(args.out) / f"{args.target}-result.json").write_text(text)
    return 0 if result["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
