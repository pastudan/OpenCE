#!/usr/bin/env python3
"""Run only our feasibility app in a fresh Apple TV simulator and remove it.
This is not a game/controller/performance validation or a physical TV test.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import shutil
import time


def capture(*args):
    return subprocess.check_output(list(map(str, args)), text=True)


def select_simulator(runtimes, types):
    available = [item for item in runtimes if item.get("isAvailable") and
                 item["identifier"].startswith("com.apple.CoreSimulator.SimRuntime.tvOS-")]
    if not available:
        raise ValueError("No installed tvOS simulator runtime; install tvOS platform support in Xcode")
    runtime = max(available, key=lambda item: tuple(map(int, item["version"].split("."))))
    supported = {item["identifier"] for item in runtime.get("supportedDeviceTypes", [])}
    devices = [item for item in types if item["name"].startswith("Apple TV") and
               item.get("productFamily", "Apple TV") == "Apple TV" and
               (not supported or item["identifier"] in supported)]
    if not devices:
        raise ValueError("No Apple TV simulator device type compatible with the installed runtime")
    return runtime, devices[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    args = parser.parse_args()
    app = args.app.resolve()
    started = time.time()
    diagnostics = app.parent / "simulator-diagnostics"
    diagnostics.mkdir(exist_ok=True)
    runtimes = json.loads(capture("xcrun", "simctl", "list", "runtimes", "--json"))["runtimes"]
    types = json.loads(capture("xcrun", "simctl", "list", "devicetypes", "--json"))["devicetypes"]
    try:
        runtime, device = select_simulator(runtimes, types)
    except ValueError as error:
        parser.error(str(error))
    # A unique simulator, never boot/delete any simulator belonging to a user.
    identifier = capture("xcrun", "simctl", "create", "OpenCE feasibility", device["identifier"],
                         runtime["identifier"]).strip()
    try:
        subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True)
        subprocess.run(["xcrun", "simctl", "boot", identifier], check=True)
        subprocess.run(["xcrun", "simctl", "bootstatus", identifier, "-b"], check=True, timeout=180)
        subprocess.run(["xcrun", "simctl", "install", identifier, str(app)], check=True, timeout=60)
        with (diagnostics / "console.txt").open("w") as console:
            result = subprocess.run(["xcrun", "simctl", "launch", "--console", identifier,
                                     "com.pastudan.opence.tvprobe"], text=True, stdout=console,
                                    stderr=subprocess.STDOUT,
                                    env=dict(os.environ, SIMCTL_CHILD_OPENCE_PROBE_AUTOMATED="1"), timeout=120)
        report = (diagnostics / "console.txt").read_text()
        print(report, flush=True)
        if result.returncode or "PASS: automatic TV platform probes" not in report:
            raise RuntimeError(f"Simulator probe did not pass (launch return {result.returncode}); see {diagnostics}")
    finally:
        # Preserve only this app's reports, not unrelated user crash data.
        try:
            logs = subprocess.run(["xcrun", "simctl", "spawn", identifier, "log", "show", "--last", "3m",
                "--style", "compact", "--predicate", 'process == "OpenCETVProbe"'],
                capture_output=True, text=True, timeout=20)
            (diagnostics / "app-log.txt").write_text(logs.stdout + logs.stderr)
            locations = [Path.home() / "Library/Logs/DiagnosticReports",
                Path.home() / "Library/Developer/CoreSimulator/Devices" / identifier / "data/Library/Logs/CrashReporter"]
            for location in locations:
                if location.is_dir():
                    for path in location.glob("OpenCETVProbe*"):
                        if path.is_file() and path.stat().st_mtime >= started:
                            shutil.copyfile(path, diagnostics / path.name)
            (diagnostics / "simulator.json").write_text(json.dumps({"id": identifier, "runtime": runtime}, indent=2))
        except (OSError, subprocess.SubprocessError) as error:
            print(f"Diagnostic collection: {error}", flush=True)
        subprocess.run(["xcrun", "simctl", "shutdown", identifier], check=False)
        subprocess.run(["xcrun", "simctl", "delete", identifier], check=False)


if __name__ == "__main__":
    main()
