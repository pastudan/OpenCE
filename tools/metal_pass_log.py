#!/usr/bin/env python3
"""The Metal host's pass log (HALO_PASS_LOG=1 with HALO_FRAME_LOG=1;
port/macos/host/host_frame_log.h), summarized: what a frame's render passes
are, what they load and store, and the GPU time of each (its stages'
timestamps) and of each of the rasterizer's phases.

    python3 tools/metal_pass_log.py PASSES.csv [--frames FRAME_LOG.csv --scene b30] [--json]

With the frame log and its scene (tools/tvos_bench.py's scenes) only the
scene's steady window is counted; else every frame from the 300th.

Times: on Apple's tile-based GPUs a pass's vertex stage (its geometry,
binned) and fragment stage (its tiles: shading, blending, loads and stores)
run apart, and passes overlap; the fragment stage's time is where fill and
bandwidth go. A pass's time is from its stage's first to last sample, so a
pass waiting on another counts that wait: sums are an upper bound, and the
frame's span (first start to last end) is the real one.
"""
import argparse
import collections
import csv
import io
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

# source/rasterizer/rasterizer.h's _rasterizer_profile_* order
PHASES = ["clear", "model_sky", "models", "env_lightmaps", "env_shadows", "env_diffuse_lights", "env_decals_light",
          "env_decals_alpha_tested", "env_textures", "env_decals_primary", "env_decals_secondary",
          "env_specular_lights", "env_specular_lightmaps", "env_reflection_lightmap_masks", "env_reflection_mirrors",
          "env_reflections", "env_transparents", "fog_planes", "fog_screen", "water", "env_decals_water",
          "detail_objects", "queued_transparents", "lens_flare_occlusion_submit", "lens_flare_occlusion_query",
          "lens_flares", "screen_effect", "hud", "screen_flash"]
LOAD = {0: "dontcare", 1: "load", 2: "clear"}
STORE = {0: "dontcare", 1: "store", 2: "resolve", 3: "store+resolve", 4: "unknown", 5: "custom"}
REASON = {0: "other", 1: "targets", 2: "blit", 3: "present", 4: "phase", 5: "flush"}
# bytes a pixel of an attachment's format in memory (BGRA8; Depth32Float_Stencil8: 4 + 1)
COLOR_BYTES = {80: 4, 70: 4, 81: 4}
DEPTH_BYTES, STENCIL_BYTES = 4, 1


def read_passes(text):
    lines = [line for line in text.splitlines() if line and not line.startswith("#")]
    rows = []
    for row in csv.DictReader(io.StringIO("\n".join(lines))):
        rows.append({key: (float(value) if key.endswith("_ms") else int(value)) for key, value in row.items()})
    return rows


def phases_of(mask):
    return [PHASES[index] for index in range(len(PHASES)) if mask >> index & 1]


def pass_traffic(row):
    """bytes the pass's loads and stores move (attachments' whole size)"""
    pixels = row["width"] * row["height"]
    color = COLOR_BYTES.get(row["color_format"], 4) * pixels
    loaded = stored = 0
    if row["color"]:
        loaded += color if row["color_load"] == 1 else 0
        stored += color if row["color_store"] in (1, 3) else 0
    if row["depth"]:
        loaded += DEPTH_BYTES * pixels if row["depth_load"] == 1 else 0
        stored += DEPTH_BYTES * pixels if row["depth_store"] in (1, 3) else 0
        loaded += STENCIL_BYTES * pixels if row["stencil_load"] == 1 else 0
        stored += STENCIL_BYTES * pixels if row["stencil_store"] in (1, 3) else 0
    return loaded, stored


def describe(row):
    targets = []
    if row["color"]:
        targets.append(f"c{row['color']}")
    if row["depth"]:
        targets.append(f"d{row['depth']}")
    action = (f"{LOAD.get(row['color_load'], '?')}/{STORE.get(row['color_store'], '?')}" if row["color"] else "-")
    if row["depth"]:
        action += (f" z {LOAD.get(row['depth_load'], '?')}/{STORE.get(row['depth_store'], '?')}"
                   f" s {LOAD.get(row['stencil_load'], '?')}/{STORE.get(row['stencil_store'], '?')}")
    phases = phases_of(row["phase_mask"]) or (["(clears)"] if row["clears"] else ["(empty)"])
    return (f"{'+'.join(targets)} {row['width']}x{row['height']} [{action}] end={REASON.get(row['reason'], '?')} "
            f"draws={row['draws']} clears={row['clears']} phases={','.join(phases)}")


