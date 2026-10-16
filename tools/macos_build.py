"""Ninja rules for the native macOS build (``ninja macos``).

A native arm64 program cannot give the game what it needs: 32-bit pointers
(its data formats hold them) and its memory at fixed addresses below 4 GB
(the Xbox's window at 0x80000000), since macOS keeps the whole of the low
4 GB from an arm64 process. So the game is compiled for wasm32, an ILP32
target, with wasi-libc (the guest); wasm2c translates that WebAssembly into
C; and the Mac's clang compiles it, with the program that runs it (the
host), into one native executable. Every guest pointer is an offset into one
block of memory the host makes; loads and stores are plain native ones
(wasm2c --unshared-memory: the threads share the memory), with guard pages
past its end instead of bounds checks. See port/macos/README.md.

The graph:

- the guest: the game sources and the port's game units, as the web build
  compiles them, and the platform layer (port/linux/src) with HALO_WASM_GUEST
  and HALO_MACOS, its SDL and file-system calls going to the host
  (port/macos/guest), linked by wasm-ld into build/macos/halo.wasm;
- wasm2c: build/macos/w2c/halo*.c, translated in several files so that they
  compile in parallel;
- the host (port/macos/host): WASI's calls, the threads, the guest's imports
  (build/macos/host_imports.c, written by tools/macos_imports.py), SDL 3 (a
  static library, built with CMake), the Metal renderer, and posix_*.c with
  mbed TLS and miniupnpc, compiled natively;
- build/macos/OpenCE.app, the executable in an application bundle.

The tools (port/macos/tools/setup_toolchain.sh): wasi-sdk's C library and
compiler runtime, Emscripten's clang (wasi-sdk's own cannot compile common
symbols for WebAssembly, which the game's MSVC-era C uses), and wabt's
wasm2c with this port's patch.
"""

import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

from .android_build import VARIADIC_PROTOTYPE_FILES
from .embed_assets import hud_assets_build, hud_configure_inputs
from .linux_build import (EXPAT_DIR, EXPAT_SOURCES, KCP_DIR, MBEDTLS_DIR, MINIUPNPC_DEFINES, MINIUPNPC_DIR,
                          MONOCYPHER_DIR, STB_VORBIS_SOURCE, TOML_DIR, XDK_INCLUDE, ZLIB_DEFINES, ZLIB_DIR,
                          ZLIB_SOURCES, compile_launcher, configuration_defines, game_sources, miniupnpc_sources,
                          musl_math_cflags, musl_math_sources, opus_cflags, opus_sources, updater_defines,
                          xdk_headers)
from .ninja_syntax import Writer

PORT_DIR = Path("port/macos")
LINUX_DIR = Path("port/linux")
ANDROID_DIR = Path("port/android")
PORT_CONFIG = LINUX_DIR / "port.json"
# the release of SDL 3 the other builds use (tools/android_build.py's SDL_TAG)
SDL_VERSION = "3.4.16"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}.tar.gz"
# the memory watch by page contents, shared with the Android host and the web
WATCH_HASH = ANDROID_DIR / "host" / "host_watch_hash.c"

# The guest's memory (port/macos/host/host_main.c): its initial size, where
# the C heap ends (wasi-libc's __heap_end), is the start of the Xbox's window
# (port/linux/src/platform.h); its maximum takes the desktop window's 512 MB too.
GUEST_HEAP_END = 0x80000000
GUEST_MEMORY_BYTES = 0xA0000000
# the main thread's stack (the game recurses deeply: scripts, the BSP's walks)
GUEST_STACK_BYTES = 8 << 20
# wasm2c's output files, compiled in parallel
W2C_OUTPUTS = 24

# The ABI the game was written for, as the other builds reproduce it
# (tools/linux_build.py LINUX_ABI_FLAGS, tools/web_build.py WEB_ABI_FLAGS).
GUEST_ABI_FLAGS = [
    "--target=wasm32-wasip1-threads",
    "-DHALO_WASM_GUEST=1",
    "-DHALO_MACOS=1",
    "-DHALO_METAL=1",
    "-fshort-wchar",
    "-ffp-contract=off",
    "-pthread",
    "-msimd128",
    "-mbulk-memory",
    "-mnontrapping-fptoint",
    "-O2",
    "-g0",
    # wasi-libc's emulations of what the platform layer includes
    "-D_WASI_EMULATED_SIGNAL",
    "-D_WASI_EMULATED_MMAN",
    "-D_WASI_EMULATED_PROCESS_CLOCKS",
    "-D_WASI_EMULATED_GETPID",
]

