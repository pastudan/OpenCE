#!/usr/bin/env python3
"""Build, sign, install, run and trace the development TV game on an Apple TV.

    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    python3 tools/tvos_deploy.py build                 # tools/tvos_build.py --platform device, under the lock
    python3 tools/tvos_deploy.py sign                  # copy to scratch, embed the profile, codesign
    python3 tools/tvos_deploy.py install               # devicectl install of the signed copy
    python3 tools/tvos_deploy.py push-data --maps DIR  # ui.map/a10.map (+ init.txt) into the app's cache
    python3 tools/tvos_deploy.py push-data --maps DIR --campaign  # every campaign map, those not there yet
    python3 tools/tvos_deploy.py backup-config         # the owner's config.toml, to a timestamped backup
    python3 tools/tvos_deploy.py restore-config [FILE] # it back (default: the newest backup)
    python3 tools/tvos_deploy.py launch --env K=V --seconds 60 --log run.log
    python3 tools/tvos_deploy.py screenshot out.png
    python3 tools/tvos_deploy.py trace --template 'Metal System Trace' --seconds 20 --output x.trace

The Apple TV is a person's TV: nothing here runs on it unless asked, and
every command that touches it is explicit. Device runs use the CoreDevice
identifier (--device, default the A12 TV), never a name: two devices share
"Dan's bedroom".

Heavy jobs (a build, a device session, a simulator) take the shared
heavy-job lock (LOCK below, or --lock): created atomically with its owner,
task and start time, deleted afterwards, waited on when someone else holds
it. --no-lock skips it, for a caller that already holds it (tools/tvos_bench.py).

Signing needs the team's development identity in this Mac's keychain (only
its SHA-1 is named here; no key ever leaves the keychain), a wildcard
development provisioning profile (--profile; not in git) and the
entitlements, port/tvos/game-development.entitlements (no secrets: the
application and team identifiers and get-task-allow). Missing pieces are
reported with what to do, not worked around.

xctrace on this TV (Xcode 27.1, tvOS 27.0, network-paired CoreDevice):
NOT WORKING. `xctrace list devices` puts it under "Devices Offline" while
devicectl reaches it; `record --attach OpenCETV` fails "Cannot find process
matching name", `--attach <pid>` (pid from devicectl) fails "Cannot find
process for provided pid", and `--all-processes` fails "Timed out waiting
for device to boot", for both Metal System Trace and Time Profiler. The
trace subcommand is kept for when Instruments can reach the device.

A launch that tvOS refuses because the TV is asleep ("System is asleep -
foreground app launch forbidden") is reported as such; devicectl cannot
wake it.

A launch is cold or warm only as its caller makes it so (tools/tvos_bench.py
decides): this reports what it saw. A launch while the TV shows another app,
or is asleep, fails on the GPU ("Insufficient Permission (to submit GPU work
from background)"): such a run is marked invalid.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
# (the defaults are the bench Mac mini's; another Mac sets OPENCE_TV_* or the
# flags. Without OPENCE_TV_SCRATCH, a Mac without the mini's folder uses
# $TMPDIR/opence-tv)
MINI_SCRATCH = Path("/var/folders/15/6xlrw1ln4w90d9z8q732vyy00000gn/T/bridge/halo-e6af79dd")
SCRATCH = Path(os.environ.get("OPENCE_TV_SCRATCH") or (
    MINI_SCRATCH if MINI_SCRATCH.is_dir() else Path(os.environ.get("TMPDIR", "/tmp")) / "opence-tv"))
LOCK = Path(os.environ.get("OPENCE_TV_LOCK") or SCRATCH / "tv-perf.lock")
DEVICE = os.environ.get("OPENCE_TV_DEVICE") or "443953B8-9072-5EAD-A947-95563A1C3D52"  # A12 TV: CoreDevice id
BUNDLE = os.environ.get("OPENCE_TV_BUNDLE") or "com.pastudan.opence.tv"
# Apple Development (team 426324Q3LG), by SHA-1
IDENTITY = os.environ.get("OPENCE_TV_IDENTITY") or "0DE31AA707B2320C376624A7D5A10C968F432235"
PROFILE = Path(os.environ.get("OPENCE_TV_PROFILE") or
               SCRATCH / "tvos-provision-xcode/Debug-appletvos/TVDevelopmentProvision.app/embedded.mobileprovision")
ENTITLEMENTS = ROOT / "port/tvos/game-development.entitlements"
APP = ROOT / "build/tvos/device/OpenCETV.app"
SIGNED = SCRATCH / "tv-deploy" / "OpenCETV.app"
OWNER = "tv-bench child of #99"
LOCK_STALE_SECONDS = 45 * 60

# what a launch's log says
BACKGROUND_GPU = "Insufficient Permission (to submit GPU work from background)"
BACKGROUND_GPU_CODE = "kIOGPUCommandBufferCallbackErrorBackgroundExecutionNotPermitted"


class DeployError(RuntimeError):
    pass


def developer_environment(environ=None):
    """the environment for xcrun: full Xcode (the system's selection is the
    command line tools, which have no devicectl)"""
    environment = dict(os.environ if environ is None else environ)
    environment.setdefault("DEVELOPER_DIR", "/Applications/Xcode.app/Contents/Developer")
    environment["PATH"] = str(Path.home() / ".local/bin") + os.pathsep + environment.get("PATH", "")
    return environment


def run(command, check=True, capture=False, timeout=None, **extra):
    command = [str(part) for part in command]
    print("+", " ".join(command), flush=True)
    return subprocess.run(command, check=check, text=True, timeout=timeout, env=developer_environment(),
                          stdout=subprocess.PIPE if capture else None, stderr=subprocess.STDOUT if capture else None,
                          **extra)


# ---------- the heavy-job lock

def lock_text(task, now=None, pid=None):
    now = now or datetime.datetime.now(datetime.timezone.utc)
    return (f"owner: {OWNER}\ntask: {task}\nstarted: {now.strftime('%Y-%m-%dT%H:%M:%SZ')}\n"
            f"pid: {pid if pid is not None else os.getpid()}\n")


def lock_parse(text):
    """the lock file's fields: owner, task, started (datetime or None), pid (int or None)"""
    fields = {}
    for line in text.splitlines():
        key, _, value = line.partition(":")
        fields[key.strip()] = value.strip()
    started = None
    try:
        started = datetime.datetime.strptime(fields.get("started", ""), "%Y-%m-%dT%H:%M:%SZ").replace(
            tzinfo=datetime.timezone.utc)
    except ValueError:
        pass
    pid = int(fields["pid"]) if fields.get("pid", "").isdigit() else None
    return {"owner": fields.get("owner", ""), "task": fields.get("task", ""), "started": started, "pid": pid}


