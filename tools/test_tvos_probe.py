"""Asset-free checks of the probe dependency/archive handling."""
import hashlib
import io
from pathlib import Path
import tarfile

import pytest
import tvos_probe as probe


def archive(path, entries):
    with tarfile.open(path, "w:bz2") as out:
        for name, data, kind in entries:
            entry = tarfile.TarInfo(name)
            entry.type = kind
            if kind == tarfile.SYMTYPE:
                entry.linkname = "../../outside"
            elif kind == tarfile.REGTYPE:
                entry.size = len(data)
            out.addfile(entry, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_boost_checksum_and_clean_extraction(tmp_path, monkeypatch):
    packed = tmp_path / probe.BOOST_ARCHIVE
    digest = archive(packed, [("boost_1_88_0/boost/version.hpp", b"verified", tarfile.REGTYPE)])
    monkeypatch.setattr(probe, "BOOST_SHA256", digest)
    target = tmp_path / "boost_1_88_0"
    outside = tmp_path / "outside"
    outside.mkdir()
    target.mkdir()
    (target / "boost").symlink_to(outside, target_is_directory=True)
    result = probe.boost_headers(tmp_path)
    assert (result / "boost/version.hpp").read_bytes() == b"verified"
    assert not (outside / "version.hpp").exists()
    (result / "boost/version.hpp").write_bytes(b"corrupt cache")
    probe.boost_headers(tmp_path)
    assert (result / "boost/version.hpp").read_bytes() == b"verified"
    assert packed.is_file()  # verified archive, not an unauthenticated stamp


@pytest.mark.parametrize("name,kind", [("../escape", tarfile.REGTYPE),
    ("/absolute", tarfile.REGTYPE), ("boost_1_88_0/link", tarfile.SYMTYPE)])
def test_reject_archive_paths_and_links(tmp_path, monkeypatch, name, kind):
    packed = tmp_path / probe.BOOST_ARCHIVE
    monkeypatch.setattr(probe, "BOOST_SHA256", archive(packed, [(name, b"bad", kind)]))
    with pytest.raises(RuntimeError):
        probe.boost_headers(tmp_path)


def test_reject_checksum_mismatch(tmp_path):
    (tmp_path / probe.BOOST_ARCHIVE).write_bytes(b"untrusted")
    with pytest.raises(RuntimeError, match="checksum mismatch"):
        probe.boost_headers(tmp_path)


def test_simulator_type_must_match_runtime():
    import tvos_simcheck
    runtime = {"identifier": "com.apple.CoreSimulator.SimRuntime.tvOS-27-0", "version": "27.0",
               "isAvailable": True, "supportedDeviceTypes": [{"identifier": "compatible"}]}
    types = [{"name": "Apple TV 4K", "identifier": "compatible"},
             {"name": "Apple TV 1080p", "identifier": "obsolete"}]
    chosen_runtime, device = tvos_simcheck.select_simulator([runtime], types)
    assert chosen_runtime is runtime and device["identifier"] == "compatible"
    with pytest.raises(ValueError, match="compatible"):
        tvos_simcheck.select_simulator([runtime], types[1:])
    with pytest.raises(ValueError, match="No installed"):
        tvos_simcheck.select_simulator([], types)


def test_probe_scene_manifest_matches_delegate():
    import plistlib
    root = Path(__file__).resolve().parent.parent
    info = plistlib.loads((root / "port/tvos/probe/Info.plist").read_bytes())
    config = info["UIApplicationSceneManifest"]["UISceneConfigurations"]["UIWindowSceneSessionRoleApplication"][0]
    source = (root / "port/tvos/probe/platform_probe.m").read_text()
    assert f'@interface {config["UISceneDelegateClassName"]} :' in source
    assert 'initWithWindowScene:' in source
