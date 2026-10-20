#!/usr/bin/env python3
"""Build the Apple TV feasibility probes, not the game yet.

    python3 tools/tvos_probe.py --platform macos
    python3 tools/tvos_probe.py --platform tvos
    python3 tools/tvos_probe.py --platform simulator

Downloads pinned source into build/tvos-probe/deps, checks its identity,
builds OpenSSL for the selected SDK, then builds libtorrent with ordinary
BitTorrent AND WebTorrent (libdatachannel), and a small platform probe.
The macOS probe runs asset-free network tests; the TV app must be run on a
paired device to validate memory/Metal/threads. An unsigned .app is not a
sideloadable build. No signing credentials, game data or running services
are needed to compile. See port/tvos/README.md.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
LIBTORRENT = "6da363d2994f17c0b3c0450d124cf73a31a73847"  # v2.1.2
OPENSSL = "286ddeaac037533bbdce65b3c689e3f7ffebf0f6"  # openssl-3.5.6
BOOST_VERSION = "1.88.0"
BOOST_ARCHIVE = "boost_1_88_0.tar.bz2"
BOOST_SHA256 = "46d9d2c06637b219270877c9e16155cbd015b6dc84349af064c088e9b5b12f7b"


def run(command, **kwargs):
    print("+", " ".join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def text(command):
    return subprocess.check_output(list(map(str, command)), text=True).strip()


def checkout(directory, url, commit, tag):
    if not directory.exists():
        run(["git", "clone", "--depth", "1", "--branch", tag, url, directory])
    actual = text(["git", "-C", directory, "rev-parse", "HEAD"])
    if actual != commit:
        raise RuntimeError(f"{directory}: expected {commit}, got {actual}; remove this dependency and retry")
    if text(["git", "-C", directory, "status", "--porcelain", "--untracked-files=no"]):
        raise RuntimeError(f"{directory}: tracked source was modified")


def boost_headers(deps):
    directory = deps / "boost_1_88_0"
    archive = deps / BOOST_ARCHIVE
    if not archive.exists():
        print(f"Downloading Boost {BOOST_VERSION}", flush=True)
        urllib.request.urlretrieve(f"https://archives.boost.io/release/{BOOST_VERSION}/source/{BOOST_ARCHIVE}", archive)
    digest = hashlib.sha256()
    with archive.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    if digest.hexdigest() != BOOST_SHA256:
        raise RuntimeError("Boost archive checksum mismatch")
    with tarfile.open(archive) as source:
        # Reject links and paths outside the one expected archive root.
        members = source.getmembers()
        for member in members:
            parts = Path(member.name).parts
            if not parts or parts[0] != directory.name or ".." in parts or member.name.startswith("/"):
                raise RuntimeError("unsafe Boost archive path")
            if not (member.isfile() or member.isdir()):
                raise RuntimeError("unexpected link or special file in Boost archive")
        # A new private tree avoids following pre-existing cached symlinks.
        # Keep and rehash the archive; a text stamp cannot authenticate headers.
        with tempfile.TemporaryDirectory(dir=deps) as stage:
            source.extractall(stage, members=members)
            if directory.is_symlink():
                directory.unlink()
            elif directory.exists():
                shutil.rmtree(directory)
            shutil.move(str(Path(stage) / directory.name), directory)
    return directory


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", choices=("macos", "tvos", "simulator"), required=True)
    parser.add_argument("--build-root", type=Path, default=ROOT / "build/tvos-probe")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 6))
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("Apple SDKs and clang on macOS are required")
    sdk = {"macos": "macosx", "tvos": "appletvos", "simulator": "appletvsimulator"}[args.platform]
    try:
        sdk_path = text(["xcrun", "--sdk", sdk, "--show-sdk-path"])
    except subprocess.CalledProcessError:
        parser.error(f"{sdk} SDK not installed: install full Xcode and its tvOS platform; see port/tvos/README.md")
    compiler = text(["xcrun", "--sdk", sdk, "--find", "clang"])
    triple = {"macos": "arm64-apple-macos14.4", "tvos": "arm64-apple-tvos17.4",
              "simulator": "arm64-apple-tvos17.4-simulator"}[args.platform]
    root = args.build_root.resolve()
    deps = root / "deps"
    deps.mkdir(parents=True, exist_ok=True)
    checkout(deps / "libtorrent", "https://github.com/arvidn/libtorrent.git", LIBTORRENT, "v2.1.2")
    run(["git", "-C", deps / "libtorrent", "submodule", "update", "--init", "--recursive", "--depth", "1"])
    # Submodule revisions are fixed by the pinned parent commit, never remote branch tips.
    boost = boost_headers(deps)
    checkout(deps / "openssl", "https://github.com/openssl/openssl.git", OPENSSL, "openssl-3.5.6")
    prefix = root / args.platform / "openssl-install"
    openssl_build = root / args.platform / "openssl-build"
    configure = [deps / "openssl/Configure", "darwin64-arm64-cc", "no-shared", "no-tests", "no-apps", "no-module",
                 "no-asm", "no-ui-console", "no-async", "no-engine", f"--prefix={prefix}",
                 f"--openssldir={prefix / 'ssl'}", f"--target={triple}", "-isysroot", sdk_path]
    build_key = repr(list(map(str, configure))) + text([compiler, '--version'])
    build_key += text(["xcrun", "--sdk", sdk, "--show-sdk-version"])
    build_key += repr({key: os.environ.get(key, "") for key in ("CFLAGS", "CPPFLAGS", "LDFLAGS")})
    stamp = prefix / "build-key.txt"
    installed = all((prefix / part).is_file() for part in ("include/openssl/ssl.h", "lib/libssl.a", "lib/libcrypto.a"))
    if not installed or not stamp.is_file() or stamp.read_text() != build_key:
        if openssl_build.exists():
            shutil.rmtree(openssl_build)
        openssl_build.mkdir(parents=True)
        environment = dict(os.environ, CC=compiler, CXX=compiler + "++", SDKROOT=sdk_path)
        # OpenSSL's Darwin ARM assembly and compiler setup, explicitly retargeted
        # to AppleTVOS. No executables, engines, assembly or dynamic modules.
        run(configure, cwd=openssl_build, env=environment)
        run(["make", f"-j{args.jobs}", "build_sw"], cwd=openssl_build, env=environment)
        run(["make", "install_sw"], cwd=openssl_build, env=environment)
        stamp.write_text(build_key)
    cmake_build = root / args.platform / "build"
    command = ["cmake", "-S", ROOT / "port/tvos/probe", "-B", cmake_build, "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_OSX_ARCHITECTURES=arm64", f"-DCMAKE_OSX_SYSROOT={sdk_path}",
               f"-DOPENCE_PROBE_PLATFORM={args.platform}", f"-DOPENCE_LIBTORRENT_SOURCE={deps / 'libtorrent'}",
               f"-DOPENCE_BOOST_SOURCE={boost}", f"-DOPENSSL_ROOT_DIR={prefix}",
               f"-DOPENSSL_INCLUDE_DIR={prefix / 'include'}", f"-DOPENSSL_SSL_LIBRARY={prefix / 'lib/libssl.a'}",
               f"-DOPENSSL_CRYPTO_LIBRARY={prefix / 'lib/libcrypto.a'}",
               "-DOPENSSL_USE_STATIC_LIBS=ON", "-DBUILD_SHARED_LIBS=OFF", "-Dwebtorrent=ON",
               "-Dbuild_tests=OFF", "-Dbuild_examples=OFF", "-Dbuild_tools=OFF", "-Dpython-bindings=OFF",
               "-Di2p=OFF", "-Dlogging=OFF", "-Dmutable-torrents=OFF", "-DNO_MEDIA=ON",
               "-DNO_EXAMPLES=ON", "-DNO_TESTS=ON"]
    if args.platform != "macos":
        command += ["-DCMAKE_SYSTEM_NAME=tvOS", "-DCMAKE_OSX_DEPLOYMENT_TARGET=17.4",
                    "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO"]
    else:
        command += ["-DCMAKE_OSX_DEPLOYMENT_TARGET=14.4"]
    run(command)
    run(["cmake", "--build", cmake_build, "--parallel", args.jobs])
    if args.platform == "macos":
        run(["ctest", "--test-dir", cmake_build, "--output-on-failure"])
    print(f"Built {args.platform} probes at {cmake_build}; this is NOT the game or a device-tested release.")


if __name__ == "__main__":
    main()
