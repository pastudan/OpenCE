"""How close a frame drawn with a half-resolution feature (display.half_res_*,
port/macos/guest/d3d8_metal.c) is to the same frame drawn at full resolution:
per region, PSNR over the colour channels, the largest difference, the mean,
and the 99.9th percentile of each pixel's largest channel difference.

    python3 tools/halfres_compare.py REFERENCE.bmp TEST.bmp [--base BASE.bmp] [--json OUT]
    python3 tools/halfres_compare.py --pairs DIR_REF DIR_TEST [--base-dir DIR_BASE] [--json OUT]

REFERENCE is the frame at full resolution, TEST the same frame with the
feature on (debug.screenshot_every's BMPs of one frozen frame:
HALO_HALF_RES_CYCLE alternates the two in one run). The regions:
- all: every pixel;
- touched / untouched: with --base, a frame without the effect at all (the
  convolution or the transparents skipped): the pixels the effect changes
  in the reference, and the rest, which a correct composite leaves exactly
  as they were (untouched must have max 0);
- edges / interior: touched pixels on and off the opaque scene's edges
  (in BASE, or REFERENCE without a base: a pixel whose 3x3 neighbourhood
  differs by more than --edge-threshold in some channel), where an upsample
  from half resolution errs most;
- centre / border: the middle and the rest of the picture by distance from
  its centre (inside --centre of the half diagonal), for a zoom's warp,
  which is sharp at its centre.

Builds a small C program with cc in a temporary folder; reads the BMPs (the
32-bit top-down ones write_screenshot writes) and writes nothing else.
"""
import argparse
import functools
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path

REGIONS = ("all", "touched", "untouched", "edges", "interior", "centre", "border")

PROGRAM = r'''
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ALL, TOUCHED, UNTOUCHED, EDGES, INTERIOR, CENTRE, BORDER, REGIONS };

static unsigned char *load(const char *path, int *width, int *height)
{
	FILE *file = fopen(path, "rb");
	unsigned char header[54], *pixels;
	int w, h, bits, offset, row;
	if (!file || fread(header, 1, 54, file) != 54 || header[0] != 'B' || header[1] != 'M') {
		fprintf(stderr, "%s: not a BMP\n", path);
		exit(2);
	}
	offset = header[10] | header[11] << 8 | header[12] << 16 | header[13] << 24;
	memcpy(&w, header + 18, 4);
	memcpy(&h, header + 22, 4);
	bits = header[28] | header[29] << 8;
	if (bits != 32) {
		fprintf(stderr, "%s: %d bits a pixel, not 32\n", path, bits);
		exit(2);
	}
	*width = w;
	*height = h < 0 ? -h : h;
	pixels = malloc((size_t)w * *height * 4);
	fseek(file, offset, SEEK_SET);
	for (row = 0; row < *height; row++) {
		/* (rows from the top: a bottom-up BMP's, turned) */
		int at = h < 0 ? row : *height - 1 - row;
		if (fread(pixels + (size_t)at * w * 4, 4, w, file) != (size_t)w) {
			fprintf(stderr, "%s: short\n", path);
			exit(2);
		}
	}
	fclose(file);
	return pixels;
}

static int edge(const unsigned char *p, int w, int h, int x, int y, int threshold)
{
	int dx, dy, c;
	const unsigned char *at = p + ((size_t)y * w + x) * 4;
	for (dy = -1; dy <= 1; dy++)
		for (dx = -1; dx <= 1; dx++) {
			int nx = x + dx, ny = y + dy;
			const unsigned char *n;
			if (nx < 0 || ny < 0 || nx >= w || ny >= h)
				continue;
			n = p + ((size_t)ny * w + nx) * 4;
			for (c = 0; c < 3; c++)
				if (abs(n[c] - at[c]) > threshold)
					return 1;
		}
	return 0;
}

/* argv: reference test base|- edge_threshold centre_fraction
stdout: a line a region: name pixels squares histogram[256] (each pixel's
largest channel difference) */
int main(int argc, char **argv)
{
	int w, h, wt, ht, wb = 0, hb = 0, x, y, c, r;
	unsigned char *ref, *test, *base = NULL;
	int threshold = atoi(argv[4]);
	double centre = atof(argv[5]);
	static unsigned long long histogram[REGIONS][256], pixels[REGIONS];
	static double squares[REGIONS];
	static const char *names[REGIONS] = { "all", "touched", "untouched", "edges", "interior", "centre", "border" };
	ref = load(argv[1], &w, &h);
	test = load(argv[2], &wt, &ht);
	if (strcmp(argv[3], "-"))
		base = load(argv[3], &wb, &hb);
	if (wt != w || ht != h || (base && (wb != w || hb != h))) {
		fprintf(stderr, "sizes differ\n");
		return 2;
	}
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			size_t i = ((size_t)y * w + x) * 4;
			int biggest = 0, in[REGIONS] = { 0 };
			double sq = 0.0, dxc = x + 0.5 - w / 2.0, dyc = y + 0.5 - h / 2.0;
			for (c = 0; c < 3; c++) {
				int d = abs(ref[i + c] - test[i + c]);
				biggest = d > biggest ? d : biggest;
				sq += (double)d * d;
			}
			in[ALL] = 1;
			if (base) {
				int touched = ref[i] != base[i] || ref[i + 1] != base[i + 1] || ref[i + 2] != base[i + 2];
				in[TOUCHED] = touched;
				in[UNTOUCHED] = !touched;
				if (touched) {
					int e = edge(base, w, h, x, y, threshold);
					in[EDGES] = e;
					in[INTERIOR] = !e;
				}
			} else {
				int e = edge(ref, w, h, x, y, threshold);
				in[EDGES] = e;
				in[INTERIOR] = !e;
			}
			if (sqrt(dxc * dxc + dyc * dyc) <= centre * sqrt(w * w + h * h) / 2.0)
				in[CENTRE] = 1;
			else
				in[BORDER] = 1;
			for (r = 0; r < REGIONS; r++)
				if (in[r]) {
					pixels[r]++;
					squares[r] += sq;
					histogram[r][biggest]++;
				}
		}
	for (r = 0; r < REGIONS; r++) {
		int b;
		printf("%s %llu %.0f", names[r], pixels[r], squares[r]);
		for (b = 0; b < 256; b++)
			printf(" %llu", histogram[r][b]);
		printf("\n");
	}
	return 0;
}
'''


