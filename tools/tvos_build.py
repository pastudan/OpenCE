#!/usr/bin/env python3
"""Build the development TV game host from the shared wasm2c guest.

    python3 tools/tvos_build.py --platform simulator
    python3 tools/tvos_build.py --platform device

This initial target does not yet contain source-selection/import UI, BC
texture fallback or completed lifecycle handling. Do not distribute it as
playable. It is separate from the feasibility probes and the Mac binary.
"""
import argparse
import os
import re
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def run(command):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=ROOT, check=True)


ASSETS = ROOT / "port/tvos/Assets.xcassets"
BRAND = "App Icon & Top Shelf Image"


def add_icons(app, sdk):
    """Compile the icon and Top Shelf images (port/tvos/Assets.xcassets, made
    by tools/app_icon.py --tvos) into the app's Assets.car, and add the keys
    actool says name them (CFBundleIcons, TVTopShelfImage) to its Info.plist.
    Runs after every build, since cmake rewrites the plist."""
    import plistlib
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        partial = Path(directory) / "partial.plist"
        run(["xcrun", "actool", ASSETS, "--compile", app, "--platform", sdk, "--target-device", "tv",
             "--minimum-deployment-target", "17.4", "--app-icon", BRAND,
             "--output-partial-info-plist", partial, "--errors", "--warnings",
             "--output-format", "human-readable-text"])
        keys = plistlib.loads(partial.read_bytes())
    if not (app / "Assets.car").is_file() or "CFBundleIcons" not in keys:
        raise RuntimeError("actool did not compile the app icon")
    info_path = app / "Info.plist"
    info = plistlib.loads(info_path.read_bytes())
    info.update(keys)
    info_path.write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY))


# The guard-page reservation of the TV build (--memcheck guard): the guest's
# 4 GiB of 32-bit indices, plus room for the largest static offset a load or
# store adds to one (and the access's size). tvOS refuses wasm2c's 8 GiB.
GUARD_RESERVE_BYTES = 0x104000000
OFFSET = re.compile(rb"instance->w2c_env_memory, \(u64\)\([^()]*\) \+ (\d+)u")
# the widest access (v128)
ACCESS_BYTES = 16


def largest_offset(w2c_dir):
    """the largest static offset in the translated code's memory accesses"""
    largest = 0
    for path in sorted(Path(w2c_dir).glob("halo_*.c")):
        for match in OFFSET.finditer(path.read_bytes()):
            largest = max(largest, int(match.group(1)))
    return largest


def check_guard_reserve(w2c_dir, reserve=GUARD_RESERVE_BYTES):
    """raise if an index plus a static offset could pass the reservation's
    end: the access would then reach whatever is mapped after it"""
    offset = largest_offset(w2c_dir)
    if (1 << 32) + offset + ACCESS_BYTES > reserve:
        raise RuntimeError(f"a guest access reaches 4 GiB + {offset:#x}, past the {reserve:#x}-byte "
                           f"guard reservation: raise GUARD_RESERVE_BYTES or build with --memcheck bounds")
    return offset


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", choices=("device", "simulator"), required=True)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 4))
    parser.add_argument("--memcheck", choices=("guard", "bounds"), default=os.environ.get("OPENCE_TV_MEMCHECK", "bounds"),
                        help="the guest's bounds: guard pages in a 4 GiB + 64 MiB reservation, or a compare and "
                             "branch on every access (bounds); OPENCE_TV_MEMCHECK sets the default")
    args = parser.parse_args()
    os.chdir(ROOT)
    if sys.platform != "darwin":
        parser.error("Xcode on macOS is required")
    sdk = "appletvos" if args.platform == "device" else "appletvsimulator"
    sdkpath = subprocess.check_output(["xcrun", "--sdk", sdk, "--show-sdk-path"], text=True).strip()
    # Import the same tool discovery as the Mac generator. Run the script
    # from the repository root so its existing relative paths stay correct.
    sys.path.insert(0, str(ROOT))
    from tools.macos_build import find_tools, SDL_VERSION
    tools = find_tools(None)
    if not tools:
        parser.error("Guest toolchains missing: port/macos/tools/setup_toolchain.sh and emsdk are required")
    # Preserve an existing configure.py invocation (release/profile flags).
    # Ninja's regeneration rule already tracks changed generator inputs.
    if not (ROOT / "build.ninja").is_file():
        run([sys.executable, "configure.py"])
    outputs = ["build/macos/halo.wasm", "build/macos/w2c/halo.h", "build/macos/w2c/halo-impl.h",
               "build/macos/generated/host_imports.c"]
    outputs += [f"build/macos/w2c/halo_{i}.c" for i in range(24)]
    run(["ninja", f"-j{args.jobs}", *outputs])
    if args.memcheck == "guard":
        offset = check_guard_reserve(ROOT / "build/macos/w2c")
        print(f"guard pages: largest static offset {offset:#x}, reservation {GUARD_RESERVE_BYTES:#x}", flush=True)
    build = ROOT / "build/tvos" / args.platform
    run(["cmake", "-S", ROOT / "port/tvos/game", "-B", build, "-G", "Ninja",
         "-DCMAKE_SYSTEM_NAME=tvOS", f"-DCMAKE_OSX_SYSROOT={sdkpath}", "-DCMAKE_OSX_ARCHITECTURES=arm64",
         "-DCMAKE_OSX_DEPLOYMENT_TARGET=17.4", "-DCMAKE_BUILD_TYPE=Release",
         f"-DOPENCE_W2C={ROOT / 'build/macos/w2c'}", f"-DOPENCE_GENERATED={ROOT / 'build/macos/generated'}",
         f"-DOPENCE_WASM_RUNTIME={tools['runtime']}", f"-DOPENCE_SIMDE={tools['simde']}",
         f"-DOPENCE_SDL_SOURCE={ROOT / 'build/macos/third_party' / ('SDL3-' + SDL_VERSION)}",
         f"-DOPENCE_GUEST_MEMCHECK={args.memcheck}", f"-DOPENCE_GUEST_RESERVE_BYTES={GUARD_RESERVE_BYTES:#x}"])
    run(["cmake", "--build", build, "--parallel", args.jobs])
    add_icons(build / "OpenCETV.app", sdk)
    platform = subprocess.check_output(["xcrun", "vtool", "-show-build", str(build / "OpenCETV.app/OpenCETV")], text=True)
    print(platform)
    expected = "TVOSSIMULATOR" if args.platform == "simulator" else "TVOS"
    if f"platform {expected}\n" not in platform:
        raise RuntimeError("incorrect executable platform")
    print(f"Built unsigned development host: {build / 'OpenCETV.app'}; import/gameplay validation not complete.")


if __name__ == "__main__":
    main()