def pid_alive(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def lock_is_stale(info, now=None, alive=pid_alive):
    """stale only when older than 45 minutes AND its process is gone (a lock
    with no pid is never taken as stale by itself: someone must look)"""
    now = now or datetime.datetime.now(datetime.timezone.utc)
    if not info["started"] or (now - info["started"]).total_seconds() < LOCK_STALE_SECONDS:
        return False
    return info["pid"] is not None and not alive(info["pid"])


class HeavyLock:
    """the shared heavy-job lock, as a context manager: atomic creation
    (O_EXCL), waits while another job holds it, deletes it afterwards (only
    if it is still this one's)"""

    def __init__(self, task, path=LOCK, wait_seconds=3600, poll=15, enabled=True):
        self.task, self.path, self.wait_seconds, self.poll, self.enabled = task, Path(path), wait_seconds, poll, enabled
        self.text = None

    def acquire(self):
        if not self.enabled:
            return self
        deadline = time.monotonic() + self.wait_seconds
        reported = None
        while True:
            self.text = lock_text(self.task)
            try:
                descriptor = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
            except FileExistsError:
                try:
                    held = self.path.read_text()
                except FileNotFoundError:
                    continue
                info = lock_parse(held)
                if lock_is_stale(info):
                    print(f"lock {self.path}: stale (from {info['started']}, pid {info['pid']} gone); removing",
                          flush=True)
                    self.path.unlink(missing_ok=True)
                    continue
                if held != reported:
                    print(f"lock {self.path} is held, waiting:\n{held}", flush=True)
                    reported = held
                if time.monotonic() >= deadline:
                    raise DeployError(f"the heavy-job lock {self.path} is still held:\n{held}")
                time.sleep(self.poll)
                continue
            with os.fdopen(descriptor, "w") as file:
                file.write(self.text)
            return self

    def release(self):
        if not self.enabled or self.text is None:
            return
        try:
            if self.path.read_text() == self.text:
                self.path.unlink()
        except FileNotFoundError:
            pass
        self.text = None

    def __enter__(self):
        return self.acquire()

    def __exit__(self, *exc):
        self.release()
        return False


# ---------- devicectl

def copy_args(direction, device, source, destination, bundle=BUNDLE):
    """devicectl's copy of a file in the app's data container (the app's
    home: Library/Caches/OpenCE/... is a path in it)"""
    if direction not in ("from", "to"):
        raise ValueError(direction)
    return ["xcrun", "devicectl", "device", "copy", direction, "--device", device,
            "--domain-type", "appDataContainer", "--domain-identifier", bundle,
            "--source", str(source), "--destination", str(destination)]


def launch_args(device, environment, bundle=BUNDLE, console=True, terminate_existing=True, arguments=()):
    """devicectl's launch: the environment as JSON (strings only)"""
    command = ["xcrun", "devicectl", "device", "process", "launch", "--device", device]
    if terminate_existing:
        command.append("--terminate-existing")
    if console:
        command.append("--console")
    if environment:
        command += ["--environment-variables", json.dumps({str(k): str(v) for k, v in environment.items()},
                                                           sort_keys=True)]
    command.append(bundle)
    if arguments:
        command += ["--", *arguments]
    return command


def parse_env(items):
    environment = {}
    for item in items or ():
        key, sep, value = item.partition("=")
        if not sep or not key:
            raise DeployError(f"--env takes KEY=VALUE, not {item!r}")
        environment[key] = value
    return environment


# ---------- what a launch's log says

FRAME_SUMMARY = re.compile(
    r"frame log summary: (?P<frames>\d+) frames; wall p50 (?P<p50>[\d.]+) p95 (?P<p95>[\d.]+) p99 (?P<p99>[\d.]+) "
    r"max (?P<max>[\d.]+) ms; >16\.7 ms (?P<over16>\d+), >33\.3 ms (?P<over33>\d+), >50 ms (?P<over50>\d+)")


def log_findings(text):
    """what a launch's console output shows: the GPU device, whether it ran
    in the background (GPU work refused: the run is invalid), traps, the
    timed exit, the frame log's summary, and the build's commit if logged"""
    findings = {
        "metal_device": None,
        # (lines that say it, by either wording)
        "background_gpu_errors": sum(1 for line in text.splitlines()
                                     if "GPU work from background" in line or BACKGROUND_GPU_CODE in line),
        "gpu_errors": len(re.findall(r"failed on the GPU", text)),
        "trap": bool(re.search(r"wasm trap|the game trapped", text, re.I)),
        "exited_on_schedule": "exiting after debug.exit_after" in text,
        "app_exit_code": None,
        "frame_log_summary": None,
        "frame_log_path": None,
        "maps": re.findall(r"frame log event: map-open (\S+)", text),
        "resolution": None,
    }
    match = re.search(r"host: Metal: (.+)", text)
    if match:
        findings["metal_device"] = match.group(1).strip()
    match = re.search(r"app terminated with the exit code (-?\d+)", text) or re.search(r"exit code (-?\d+)", text)
    if match:
        findings["app_exit_code"] = int(match.group(1))
    match = FRAME_SUMMARY.search(text)
    if match:
        findings["frame_log_summary"] = {k: (int(v) if k in ("frames", "over16", "over33", "over50") else float(v))
                                         for k, v in match.groupdict().items()}
    match = re.search(r"host: frame log: (/\S+\.csv)", text)
    if match:
        findings["frame_log_path"] = match.group(1)
    match = re.search(r"screen: \S+ drawn at (\d+x\d+)", text)
    if match:
        findings["resolution"] = match.group(1)
    findings["valid"] = (findings["background_gpu_errors"] == 0 and not findings["trap"]
                         and findings["metal_device"] is not None)
    findings["asleep"] = "System is asleep" in text
    findings["invalid_reason"] = (
        "the TV is asleep: tvOS refused a foreground launch (wake it with the remote)" if findings["asleep"]
        else "GPU work refused in the background (the TV was not showing the app)" if findings["background_gpu_errors"]
        else "the game trapped" if findings["trap"]
        else "no Metal device logged (the app did not start)" if findings["metal_device"] is None else None)
    return findings


# ---------- signing

def check_signing_inputs(profile=PROFILE, entitlements=ENTITLEMENTS, identity=IDENTITY, keychain_check=True):
    problems = []
    if not Path(profile).is_file():
        problems.append(
            f"provisioning profile missing: {profile}\n"
            "  It is a development profile for team 426324Q3LG (a wildcard App ID) and is not in git. Make one\n"
            "  again by building any tvOS app for this TV in Xcode with automatic signing (team 426324Q3LG),\n"
            "  then pass its embedded.mobileprovision with --profile (or put it at the path above).")
    if not Path(entitlements).is_file():
        problems.append(f"entitlements missing: {entitlements} (it is in git: port/tvos/game-development.entitlements)")
    if keychain_check:
        found = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"], capture_output=True,
                               text=True).stdout
        if identity not in found:
            problems.append(f"signing identity {identity} is not in this Mac's keychain (Xcode > Settings > "
                            "Accounts must have the team's Apple Development certificate). Do not export keys "
                            "or add trust overrides to work around this.")
    return problems


