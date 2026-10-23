"""tools/tvos_build.py: the guard-page reservation's check against the
translated code's static offsets, and the TV CMake file's two modes."""
from pathlib import Path

import pytest

import tvos_build

ROOT = Path(__file__).resolve().parent.parent
ACCESS = ("  var_i0 = i32_load_default32(wasm_rt_local_memory_base, wasm_rt_local_memory_size, "
          "instance->w2c_env_memory, (u64)(var_i0) + {}u);\n")
STORE = ("  v128_store_default32(wasm_rt_local_memory_base, wasm_rt_local_memory_size, "
         "instance->w2c_env_memory, (u64)(var_i1) + {}u, var_v0);\n")
LANE = ("  var_v0 = v128_load32_lane1(wasm_rt_local_memory_base, wasm_rt_local_memory_size, "
        "instance->w2c_env_memory, (u64)(var_i2) + {}u, var_v0);\n")


def write(tmp_path, *lines):
    (tmp_path / "halo_0.c").write_text("".join(lines))
    (tmp_path / "halo_1.c").write_text(ACCESS.format(8))
    return tmp_path


def test_largest_offset_reads_every_kind_of_access(tmp_path):
    w2c = write(tmp_path, ACCESS.format(16), STORE.format(49078504), LANE.format(4096),
                "  i32_load_default32(wasm_rt_local_memory_base, wasm_rt_local_memory_size, "
                "instance->w2c_env_memory, (u64)(var_i0));\n")
    assert tvos_build.largest_offset(w2c) == 49078504
    w2c = write(tmp_path, LANE.format(70000000))
    assert tvos_build.largest_offset(w2c) == 70000000


def test_guard_reserve_covers_the_offsets(tmp_path):
    # the game's largest (2026-10: 0x2ece338, ~47 MiB) is inside the 64 MiB guard
    assert tvos_build.check_guard_reserve(write(tmp_path, STORE.format(0x2ece338))) == 0x2ece338
    limit = tvos_build.GUARD_RESERVE_BYTES - (1 << 32) - tvos_build.ACCESS_BYTES
    assert tvos_build.check_guard_reserve(write(tmp_path, ACCESS.format(limit))) == limit
    with pytest.raises(RuntimeError, match="past the"):
        tvos_build.check_guard_reserve(write(tmp_path, ACCESS.format(limit + 1)))


def test_cmake_has_both_modes_and_the_reservation():
    text = (ROOT / "port/tvos/game/CMakeLists.txt").read_text()
    assert 'OPENCE_GUEST_MEMCHECK STREQUAL "guard"' in text
    assert "WASM_RT_MEMCHECK_GUARD_PAGES=1 WASM_RT_MEMCHECK_BOUNDS_CHECK=0" in text
    assert "HALO_GUEST_RESERVE_BYTES=${OPENCE_GUEST_RESERVE_BYTES}" in text
    assert "WASM_RT_MEMCHECK_BOUNDS_CHECK=1 WASM_RT_MEMCHECK_GUARD_PAGES=0" in text
    host = (ROOT / "port/macos/host/host_main.c").read_text()
    assert "#ifdef HALO_GUEST_RESERVE_BYTES" in host and "mprotect(base" in host
