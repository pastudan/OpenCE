"""tools/tvos_deploy.py: the parts that need no device (argument building,
log parsing, the heavy-job lock, signing inputs)."""
import datetime
import json
from pathlib import Path
import re

import pytest

import tvos_deploy as deploy

ROOT = Path(__file__).resolve().parent.parent
UTC = datetime.timezone.utc

# ---------- the heavy-job lock

def test_lock_text_round_trips():
    now = datetime.datetime(2026, 10, 21, 7, 6, 47, tzinfo=UTC)
    info = deploy.lock_parse(deploy.lock_text("ninja macos", now=now, pid=4321))
    assert info == {"owner": "tv-bench child of #99", "task": "ninja macos", "started": now, "pid": 4321}


def test_lock_is_acquired_atomically_and_released(tmp_path):
    path = tmp_path / "tv-perf.lock"
    with deploy.HeavyLock("first", path=path, wait_seconds=0):
        assert "task: first" in path.read_text()
        # a second taker waits, then gives up and names the holder
        with pytest.raises(deploy.DeployError, match="task: first"):
            deploy.HeavyLock("second", path=path, wait_seconds=0, poll=0).acquire()
    assert not path.exists()


def test_lock_release_leaves_someone_elses_lock(tmp_path):
    path = tmp_path / "tv-perf.lock"
    lock = deploy.HeavyLock("mine", path=path).acquire()
    path.write_text("owner: other\ntask: theirs\n")
    lock.release()
    assert path.read_text().startswith("owner: other")


def test_lock_disabled_touches_nothing(tmp_path):
    path = tmp_path / "tv-perf.lock"
    with deploy.HeavyLock("x", path=path, enabled=False):
        assert not path.exists()


def test_stale_lock_needs_age_and_a_dead_process():
    now = datetime.datetime(2026, 10, 21, 12, 0, tzinfo=UTC)
    old = {"owner": "o", "task": "t", "started": now - datetime.timedelta(minutes=50), "pid": 99}
    young = dict(old, started=now - datetime.timedelta(minutes=10))
    assert deploy.lock_is_stale(old, now, alive=lambda pid: False)
    assert not deploy.lock_is_stale(old, now, alive=lambda pid: True)
    assert not deploy.lock_is_stale(young, now, alive=lambda pid: False)
    # (no pid: never taken as stale by itself)
    assert not deploy.lock_is_stale(dict(old, pid=None), now, alive=lambda pid: False)


# ---------- devicectl/xctrace arguments

def test_launch_args_use_the_device_identifier_and_json_environment():
    command = deploy.launch_args(deploy.DEVICE, {"HALO_FRAME_LOG": "1", "HALO_EXIT_AFTER": 60})
    assert command[:6] == ["xcrun", "devicectl", "device", "process", "launch", "--device"]
    assert command[6] == "443953B8-9072-5EAD-A947-95563A1C3D52"
    assert "--console" in command and "--terminate-existing" in command
    environment = json.loads(command[command.index("--environment-variables") + 1])
    assert environment == {"HALO_EXIT_AFTER": "60", "HALO_FRAME_LOG": "1"}
    assert command[-1] == "com.pastudan.opence.tv"


def test_copy_args_use_the_app_container():
    command = deploy.copy_args("to", deploy.DEVICE, "/tmp/x", "Library/Caches/OpenCE/config.toml")
    assert command[3:5] == ["copy", "to"]
    assert command[command.index("--domain-type") + 1] == "appDataContainer"
    assert command[command.index("--domain-identifier") + 1] == "com.pastudan.opence.tv"
    with pytest.raises(ValueError):
        deploy.copy_args("sideways", deploy.DEVICE, "a", "b")


def test_trace_args():
    command = deploy.trace_args("Metal System Trace", "/tmp/a.trace", 20, "UDID", "attach", "OpenCETV")
    assert command[:3] == ["xcrun", "xctrace", "record"]
    assert command[command.index("--template") + 1] == "Metal System Trace"
    assert command[command.index("--time-limit") + 1] == "20s"
    assert command[-2:] == ["--attach", "OpenCETV"]
    assert deploy.trace_args("Time Profiler", "o", 5, "U", "all")[-1] == "--all-processes"


def test_parse_env():
    assert deploy.parse_env(["A=1", "B=x=y"]) == {"A": "1", "B": "x=y"}
    with pytest.raises(deploy.DeployError):
        deploy.parse_env(["nope"])


