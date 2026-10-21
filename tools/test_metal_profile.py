"""Check diagnostic phase indices against the real game enum."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent


def test_profile_phase_indices_match_game():
    source = (ROOT / 'source/rasterizer/rasterizer.h').read_text()
    start = source.index('_rasterizer_profile_clear = 0')
    end = source.index('NUMBER_OF_RASTERIZER_PROFILES', start)
    names = re.findall(r'\b_rasterizer_profile_\w+', source[start:end])
    assert len(names) == 29
    for name, expected in [('environment_fog', 17), ('environment_fog_screen', 18),
                           ('queued_transparents', 22), ('screen_effect', 26)]:
        assert names.index('_rasterizer_profile_' + name) == expected


def test_diagnostics_are_opt_in_and_do_not_force_gpu_wait():
    source = (ROOT / 'port/macos/host/host_metal.m').read_text()
    assert 'getenv("HALO_METAL_PROFILE")' in source
    start = source.index('void host_metal_profile_phase(')
    end = source.index('/* ---------- textures */', start)
    assert 'if (!profile_enabled)' in source[start:end]
    assert 'waitUntilCompleted' not in source[start:end]
    assert 'Metal interval frame' in source  # brief effect activations aren't sampled away
    assert 'serial - previous_serial' in source