@functools.lru_cache(maxsize=None)
def program():
    folder = Path(tempfile.mkdtemp(prefix="halfres-compare-"))
    (folder / "compare.c").write_text(PROGRAM)
    subprocess.run(["cc", "-std=c99", "-O2", str(folder / "compare.c"), "-o", str(folder / "compare"), "-lm"],
                   check=True)
    return folder / "compare"


def psnr(squares, values):
    return math.inf if not squares else 10 * math.log10(255.0 * 255.0 * values / squares)


def summarize(pixels, squares, histogram):
    """a region's numbers from its pixel count, squared channel differences
    and histogram of each pixel's largest channel difference"""
    if not pixels:
        return {"pixels": 0}
    largest = max((value for value, count in enumerate(histogram) if count), default=0)
    total = sum(value * count for value, count in enumerate(histogram))
    # (the 99.9th percentile of the pixels' largest channel difference)
    wanted, seen, p999 = pixels * 0.999, 0, 0
    for value, count in enumerate(histogram):
        seen += count
        if seen >= wanted:
            p999 = value
            break
    return {"pixels": pixels, "psnr_db": psnr(squares, pixels * 3), "max": largest,
            "mean_largest": total / pixels, "p99_9": p999,
            "changed": 1.0 - histogram[0] / pixels, "over_8": sum(histogram[9:]) / pixels}


def compare(reference, test, base=None, edge_threshold=24, centre=0.25):
    """each region's numbers (REGIONS), for a pair of BMPs"""
    result = subprocess.run([str(program()), str(reference), str(test), str(base) if base else "-",
                             str(edge_threshold), str(centre)], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    report = {}
    for line in result.stdout.split("\n"):
        if not line:
            continue
        name, pixels, squares, *histogram = line.split()
        if name in ("touched", "untouched") and not base:
            continue
        report[name] = summarize(int(pixels), float(squares), [int(count) for count in histogram])
    return report


def table(rows):
    """markdown, a line a (label, region) with numbers"""
    lines = ["| frame | region | pixels | PSNR dB | max | p99.9 | mean | changed | >8 |", "|---|---|---|---|---|---|---|---|---|"]
    for label, report in rows:
        for region in REGIONS:
            numbers = report.get(region)
            if not numbers or not numbers["pixels"]:
                continue
            lines.append(f"| {label} | {region} | {numbers['pixels']} | {numbers['psnr_db']:.2f} | {numbers['max']} "
                         f"| {numbers['p99_9']} | {numbers['mean_largest']:.3f} | {100 * numbers['changed']:.2f}% "
                         f"| {100 * numbers['over_8']:.3f}% |")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reference", nargs="?")
    parser.add_argument("test", nargs="?")
    parser.add_argument("--base")
    parser.add_argument("--pairs", nargs=2, metavar=("DIR_REF", "DIR_TEST"),
                        help="every BMP name in both folders, compared")
    parser.add_argument("--base-dir")
    parser.add_argument("--edge-threshold", type=int, default=24)
    parser.add_argument("--centre", type=float, default=0.25)
    parser.add_argument("--json")
    args = parser.parse_args(argv)
    rows = []
    if args.pairs:
        first, second = (Path(folder) for folder in args.pairs)
        for path in sorted(first.glob("*.bmp")):
            if (second / path.name).is_file():
                base = Path(args.base_dir) / path.name if args.base_dir else None
                rows.append((path.name, compare(path, second / path.name, base if base and base.is_file() else None,
                                                args.edge_threshold, args.centre)))
    elif args.reference and args.test:
        rows.append((Path(args.test).name, compare(args.reference, args.test, args.base, args.edge_threshold,
                                                   args.centre)))
    else:
        parser.error("REFERENCE and TEST, or --pairs")
    print(table(rows))
    if args.json:
        Path(args.json).write_text(json.dumps(dict(rows), indent=2, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