def profile_plist(profile):
    """a provisioning profile's contents (it is a signed plist)"""
    import plistlib
    decoded = subprocess.run(["security", "cms", "-D", "-i", str(profile)], capture_output=True, check=True).stdout
    return plistlib.loads(decoded)


def profile_identity(profile):
    """the SHA-1 of the profile's first development certificate that is in
    this Mac's keychain as a signing identity (only hashes are compared)"""
    import hashlib
    found = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"], capture_output=True,
                           text=True).stdout
    for certificate in profile_plist(profile).get("DeveloperCertificates", []):
        digest = hashlib.sha1(certificate).hexdigest().upper()
        if digest in found:
            return digest
    raise DeployError(f"no signing identity in this Mac's keychain matches a certificate of {profile}")


def entitlements_for(team, bundle):
    """the development entitlements (as port/tvos/game-development.entitlements)
    for another team and bundle identifier"""
    import plistlib
    return plistlib.dumps({"application-identifier": f"{team}.{bundle}",
                           "com.apple.developer.team-identifier": team, "get-task-allow": True})


def sign(app=APP, output=SIGNED, profile=PROFILE, entitlements=ENTITLEMENTS, identity=IDENTITY, bundle=None):
    """bundle: when not the app's own, the copy's CFBundleIdentifier is
    rewritten and the entitlements made for the profile's team and it (for a
    profile of another team, e.g. a personal team)"""
    if identity == "auto" and Path(profile).is_file():
        identity = profile_identity(profile)
    problems = check_signing_inputs(profile, entitlements, identity)
    if problems:
        raise DeployError("cannot sign:\n" + "\n".join(problems))
    if not (Path(app) / "OpenCETV").is_file():
        raise DeployError(f"{app} has no executable: build it first (tvos_deploy.py build)")
    output = Path(output)
    if output.exists():
        shutil.rmtree(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    run(["ditto", app, output])
    shutil.copyfile(profile, output / "embedded.mobileprovision")
    if bundle and bundle != "com.pastudan.opence.tv":
        team = profile_plist(profile)["TeamIdentifier"][0]
        run(["plutil", "-replace", "CFBundleIdentifier", "-string", bundle, output / "Info.plist"])
        entitlements = output.parent / f"{team}.{bundle}.entitlements"
        entitlements.write_bytes(entitlements_for(team, bundle))
    run(["codesign", "--force", "--sign", identity, "--entitlements", entitlements, "--generate-entitlement-der",
         "--timestamp=none", output])
    run(["codesign", "--verify", "--strict", output])
    commit = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short=8", "HEAD"], capture_output=True,
                            text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip()
    (output.parent / "build-info.json").write_text(json.dumps({
        "commit": commit, "dirty": bool(dirty), "signed": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "app": str(output)}, indent=2))
    print(f"signed {output} (commit {commit}{', with uncommitted changes' if dirty else ''})")
    return output


# ---------- the commands

def command_build(args):
    with HeavyLock("tvos_build.py --platform device", enabled=not args.no_lock):
        run([sys.executable, ROOT / "tools/tvos_build.py", "--platform", args.platform, "--jobs", args.jobs])


def command_sign(args):
    sign(Path(args.app), Path(args.output), Path(args.profile), Path(args.entitlements), args.identity, BUNDLE)


def command_install(args):
    app = Path(args.app)
    if not (app / "embedded.mobileprovision").is_file():
        raise DeployError(f"{app} is not signed with a profile: run tvos_deploy.py sign first")
    with HeavyLock("devicectl install", enabled=not args.no_lock):
        run(["xcrun", "devicectl", "device", "install", "app", "--device", args.device, app], timeout=600)


# the campaign's maps (and the menus'), in the order of its levels
CAMPAIGN_MAPS = ("ui.map", "a10.map", "a30.map", "a50.map", "b30.map", "b40.map", "c10.map", "c20.map", "c40.map",
                 "d20.map", "d40.map")


def remote_files(device, subdirectory, list_json):
    """the files in a folder of the app's container: {name: size} ({} if none)"""
    run(["xcrun", "devicectl", "device", "info", "files", "--device", device, "--domain-type", "appDataContainer",
         "--domain-identifier", BUNDLE, "--subdirectory", subdirectory, "--no-recurse", "--json-output", list_json],
        check=False, capture=True, timeout=180)
    try:
        entries = json.loads(Path(list_json).read_text())["result"]["files"]
    except (OSError, ValueError, KeyError):
        return {}
    return {entry["name"]: entry.get("metadata", {}).get("size", 0) for entry in entries
            if not entry.get("resources", {}).get("isDirectory")}


def maps_to_push(names, maps, remote):
    """the maps whose copy on the TV is missing or of another size"""
    return [name for name in names if remote.get(name) != (Path(maps) / name).stat().st_size]


def command_push_data(args):
    maps = Path(args.maps)
    names = list(CAMPAIGN_MAPS) if args.campaign else list(args.map_files)
    for name in names:
        if not (maps / name).is_file():
            raise DeployError(f"{maps / name} missing")
    with HeavyLock("devicectl push-data", enabled=not args.no_lock):
        remote = remote_files(args.device, "Library/Caches/OpenCE/maps", SCRATCH / "tv-deploy-maps.json")
        pushing = maps_to_push(names, maps, remote) if not args.force else names
        print(f"maps: {len(names) - len(pushing)} already on the TV at the same size, pushing {pushing}", flush=True)
        for name in pushing:
            run(copy_args("to", args.device, maps / name, f"Library/Caches/OpenCE/maps/{name}"), timeout=1800)
        if args.init:
            run(copy_args("to", args.device, args.init, "Library/Caches/OpenCE/init.txt"), timeout=120)


def backup_config(device, directory=SCRATCH / "tv-config-backups"):
    """the owner's config.toml, copied off the TV to a timestamped file;
    its path (raises when it cannot: nothing may change without a backup)"""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    destination = directory / f"config-{stamp}.toml"
    run(copy_args("from", device, "Library/Caches/OpenCE/config.toml", destination), timeout=120)
    if not destination.is_file() or destination.stat().st_size == 0:
        raise DeployError(f"the backup of config.toml is missing or empty: {destination}")
    print(f"backed up the TV's config.toml to {destination}")
    return destination


def newest_backup(directory=SCRATCH / "tv-config-backups"):
    backups = sorted(Path(directory).glob("config-*.toml"))
    if not backups:
        raise DeployError(f"no backups in {directory}")
    return backups[-1]


def restore_config(device, backup):
    run(copy_args("to", device, backup, "Library/Caches/OpenCE/config.toml"), timeout=120)
    print(f"restored the TV's config.toml from {backup}")


def command_backup_config(args):
    with HeavyLock("devicectl backup-config", enabled=not args.no_lock):
        backup_config(args.device)


def command_restore_config(args):
    with HeavyLock("devicectl restore-config", enabled=not args.no_lock):
        restore_config(args.device, Path(args.backup) if args.backup else newest_backup())


def launch(device, environment, log_path, seconds=None, timeout_margin=120, bundle=BUNDLE):
    """the app launched with its console to log_path; HALO_EXIT_AFTER ends it
    after `seconds`. Returns log_findings of the log, plus the wall time."""
    environment = dict(environment)
    if seconds:
        environment["HALO_EXIT_AFTER"] = str(seconds)
    log_path = Path(log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    command = launch_args(device, environment, bundle)
    print("+", " ".join(command), flush=True)
    with log_path.open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, text=True,
                                   env=developer_environment())
        try:
            process.wait(timeout=(seconds or 600) + timeout_margin)
        except subprocess.TimeoutExpired:
            # (devicectl forwards SIGINT to the app)
            process.send_signal(2)
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    findings = log_findings(log_path.read_text(errors="replace"))
    findings["devicectl_exit"] = process.returncode
    findings["wall_seconds"] = round(time.monotonic() - started, 1)
    return findings


def command_launch(args):
    environment = parse_env(args.env)
    with HeavyLock(f"devicectl launch {args.seconds}s", enabled=not args.no_lock):
        findings = launch(args.device, environment, args.log, args.seconds)
    print(json.dumps(findings, indent=2))
    if not findings["valid"]:
        print(f"INVALID RUN: {findings['invalid_reason']}", file=sys.stderr)
        return 1
    return 0


def copy_from_app(device, source, destination):
    run(copy_args("from", device, source, destination), timeout=300)


def command_pull(args):
    with HeavyLock("devicectl pull", enabled=not args.no_lock):
        copy_from_app(args.device, args.source, args.destination)


def command_screenshot(args):
    with HeavyLock("devicectl screenshot", enabled=not args.no_lock):
        run(["xcrun", "devicectl", "device", "capture", "screenshot", "--device", args.device,
             "--destination", args.output], timeout=60)


def trace_args(template, output, seconds, device, target="attach", process="OpenCETV", environment=None):
    """xctrace's record. On a tvOS device it can attach to a running process
    by name or pid, or record all processes; --launch takes a path or bundle
    on the device. xctrace names the device by its UDID (not CoreDevice's
    identifier): tvos_deploy.py looks it up (device_udid)."""
    command = ["xcrun", "xctrace", "record", "--template", template, "--device", device, "--output", str(output),
               "--time-limit", f"{int(seconds)}s", "--no-prompt"]
    if target == "attach":
        command += ["--attach", process]
    elif target == "all":
        command += ["--all-processes"]
    elif target == "launch":
        for key, value in (environment or {}).items():
            command += ["--env", f"{key}={value}"]
        command += ["--launch", "--", process]
    else:
        raise ValueError(target)
    return command


def device_udid(device):
    """the hardware UDID of a CoreDevice identifier (what xctrace wants)"""
    path = SCRATCH / "tv-deploy" / "devices.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    run(["xcrun", "devicectl", "list", "devices", "--json-output", path], capture=True, timeout=60)
    for entry in json.loads(path.read_text())["result"]["devices"]:
        if entry["identifier"] == device:
            return entry["hardwareProperties"]["udid"]
    raise DeployError(f"no device {device}")


def device_pid(device, executable_suffix):
    """the pid of a running process on the device whose executable path ends so, or None"""
    path = SCRATCH / "tv-deploy" / "processes.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    run(["xcrun", "devicectl", "device", "info", "processes", "--device", device, "--json-output", path],
        capture=True, timeout=60, check=False)
    try:
        processes = json.loads(path.read_text())["result"]["runningProcesses"]
    except (OSError, ValueError, KeyError):
        return None
    for process in processes:
        if process.get("executable", "").endswith(executable_suffix):
            return process.get("processIdentifier")
    return None