GUEST_CODE_FLAGS = [
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    # wasi-libc's wide string functions have a 32-bit wchar_t
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

GAME_FLAGS = [
    "-std=gnu89",
    "-D__STRICT_ANSI__",
    "-w",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=int-conversion",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=return-type",
]

# game files that call C library functions without a prototype
PROTOTYPE_FILES = {
    "source/bungie_net/common/random_numbers.c": "time.h",
}

# platform units the guest does without: what the host does natively
# (posix_*.c; memory_watch.c, page protection: web_memory_watch.c's hashing
# instead) and the OpenGL renderer (d3d8_gl.c, gl_functions.c and
# xgpu_post.c: the device is port/macos/guest/d3d8_metal.c, drawing with the
# host's Metal)
GUEST_EXCLUDE = {
    "d3d8_gl.c",
    "memory_watch.c",
    "posix_files.c",
    "posix_net.c",
    "posix_update.c",
    "posix_upnp.c",
    "posix_trace_marker.c",
    "posix_dtls.c",
    "gl_functions.c",
    "xgpu_post.c",
}

# the host's native flags (Apple silicon)
HOST_FLAGS = ["-arch", "arm64", "-mmacosx-version-min=14.4", "-O2", "-g"]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def toolchains() -> Path:
    return Path(os.environ.get("HALO_TOOLCHAINS", str(Path.home() / "toolchains")))


def find_tools(sln: Any) -> Optional[Dict[str, Path]]:
    """wasi-sdk's sysroot and runtime, Emscripten's clang and wasm-ld, and the
    patched wasm2c with its runtime; None if any is missing"""
    root = toolchains()
    wasi_sdk = next(iter(sorted(root.glob("wasi-sdk-*-arm64-macos"), reverse=True)), None)
    emsdk = Path(os.environ.get("EMSDK", str(Path.home() / "emsdk"))) / "upstream" / "bin"
    wasm2c = root / "wabt" / "bin" / "wasm2c"
    runtime = root / "wabt-src" / "wasm2c"
    if not wasi_sdk or not (emsdk / "clang").is_file() or not wasm2c.is_file() or not runtime.is_dir():
        return None
    resource = next(iter(sorted((wasi_sdk / "lib" / "clang").glob("*"), reverse=True)), None)
    if not resource:
        return None
    return {
        "sysroot": wasi_sdk / "share" / "wasi-sysroot",
        "resource": resource,
        "clang": emsdk / "clang",
        "wasm_ld": emsdk / "wasm-ld",
        "wasm2c": wasm2c,
        "runtime": runtime,
        "simde": root / "wabt-src" / "third_party" / "simde",
    }


def macos_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "host", PORT_DIR / "guest", Path("tools/macos_imports.py"),
            *hud_configure_inputs()]


