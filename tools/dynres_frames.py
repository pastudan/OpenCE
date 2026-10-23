#!/usr/bin/env python3
"""Numerical checks of dynamic resolution's pictures (port/tvos/PERFORMANCE.md,
"Dynamic resolution (A12)"), on the Mac build: a saved state loaded, the
game's time frozen over telnet (game_speed 0), and the back buffer written
every N frames (debug.screenshot_every) while HALO_DYNRES_FIXED alternates
the scene's fraction every N frames in the same run, so that frames of the
same moment are compared (two launches differ by animation that game_speed
does not stop: PERFORMANCE.md, "GPU bandwidth").

    dynres_frames.py capture SCENE OUT [--fixed 0.75,toggle:30,1.0] [--env K=V ...]
    dynres_frames.py compare OUT                 # frames of the two fractions in one run
    dynres_frames.py identical A B               # same-numbered frames of two runs: byte-identical?
    dynres_frames.py edges OUT                   # the reduced frames' view edges: no black or garbage

compare: the reduced frame against the whole one (PSNR, the largest
difference), and its rows and columns at the views' edges against the
whole frame's (an upscale reading outside the scene's part of its target
shows there as black or another frame's pixels).
"""
import argparse
import math
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tvos_bench as bench  # noqa: E402

SCRATCH = bench.SCRATCH
APP = bench.ROOT / "build/macos/OpenCE.app/Contents/MacOS/OpenCE"


def capture(scene, out, fixed, extra, freeze_at=14.0, seconds=26, every=30):
    out = Path(out)
    shutil.rmtree(out, ignore_errors=True)
    (out / "shots").mkdir(parents=True)
    root = bench.scene_data_root(scene, SCRATCH / "halo_data/maps")
    env = dict(os.environ, HALO_DATA_ROOT=str(root), HALO_SAVE_ROOT=str(root / "save"), HALO_HIDDEN_WINDOW="1",
               HALO_EXIT_AFTER=str(seconds), HALO_RESOLUTION_SCALING="2160", HALO_WINDOW_SIZE="1920x1080",
               HALO_HIGH_RES_HUD="true", HALO_HIGH_RES_TEXT="true", HALO_TELNET_CONSOLE="1", HALO_TELEMETRY="off",
               HALO_SCREENSHOT_DIR=str(out / "shots"), HALO_SCREENSHOT_EVERY=str(every), **extra)
    if fixed:
        env["HALO_DYNRES_FIXED"] = fixed
    with open(out / "run.log", "w") as log:
        process = subprocess.Popen([str(APP)], env=env, stdout=log, stderr=subprocess.STDOUT)
        started = time.monotonic()
        connection = None
        try:
            while time.monotonic() - started < freeze_at:
                time.sleep(0.1)
            while connection is None and process.poll() is None:
                try:
                    connection = socket.create_connection(("127.0.0.1", 2323), timeout=2)
                except OSError:
                    time.sleep(0.25)
            if connection:
                connection.sendall(b"(game_speed 0)\r\n")
            # (only the frames after the freeze are compared: those before
            # are deleted as they come, keeping a capture to ~10 frames,
            # ~0.3 GB at 3840x2160, not 3 GB)
            frozen = time.monotonic()
            while process.poll() is None:
                if time.monotonic() - frozen < 4.0:
                    for shot in frames(out):
                        shot.unlink(missing_ok=True)
                time.sleep(0.5)
            process.wait(timeout=seconds + 120)
        finally:
            if process.poll() is None:
                process.kill()
    (out / "meta.txt").write_text(f"scene {scene}\nfixed {fixed}\nevery {every}\n{extra}\n")


def load(path):
    data = Path(path).read_bytes()
    width, height = struct.unpack("<ii", data[18:26])
    return width, abs(height), data[54:]


def frames(out):
    return sorted((Path(out) / "shots").glob("frame*.bmp"))


def number(path):
    return int(path.stem[5:])


def difference(a, b, width, rows=None, columns=None):
    """(largest, mean absolute, PSNR) over RGB of the pixels given (rows and
    columns: ranges; None for all)"""
    total = biggest = count = 0
    squared = 0
    height = len(a) // (width * 4)
    for y in rows if rows is not None else range(height):
        base = y * width * 4
        for x in columns if columns is not None else range(width):
            i = base + x * 4
            for c in range(3):
                d = a[i + c] - b[i + c]
                if d < 0:
                    d = -d
                total += d
                squared += d * d
                if d > biggest:
                    biggest = d
            count += 3
    mean = total / count if count else 0.0
    mse = squared / count if count else 0.0
    psnr = 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)
    return biggest, mean, psnr


