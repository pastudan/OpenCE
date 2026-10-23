#!/usr/bin/env python3
"""The TV game's play telemetry (port/macos/host/host_telemetry.h), on the Mac.

    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    # the new or grown sessions copied off the Apple TV (the app need not run,
    # and may: a session's closed parts are static; reads only, nothing is
    # copied to the TV), and with --watch again every N seconds, printing the
    # new hitches over 200 ms and lag stretches as they come:
    python3 tools/tv_telemetry.py pull [--archive DIR] [--device ID] [--watch N]
    # what they say, as markdown (a session's file, or folders of them):
    python3 tools/tv_telemetry.py report [PATH ...] [--out report.md] [--last N]
    # the lines as the TV writes them, over the local network (the TV's
    # config.toml: debug.telemetry_live = "<this Mac's IPv4 address>:47777"):
    python3 tools/tv_telemetry.py listen [--port 47777] [--archive DIR]

The archive (--archive, default ~/code/halo/tv-telemetry, outside the
repository) keeps a folder per device: <archive>/<device>/<session>.jsonl.
The TV writes a session in parts, <UTC start>-<pid>.p001.jsonl, .p002, ...,
of at most 8 MB or 30 minutes, each beginning with the session's line and
its "part" (older builds: one <UTC start>-<pid>.jsonl); the report takes a
session's parts as one session. A part still being written can fail to copy
(devicectl breaks off a growing file at 10 MB): the pull retries it, keeps
the most it got, and says what it skipped. The TV keeps its newest 40
sessions, at most 200 MB (host_telemetry.c), so pull after each play
session.

The records: one JSON object a line, "k" its kind (host_telemetry.h):
"session", "s" (a second), "hitch" (a frame over 50 ms), "event", "end".
"""
import argparse
import collections
import datetime
import json
import math
import os
from pathlib import Path
import re
import socket
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tvos_deploy as deploy  # noqa: E402

ARCHIVE = Path(os.environ.get("OPENCE_TV_TELEMETRY", Path.home() / "code/halo/tv-telemetry"))
REMOTE = "Library/Caches/OpenCE/telemetry"
LIVE_PORT = 47777
THERMAL = ("nominal", "fair", "serious", "critical")
# a position bucket's side, in world units (a world unit is 10 feet, about 3 m)
BUCKET = 10.0
# a second counts as laggy with this many display slots missed (of 60)
LAG_MISSED = 3
# what pull --watch calls out: a hitch over this many ms, or this many
# seconds in a row of at least this many frames over 33 ms or slots missed
WATCH_HITCH_MS = 200.0
WATCH_SECONDS = 2
WATCH_BAD = 10
# copies of a part still being written, at most
COPY_ATTEMPTS = 4
PART = re.compile(r"\.p(\d+)\.jsonl$")


# ---------- reading

def read_lines(path):
    """the records of a file, skipping a partly written last line"""
    records = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except ValueError:
                continue
    return records


def session_files(paths):
    files = []
    for path in paths:
        path = Path(path)
        if path.is_dir():
            files += sorted(path.rglob("*.jsonl"))
        elif path.is_file():
            files.append(path)
    return files


def session_of(name):
    """a file's session (its name up to the first '.': <UTC start>-<pid>)
    and its part (1 for a file of one part)"""
    name = Path(name).name
    match = PART.search(name)
    return name.split(".", 1)[0], int(match.group(1)) if match else 1


def load_sessions(paths, last=None):
    """[{name, session, seconds, hitches, events, end, parts, paths}], oldest
    first: a session's parts (the same start and pid) one session, in order"""
    groups = collections.defaultdict(list)
    for path in session_files(paths):
        name, part = session_of(path)
        groups[(str(path.parent), name)].append((part, path))
    sessions = []
    for (_, name), parts in groups.items():
        parts.sort()
        kinds = collections.defaultdict(list)
        for _, path in parts:
            for record in read_lines(path):
                kinds[record.get("k")].append(record)
        if not any(kinds.values()):
            continue
        sessions.append({"name": name, "path": str(parts[0][1]), "paths": [str(p) for _, p in parts],
                         "parts": len(parts), "session": (kinds["session"] or [{}])[0], "seconds": kinds["s"],
                         "hitches": kinds["hitch"], "events": kinds["event"], "end": (kinds["end"] or [None])[-1]})
    sessions.sort(key=lambda s: s["name"])
    if last:
        sessions = sessions[-last:]
    return sessions


# ---------- arithmetic

def mean(values):
    values = [v for v in values if v is not None]
    return sum(values) / len(values) if values else 0.0


def pct(values, fraction):
    values = sorted(v for v in values if v is not None)
    if not values:
        return 0.0
    rank = max(1, min(len(values), math.ceil(fraction * len(values))))
    return values[rank - 1]


def correlation(xs, ys):
    pairs = [(x, y) for x, y in zip(xs, ys) if x is not None and y is not None]
    if len(pairs) < 3:
        return 0.0
    xs, ys = zip(*pairs)
    mx, my = mean(xs), mean(ys)
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    if not sx or not sy:
        return 0.0
    return sum((x - mx) * (y - my) for x, y in pairs) / (sx * sy)