def generate_macos_build(n: Writer, sln: Any) -> None:
    if sys.platform != "darwin" or not PORT_CONFIG.is_file() or not (PORT_DIR / "host").is_dir():
        return
    tools = find_tools(sln)
    if not tools:
        n.comment("macOS build: its tools are missing (port/macos/tools/setup_toolchain.sh)")
        return
    config: Dict[str, Any] = json.loads(PORT_CONFIG.read_text(encoding="utf-8"))
    release = getattr(sln, "port_release", False)

    build_dir: Path = sln.build_dir / "macos"
    obj_dir = build_dir / "obj"
    host_obj_dir = build_dir / "host"
    w2c_dir = build_dir / "w2c"
    semantics_header = sln.build_dir / "linux" / "halo_msvc_semantics.h"
    platform_semantics_header = sln.build_dir / "linux" / "platform_msvc_semantics.h"
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    third_party = build_dir / "third_party"
    sdl_source = third_party / f"SDL3-{SDL_VERSION}"
    sdl_build = build_dir / "sdl3-build"
    libsdl = sdl_build / "libSDL3.a"
    sdl_include = sdl_source / "include"

    n.comment("macOS build (ninja macos); see port/macos/README.md")
    n.variable("macos_guest_cc", _quote(tools["clang"]))
    n.variable("macos_wasm_ld", _quote(tools["wasm_ld"]))
    n.variable("macos_wasm2c", _quote(tools["wasm2c"]))
    n.rule(
        name="macos_guest_cc",
        command=f"{compile_launcher(sln)}$macos_guest_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="MACOS GUEST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="macos_host_cc",
        command=f"{compile_launcher(sln)}clang -MMD -MF $out.d $cflags -c $in -o $out",
        description="MACOS CC $out",
        depfile="$out.d",
        deps="gcc",
    )

    # ---------- SDL 3: fetched, and built as a static library

    n.rule(
        name="macos_sdl3_fetch",
        command=(f"mkdir -p {third_party} && curl -fsSL -o {third_party}/sdl3.tar.gz {SDL_URL} && "
                 f"tar xzf {third_party}/sdl3.tar.gz -C {third_party} && rm {third_party}/sdl3.tar.gz && "
                 f"touch $out"),
        description="MACOS SDL3 (fetched once)",
        pool="console",
        restat=True,
    )
    sdl_stamp = third_party / "sdl3.stamp"
    n.build(outputs=sdl_stamp, rule="macos_sdl3_fetch")
    n.rule(
        name="macos_sdl3",
        command=(f"cmake -S {sdl_source} -B {sdl_build} -G Ninja -DCMAKE_BUILD_TYPE=Release "
                 f"-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4 "
                 f"-DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "
                 f"-DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF "
                 f"> {build_dir}/sdl3-configure.log && ninja -C {sdl_build} > {build_dir}/sdl3-build.log"),
        description="MACOS SDL3",
        pool="console",
        restat=True,
    )
    n.build(outputs=libsdl, rule="macos_sdl3", implicit=[sdl_stamp])

    # ---------- the guest

    abi = " ".join(GUEST_ABI_FLAGS + configuration_defines(sln) + [
        f"--sysroot={_quote(tools['sysroot'])}", f"-resource-dir {_quote(tools['resource'])}"])
    code = " ".join(GUEST_CODE_FLAGS)
    implicit_headers = [*xdk_headers(), prefix_header, semantics_header, platform_semantics_header, sdl_stamp]
    guest_objects: List[Path] = []

    def add_guest(source: Path, cflags: str) -> None:
        relative = source.relative_to(build_dir) if str(source).startswith(str(build_dir)) else source
        obj = obj_dir / relative.with_suffix(".o")
        n.build(outputs=obj, rule="macos_guest_cc", inputs=source, implicit=implicit_headers,
                variables={"cflags": cflags})
        guest_objects.append(obj)

    game_cflags = " ".join([
        abi, code, " ".join(GAME_FLAGS),
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{LINUX_DIR}/include",
        f"-iquote {Path(config['game_sources'])}",
        *(f"-D{define}" for define in config["game"].get("defines", [])),
        *(f"-I{_quote(Path(directory))}" for directory in config["game"].get("include_dirs", [])),
        f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        name = source.as_posix()
        cflags = game_cflags
        if name == "source/main/main.c":
            cflags += " " + updater_defines(release)
        if name in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {ANDROID_DIR}/include/halo_android_variadic_prototypes.h"
        if name in PROTOTYPE_FILES:
            cflags += f" -include {PROTOTYPE_FILES[name]}"
        if name == "source/shell/shell_xbox.c":
            # the game starts once the guest has entered its data folder
            # (port/macos/guest/guest_main.c)
            cflags += " -Dmain=halo_game_main"
        add_guest(source, cflags)
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        add_guest(source, game_cflags)
    add_guest(STB_VORBIS_SOURCE, game_cflags)

    platform_dir = Path(config["platform_sources"])
    platform_cflags = " ".join([
        abi, code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w",
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{platform_dir}", f"-I{LINUX_DIR}/include", f"-I{PORT_DIR}/guest", f"-I{ANDROID_DIR}/host",
        f"-I{TOML_DIR}", f"-I{EXPAT_DIR}", f"-I{KCP_DIR}", f"-I{MONOCYPHER_DIR}", f"-I{ZLIB_DIR}",
        f"-I{_quote(sdl_include)}", "-Isource -Isource/cseries", f"-idirafter {XDK_INCLUDE}",
    ])
    for source in sorted(platform_dir.glob("*.c")):
        if source.name in GUEST_EXCLUDE:
            continue
        add_guest(source, platform_cflags)
    for source in hud_assets_build(n, "macos", build_dir / "generated" / "hud_hires_assets.c"):
        add_guest(source, platform_cflags)
    # the guest's runtime: its SDL and Metal calls to the host, its entry
    # point, the memory watch by page contents (the web build's), and the
    # posix_* calls through the host's errno (tools/android_posix_stubs.py)
    for source in sorted((PORT_DIR / "guest").glob("*.c")):
        add_guest(source, platform_cflags)
    add_guest(Path("port/web/src/web_memory_watch.c"), platform_cflags)
    plain = " ".join([abi, "-std=gnu11", "-w"])
    add_guest(WATCH_HASH, plain)
    posix_stubs = build_dir / "generated" / "guest_posix.c"
    n.rule(
        name="macos_posix_stubs",
        command=f"$python tools/android_posix_stubs.py {platform_dir}/posix.h $out $out.imports",
        description="MACOS POSIX STUBS $out",
    )
    n.build(outputs=posix_stubs, rule="macos_posix_stubs",
            implicit=[platform_dir / "posix.h", Path("tools/android_posix_stubs.py")])
    add_guest(posix_stubs, " ".join([abi, "-std=gnu11", "-w", f"-I{platform_dir}", f"-I{PORT_DIR}/guest",
                                     f"-include {PORT_DIR}/guest/guest_host.h"]))

    add_guest(TOML_DIR / "tomlc17.c", plain)
    for name in EXPAT_SOURCES:
        add_guest(EXPAT_DIR / name, " ".join([abi, "-std=gnu11", f"-I{EXPAT_DIR}", "-w"]))
    add_guest(KCP_DIR / "ikcp.c", plain)
    for source in opus_sources():
        add_guest(source, opus_cflags(abi))
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        add_guest(MONOCYPHER_DIR / name, plain)
    for name in ZLIB_SOURCES:
        add_guest(ZLIB_DIR / name, " ".join([abi, "-std=gnu11", *ZLIB_DEFINES, "-w"]))
    for source in musl_math_sources():
        add_guest(source, musl_math_cflags(abi))

    guest_wasm = build_dir / "halo.wasm"
    n.rule(
        name="macos_guest_link",
        command="$macos_guest_cc $ldflags -o $out @$out.rsp $libs",
        description="MACOS GUEST LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=guest_wasm, rule="macos_guest_link", inputs=guest_objects, variables={
        "ldflags": " ".join([
            abi, f"-fuse-ld={_quote(tools['wasm_ld'])}",
            "-Wl,--import-memory,--shared-memory",
            f"-Wl,--initial-memory={GUEST_HEAP_END},--max-memory={GUEST_MEMORY_BYTES}",
            f"-Wl,-z,stack-size={GUEST_STACK_BYTES}",
            # what the guest calls and does not define: the host's
            # (port/macos/guest/guest_host.h; tools/macos_imports.py)
            "-Wl,--allow-undefined",
            "-Wl,--export=halo_guest_malloc,--export=halo_guest_free",
        ]),
        "libs": "-lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks -lwasi-emulated-getpid",
    })

    # ---------- wasm2c

    w2c_files = [w2c_dir / "halo.h", w2c_dir / "halo-impl.h"] + [w2c_dir / f"halo_{index}.c"
                                                                for index in range(W2C_OUTPUTS)]
    n.rule(
        name="macos_wasm2c",
        command=(f"mkdir -p {w2c_dir} && $macos_wasm2c --enable-threads --unshared-memory "
                 f"--num-outputs={W2C_OUTPUTS} -n halo $in -o {w2c_dir}/halo.c"),
        description="MACOS WASM2C $in",
        restat=True,
    )
    n.build(outputs=w2c_files, rule="macos_wasm2c", inputs=guest_wasm, implicit=[tools["wasm2c"]])

    # ---------- the host

    runtime = tools["runtime"]
    w2c_flags = [
        "-std=gnu11", f"-I{_quote(runtime)}", f"-I{_quote(tools['simde'])}", f"-I{w2c_dir}",
        # bounds by guard pages (wasm2c's default on 64-bit), and a trap is
        # the end of the game (host_main.c's host_trap)
        "-DWASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1",
        "-DWASM_RT_TRAP_HANDLER=host_trap",
    ]
    host_include = [f"-I{PORT_DIR}/host", f"-I{PORT_DIR}/guest", f"-I{platform_dir}", f"-I{_quote(sdl_include)}"]
    host_objects: List[Path] = []

    def add_host(source: Path, cflags: List[str], implicit: Optional[List[Path]] = None) -> None:
        obj = host_obj_dir / source.with_suffix(".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source,
                implicit=[sdl_stamp, *(implicit or [])], variables={"cflags": " ".join(cflags)})
        host_objects.append(obj)

    # the translated game: no warnings (machine-made C), and the optimiser's
    # full attention
    for index in range(W2C_OUTPUTS):
        add_host(w2c_dir / f"halo_{index}.c", HOST_FLAGS + w2c_flags + ["-O3", "-w"],
                 implicit=[w2c_dir / "halo.h", w2c_dir / "halo-impl.h"])
    for name in ("wasm-rt-impl.c", "wasm-rt-mem-impl.c"):
        add_host(runtime / name, HOST_FLAGS + w2c_flags + ["-w"])
    host_imports = build_dir / "generated" / "host_imports.c"
    n.rule(
        name="macos_host_imports",
        command=f"$python tools/macos_imports.py $out {PORT_DIR}/guest/guest_host.h {platform_dir}/posix.h",
        description="MACOS IMPORTS $out",
    )
    n.build(outputs=host_imports, rule="macos_host_imports",
            implicit=[Path("tools/macos_imports.py"), PORT_DIR / "guest" / "guest_host.h", platform_dir / "posix.h"])
    host_flags = HOST_FLAGS + w2c_flags + host_include + ["-Wall", "-Wno-unused-function"]
    add_host(host_imports, host_flags + ["-Wno-unused-variable"], implicit=[w2c_dir / "halo.h"])
    for source in sorted((PORT_DIR / "host").glob("*.c")):
        add_host(source, host_flags, implicit=[w2c_dir / "halo.h"])
    for source in sorted((PORT_DIR / "host").glob("*.m")):
        add_host(source, host_flags + ["-fobjc-arc"], implicit=[w2c_dir / "halo.h"])
    # the platform layer's native half (posix.h), as on Linux, with mbed TLS
    # (internet play's DTLS with browsers) and miniupnpc (its router)
    posix_flags = HOST_FLAGS + ["-std=gnu11", "-D_GNU_SOURCE", "-w", f"-I{platform_dir}"]
    mbedtls_include = f"-I{MBEDTLS_DIR / 'include'}"
    for name in ("posix_files.c", "posix_net.c"):
        add_host(platform_dir / name, posix_flags)
    add_host(platform_dir / "posix_dtls.c", posix_flags + [mbedtls_include])
    add_host(platform_dir / "posix_upnp.c", posix_flags + [f"-I{MINIUPNPC_DIR / 'include'}", "-DMINIUPNP_STATICLIB"])
    for source in sorted((MBEDTLS_DIR / "library").glob("*.c")):
        add_host(source, HOST_FLAGS + ["-std=gnu11", mbedtls_include, f"-I{MBEDTLS_DIR / 'library'}", "-w"])
    for source in miniupnpc_sources():
        add_host(source, HOST_FLAGS + ["-std=gnu11", *MINIUPNPC_DEFINES, f"-I{MINIUPNPC_DIR / 'include'}",
                                       f"-I{MINIUPNPC_DIR / 'src'}", "-w"])

    executable = build_dir / "OpenCE.app" / "Contents" / "MacOS" / "OpenCE"
    frameworks = ["Cocoa", "Metal", "QuartzCore", "IOKit", "CoreAudio", "AudioToolbox", "CoreHaptics",
                  "GameController", "ForceFeedback", "Carbon", "CoreVideo", "UniformTypeIdentifiers",
                  "AVFoundation", "CoreMedia"]
    n.rule(
        name="macos_link",
        command="mkdir -p $$(dirname $out) && clang $ldflags -o $out @$out.rsp $libs",
        description="MACOS LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=executable, rule="macos_link", inputs=host_objects, implicit=[libsdl], variables={
        "ldflags": " ".join(HOST_FLAGS),
        "libs": " ".join([_quote(libsdl), "-liconv", *(f"-framework {name}" for name in frameworks)]),
    })
    info_plist = build_dir / "OpenCE.app" / "Contents" / "Info.plist"
    n.rule(name="macos_copy", command="cp $in $out", description="MACOS COPY $out")
    n.build(outputs=info_plist, rule="macos_copy", inputs=PORT_DIR / "Info.plist")
    n.build(outputs="macos", rule="phony", inputs=[executable, info_plist])
    n.newline()
