"""tools/halfres_compare.py on synthetic BMPs: identical frames, a known
difference in one region, the untouched region of a base, and the
setting's entry in config.toml's table (port/linux/src/port_config.c)."""
import math
import re
import shutil
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import halfres_compare as hc  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def bmp(path, width, height, pixel):
    """a 32-bit top-down BMP, pixel(x, y) -> (b, g, r)"""
    data = bytearray()
    for y in range(height):
        for x in range(width):
            data += bytes(pixel(x, y)) + b"\xff"
    header = b"BM" + struct.pack("<IHHI", 54 + len(data), 0, 0, 54) + struct.pack(
        "<IiiHHIIiiII", 40, width, -height, 1, 32, 0, len(data), 0, 0, 0, 0)
    path.write_bytes(header + data)
    return path


needs_cc = pytest.mark.skipif(not shutil.which("cc"), reason="needs cc")


@needs_cc
def test_identical(tmp_path):
    a = bmp(tmp_path / "a.bmp", 16, 8, lambda x, y: (x * 8, y * 16, 40))
    report = hc.compare(a, a)
    assert report["all"]["pixels"] == 128
    assert report["all"]["max"] == 0 and math.isinf(report["all"]["psnr_db"])


@needs_cc
def test_known_difference_and_untouched(tmp_path):
    flat = lambda x, y: (100, 100, 100)
    base = bmp(tmp_path / "base.bmp", 16, 8, flat)
    # the effect touches the left half (x < 8) in the reference
    reference = bmp(tmp_path / "ref.bmp", 16, 8, lambda x, y: (120, 100, 100) if x < 8 else (100, 100, 100))
    # the test differs by 4 in one channel there, and is exact elsewhere
    test = bmp(tmp_path / "test.bmp", 16, 8, lambda x, y: (124, 100, 100) if x < 8 else (100, 100, 100))
    report = hc.compare(reference, test, base)
    assert report["touched"]["pixels"] == 64 and report["untouched"]["pixels"] == 64
    assert report["untouched"]["max"] == 0
    assert report["touched"]["max"] == 4 and report["touched"]["p99_9"] == 4
    # one channel of three off by 4: mean square 16/3
    assert report["touched"]["psnr_db"] == pytest.approx(10 * math.log10(255 ** 2 / (16 / 3)))
    assert report["all"]["changed"] == pytest.approx(0.5)


@needs_cc
def test_edges_from_base(tmp_path):
    # a vertical edge in the base at x = 8: columns 7 and 8 are its pixels
    base = bmp(tmp_path / "base.bmp", 16, 4, lambda x, y: (0, 0, 0) if x < 8 else (200, 200, 200))
    reference = bmp(tmp_path / "ref.bmp", 16, 4, lambda x, y: (10, 0, 0) if x < 8 else (210, 200, 200))
    report = hc.compare(reference, reference, base)
    assert report["edges"]["pixels"] == 8 and report["interior"]["pixels"] == 56


def test_settings_documented():
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    readme = (ROOT / "port/linux/README.md").read_text()
    for name, environment in (("display.half_res_convolution", "HALO_HALF_RES_CONVOLUTION"),):
        assert re.search(r'\{ "' + re.escape(name) + r'", _config_boolean, "false", "' + environment + '"', config)
        assert f"| `{name}` | `false` | `{environment}` |" in readme
