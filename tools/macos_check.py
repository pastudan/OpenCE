#!/usr/bin/env python3
"""Checks a built macOS app (port/macos/README.md), as the GitHub workflow
does after building it:

    python3 tools/macos_check.py build/macos/OpenCE.app

- the app is put together: its executable is arm64, Info.plist names it and
  its icon, and the icon is there; a signed app's signature verifies;
- it starts: with an empty data folder (no maps), a hidden window and
  debug.exit_after, the game comes up, Metal opens a device, the settings
  are written, and it exits by itself, cleanly, within the time given.

It needs no game data, so it shows only that the build runs, not that it
draws a level right.
"""

import os
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path

# seconds the game runs before it exits by itself, and the most it may take
RUN_SECONDS = 8
TIMEOUT_SECONDS = 90


def fail(message: str) -> None:
    print(f"FAIL: {message}", flush=True)
    sys.exit(1)


def check_bundle(app: Path) -> Path:
    info = plistlib.loads((app / "Contents" / "Info.plist").read_bytes())
    executable = app / "Contents" / "MacOS" / info["CFBundleExecutable"]
    if not executable.is_file():
        fail(f"{executable} is missing")
    architectures = subprocess.run(["lipo", "-archs", str(executable)], capture_output=True, text=True,
                                   check=True).stdout.split()
    if architectures != ["arm64"]:
        fail(f"{executable.name} is {architectures}, not arm64")
    icon = app / "Contents" / "Resources" / (info.get("CFBundleIconFile", "") + ".icns")
    if not icon.is_file():
        fail(f"the icon Info.plist names, {icon}, is missing")
    signed = subprocess.run(["codesign", "-dv", str(app)], capture_output=True, text=True)
    if signed.returncode == 0:
        verify = subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], capture_output=True,
                                text=True)
        if verify.returncode != 0:
            fail(f"the signature does not verify: {verify.stderr.strip()}")
        print("signature verifies", flush=True)
    print(f"{app.name}: {info['CFBundleExecutable']} (arm64), {icon.name}", flush=True)
    return executable


def check_start(executable: Path) -> None:
    with tempfile.TemporaryDirectory() as root:
        environment = dict(os.environ, HALO_DATA_ROOT=root, HALO_HIDDEN_WINDOW="1",
                           HALO_EXIT_AFTER=str(RUN_SECONDS))
        try:
            run = subprocess.run([str(executable)], env=environment, capture_output=True, text=True,
                                 timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            fail(f"the game did not exit within {TIMEOUT_SECONDS} s")
        output = run.stdout + run.stderr
        print(output, flush=True)
        if run.returncode != 0:
            fail(f"the game exited with {run.returncode}")
        for expected in ("host: Metal:", "data root:"):
            if expected not in output:
                fail(f"no {expected!r} in the game's output")
        if not (Path(root) / "config.toml").is_file():
            fail("the game wrote no config.toml")
    print("the game started, opened Metal and exited cleanly", flush=True)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    app = Path(sys.argv[1])
    check_start(check_bundle(app))
    return 0


if __name__ == "__main__":
    sys.exit(main())
