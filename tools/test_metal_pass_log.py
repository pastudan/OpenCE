"""The Metal host's pass log (HALO_PASS_LOG) is opt-in and diagnostic, and
tools/metal_pass_log.py reads and sums it."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import metal_pass_log as passes  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

HEADER = ("serial,index,color,depth,width,height,color_format,depth_format,color_load,depth_load,stencil_load,"
          "color_store,depth_store,stencil_store,reason,draws,clears,fog_draws,transparent_draws,target_samples,"
          "first_phase,last_phase,phase_mask,gpu_ms,vertex_start_ms,vertex_ms,fragment_start_ms,fragment_ms")


def log(frames):
    lines = ["# OpenCE pass log, 0 passes, gpu tick 1.000000 ns", HEADER]
    for serial, frame in enumerate(frames, 1):
        t = serial * 16.7
        for index, (color, width, load, reason, mask, fragment) in enumerate(frame):
            lines.append(f"{serial},{index},{color},{2 if width > 1000 else 0},{width},{width // 2},80,"
                         f"{260 if width > 1000 else 0},{load},{load},{load},1,1,1,{reason},3,0,0,0,0,0,0,{mask},"
                         f"0,{t + index},0.01,{t + index},{fragment}")
    return "\n".join(lines) + "\n"


def test_summary_counts_reentries_and_phase_times():
    # a frame: the main target cleared (models), a shadow map, the main target loaded again (shadows)
    frame = [(1, 3840, 2, 1, 1 << 2, 1.0), (7, 128, 2, 1, 1 << 4, 0.25), (1, 3840, 1, 3, 1 << 4, 2.0)]
    summary = passes.summarize(passes.read_passes(log([frame] * 10)), serials=set(range(1, 11)))
    assert summary["frames"] == 10 and summary["passes_per_frame"] == 3
    assert summary["pass_end_reasons_per_frame"] == {"targets": 2.0, "present": 1.0}
    phases = {row["phase"]: row["fragment_ms"] for row in summary["phases"]}
    assert phases == {"models": 1.0, "env_shadows": 2.25}
    # the main target's reload: color 4 + depth 4 + stencil 1 bytes a pixel
    pixels = 3840 * 1920
    loads = 128 * 64 * 4 + pixels * 9
    assert summary["load_mb_per_frame"] == round(loads / 1e6, 1)
    assert summary["typical_frame"][2]["pass"].startswith("c1+d2 3840x1920 [load/store z load/store s load/store] "
                                                          "end=present")


def test_hoisting_unparks_before_everything_that_must_follow_the_screen():
    metal = (ROOT / "port/macos/host/host_metal.m").read_text()

    def body(signature):
        start = metal.index(signature)
        return metal[start:metal.index("\n}\n", start)]
    # a readback, the present and a feedback copy see the screen's pass whole
    assert "park_end();\n\t\tblit = blit_begin();" in body("int host_metal_texture_read(")
    assert "pass_flush();\n\t\tpark_end();" in body("void host_metal_present(void)")
    assert "park_end();\n\tblit = blit_begin();" in body("static id<MTLTexture> feedback_copy(")
    # the parked targets redefined or deleted: unparked first
    for signature in ("void host_metal_texture_define(", "void host_metal_texture_delete("):
        assert "(park.main_pass.color == handle || park.main_pass.depth == handle))\n\t\t\tpark_end();" in body(signature)
    # every blit says what it reads and writes
    for signature in ("void host_metal_texture_copy_level(", "void host_metal_texture_generate_mipmaps(",
                      "void host_metal_texture_upload("):
        assert "park_blit(" in body(signature) and "blit_mark(" in body(signature)
    # a draw that samples what the parked pass drew unparks; draws mark what they read
    draw = body("void host_metal_draw(")
    assert "commands_written == park.generation" in draw and "park_end();" in draw
    assert ".commands_read = commands_generation;" in draw
    # a hoisted target the frame has read is renamed or unparks; one it wrote unparks
    check = body("static void park_check_targets(void)")
    assert "commands_written == park.generation" in check and "park_rename(" in check
    # the frame's completion waits for its hoisted buffers (its ring and samples are reused after)
    assert "[early waitUntilCompleted];" in metal
    assert 'getenv("HALO_METAL_HOIST")' in metal


def test_pass_log_is_opt_in_and_does_not_wait():
    metal = (ROOT / "port/macos/host/host_metal.m").read_text()
    frame_log = (ROOT / "port/macos/host/host_frame_log.c").read_text()
    assert 'getenv("HALO_PASS_LOG")' in frame_log
    # only with the frame log's breakdown on (level 2)
    assert "if (host_frame_log_level >= 2)\n\t\tpass_log_start();" in frame_log
    start = metal.index("/* ---------- the pass log")
    end = metal.index("/* what the encoder has, to set only what changes */")
    section = metal[start:end]
    assert "waitUntilCompleted" not in section
    # the diagnostic switches change nothing unless set
    for name in ("HALO_METAL_PASS_SPLIT", "HALO_METAL_SKIP_PHASES", "HALO_METAL_PASS_TIMING"):
        assert f'getenv("{name}")' in section
    assert "if (host_pass_log_enabled)\n\t\tpass_log_begin(descriptor);" in metal
