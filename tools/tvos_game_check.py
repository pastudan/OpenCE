#!/usr/bin/env python3
"""Smoke-test the TV game host using the owner's already extracted maps.

DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 tools/tvos_game_check.py \
    build/tvos/simulator/OpenCETV.app --data-root /path/holding/maps [--map a10]

Creates only its own simulator, copies no game data into source/artifacts,
and requires Metal, frame output and the timed exit marker. This is NOT a
visual/controller test or a measurement of physical Apple TV performance.
Uses the simulator-only developer data-root override; save files stay in
its disposable cache. A map test briefly enables the existing local telnet
console (loopback) to request the map.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time

from tvos_simcheck import capture, select_simulator


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("app", type=Path)
    parser.add_argument("--data-root", type=Path, required=True)
    parser.add_argument("--map", choices=("a10", "bloodgulch"))
    parser.add_argument("--profile", action="store_true", help="opt-in Metal/effect timing; simulator numbers are not TV timings")
    parser.add_argument("--resolution", choices=("720", "1080", "2160"), default="720")
    args = parser.parse_args()
    data = args.data_root.resolve()
    if not (data / "maps/ui.map").is_file():
        parser.error("data-root must contain maps/ui.map from your own disc")
    app = args.app.resolve()
    runtime, device = select_simulator(json.loads(capture("xcrun", "simctl", "list", "runtimes", "--json"))["runtimes"],
        json.loads(capture("xcrun", "simctl", "list", "devicetypes", "--json"))["devicetypes"])
    # Do not send commands to somebody else's running debug console.
    if args.map:
        with socket.socket() as check:
            check.bind(("127.0.0.1", 2323))
    identifier = capture("xcrun", "simctl", "create", "OpenCE game smoke", device["identifier"],
                         runtime["identifier"]).strip()
    diagnostics = app.parent / "game-smoke-diagnostics"
    diagnostics.mkdir(exist_ok=True)
    process = None
    try:
        subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True)
        subprocess.run(["xcrun", "simctl", "boot", identifier], check=True)
        subprocess.run(["xcrun", "simctl", "bootstatus", identifier, "-b"], check=True, timeout=180)
        subprocess.run(["xcrun", "simctl", "install", identifier, str(app)], check=True, timeout=60)
        environment = dict(os.environ, SIMCTL_CHILD_HALO_DATA_ROOT=str(data), SIMCTL_CHILD_HALO_EXIT_AFTER="30",
            SIMCTL_CHILD_HALO_GPU_STATS="1", SIMCTL_CHILD_HALO_METAL_PROFILE="1" if args.profile else "0",
            SIMCTL_CHILD_HALO_RESOLUTION_SCALING=args.resolution, SIMCTL_CHILD_HALO_HIGH_RES_HUD="false",
            SIMCTL_CHILD_HALO_HIGH_RES_TEXT="false", SIMCTL_CHILD_HALO_TELNET_CONSOLE="1" if args.map else "0")
        with (diagnostics / "console.txt").open("w") as log:
            process = subprocess.Popen(["xcrun", "simctl", "launch", "--console", identifier,
                "com.pastudan.opence.tv"], env=environment, stdout=log, stderr=subprocess.STDOUT)
            if args.map:
                deadline = time.monotonic() + 20
                while True:
                    try:
                        with socket.create_connection(("127.0.0.1", 2323), timeout=1) as console:
                            console.sendall(f"map_name {args.map}\r\n".encode("ascii"))
                        break
                    except OSError:
                        if process.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError("Game console did not start")
                        time.sleep(0.25)
            code = process.wait(timeout=100)
        report = (diagnostics / "console.txt").read_text()
        print(report, flush=True)
        for marker in ("host: Metal:", "high-res hud:", "frame 120:", "exiting after debug.exit_after"):
            if marker not in report:
                raise RuntimeError(f"Missing smoke-test marker: {marker}")
        if code or "wasm trap" in report.lower():
            raise RuntimeError(f"Game smoke test failed (simctl return {code})")
        if args.map and "map's 983 tags" in report and report.count("high-res hud:") < 2:
            raise RuntimeError("Map request did not produce a second map load")
        print(f"PASS: TV simulator game host {args.map or 'menu'} rendered and exited on schedule")
    finally:
        if process and process.poll() is None:
            process.kill()
            process.wait()
        try:
            logs = subprocess.run(["xcrun", "simctl", "spawn", identifier, "log", "show", "--last", "3m",
                "--style", "compact", "--predicate", 'process == "OpenCETV"'],
                capture_output=True, text=True, timeout=20)
            (diagnostics / "app-log.txt").write_text(logs.stdout + logs.stderr)
        finally:
            subprocess.run(["xcrun", "simctl", "shutdown", identifier], check=False)
            subprocess.run(["xcrun", "simctl", "delete", identifier], check=False)


if __name__ == "__main__":
    main()