def slope_per_minute(times, values):
    """least squares: the change a minute"""
    pairs = [(t, v) for t, v in zip(times, values) if v is not None]
    if len(pairs) < 2:
        return 0.0
    ts, vs = zip(*pairs)
    mt, mv = mean(ts), mean(vs)
    denominator = sum((t - mt) ** 2 for t in ts)
    return 60.0 * sum((t - mt) * (v - mv) for t, v in pairs) / denominator if denominator else 0.0


def gameplay(seconds):
    """the seconds of play: the app active, a game running, frames drawn"""
    return [s for s in seconds if s.get("active", 1) and s.get("frames") and s.get("game_ticks", -1) >= 0]


def missed_rate(seconds):
    """display slots missed a minute"""
    duration = sum(s.get("dur", 1.0) for s in seconds)
    return 60.0 * sum(s.get("missed", 0) for s in seconds) / duration if duration else 0.0


def lag_stretches(seconds, threshold=LAG_MISSED, gap=2):
    """runs of laggy seconds (at most gap smooth seconds inside one):
    [(first index, last index)]"""
    stretches, start, last = [], None, None
    for index, second in enumerate(seconds):
        if second.get("missed", 0) >= threshold:
            if start is None:
                start = index
            elif index - last > gap + 1:
                stretches.append((start, last))
                start = index
            last = index
    if start is not None:
        stretches.append((start, last))
    return stretches


