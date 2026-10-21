"""How the host's DXT conversions (port/macos/host/host_textures.c) compare
with the guest's decoding on a map's own textures: every DXT bitmap of the
maps given (Xbox cache files), all its levels and faces, converted as the
game would on a GPU without BC formats (the Apple TV's A12).

    python3 tools/texture_formats.py MAPS/a10.map MAPS/b30.map [--json OUT]

For each of DXT1, DXT3 and DXT5 it reports the bytes as the map has them, as
BGRA8 and as ASTC 4x4; the ASTC error against the exact BGRA8 decoding (PSNR
over all texels and channels, and how many blocks have a largest error of
0-2, 3-4 and more 255ths); and the spread of each texture's own PSNR and
largest error. BGRA8 ("exact") is checked to equal the guest's decoding by
tools/test_texture_formats.py, so its error is 0 and not reported.

Builds a small C program with cc (host_textures.c and the measuring loop
below) in a temporary folder. Reads the maps; writes nothing else.
"""
import argparse
import json
import math
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "port/macos/host"
TAG_BASE = 0x803A6000
DXT = {14: "dxt1", 15: "dxt3", 16: "dxt5"}
TYPES = {0: "2d", 1: "3d", 2: "cube"}

MEASURE = r'''
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "host_textures.h"

/* stdin: lines "kind blocks", each followed by that many blocks' bytes;
stdout: a line per texture: kind blocks squares worst exact rounded approximated */
int main(void)
{
	int kind;
	unsigned long blocks;
	while (scanf("%d %lu", &kind, &blocks) == 2) {
		unsigned bytes = host_textures_bc_block_bytes(kind), i, t, c;
		unsigned char *data = malloc(blocks * bytes);
		double squares = 0;
		int worst = 0;
		unsigned long counts[NUMBER_OF_HOST_TEXTURE_ASTC_ERRORS] = { 0 };
		getchar();
		if (fread(data, bytes, blocks, stdin) != blocks) return 1;
		for (i = 0; i < blocks; i++) {
			unsigned char astc[16];
			uint32_t ref[16], got[16];
			int error = host_textures_astc_block(kind, data + i * bytes, astc);
			host_textures_bc_decode_block(kind, data + i * bytes, ref);
			host_textures_astc_decode(astc, got);
			for (t = 0; t < 16; t++)
				for (c = 0; c < 4; c++) {
					int d = (int)((ref[t] >> (c * 8)) & 255) - (int)((got[t] >> (c * 8)) & 255);
					squares += d * d;
				}
			counts[error <= 2 ? _astc_exact : error <= 4 ? _astc_rounded : _astc_approximated]++;
			worst = error > worst ? error : worst;
		}
		printf("%d %lu %.0f %d %lu %lu %lu\n", kind, blocks, squares, worst, counts[0], counts[1], counts[2]);
		fflush(stdout);
		free(data);
	}
	return 0;
}
'''


def dxt_bitmaps(path):
    """each DXT bitmap of an Xbox map: name, format, size, and its blocks
    (all levels and faces, as the map stores them)"""
    raw = Path(path).read_bytes()
    data = raw[:0x800] + zlib.decompressobj().decompress(raw[0x800:])
    offset, size = struct.unpack_from("<ii", data, 0x10)
    tags = data[offset:offset + size]

    def read(address, count):
        return tags[address - TAG_BASE:address - TAG_BASE + count]

    instances, = struct.unpack_from("<I", tags, 0)
    count, = struct.unpack_from("<I", tags, 0xC)
    seen = set()
    for index in range(count):
        group, _, _, _, name, address, _, _ = struct.unpack("<4sIIIIIII", read(instances + index * 0x20, 0x20))
        if group[::-1] != b"bitm":
            continue
        name = read(name, 256).split(b"\0")[0].decode(errors="replace")
        header = read(address, 0x6C)
        file_offset, = struct.unpack_from("<I", header, 0x38)
        bitmap_count, bitmaps = struct.unpack_from("<II", header, 0x60)
        for number in range(bitmap_count):
            bitmap = read(bitmaps + number * 0x30, 0x30)
            width, height, depth, kind, format_, flags = struct.unpack_from("<hhhhhH", bitmap, 4)
            pixels, pixels_size = struct.unpack_from("<ii", bitmap, 0x18)
            if format_ not in DXT or pixels + file_offset in seen:
                continue
            seen.add(pixels + file_offset)
            yield {"name": f"{name}#{number}", "format": DXT[format_], "type": TYPES.get(kind, kind),
                   "width": width, "height": height,
                   "blocks": data[pixels + file_offset:pixels + file_offset + pixels_size]}


