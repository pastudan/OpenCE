"""Synthetic XDVDFS images for the new read-at parser; no game data."""
import ctypes as C
from pathlib import Path
import struct
import subprocess

import pytest

ROOT = Path(__file__).resolve().parent.parent
READ = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint64, C.c_void_p, C.c_size_t)
CANCEL = C.CFUNCTYPE(C.c_int, C.c_void_p)


class Source(C.Structure):
    _fields_ = [("context", C.c_void_p), ("size", C.c_uint64), ("read", READ), ("cancelled", CANCEL)]


class File(C.Structure):
    _fields_ = [("name", C.c_char * 256), ("offset", C.c_uint64), ("size", C.c_uint64)]


class Layout(C.Structure):
    _fields_ = [("count", C.c_uint), ("total_size", C.c_uint64), ("files", File * 256)]


@pytest.fixture(scope="module")
def library(tmp_path_factory):
    output = tmp_path_factory.mktemp("image-parser") / "parser.dylib"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                    str(ROOT / "port/tvos/import/image_source.c"), "-o", str(output)], check=True)
    lib = C.CDLL(str(output))
    lib.opence_image_discover.argtypes = [C.POINTER(Source), C.POINTER(Layout), C.c_char_p, C.c_size_t]
    lib.opence_image_read_exact.argtypes = [C.POINTER(Source), C.c_uint64, C.c_void_p, C.c_size_t,
                                          C.c_char_p, C.c_size_t]
    return lib


def entry(name, sector, size, directory=False, right=0):
    encoded = name.encode("ascii")
    return struct.pack("<HHIIBB", 0, right, sector, size, 0x10 if directory else 0, len(encoded)) + encoded


def fixture():
    image = bytearray(0x30000)
    image[0x10000:0x10014] = b"MICROSOFT*XBOX*MEDIA"
    image[0x107ec:0x10800] = b"MICROSOFT*XBOX*MEDIA"
    struct.pack_into("<II", image, 0x10014, 40, 2048)
    root = entry("maps", 41, 2048, True)
    image[40 * 2048:40 * 2048 + len(root)] = root
    maps = entry("ui.map", 42, 2048)
    image[41 * 2048:41 * 2048 + len(maps)] = maps
    header = 42 * 2048
    image[header:header + 4] = b"daeh"
    struct.pack_into("<I", image, header + 4, 5)
    build = b"01.10.12.2276\0"
    image[header + 0x40:header + 0x40 + len(build)] = build
    image[header + 0x7fc:header + 0x800] = b"toof"
    assert len(image) == 0x30000
    return image


def discover(library, image, cancelled=False, fail_reads=False):
    calls, violations = [], []
    @READ
    def read(_, offset, pointer, size):
        if offset > len(image) or size > len(image) - offset:
            violations.append((offset, size))
            return 0
        calls.append((offset, size))
        if fail_reads:
            return 0
        C.memmove(pointer, bytes(image[offset:offset + size]), size)
        return 1
    @CANCEL
    def cancel(_):
        return cancelled
    source = Source(None, len(image), read, cancel)
    layout, error = Layout(), C.create_string_buffer(256)
    ok = library.opence_image_discover(C.byref(source), C.byref(layout), error, len(error))
    assert not violations, violations
    return ok, layout, error.value.decode(), calls


def test_minimal_valid_map_and_only_needed_reads(library):
    ok, layout, error, calls = discover(library, fixture())
    assert ok, error
    assert layout.count == 1 and layout.total_size == 2048
    assert layout.files[0].name == b"ui.map"
    assert layout.files[0].offset == 42 * 2048
    assert calls == [(0x10000, 2048), (40 * 2048, 2048), (41 * 2048, 2048), (42 * 2048, 2048)]


@pytest.mark.parametrize("offset,value", [(0x10000, b"bad!"), (42 * 2048, b"bad!"),
    (42 * 2048 + 4, struct.pack("<I", 7)), (42 * 2048 + 0x40, b"01.10.12.2276evil")])
def test_bad_volume_and_map_headers(library, offset, value):
    image = fixture()
    image[offset:offset + len(value)] = value
    ok, layout, _, _ = discover(library, image)
    assert not ok and layout.count == 0


@pytest.mark.parametrize("name", ["../bad", "a/b.map", "a\\b.map", "..", "bad.", "bad "])
def test_unsafe_names(library, name):
    image = fixture()
    valid = entry("ui.map", 42, 2048, right=8)
    image[41 * 2048:41 * 2048 + len(valid)] = valid
    item = entry(name, 43, 0)
    image[41 * 2048 + 32:41 * 2048 + 32 + len(item)] = item
    ok, _, error, _ = discover(library, image)
    assert not ok and error == "Malformed image directory"


def test_cycle(library):
    image = fixture()
    item = entry("ui.map", 42, 2048, right=8)
    image[41 * 2048:41 * 2048 + len(item)] = item
    other = entry("ignored.bin", 43, 0, right=8)
    image[41 * 2048 + 32:41 * 2048 + 32 + len(other)] = other
    ok, _, error, _ = discover(library, image)
    assert not ok and error == "Malformed image directory"


def test_out_of_bounds_extent_never_reaches_callback(library):
    image = fixture()
    struct.pack_into("<I", image, 41 * 2048 + 4, 0xffffffff)
    ok, _, _, calls = discover(library, image)
    assert not ok and len(calls) == 3


def test_cancel_and_source_errors(library):
    ok, _, error, calls = discover(library, fixture(), cancelled=True)
    assert not ok and error == "Import cancelled" and not calls
    ok, _, error, calls = discover(library, fixture(), fail_reads=True)
    assert not ok and error == "Image read failed" and len(calls) == 1


def test_duplicate_casefolded_names(library):
    image = fixture()
    item = entry("ui.map", 42, 2048, right=8)
    image[41 * 2048:41 * 2048 + len(item)] = item
    item = entry("UI.MAP", 43, 2048)
    image[41 * 2048 + 32:41 * 2048 + 32 + len(item)] = item
    image[43 * 2048:44 * 2048] = image[42 * 2048:43 * 2048]
    ok, _, error, _ = discover(library, image)
    assert not ok and error == "Malformed image directory"


def test_overflow_read_rejected_before_callback(library):
    violations = []
    @READ
    def forbidden(*args):
        violations.append(args)
        return 0
    source = Source(None, 4096, forbidden, CANCEL())
    error = C.create_string_buffer(128)
    byte = C.c_char()
    assert not library.opence_image_read_exact(C.byref(source), 2**64 - 1, C.byref(byte), 2,
                                              error, len(error))
    assert not violations


def test_root_maps_directory_alias(library):
    image = fixture()
    struct.pack_into('<I', image, 40 * 2048 + 4, 40)
    ok, _, error, _ = discover(library, image)
    assert not ok and error == 'Overlapping image directories'


def test_overlapping_record_pointer(library):
    image = fixture()
    # Child points into the ui.map record's name, not at a new record.
    struct.pack_into('<H', image, 41 * 2048 + 2, 4)
    ok, _, error, _ = discover(library, image)
    assert not ok and error == 'Malformed image directory'


def test_map_extent_overlap(library):
    image = fixture()
    first = entry('ui.map', 42, 2048, right=8)
    second = entry('a.map', 42, 2048)
    image[41 * 2048:41 * 2048 + len(first)] = first
    image[41 * 2048 + 32:41 * 2048 + 32 + len(second)] = second
    ok, _, error, _ = discover(library, image)
    assert not ok and error == 'Overlapping map file extents'
