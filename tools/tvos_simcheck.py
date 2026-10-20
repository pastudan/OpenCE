#!/usr/bin/env python3
"""Run only our feasibility app in a fresh Apple TV simulator and remove it.
This is not a game/controller/performance validation or a physical TV test.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def capture(*args):
    return subprocess.check_output(list(map(str, args)), text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    args = parser.parse_args()
    app = args.app.resolve()
    runtimes = json.loads(capture("xcrun", "simctl", "list", "runtimes", "--json"))["runtimes"]
    available = [item for item in runtimes if item.get("isAvailable") and
                 item["identifier"].startswith("com.apple.CoreSimulator.SimRuntime.tvOS-")]
    if not available:
        parser.error("No installed tvOS simulator runtime; install tvOS platform support in Xcode")
    runtime = sorted(available, key=lambda item: tuple(map(int, item["version"].split("."))))[-1]
    types = json.loads(capture("xcrun", "simctl", "list", "devicetypes", "--json"))["devicetypes"]
    devices = [item for item in types if item["name"].startswith("Apple TV") and
               item.get("productFamily", "Apple TV") == "Apple TV"]
    if not devices:
        parser.error("No Apple TV simulator device type")
    # A unique simulator, never boot/delete any simulator belonging to a user.
    identifier = capture("xcrun", "simctl", "create", "OpenCE feasibility", devices[-1]["identifier"],
                         runtime["identifier"]).strip()
    try:
        subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True)
        subprocess.run(["xcrun", "simctl", "boot", identifier], check=True)
        subprocess.run(["xcrun", "simctl", "bootstatus", identifier, "-b"], check=True, timeout=180)
        subprocess.run(["xcrun", "simctl", "install", identifier, str(app)], check=True, timeout=60)
        result = subprocess.run(["xcrun", "simctl", "launch", "--console", identifier,
                                 "com.pastudan.opence.tvprobe"], check=True, text=True, capture_output=True,
                                env=dict(os.environ, SIMCTL_CHILD_OPENCE_PROBE_AUTOMATED="1"), timeout=120)
        print(result.stdout, result.stderr, flush=True)
        if "PASS: automatic TV platform probes" not in result.stdout + result.stderr:
            raise RuntimeError("Simulator app did not report a completed passing probe")
    finally:
        subprocess.run(["xcrun", "simctl", "shutdown", identifier], check=False)
        subprocess.run(["xcrun", "simctl", "delete", identifier], check=False)


if __name__ == "__main__":
    main()
