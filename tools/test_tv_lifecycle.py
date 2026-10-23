"""The app's lifecycle on the Apple TV (port/macos/host/host_lifecycle.h):
no GPU work committed while the app is not active, the game stopped and
told so, and no blocking GPU wait on the frame's path but the in-flight
throttle. Checked in the sources: the host cannot run here without a GPU
and UIKit (the device's runs are in port/tvos/PERFORMANCE.md, "Lifecycle
(A12)")."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "port/macos/host"


def read(path):
    return (ROOT / path).read_text()


def function_bodies(text):
    """{name: body} of the file's top-level C functions (a line that starts
    a function at column 0, its body to the next column-0 brace)"""
    bodies = {}
    for match in re.finditer(r"^(?:static )?[\w <>*]+?\b(\w+)\([^;{]*\)\s*\n\{\n(.*?)^\}", text, re.M | re.S):
        bodies.setdefault(match.group(1), match.group(2))
    return bodies


def test_every_commit_passes_the_gate():
    """each [... commit] in the renderer has host_lifecycle_gate() before it
    in its function (the present's at the function's top)"""
    text = read("port/macos/host/host_metal.m")
    bodies = function_bodies(text)
    commits = {name: body for name, body in bodies.items() if re.search(r"\[\w+ commit\]", body)}
    assert set(commits) >= {"encoder_end", "park_commit_hoisted", "host_metal_texture_bc", "host_metal_texture_read",
                            "host_metal_present"}
    for name, body in commits.items():
        for match in re.finditer(r"\[\w+ commit\]", body):
            assert "host_lifecycle_gate()" in body[:match.start()], f"{name}: a commit not gated"


def test_present_gates_before_taking_a_drawable():
    body = function_bodies(read("port/macos/host/host_metal.m"))["host_metal_present"]
    assert body.index("host_lifecycle_gate()") < body.index("nextDrawable")
    assert body.index("host_lifecycle_frame()") > body.index("[commands commit]")


def test_no_blocking_gpu_wait_on_the_frame_path():
    """the GPU waits of host_metal.m and where each may be: the frames in
    flight throttle (FRAMES_IN_FLIGHT, a semaphore), the readback
    (debug.screenshot_every only), the completed handler's hoisted buffers
    (on Metal's completion thread, not the game's) and the drain (the app
    resigning active)"""
    text = read("port/macos/host/host_metal.m")
    bodies = function_bodies(text)
    # (prepare_map and prepare_rest wait for compile slots on the shader
    # pack's own queue, prepared.queue, never on the game's thread)
    allowed = {"waitUntilCompleted": {"host_metal_texture_read", "host_metal_present", "host_metal_drain"},
               "waitUntilScheduled": set(),
               "dispatch_semaphore_wait": {"frame_begin", "host_metal_drain", "prepare_map", "prepare_rest"}}
    # every call of prepare_map is in a block sent to prepared.queue, and
    # prepare_rest only by prepare_rest_schedule (on prepared.next_queue)
    for at in [m.start() for m in re.finditer(r"(?<!void )prepare_map\(", text)]:
        assert "dispatch_async(prepared.queue" in text[max(0, at - 1200):at], "prepare_map called off prepared.queue"
    assert {name for name, body in bodies.items() if re.search(r"\bprepare_rest\(", body)} <= \
        {"prepare_rest_schedule"}
    for call, functions in allowed.items():
        found = {name for name, body in bodies.items() if call in body}
        assert found <= functions, f"{call} in {found - functions}"
    assert re.search(r"#define FRAMES_IN_FLIGHT [23]\b", text)
    # (the present's waitUntilCompleted is inside its completed handler)
    present = bodies["host_metal_present"]
    handler = present.index("addCompletedHandler")
    assert present.index("waitUntilCompleted") > handler
    # (the readback is only the screenshots' debug setting)
    guest = read("port/macos/guest/d3d8_metal.c")
    assert [m.start() for m in re.finditer(r"host_metal_texture_read\(", guest)] and \
        "write_screenshot" in function_bodies(guest) and \
        "host_metal_texture_read(" in function_bodies(guest)["write_screenshot"]
    assert guest.count("host_metal_texture_read(") == 1


def test_lifecycle_watches_sdl_app_events_and_the_launch_state():
    text = read("port/macos/host/host_lifecycle.m")
    for event in ("SDL_EVENT_WILL_ENTER_BACKGROUND", "SDL_EVENT_DID_ENTER_BACKGROUND",
                  "SDL_EVENT_WILL_ENTER_FOREGROUND", "SDL_EVENT_DID_ENTER_FOREGROUND", "SDL_EVENT_TERMINATING",
                  "SDL_EVENT_LOW_MEMORY"):
        assert event in text
    assert "SDL_AddEventWatch" in text and "applicationState" in text
    assert "DISPATCH_SOURCE_TYPE_MEMORYPRESSURE" in text
    stop = function_bodies(text)["stop"]
    # audio paused, the GPU drained, before the wait; time away skipped after
    order = [stop.index(call) for call in ("host_sdl_audio_pause_all(1)", "host_metal_drain()", "while (atomic_load(&away))",
                                           "host_frame_log_skip", "host_sdl_audio_pause_all(0)")]
    assert order == sorted(order)
    # the memory warning: a telemetry event with the footprint, and a trim of a
    # step a frame (host_lifecycle_frame), not all at once
    frame = function_bodies(text)["host_lifecycle_frame"]
    assert 'host_frame_log_event("memory"' in frame and "footprint" in frame
    assert "host_metal_trim(trim_step, 0)" in frame and "for (" not in frame


def test_the_game_is_paused_and_its_first_frame_clamped():
    main = read("source/main/main.c")
    assert main.count("if (halo_platform_resumed_frame())") == 2
    widget = read("source/interface/ui_widget.c")
    assert "halo_platform_pause_requested()" in function_bodies(widget)["ui_check_for_pause_game"]
    platform = read("port/linux/src/sdl_platform.c")
    assert "metal_device_lifecycle_stops()" in platform
    assert "unsigned int host_lifecycle_stops(void);" in read("port/macos/guest/guest_host.h")