def test_signing_problems_are_explained(tmp_path):
    problems = deploy.check_signing_inputs(tmp_path / "missing.mobileprovision", tmp_path / "missing.entitlements",
                                           keychain_check=False)
    assert len(problems) == 2
    assert "Xcode" in problems[0] and "--profile" in problems[0]


def test_entitlements_in_git_hold_no_secrets():
    text = (ROOT / "port/tvos/game-development.entitlements").read_text()
    keys = re.findall(r"<key>([^<]+)</key>", text)
    assert keys == ["application-identifier", "com.apple.developer.team-identifier", "get-task-allow"]
    assert "426324Q3LG.com.pastudan.opence.tv" in text


# ---------- what a launch's log says

DEVICE_LOG = """TV game entry: data at /var/mobile/Containers/Data/Application/X/Library/Caches/OpenCE (purgeable cache)\r
halo-linux: screen: 852x480 drawn at 3840x2160\r
host: Metal: Apple A12 GPU\r
host: frame log event: map-open a10.map at frame 1\r
host: Metal: frame 9 failed on the GPU: Insufficient Permission (to submit GPU work from background) (00000006:kIOGPUCommandBufferCallbackErrorBackgroundExecutionNotPermitted)\r
host: Metal: frame 10 failed on the GPU: Insufficient Permission (to submit GPU work from background) (00000006:kIOGPUCommandBufferCallbackErrorBackgroundExecutionNotPermitted)\r
halo-linux: exiting after debug.exit_after\r
host: frame log summary: 4754 frames; wall p50 16.66 p95 33.19 p99 34.36 max 259.32 ms; >16.7 ms 2000, >33.3 ms 22, >50 ms 2; GPU p50 0.37 p95 0.47 max 2.35 ms (4753 frames); compiles 179.6 ms\r
host: frame log: /var/mobile/Containers/Data/Application/X/Library/Caches/OpenCE/frame_log.csv\r
The app terminated with the exit code 0.
"""


def test_log_findings_flag_background_gpu_work():
    findings = deploy.log_findings(DEVICE_LOG)
    assert findings["metal_device"] == "Apple A12 GPU"
    assert findings["background_gpu_errors"] == 2
    assert not findings["valid"] and "background" in findings["invalid_reason"]
    assert findings["exited_on_schedule"] and findings["app_exit_code"] == 0
    assert findings["resolution"] == "3840x2160"
    assert findings["maps"] == ["a10.map"]
    assert findings["frame_log_summary"]["p95"] == 33.19 and findings["frame_log_summary"]["over50"] == 2
    assert findings["frame_log_path"].endswith("frame_log.csv")


def test_log_findings_asleep():
    text = ('ERROR: The application failed to launch. (com.apple.dt.CoreDeviceError error 10002 (0x2712))\n'
            '  NSLocalizedFailureReason = Transaction failed. (System is asleep - foreground app launch forbidden)\n')
    findings = deploy.log_findings(text)
    assert findings["asleep"] and not findings["valid"] and "asleep" in findings["invalid_reason"]


def test_log_findings_valid_run():
    findings = deploy.log_findings(DEVICE_LOG.replace("Insufficient Permission (to submit GPU work from background) "
                                                      "(00000006:kIOGPUCommandBufferCallbackErrorBackgroundExecutionNotPermitted)",
                                                      "something else"))
    assert findings["valid"] and findings["invalid_reason"] is None
    assert not deploy.log_findings("nothing at all")["valid"]
    assert deploy.log_findings("host: Metal: Apple A12 GPU\nhost: fatal: the game trapped: out of bounds")["trap"]


def test_push_data_skips_maps_the_tv_has_at_the_same_size(tmp_path):
    for name, size in (("ui.map", 10), ("a10.map", 20), ("a30.map", 30)):
        (tmp_path / name).write_bytes(b"x" * size)
    remote = {"ui.map": 10, "a10.map": 19}
    assert deploy.maps_to_push(["ui.map", "a10.map", "a30.map"], tmp_path, remote) == ["a10.map", "a30.map"]
    assert len(deploy.CAMPAIGN_MAPS) == 11 and deploy.CAMPAIGN_MAPS[0] == "ui.map" and "d40.map" in deploy.CAMPAIGN_MAPS