def summarize(rows, serials=None):
    frames = collections.OrderedDict()
    for row in rows:
        if serials is not None and row["serial"] not in serials:
            continue
        frames.setdefault(row["serial"], []).append(row)
    if serials is None:
        keys = list(frames)[300:]
        frames = collections.OrderedDict((key, frames[key]) for key in keys)
    timed = {serial: passes for serial, passes in frames.items() if passes and all(p["fragment_ms"] or p["gpu_ms"] or
                                                                                    not p["draws"] for p in passes)}
    count = len(frames) or 1
    phase_fragment = collections.Counter()
    phase_vertex = collections.Counter()
    phase_draws = collections.Counter()
    reason = collections.Counter()
    per_index = collections.defaultdict(list)
    loaded = stored = 0
    passes = 0
    span, fragment_sum = [], []
    for serial, frame in frames.items():
        passes += len(frame)
        first = last = None
        total = 0.0
        for row in frame:
            reason[REASON.get(row["reason"], "?")] += 1
            load, store = pass_traffic(row)
            loaded += load
            stored += store
            key = "+".join(phases_of(row["phase_mask"])) or ("clears" if row["clears"] else "empty")
            time = row["fragment_ms"] or row["gpu_ms"]
            if serial in timed:
                phase_fragment[key] += time
                phase_vertex[key] += row["vertex_ms"]
                total += time
                start = row["vertex_start_ms"] or row["fragment_start_ms"]
                end = (row["fragment_start_ms"] + row["fragment_ms"]) if row["fragment_ms"] else 0
                if start:
                    first = start if first is None else min(first, start)
                if end:
                    last = end if last is None else max(last, end)
            phase_draws[key] += row["draws"]
            per_index[row["index"]].append(row)
        if serial in timed:
            fragment_sum.append(total)
            if first is not None and last is not None:
                span.append(last - first)
    timed_count = len(timed) or 1
    # the typical frame: each pass index's most common description
    typical = []
    for index in sorted(per_index):
        rows_at = per_index[index]
        if len(rows_at) < count * 0.5:
            continue
        description = collections.Counter(describe(row) for row in rows_at).most_common(1)[0]
        timed_rows = [row for row in rows_at if row["serial"] in timed]
        typical.append({
            "index": index, "share": round(len(rows_at) / count, 2), "pass": description[0],
            "same": round(description[1] / len(rows_at), 2),
            "fragment_ms": round(sum(row["fragment_ms"] or row["gpu_ms"] for row in timed_rows) / (len(timed_rows) or 1), 3),
            "vertex_ms": round(sum(row["vertex_ms"] for row in timed_rows) / (len(timed_rows) or 1), 3),
            "load_store_mb": round(sum(sum(pass_traffic(row)) for row in rows_at) / len(rows_at) / 1e6, 1),
        })

    def mean(values):
        return round(sum(values) / len(values), 3) if values else None

    def pct(values, fraction):
        if not values:
            return None
        ordered = sorted(values)
        return round(ordered[min(len(ordered) - 1, max(0, int(fraction * len(ordered) + 0.999999) - 1))], 3)

    return {
        "frames": len(frames), "timed_frames": len(timed),
        "passes_per_frame": round(passes / count, 2),
        "pass_end_reasons_per_frame": {key: round(value / count, 2) for key, value in reason.most_common()},
        "load_mb_per_frame": round(loaded / count / 1e6, 1), "store_mb_per_frame": round(stored / count / 1e6, 1),
        "fragment_sum_ms": {"mean": mean(fragment_sum), "p50": pct(fragment_sum, 0.5), "p95": pct(fragment_sum, 0.95)},
        "frame_span_ms": {"mean": mean(span), "p50": pct(span, 0.5), "p95": pct(span, 0.95)},
        "phases": [{"phase": key, "fragment_ms": round(value / timed_count, 3),
                    "vertex_ms": round(phase_vertex[key] / timed_count, 3),
                    "draws": round(phase_draws[key] / count, 1)} for key, value in phase_fragment.most_common()],
        "typical_frame": typical,
    }


def window_serials(frame_log, scene):
    import tvos_bench as bench
    events, rows = bench.parse_frame_log(Path(frame_log).read_text())
    window, description = bench.scene_window(scene, events, rows)
    if not window:
        raise SystemExit(f"no window: {description}")
    begin = window[0]["t_ms"]
    return {row["serial"] for row in window if row["t_ms"] - begin >= bench.WARMUP_SECONDS * 1000.0}, description


def text_report(summary):
    lines = [f"frames {summary['frames']} (timed {summary['timed_frames']}); passes/frame {summary['passes_per_frame']}; "
             f"ends {summary['pass_end_reasons_per_frame']}",
             f"attachment loads {summary['load_mb_per_frame']} MB + stores {summary['store_mb_per_frame']} MB a frame",
             f"fragment-stage sum {summary['fragment_sum_ms']} ms; frame GPU span {summary['frame_span_ms']} ms", "",
             "| phase(s) of the pass | fragment ms/frame | vertex ms/frame | draws/frame |", "|---|---|---|---|"]
    for phase in summary["phases"]:
        lines.append(f"| {phase['phase']} | {phase['fragment_ms']:.3f} | {phase['vertex_ms']:.3f} | {phase['draws']} |")
    lines += ["", "| # | in frames | pass (most common) | same | fragment ms | vertex ms | load+store MB |",
              "|---|---|---|---|---|---|---|"]
    for row in summary["typical_frame"]:
        lines.append(f"| {row['index']} | {row['share']} | {row['pass']} | {row['same']} | {row['fragment_ms']} | "
                     f"{row['vertex_ms']} | {row['load_store_mb']} |")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("passes")
    parser.add_argument("--frames", help="the run's frame log (HALO_FRAME_LOG's CSV)")
    parser.add_argument("--scene", help="its scene (tools/tvos_bench.py), for the steady window")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    serials, description = None, "every frame from the 300th"
    if args.frames and args.scene:
        serials, description = window_serials(args.frames, args.scene)
    summary = summarize(read_passes(Path(args.passes).read_text()), serials)
    summary["window"] = description
    print(json.dumps(summary, indent=2) if args.json else f"{description}\n" + text_report(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
