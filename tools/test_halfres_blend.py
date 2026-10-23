"""display.half_res_transparents' blending (port/macos/host/host_metal.m, "the
queued transparents at half resolution"): for every pair of factors a run
may have (port/macos/guest/d3d8_metal.c, half_run_eligible), a sequence of
draws blended one after another into a pixel equals S + M * dst, with S
blended as the draws are and M with the host's multiply factors
(halfres_multiply_source, taken from host_metal.m), from S = 0, M = 1; and
a blend outside them (min, max, subtract, a destination-alpha factor)
does not. No clamping (the accumulators are floats); the 8-bit target's
clamping between draws is the one difference, measured on frames instead
(tools/halfres_compare.py)."""
import random
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
HOST = (ROOT / "port/macos/host/host_metal.m").read_text()
GUEST = (ROOT / "port/macos/guest/d3d8_metal.c").read_text()

# the factors as Metal's (metal_protocol.h): (name, function of src rgb, src a, dst rgb)
FACTORS = {
    "METAL_BLEND_ZERO": lambda s, a, d: 0.0,
    "METAL_BLEND_ONE": lambda s, a, d: 1.0,
    "METAL_BLEND_SOURCE_COLOR": lambda s, a, d: s,
    "METAL_BLEND_ONE_MINUS_SOURCE_COLOR": lambda s, a, d: 1.0 - s,
    "METAL_BLEND_SOURCE_ALPHA": lambda s, a, d: a,
    "METAL_BLEND_ONE_MINUS_SOURCE_ALPHA": lambda s, a, d: 1.0 - a,
    "METAL_BLEND_DESTINATION_COLOR": lambda s, a, d: d,
}
D3D_TO_METAL = {"D3DBLEND_ZERO": "METAL_BLEND_ZERO", "D3DBLEND_ONE": "METAL_BLEND_ONE",
                "D3DBLEND_SRCCOLOR": "METAL_BLEND_SOURCE_COLOR", "D3DBLEND_INVSRCCOLOR": "METAL_BLEND_ONE_MINUS_SOURCE_COLOR",
                "D3DBLEND_SRCALPHA": "METAL_BLEND_SOURCE_ALPHA", "D3DBLEND_INVSRCALPHA": "METAL_BLEND_ONE_MINUS_SOURCE_ALPHA",
                "D3DBLEND_DESTCOLOR": "METAL_BLEND_DESTINATION_COLOR"}


def eligible_factors():
    """the source and destination factors half_run_eligible lets into a run"""
    body = GUEST[GUEST.index("static BOOL half_run_eligible(void)"):]
    body = body[:body.index("\n}\n")]
    source = re.search(r"if \(source != (.*?)\)\n\t\{", body, re.S).group(1)
    destination = re.search(r"if \(destination != (.*?)\)\n\t\{", body, re.S).group(1)
    return (re.findall(r"D3DBLEND_\w+", source), re.findall(r"D3DBLEND_\w+", destination))


def multiply_source(source):
    """host_metal.m's halfres_multiply_source, read from its text"""
    body = HOST[HOST.index("static metal_u32 halfres_multiply_source("):]
    body = body[:body.index("\n}\n")]
    match = re.search(r"return source == (\w+) \? (\w+) : (\w+);", body)
    assert match, "halfres_multiply_source is not the one rule this test knows"
    return match.group(2) if source == match.group(1) else match.group(3)


def blend(source_factor, destination_factor, src, alpha, dst):
    return src * FACTORS[source_factor](src, alpha, dst) + dst * FACTORS[destination_factor](src, alpha, dst)


def run(draws, start):
    """the pixel at full resolution, and S + M * start"""
    full, s, m = start, 0.0, 1.0
    for source_factor, destination_factor, src, alpha in draws:
        full = blend(source_factor, destination_factor, src, alpha, full)
        s = blend(source_factor, destination_factor, src, alpha, s)
        m = blend(multiply_source(source_factor), destination_factor, src, alpha, m)
    return full, s + m * start


def random_draws(random_source, sources, destinations, count):
    return [(D3D_TO_METAL[random_source.choice(sources)], D3D_TO_METAL[random_source.choice(destinations)],
             random_source.random(), random_source.random()) for _ in range(count)]


def test_every_eligible_pair_composites_exactly():
    sources, destinations = eligible_factors()
    assert set(sources) >= {"D3DBLEND_ONE", "D3DBLEND_SRCALPHA", "D3DBLEND_DESTCOLOR"}
    for source in sources:
        for destination in destinations:
            for start in (0.0, 0.3, 1.0):
                full, composite = run([(D3D_TO_METAL[source], D3D_TO_METAL[destination], 0.6, 0.25)] * 3, start)
                assert composite == pytest.approx(full, abs=1e-12), (source, destination, start)


def test_random_sequences():
    sources, destinations = eligible_factors()
    random_source = random.Random(106)
    for _ in range(2000):
        draws = random_draws(random_source, sources, destinations, random_source.randint(1, 12))
        start = random_source.random()
        full, composite = run(draws, start)
        assert composite == pytest.approx(full, abs=1e-9)


def test_an_ineligible_blend_does_not():
    """the negative control: max (a component's larger) is not affine in
    what is under it, and the accumulation gets it wrong"""
    s, m, start, src = 0.0, 1.0, 0.8, 0.5
    full = max(src, start)
    s, m = max(src, s), max(src, m)
    assert abs((s + m * start) - full) > 0.1


def test_a_wrong_multiply_rule_is_caught():
    """the negative control: M blended with the draw's own source factor"""
    sources, destinations = eligible_factors()
    random_source = random.Random(7)
    wrong = 0
    for _ in range(200):
        draws = random_draws(random_source, sources, destinations, 4)
        start = random_source.random()
        full, s, m = start, 0.0, 1.0
        for source_factor, destination_factor, src, alpha in draws:
            full = blend(source_factor, destination_factor, src, alpha, full)
            s = blend(source_factor, destination_factor, src, alpha, s)
            m = blend(source_factor, destination_factor, src, alpha, m)
        wrong += abs(s + m * start - full) > 1e-6
    assert wrong > 100