def dark_share(pixels, width, rows, columns, threshold=8):
    """the share of the pixels given that are near black"""
    dark = count = 0
    for y in rows:
        base = y * width * 4
        for x in columns:
            i = base + x * 4
            count += 1
            dark += pixels[i] < threshold and pixels[i + 1] < threshold and pixels[i + 2] < threshold
    return dark / count if count else 0.0


def toggle_parts(out):
    meta = (Path(out) / "meta.txt").read_text()
    fixed = [line.split(" ", 1)[1] for line in meta.splitlines() if line.startswith("fixed ")][0]
    every = int([line.split(" ", 1)[1] for line in meta.splitlines() if line.startswith("every ")][0])
    first = float(fixed.split(",")[0])
    toggle = int(fixed.split("toggle:")[1].split(",")[0])
    other = float(fixed.split(",")[-1])
    return first, toggle, other, every


def fraction_of(serial, first, toggle, other):
    # (the host's frame serial is the guest's frame + 1: screenshots are
    # numbered by the guest's frame before its present)
    return other if ((serial + 1) // toggle) % 2 else first


def compare(out, edge_band=3):
    first, toggle, other, _ = toggle_parts(out)
    shots = frames(out)
    reduced = [s for s in shots if fraction_of(number(s), first, toggle, other) < 1.0]
    whole = [s for s in shots if fraction_of(number(s), first, toggle, other) >= 1.0]
    print(f"{out}: {len(reduced)} reduced ({first if first < 1 else other}), {len(whole)} whole frames")
    for shot in reduced[:3]:
        partner = min(whole, key=lambda w: abs(number(w) - number(shot)))
        wa, ha, pa = load(shot)
        wb, hb, pb = load(partner)
        if (wa, ha) != (wb, hb):
            print(shot.name, "size differs")
            continue
        biggest, mean, psnr = difference(pa, pb, wa, rows=range(0, ha, 4), columns=range(0, wa, 4))
        # (the 3D view's bounds: the whole frame's rows and columns with any
        # picture, inside letterbox bars a cinematic draws)
        top = next(y for y in range(ha) if any(pb[(y * wa + x) * 4 + c] > 8 for x in range(0, wa, 8) for c in range(3)))
        bottom = next(y for y in range(ha - 1, -1, -1)
                      if any(pb[(y * wa + x) * 4 + c] > 8 for x in range(0, wa, 8) for c in range(3)))
        edges = [f"view rows {top}-{bottom}"]
        for name, rows, columns in (("top", range(top, top + edge_band), range(0, wa, 2)),
                                    ("bottom", range(bottom - edge_band + 1, bottom + 1), range(0, wa, 2)),
                                    ("left", range(top, bottom + 1, 2), range(0, edge_band)),
                                    ("right", range(top, bottom + 1, 2), range(wa - edge_band, wa))):
            e_big, e_mean, e_psnr = difference(pa, pb, wa, rows=rows, columns=columns)
            edges.append(f"{name} mean {e_mean:.2f} psnr {e_psnr:.1f} dark {dark_share(pa, wa, rows, columns):.3f}"
                         f"/{dark_share(pb, wa, rows, columns):.3f}")
        print(f"{shot.name} vs {partner.name}: whole frame (1/16 sampled) max {biggest} mean {mean:.2f} "
              f"psnr {psnr:.1f}; " + "; ".join(edges))


def identical(a, b):
    names = sorted(set(p.name for p in frames(a)) & set(p.name for p in frames(b)))
    same = 0
    for name in names:
        _, _, pa = load(Path(a) / "shots" / name)
        _, _, pb = load(Path(b) / "shots" / name)
        if pa == pb:
            same += 1
        else:
            wa = load(Path(a) / "shots" / name)[0]
            print(name, "differs:", difference(pa, pb, wa, rows=range(0, len(pa) // (wa * 4), 4),
                                               columns=range(0, wa, 4)))
    print(f"{same} of {len(names)} frames byte-identical")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("capture")
    p.add_argument("scene")
    p.add_argument("out")
    p.add_argument("--fixed", default="")
    p.add_argument("--every", type=int, default=30)
    p.add_argument("--freeze-at", type=float, default=14.0, help="seconds after launch to freeze the game")
    p.add_argument("--seconds", type=int, default=26)
    p.add_argument("--env", action="append", default=[])
    p = sub.add_parser("compare")
    p.add_argument("out")
    p = sub.add_parser("identical")
    p.add_argument("a")
    p.add_argument("b")
    args = parser.parse_args()
    if args.command == "capture":
        capture(args.scene, args.out, args.fixed, dict(item.split("=", 1) for item in args.env), args.freeze_at,
                args.seconds, args.every)
    elif args.command == "compare":
        compare(args.out)
    else:
        identical(args.a, args.b)


if __name__ == "__main__":
    main()