def period(signal, times, low=8, high=600):
    """the strongest repeat of a per-second signal, by autocorrelation over
    lags of low to high seconds: (seconds, strength 0-1) or (None, 0)"""
    if len(signal) < low * 3:
        return None, 0.0
    m = mean(signal)
    centered = [v - m for v in signal]
    variance = sum(v * v for v in centered)
    if not variance:
        return None, 0.0
    best, strength = None, 0.0
    for lag in range(low, min(high, len(signal) // 2) + 1):
        value = sum(centered[i] * centered[i + lag] for i in range(len(signal) - lag)) / variance
        # (a little weight for the shorter lag: a period's multiples score too)
        if value > strength * 1.05:
            best, strength = lag, value
    return best, strength


# ---------- what a hitch was

CAUSES = (
    ("shader compile", lambda h: h.get("compile_ms", 0)),
    ("texture conversion (DXT on the host)", lambda h: h.get("tex_convert_ms", 0)),
    ("file I/O on the main thread", lambda h: h.get("file_ms", 0)),
    ("shader manifest write", lambda h: h.get("manifest_ms", 0)),
    ("waiting for a drawable (GPU or display behind)", lambda h: h.get("wait_drawable_ms", 0)),
    ("waiting for a frame in flight (GPU behind)", lambda h: h.get("wait_inflight_ms", 0)),
    ("readback wait", lambda h: h.get("wait_readback_ms", 0)),
    ("Metal encoding on the host", lambda h: h.get("host_ms", 0)),
    ("the game's own work (guest)", lambda h: h.get("guest_ms", 0)),
)


def hitch_cause(hitch):
    """the largest part of a hitch's frame, and its ms"""
    name, value = max(((name, part(hitch)) for name, part in CAUSES), key=lambda pair: pair[1])
    if hitch.get("tex_uploads", 0) and name == "Metal encoding on the host":
        name = "texture uploads / Metal encoding on the host"
    return name, value


# ---------- the report

def _f(value, digits=1):
    return f"{value:.{digits}f}"


def group_table(rows, key, title, limit=None, sort=None):
    groups = collections.defaultdict(list)
    for row in rows:
        groups[key(row)].append(row)
    items = list(groups.items())
    items.sort(key=sort or (lambda item: -missed_rate(item[1])))
    if limit:
        items = items[:limit]
    lines = [f"| {title} | seconds | fps | wall p95 ms | missed slots /min | laggy s | guest ms | host ms | "
             "GPU p50 | GPU p95 | draws | passes | tex converts/s | stream MB/s | thermal |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for name, seconds in items:
        lines.append(
            f"| {name} | {len(seconds)} | {_f(mean(s['fps'] for s in seconds))} | "
            f"{_f(pct([s['wall_p95'] for s in seconds], 0.5))} | {_f(missed_rate(seconds))} | "
            f"{sum(1 for s in seconds if s.get('missed', 0) >= LAG_MISSED)} | "
            f"{_f(mean(s.get('guest_ms') for s in seconds), 2)} | {_f(mean(s.get('host_ms') for s in seconds), 2)} | "
            f"{_f(mean(s.get('gpu_p50') for s in seconds), 2)} | {_f(mean(s.get('gpu_p95') for s in seconds), 2)} | "
            f"{_f(mean(s.get('draws') for s in seconds), 0)} | {_f(mean(s.get('passes') for s in seconds))} | "
            f"{_f(mean(s.get('tex_converts', 0) for s in seconds), 2)} | "
            f"{_f(mean(s.get('stream_mb', 0) for s in seconds))} | "
            f"{_f(mean(s.get('thermal', 0) for s in seconds), 2)} |")
    return lines


def position_key(second):
    pos = (second.get("pos") or [[0, 0, 0]])[0]
    return (f"{second.get('map') or '?'} bsp {second.get('bsp', -1)} "
            f"({int(math.floor(pos[0] / BUCKET) * BUCKET)}, {int(math.floor(pos[1] / BUCKET) * BUCKET)})")


# what is compared between laggy and smooth seconds
METRICS = (("guest_ms", "guest ms a frame"), ("host_ms", "host ms a frame"), ("gpu_p50", "GPU ms p50"),
           ("gpu_p95", "GPU ms p95"), ("wait_drawable_ms", "drawable wait ms"),
           ("wait_inflight_ms", "in-flight wait ms"), ("compile_ms", "compile ms (the second)"),
           ("tex_converts", "textures converted"), ("tex_convert_ms", "conversion ms"),
           ("tex_uploads", "texture uploads"), ("tex_deletes", "textures dropped"),
           ("stream_mb", "streamed MB"), ("ring_overflows", "ring overflows"), ("file_reads", "file reads"),
           ("file_ms", "file ms"), ("manifest_writes", "manifest writes"), ("draws", "draws a frame"),
           ("passes", "passes a frame"), ("shadow_draws", "shadow draws a frame"),
           ("transparent_draws", "transparent draws a frame"), ("views", "views"), ("thermal", "thermal state"),
           ("footprint_mb", "footprint MB"), ("metal_mb", "Metal MB"))


def lag_analysis(seconds):
    """laggy against smooth seconds, and how the lag repeats"""
    lines = []
    if not seconds:
        return lines
    laggy = [s for s in seconds if s.get("missed", 0) >= LAG_MISSED]
    smooth = [s for s in seconds if s.get("missed", 0) == 0]
    stretches = lag_stretches(seconds)
    times = [s["t"] for s in seconds]
    signal = [min(s.get("missed", 0), 30) for s in seconds]
    repeat, strength = period(signal, times)
    lines.append(f"- {len(laggy)} laggy seconds (>= {LAG_MISSED} display slots missed) of {len(seconds)}, "
                 f"in {len(stretches)} stretches; {len(smooth)} seconds missed none.")
    if stretches:
        lengths = [seconds[b]["t"] - seconds[a]["t"] + 1 for a, b in stretches]
        starts = [seconds[a]["t"] for a, _ in stretches]
        gaps = [b - a for a, b in zip(starts, starts[1:])]
        lines.append(f"- Stretches last {_f(statistics.median(lengths), 0)} s (median; longest "
                     f"{_f(max(lengths), 0)} s); they begin {_f(statistics.median(gaps), 0) if gaps else '-'} s apart "
                     f"(median of {len(gaps)} gaps).")
        first = seconds[stretches[0][0]]["t"]
        lines.append(f"- The first begins {_f(first / 60.0)} min into the session "
                     f"({len([1 for a, _ in stretches if seconds[a]['t'] < seconds[-1]['t'] / 2])} in its first half, "
                     f"{len([1 for a, _ in stretches if seconds[a]['t'] >= seconds[-1]['t'] / 2])} in its second).")
    if repeat and strength >= 0.2:
        lines.append(f"- **Periodic:** the missed slots repeat every ~{repeat} s (autocorrelation {strength:.2f}).")
    elif repeat:
        lines.append(f"- No clear period (strongest repeat {repeat} s, autocorrelation {strength:.2f}).")
    if laggy and smooth:
        lines += ["", "What differs in the laggy seconds (mean, laggy vs smooth; sorted by the ratio):", "",
                  "| metric | laggy | smooth | ratio | correlation with missed slots |", "|---|---|---|---|---|"]
        rows = []
        for key, label in METRICS:
            a = mean(s.get(key, 0) for s in laggy)
            b = mean(s.get(key, 0) for s in smooth)
            ratio = a / b if b else (float("inf") if a else 1.0)
            r = correlation([s.get(key, 0) for s in seconds], [s.get("missed", 0) for s in seconds])
            rows.append((ratio if ratio != float("inf") else 1e9, label, a, b, r))
        rows.sort(key=lambda row: -abs(math.log(row[0])) if row[0] > 0 else 0)
        for ratio, label, a, b, r in rows:
            shown = "new" if ratio >= 1e9 else f"{ratio:.2f}x"
            lines.append(f"| {label} | {_f(a, 2)} | {_f(b, 2)} | {shown} | {r:+.2f} |")
    return lines


def thermal_analysis(seconds):
    lines = []
    if not seconds:
        return lines
    by_state = collections.defaultdict(list)
    for second in seconds:
        by_state[second.get("thermal", 0)].append(second)
    lines += ["| thermal state | seconds | fps | GPU p50 ms | GPU p95 ms | guest ms | missed slots /min |",
              "|---|---|---|---|---|---|---|"]
    for state in sorted(by_state):
        rows = by_state[state]
        lines.append(f"| {THERMAL[state] if 0 <= state < 4 else state} | {len(rows)} | {_f(mean(s['fps'] for s in rows))} | "
                     f"{_f(mean(s.get('gpu_p50') for s in rows), 2)} | {_f(mean(s.get('gpu_p95') for s in rows), 2)} | "
                     f"{_f(mean(s.get('guest_ms') for s in rows), 2)} | {_f(missed_rate(rows))} |")
    thermal = [s.get("thermal", 0) for s in seconds]
    lines.append("")
    lines.append(f"- Correlation of the thermal state with GPU p50: {correlation(thermal, [s.get('gpu_p50') for s in seconds]):+.2f}; "
                 f"with missed slots: {correlation(thermal, [s.get('missed', 0) for s in seconds]):+.2f}; "
                 f"with fps: {correlation(thermal, [s.get('fps') for s in seconds]):+.2f}.")
    changes = [(a["t"], a.get("thermal", 0), b.get("thermal", 0)) for a, b in zip(seconds, seconds[1:])
               if a.get("thermal", 0) != b.get("thermal", 0)]
    if changes:
        lines.append("- Changes: " + ", ".join(f"{_f(t / 60.0)} min {THERMAL[a] if 0 <= a < 4 else a} -> "
                                             f"{THERMAL[b] if 0 <= b < 4 else b}" for t, a, b in changes[:20]))
    else:
        lines.append(f"- No change: {THERMAL[thermal[0]] if 0 <= thermal[0] < 4 else thermal[0]} throughout.")
    return lines


def trends(seconds):
    lines = []
    if len(seconds) < 30:
        return ["- Too short for trends."]
    times = [s["t"] for s in seconds]
    for key, label, unit in (("footprint_mb", "Memory footprint", "MB"), ("metal_mb", "Metal allocations", "MB"),
                             ("gpu_p50", "GPU ms p50", "ms"), ("gpu_p95", "GPU ms p95", "ms"),
                             ("guest_ms", "Guest ms a frame", "ms"), ("host_ms", "Host ms a frame", "ms"),
                             ("fps", "fps", "")):
        values = [s.get(key) for s in seconds]
        if not any(values):
            continue
        n = max(1, len(values) // 10)
        lines.append(f"- {label}: {_f(mean(values[:n]), 2)} {unit} in the first tenth, {_f(mean(values[-n:]), 2)} {unit} "
                     f"in the last; {slope_per_minute(times, values):+.3f} {unit}/min; max {_f(max(v or 0 for v in values), 2)}.")
    footprint = [s.get("footprint_mb") or 0 for s in seconds]
    drops = [(seconds[i]["t"], footprint[i - 1] - footprint[i]) for i in range(1, len(footprint))
             if footprint[i - 1] - footprint[i] > 20]
    if drops:
        gaps = [b[0] - a[0] for a, b in zip(drops, drops[1:])]
        lines.append(f"- Footprint drops of over 20 MB: {len(drops)} (largest {_f(max(d for _, d in drops))} MB)"
                     + (f", {_f(statistics.median(gaps), 0)} s apart (median)" if gaps else "") + ": a sawtooth "
                     "if they repeat, memory freed in bulk.")
    deletes = [s.get("tex_deletes", 0) for s in seconds]
    if any(deletes):
        sweep, strength = period(deletes, times, low=5, high=120)
        lines.append(f"- Textures dropped: {sum(deletes)}; they come every ~{sweep} s (autocorrelation {strength:.2f})"
                     if sweep else f"- Textures dropped: {sum(deletes)}.")
    return lines


def verdict(seconds, hitches):
    """the likeliest causes of the lag, in a few lines: what grows with it
    (a ratio of laggy to smooth seconds, and a correlation), the thermal
    state, the period. Evidence, not proof."""
    laggy = [s for s in seconds if s.get("missed", 0) >= LAG_MISSED]
    smooth = [s for s in seconds if s.get("missed", 0) == 0]
    if not laggy:
        return ["- No laggy seconds (none missed 3 or more display slots)."]
    lines = []
    suspects = []
    for key, label in METRICS:
        if key in ("views", "footprint_mb", "metal_mb"):
            continue
        a = mean(s.get(key, 0) for s in laggy)
        b = mean(s.get(key, 0) for s in smooth) if smooth else 0.0
        r = correlation([s.get(key, 0) for s in seconds], [s.get("missed", 0) for s in seconds])
        if a > b * 1.25 and a - b > 0.05 and r > 0.2:
            suspects.append((r, label, a, b))
    suspects.sort(reverse=True)
    for r, label, a, b in suspects[:4]:
        lines.append(f"- In the laggy seconds **{label}** is {_f(a, 2)} against {_f(b, 2)} in the smooth ones "
                     f"(correlation with the missed slots {r:+.2f}).")
    thermal = correlation([s.get("thermal", 0) for s in seconds], [s.get("missed", 0) for s in seconds])
    states = sorted({s.get("thermal", 0) for s in seconds})
    if len(states) > 1:
        lines.append(f"- The thermal state went {' -> '.join(THERMAL[x] for x in states)}; its correlation with "
                     f"the missed slots is {thermal:+.2f}, with GPU p50 "
                     f"{correlation([s.get('thermal', 0) for s in seconds], [s.get('gpu_p50') for s in seconds]):+.2f}.")
    else:
        lines.append(f"- The thermal state stayed {THERMAL[states[0]] if 0 <= states[0] < 4 else states[0]}: "
                     "throttling the OS reported does not explain it.")
    split_laggy = mean(s.get("views", 1) for s in laggy)
    split_smooth = mean(s.get("views", 1) for s in smooth) if smooth else split_laggy
    if split_laggy >= 1.5 or split_smooth >= 1.5:
        lines.append(f"- Views drawn: {_f(split_laggy, 2)} in the laggy seconds, {_f(split_smooth, 2)} in the smooth.")
    repeat, strength = period([min(s.get("missed", 0), 30) for s in seconds], [s["t"] for s in seconds])
    if repeat and strength >= 0.2:
        lines.append(f"- The lag repeats every ~{repeat} s (autocorrelation {strength:.2f}).")
    causes = collections.Counter(hitch_cause(h)[0] for h in hitches)
    if causes:
        lines.append("- Hitches (frames over 50 ms) were mostly: " +
                     ", ".join(f"{name} ({count})" for name, count in causes.most_common(3)) + ".")
    if not suspects:
        lines.append("- Nothing the recorder counts grows with the lag: look at the guest/GPU split and the "
                     "per-view numbers below.")
    return lines


def report(sessions):
    lines = ["# OpenCE TV play telemetry", ""]
    if not sessions:
        return "\n".join(lines + ["No sessions."]) + "\n"
    all_seconds, all_hitches = [], []
    lines += ["## Sessions", "", "| session | device | minutes | maps | players | fps | missed slots /min | hitches | "
              "thermal max | footprint first -> max MB |", "|---|---|---|---|---|---|---|---|---|---|"]
    for session in sessions:
        seconds = session["seconds"]
        play = gameplay(seconds)
        maps = sorted({s.get("map") for s in play if s.get("map")})
        players = sorted({s.get("players", 0) for s in play})
        footprint = [s.get("footprint_mb") or 0 for s in seconds]
        lines.append(f"| {session['name']} | {session['session'].get('device', '?')} | "
                     f"{_f(sum(s.get('dur', 1) for s in seconds) / 60.0)} | {', '.join(maps) or '-'} | "
                     f"{', '.join(map(str, players)) or '-'} | {_f(mean(s['fps'] for s in play))} | "
                     f"{_f(missed_rate(play))} | {len(session['hitches'])} | "
                     f"{THERMAL[max((s.get('thermal', 0) for s in seconds), default=0)]} | "
                     f"{_f(footprint[0] if footprint else 0, 0)} -> {_f(max(footprint, default=0), 0)} |")
        # (seconds of the sessions one after another, for the tables)
        for second in play:
            all_seconds.append(dict(second, session=session["name"]))
        for hitch in session["hitches"]:
            all_hitches.append(dict(hitch, session=session["name"]))

    lines += ["", "## Verdict: what the lag goes with", ""] + verdict(all_seconds, all_hitches)
    lines += ["", "## Where the time goes (seconds of play)", ""]
    lines += group_table(all_seconds, lambda s: f"{s.get('map') or '?'}, {s.get('players', 0)} player(s)",
                         "map, players", sort=lambda item: item[0])
    lines += ["", "Frame budget, mean ms a frame: guest (the game) / host (Metal encoding) / GPU (command buffers) / "
              "waits.", ""]
    lines.append(f"- guest {_f(mean(s.get('guest_ms') for s in all_seconds), 2)}, host "
                 f"{_f(mean(s.get('host_ms') for s in all_seconds), 2)}, GPU {_f(mean(s.get('gpu_ms') for s in all_seconds), 2)}, "
                 f"drawable wait {_f(mean(s.get('wait_drawable_ms') for s in all_seconds), 2)}, in-flight wait "
                 f"{_f(mean(s.get('wait_inflight_ms') for s in all_seconds), 2)}.")
    split = [s for s in all_seconds if s.get("views", 1) >= 2]
    if split:
        lines += ["", "### Split screen: per view (mean a frame)", "",
                  "| views | seconds | view | draws | passes | shadow draws | fog draws | transparent draws | ms |",
                  "|---|---|---|---|---|---|---|---|---|"]
        for count in sorted({s.get("views") for s in all_seconds}):
            rows = [s for s in all_seconds if s.get("views") == count]
            for index, name in enumerate(("player 1", "player 2", "player 3", "player 4", "other")):
                views = [s["view"][index] for s in rows if s.get("view") and len(s["view"]) > index]
                if not views or not any(v.get("draws") for v in views):
                    continue
                lines.append(f"| {count} | {len(rows)} | {name} | {_f(mean(v.get('draws') for v in views), 0)} | "
                             f"{_f(mean(v.get('passes') for v in views))} | {_f(mean(v.get('shadow') for v in views))} | "
                             f"{_f(mean(v.get('fog') for v in views))} | {_f(mean(v.get('transparent') for v in views))} | "
                             f"{_f(mean(v.get('ms') for v in views), 2)} |")

    lines += ["", "## By BSP", ""]
    lines += group_table(all_seconds, lambda s: f"{s.get('map') or '?'} bsp {s.get('bsp', -1)}, "
                         f"{s.get('players', 0)}p", "map, BSP, players")
    lines += ["", f"## Worst regions (player 1's position in {BUCKET:g}-unit squares, by missed slots a minute)", ""]
    worst = collections.defaultdict(list)
    for second in all_seconds:
        worst[position_key(second)].append(second)
    regions = [(key, rows) for key, rows in worst.items() if len(rows) >= 5]
    regions.sort(key=lambda item: -missed_rate(item[1]))
    lines += group_table([s for key, rows in regions[:15] for s in rows], position_key, "region", limit=15)

    lines += ["", "## Hitches (frames over 50 ms)", ""]
    if all_hitches:
        causes = collections.Counter()
        cause_ms = collections.defaultdict(list)
        for hitch in all_hitches:
            name, value = hitch_cause(hitch)
            causes[name] += 1
            cause_ms[name].append(hitch.get("wall_ms", 0))
        lines += ["| cause (the frame's largest part) | hitches | median frame ms | worst ms |", "|---|---|---|---|"]
        for name, count in causes.most_common():
            lines.append(f"| {name} | {count} | {_f(statistics.median(cause_ms[name]))} | {_f(max(cause_ms[name]))} |")
        events = collections.Counter(h.get("event") for h in all_hitches if h.get("event"))
        if events:
            lines += ["", "Events in a hitch's frame: " + ", ".join(f"{name} ({count})" for name, count in events.most_common(10))]
        lines += ["", "Worst 10:", "", "| session | t (min) | map | bsp | views | wall | guest | host | GPU | compile | "
                  "converts (ms) | uploads | file ms | event |", "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for hitch in sorted(all_hitches, key=lambda h: -h.get("wall_ms", 0))[:10]:
            lines.append(f"| {hitch['session']} | {_f(hitch['t'] / 60.0, 2)} | {hitch.get('map')} | {hitch.get('bsp')} | "
                         f"{hitch.get('views')} | {_f(hitch.get('wall_ms', 0))} | {_f(hitch.get('guest_ms', 0))} | "
                         f"{_f(hitch.get('host_ms', 0))} | {_f(hitch.get('gpu_ms', 0))} | {_f(hitch.get('compile_ms', 0))} | "
                         f"{hitch.get('tex_converts', 0)} ({_f(hitch.get('tex_convert_ms', 0))}) | {hitch.get('tex_uploads', 0)} | "
                         f"{_f(hitch.get('file_ms', 0))} | {hitch.get('event') or ''} |")
    else:
        lines.append("None.")

    for session in sessions:
        play = gameplay(session["seconds"])
        if len(play) < 30:
            continue
        lines += ["", f"## {session['name']}: the lag over time", ""]
        lines += lag_analysis(play)
        lines += ["", "### Trends", ""] + trends(play)
        lines += ["", "### Thermal state", ""] + thermal_analysis(play)
        lines += ["", "### Minute by minute", "", "| min | map | bsp | views | fps | missed | laggy s | GPU p95 | guest | "
                  "tex conv | stream MB/s | footprint MB | thermal |", "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        minutes = collections.defaultdict(list)
        for second in play:
            minutes[int(second["t"] // 60)].append(second)
        for minute in sorted(minutes):
            rows = minutes[minute]
            lines.append(f"| {minute} | {rows[-1].get('map')} | {rows[-1].get('bsp')} | {rows[-1].get('views')} | "
                         f"{_f(mean(s['fps'] for s in rows))} | {sum(s.get('missed', 0) for s in rows)} | "
                         f"{sum(1 for s in rows if s.get('missed', 0) >= LAG_MISSED)} | "
                         f"{_f(mean(s.get('gpu_p95') for s in rows), 2)} | {_f(mean(s.get('guest_ms') for s in rows), 2)} | "
                         f"{sum(s.get('tex_converts', 0) for s in rows)} | {_f(mean(s.get('stream_mb', 0) for s in rows))} | "
                         f"{_f(rows[-1].get('footprint_mb', 0), 0)} | {THERMAL[rows[-1].get('thermal', 0)]} |")
    lines += ["", "## Lifecycle and memory events", ""] + lifecycle_events(sessions)
    overhead = [s.get("self_ms", 0) for s in all_seconds]
    if overhead:
        lines += ["", "## The recorder's own cost", "",
                  f"- {_f(mean(overhead), 3)} ms a second on the main thread (mean; max {_f(max(overhead), 3)}), "
                  f"{_f(mean(s.get('writer_ms', 0) for s in all_seconds), 3)} ms a second on its writer's queue."]
    return "\n".join(lines) + "\n"


def lifecycle_events(sessions):
    """the app leaving the foreground and coming back, and the memory
    warnings (port/macos/host/host_lifecycle.h): each with the footprint of
    the second before, so whether the system warned before it took memory
    back shows"""
    lines = []
    for session in sessions:
        events = [e for e in session["events"] if e.get("kind") in ("lifecycle", "memory", "active")]
        if not events:
            continue
        lines += [f"{session['name']}:", "", "| t (s) | kind | detail | footprint MB (second before) |",
                  "|---|---|---|---|"]
        seconds = session["seconds"]
        for event in events:
            before = [s for s in seconds if s.get("t", 0) <= event.get("t", 0)]
            footprint = before[-1].get("footprint_mb") if before else None
            lines.append(f"| {_f(event.get('t', 0), 1)} | {event.get('kind')} | {event.get('detail', '')} | "
                         f"{_f(footprint, 0) if footprint is not None else '-'} |")
        lines.append("")
    return lines or ["None."]


# ---------- pulling

def remote_sessions(device, list_json):
    """the TV's session files: {name: size}; {} when it has none (or cannot say)"""
    try:
        deploy.run(["xcrun", "devicectl", "device", "info", "files", "--device", device,
                    "--domain-type", "appDataContainer", "--domain-identifier", deploy.BUNDLE,
                    "--subdirectory", REMOTE, "--no-recurse", "--json-output", list_json],
                   check=False, capture=True, timeout=180)
        files = json.loads(Path(list_json).read_text())["result"]["files"]
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError):
        return {}
    return {entry["name"]: entry.get("metadata", {}).get("size", 0) for entry in files
            if entry.get("name", "").endswith(".jsonl") and not entry.get("resources", {}).get("isDirectory")}


def to_pull(remote, archive):
    """the files new to the archive, or grown since they were copied"""
    names = []
    for name, size in sorted(remote.items()):
        local = Path(archive) / name
        if not local.is_file() or local.stat().st_size != size:
            names.append(name)
    return names


def newest_parts(names):
    """each session's newest part: the one that may still be growing"""
    newest = {}
    for name in names:
        session, part = session_of(name)
        if session not in newest or part > session_of(newest[session])[1]:
            newest[session] = name
    return set(newest.values())


def copy_one(device, name, archive, attempts):
    """a file copied off the TV to archive/name, never raising: "copied",
    "partial" (a broken-off copy, the largest of the attempts, kept when it
    is more than the archive had) or "skipped" """
    best, best_size = None, -1
    for attempt in range(attempts):
        partial = archive / f"{name}.{attempt}.part"
        partial.unlink(missing_ok=True)
        try:
            deploy.run(deploy.copy_args("from", device, f"{REMOTE}/{name}", partial), timeout=600)
            ok = partial.is_file()
        except (OSError, subprocess.SubprocessError) as error:
            print(f"  copy of {name} failed (attempt {attempt + 1} of {attempts}): {str(error)[:200]}", flush=True)
            ok = False
        size = partial.stat().st_size if partial.is_file() else -1
        if ok:
            if best:
                best.unlink(missing_ok=True)
            partial.replace(archive / name)
            return "copied"
        if size > best_size:
            if best:
                best.unlink(missing_ok=True)
            best, best_size = partial, size
        else:
            partial.unlink(missing_ok=True)
    local = archive / name
    have = local.stat().st_size if local.is_file() else 0
    if best and best_size > have:
        best.replace(local)
        return "partial"
    if best:
        best.unlink(missing_ok=True)
    return "skipped"


def clean_parts(archive):
    for leftover in Path(archive).glob("*.part"):
        leftover.unlink(missing_ok=True)


def pull(device, archive, quiet=False):
    """(the device's folder, [names copied or partly copied], [names skipped]);
    never raises for the TV's sake"""
    archive = Path(archive) / device[:8]
    archive.mkdir(parents=True, exist_ok=True)
    clean_parts(archive)
    remote = remote_sessions(device, archive / ".remote.json")
    names = to_pull(remote, archive)
    growing = newest_parts(remote)
    sessions = {session_of(name)[0] for name in remote}
    if not quiet or names:
        print(f"{len(remote)} files ({len(sessions)} sessions) on the TV, {len(names)} new or grown", flush=True)
    pulled, skipped = [], []
    try:
        for name in names:
            # (a closed part is static: a failure is the link's; the newest
            # may be growing, which devicectl can break off at 10 MB)
            result = copy_one(device, name, archive, COPY_ATTEMPTS if name in growing else 2)
            if result == "skipped":
                skipped.append(name)
                print(f"warning: skipped {name} (it did not copy; the next pull tries again)", flush=True)
            else:
                pulled.append(name)
                if result == "partial":
                    print(f"warning: {name}: kept a partial copy "
                          f"({(archive / name).stat().st_size} of {remote[name]} bytes)", flush=True)
    finally:
        clean_parts(archive)
    if skipped:
        print(f"skipped {len(skipped)}: {', '.join(skipped)}", flush=True)
    return archive, pulled, skipped


# ---------- watching

def watch_alerts(sessions, seen):
    """the lines worth calling out that seen (a set, updated) has not had:
    hitches over WATCH_HITCH_MS, and WATCH_SECONDS or more seconds in a row
    of WATCH_BAD frames over 33 ms or slots missed"""
    lines = []
    for session in sessions:
        name = session["name"]
        for hitch in session["hitches"]:
            key = (name, "hitch", hitch.get("t"))
            if hitch.get("wall_ms", 0) > WATCH_HITCH_MS and key not in seen:
                seen.add(key)
                cause, ms = hitch_cause(hitch)
                lines.append(f"{name} t={hitch.get('t', 0) / 60.0:.2f} min HITCH {hitch.get('wall_ms', 0):.0f} ms "
                             f"{hitch.get('map')} bsp {hitch.get('bsp')} {hitch.get('views')}v: {cause} "
                             f"({ms:.0f} ms) {hitch.get('event') or ''}".rstrip())
        run = []
        for second in session["seconds"] + [None]:
            if second is not None and (second.get("over33", 0) >= WATCH_BAD or second.get("missed", 0) >= WATCH_BAD):
                run.append(second)
                continue
            if len(run) >= WATCH_SECONDS:
                key = (name, "lag", run[0].get("t"))
                # (a stretch still going when it was first seen is told again when it ends)
                ended = second is not None
                if key not in seen or (ended and (name, "lag-ended", run[0].get("t")) not in seen):
                    if ended:
                        seen.add((name, "lag-ended", run[0].get("t")))
                    seen.add(key)
                    lines.append(f"{name} t={run[0].get('t', 0) / 60.0:.2f} min LAG {len(run)} s"
                                 f"{'' if ended else ' (so far)'} {run[-1].get('map')} bsp {run[-1].get('bsp')} "
                                 f"{run[-1].get('views')}v: fps {mean(s.get('fps') for s in run):.1f}, "
                                 f"missed {sum(s.get('missed', 0) for s in run)}, over 33 ms "
                                 f"{sum(s.get('over33', 0) for s in run)}, GPU p95 "
                                 f"{mean(s.get('gpu_p95') for s in run):.1f}, guest "
                                 f"{mean(s.get('guest_ms') for s in run):.1f} ms")
            run = []
    return lines


def watch(device, archive, interval):
    seen, first = set(), True
    try:
        while True:
            folder, _, _ = pull(device, archive, quiet=not first)
            sessions = load_sessions([folder], last=1)
            alerts = watch_alerts(sessions, seen)
            if first:
                print(f"watching {sessions[0]['name'] if sessions else '(no session yet)'} every {interval} s: "
                      f"{len(alerts)} hitches and lag stretches so far (Ctrl-C ends)", flush=True)
                first = False
            else:
                for line in alerts:
                    print(line, flush=True)
            time.sleep(interval)
    except KeyboardInterrupt:
        pass


# ---------- live

def listen(port, archive):
    archive = Path(archive) / "live"
    archive.mkdir(parents=True, exist_ok=True)
    out = archive / (datetime.datetime.now().strftime("%Y%m%dT%H%M%S") + "-live.jsonl")
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind(("0.0.0.0", port))
    print(f"listening on UDP {port}; writing {out} (Ctrl-C ends)", flush=True)
    with out.open("a") as handle:
        try:
            while True:
                data, sender = receiver.recvfrom(65536)
                text = data.decode("utf-8", "replace")
                handle.write(text if text.endswith("\n") else text + "\n")
                handle.flush()
                try:
                    record = json.loads(text)
                except ValueError:
                    continue
                if record.get("k") == "s":
                    print(f"{sender[0]} t={record['t']:.0f}s {record.get('map')} bsp {record.get('bsp')} "
                          f"{record.get('views')}v fps {record.get('fps', 0):.1f} missed {record.get('missed')} "
                          f"guest {record.get('guest_ms', 0):.1f} GPU p95 {record.get('gpu_p95', 0):.1f} "
                          f"thermal {record.get('thermal')} {record.get('footprint_mb', 0):.0f} MB", flush=True)
                elif record.get("k") in ("hitch", "event"):
                    print(f"{sender[0]} {record['k']}: {json.dumps(record)[:200]}", flush=True)
        except KeyboardInterrupt:
            pass
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("pull")
    p.add_argument("--device", default=deploy.DEVICE)
    p.add_argument("--archive", default=str(ARCHIVE))
    p.add_argument("--report", action="store_true", help="then report the newest session")
    p.add_argument("--watch", type=float, metavar="N", help="pull again every N seconds, printing new trouble")
    p = sub.add_parser("report")
    p.add_argument("paths", nargs="*", help=f"sessions or folders (default: {ARCHIVE})")
    p.add_argument("--last", type=int, help="only the newest N sessions")
    p.add_argument("--out", help="also write the markdown here")
    p = sub.add_parser("listen")
    p.add_argument("--port", type=int, default=LIVE_PORT)
    p.add_argument("--archive", default=str(ARCHIVE))
    args = parser.parse_args(argv)
    if args.command == "pull":
        if args.watch:
            watch(args.device, args.archive, max(5.0, args.watch))
            return 0
        folder, names, skipped = pull(args.device, args.archive)
        print(f"archive: {folder}")
        if args.report and (names or list(folder.glob("*.jsonl"))):
            print(report(load_sessions([folder], last=1)))
        return 0
    if args.command == "listen":
        listen(args.port, args.archive)
        return 0
    text = report(load_sessions(args.paths or [ARCHIVE], args.last))
    if args.out:
        Path(args.out).write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