def psnr(squares, values):
    return 99.0 if not squares else 10 * math.log10(255.0 * 255.0 * values / squares)


def percentiles(values, points=(0.01, 0.05, 0.25, 0.5)):
    values = sorted(values)
    return {f"p{int(p * 100)}": values[min(len(values) - 1, int(p * len(values)))] for p in points} if values else {}


def measure(maps):
    with tempfile.TemporaryDirectory() as folder:
        program = Path(folder) / "measure"
        (Path(folder) / "measure.c").write_text(MEASURE)
        subprocess.run(["cc", "-std=c11", "-O2", "-I", str(HOST), str(Path(folder) / "measure.c"),
                        str(HOST / "host_textures.c"), "-o", str(program), "-lm"], check=True)
        textures = [bitmap for path in maps for bitmap in dxt_bitmaps(path)]
        stdin = bytearray()
        for bitmap in textures:
            kind = ("dxt1", "dxt3", "dxt5").index(bitmap["format"])
            block = 8 if kind == 0 else 16
            usable = len(bitmap["blocks"]) // block * block
            bitmap["blocks"] = bitmap["blocks"][:usable]
            stdin += f"{kind} {usable // block}\n".encode() + bitmap["blocks"]
        lines = subprocess.run([str(program)], input=bytes(stdin), capture_output=True, check=True).stdout.split(b"\n")
    results = {}
    for bitmap, line in zip(textures, lines):
        kind, blocks, squares, worst, exact, rounded, approximated = line.split()
        entry = results.setdefault(bitmap["format"], {"textures": 0, "blocks": 0, "squares": 0.0,
                                                       "block_errors": [0, 0, 0], "texture_psnr": [],
                                                       "texture_worst": [], "bytes_map": 0})
        entry["textures"] += 1
        entry["blocks"] += int(blocks)
        entry["squares"] += float(squares)
        entry["block_errors"] = [a + int(b) for a, b in zip(entry["block_errors"], (exact, rounded, approximated))]
        entry["texture_psnr"].append(psnr(float(squares), int(blocks) * 64))
        entry["texture_worst"].append(int(worst))
        entry["bytes_map"] += len(bitmap["blocks"])
    report = {}
    for name, entry in sorted(results.items()):
        texels = entry["blocks"] * 16
        report[name] = {
            "textures": entry["textures"], "blocks": entry["blocks"],
            "MiB_map": entry["bytes_map"] / 2**20, "MiB_bgra8": texels * 4 / 2**20, "MiB_astc": entry["blocks"] * 16 / 2**20,
            "psnr_db": psnr(entry["squares"], texels * 4),
            "blocks_error_0_2": entry["block_errors"][0] / entry["blocks"],
            "blocks_error_3_4": entry["block_errors"][1] / entry["blocks"],
            "blocks_error_over_4": entry["block_errors"][2] / entry["blocks"],
            "texture_psnr_db": percentiles(entry["texture_psnr"]),
            "textures_worst_over_4": sum(1 for w in entry["texture_worst"] if w > 4) / entry["textures"],
            "textures_worst_over_32": sum(1 for w in entry["texture_worst"] if w > 32) / entry["textures"],
        }
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("maps", nargs="+")
    parser.add_argument("--json")
    args = parser.parse_args(argv)
    report = measure(args.maps)
    print("| format | textures | MiB in map | as BGRA8 | as ASTC 4x4 | ASTC PSNR dB | blocks err <=2 | 3-4 | >4 "
          "| texture PSNR p1/p5/p25/p50 | textures with a block err >4 | >32 |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for name, row in report.items():
        p = row["texture_psnr_db"]
        print(f"| {name.upper()} | {row['textures']} | {row['MiB_map']:.1f} | {row['MiB_bgra8']:.1f} | {row['MiB_astc']:.1f} "
              f"| {row['psnr_db']:.1f} | {100 * row['blocks_error_0_2']:.1f}% | {100 * row['blocks_error_3_4']:.1f}% "
              f"| {100 * row['blocks_error_over_4']:.1f}% | {p['p1']:.1f}/{p['p5']:.1f}/{p['p25']:.1f}/{p['p50']:.1f} "
              f"| {100 * row['textures_worst_over_4']:.0f}% | {100 * row['textures_worst_over_32']:.0f}% |")
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