def command_trace(args):
    environment = parse_env(args.env)
    with HeavyLock(f"xctrace {args.template}", enabled=not args.no_lock):
        udid = device_udid(args.device)
        if args.launch_app:
            # (the app is launched by devicectl, with its environment, and the
            # trace attaches once it runs)
            log = Path(args.output).with_suffix(".log")
            environment["HALO_EXIT_AFTER"] = str(args.seconds + args.attach_after + 10)
            command = launch_args(args.device, environment, console=True)
            print("+", " ".join(command), flush=True)
            with log.open("w") as handle:
                launched = subprocess.Popen(command, stdout=handle, stderr=subprocess.STDOUT,
                                            env=developer_environment())
                time.sleep(args.attach_after)
                result = run(trace_args(args.template, args.output, args.seconds, udid, "attach", "OpenCETV"),
                             check=False, timeout=args.seconds + 300)
                launched.wait(timeout=args.seconds + args.attach_after + 120)
            print(json.dumps(log_findings(log.read_text(errors="replace")), indent=2))
        else:
            result = run(trace_args(args.template, args.output, args.seconds, udid, args.target, args.process,
                                    environment), check=False, timeout=args.seconds + 300)
    return result.returncode


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default=DEVICE, help="CoreDevice identifier (default: the A12 TV)")
    parser.add_argument("--no-lock", action="store_true", help="do not take the heavy-job lock (caller holds it)")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("build")
    p.add_argument("--platform", choices=("device", "simulator"), default="device")
    p.add_argument("--jobs", default="4")
    p.set_defaults(handler=command_build)

    p = sub.add_parser("sign")
    p.add_argument("--app", default=APP)
    p.add_argument("--output", default=SIGNED)
    p.add_argument("--profile", default=PROFILE)
    p.add_argument("--entitlements", default=ENTITLEMENTS)
    p.add_argument("--identity", default=IDENTITY,
                   help="signing identity's SHA-1, or 'auto': the keychain's identity for the profile's certificate")
    p.set_defaults(handler=command_sign)

    p = sub.add_parser("install")
    p.add_argument("--app", default=SIGNED)
    p.set_defaults(handler=command_install)

    p = sub.add_parser("push-data")
    p.add_argument("--maps", required=True, help="a folder holding the maps (your own)")
    p.add_argument("--map-files", nargs="+", default=["ui.map", "a10.map"])
    p.add_argument("--campaign", action="store_true", help=f"all of {', '.join(CAMPAIGN_MAPS)}")
    p.add_argument("--force", action="store_true", help="push them even if the TV has them at the same size")
    p.add_argument("--init", help="an init.txt to put beside them")
    p.set_defaults(handler=command_push_data)

    p = sub.add_parser("backup-config")
    p.set_defaults(handler=command_backup_config)
    p = sub.add_parser("restore-config")
    p.add_argument("backup", nargs="?")
    p.set_defaults(handler=command_restore_config)

    p = sub.add_parser("launch")
    p.add_argument("--env", action="append", default=[], help="KEY=VALUE for the app (repeatable)")
    p.add_argument("--seconds", type=float, default=60, help="HALO_EXIT_AFTER")
    p.add_argument("--log", default=SCRATCH / "tv-deploy" / "launch.log")
    p.set_defaults(handler=command_launch)

    p = sub.add_parser("pull", help="copy a file out of the app's container")
    p.add_argument("source", help="e.g. Library/Caches/OpenCE/frame_log.csv")
    p.add_argument("destination")
    p.set_defaults(handler=command_pull)

    p = sub.add_parser("screenshot")
    p.add_argument("output")
    p.set_defaults(handler=command_screenshot)

    p = sub.add_parser("trace")
    p.add_argument("--template", default="Metal System Trace")
    p.add_argument("--seconds", type=int, default=20)
    p.add_argument("--output", required=True)
    p.add_argument("--target", choices=("attach", "all", "launch"), default="attach")
    p.add_argument("--process", default="OpenCETV")
    p.add_argument("--launch-app", action="store_true",
                   help="launch the app with devicectl (--env applies), then attach after --attach-after seconds")
    p.add_argument("--attach-after", type=int, default=10)
    p.add_argument("--env", action="append", default=[])
    p.set_defaults(handler=command_trace)

    args = parser.parse_args(argv)
    try:
        return args.handler(args) or 0
    except DeployError as error:
        print(f"tvos_deploy: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
