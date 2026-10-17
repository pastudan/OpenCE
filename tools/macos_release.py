#!/usr/bin/env python3
"""Signs the macOS app (port/macos/README.md) for distribution, notarizes it
with Apple and packs it in a disk image:

    python3 tools/macos_release.py dist/halo-macos-release/OpenCE.app

1. sign: the app with a Developer ID Application certificate, under the
   hardened runtime, with port/macos/OpenCE.entitlements and a secure
   timestamp (which notarization requires);
2. notarize: sends a zip of it to Apple's notary service and waits for the
   verdict (its log is printed if Apple refuses it), then staples the
   ticket to the app, so Gatekeeper accepts it offline;
3. package: OpenCE-macos.dmg beside the app, the app and a link to
   /Applications, itself signed, notarized and stapled.

The certificate is found in the keychain by its name, MACOS_SIGN_IDENTITY
("Developer ID Application: Name (TEAMID)"), or the only Developer ID
Application identity there. Notarization authenticates with an App Store
Connect API key, MACOS_NOTARY_KEY (the .p8 file), MACOS_NOTARY_KEY_ID and
MACOS_NOTARY_ISSUER; or with a profile notarytool store-credentials saved
in the keychain, MACOS_NOTARY_PROFILE. Without either it signs and packs
only, and says so (--sign-only does the same on purpose). With no
Developer ID at all it stops: an app signed ad hoc (tools/ci_build.py)
cannot be notarized. MACOS_SIGN_IDENTITY=- signs ad hoc under the hardened
runtime all the same, to check that the app runs under it.

On a CI runner, MACOS_CERTIFICATE (the certificate and its key, a .p12,
base64) and MACOS_CERTIFICATE_PASSWORD put the identity in a keychain of
its own first, deleted again at the end (.github/workflows/macos.yml).
Needs the Command Line Tools (codesign, notarytool, stapler, hdiutil).
"""

import argparse
import atexit
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENTITLEMENTS = ROOT / "port/macos/OpenCE.entitlements"
VOLUME_NAME = "OpenCE"


def run(command, **options) -> subprocess.CompletedProcess:
    print("+", " ".join(str(part) for part in command), flush=True)
    return subprocess.run([str(part) for part in command], check=True, **options)


def output(command) -> str:
    return subprocess.run([str(part) for part in command], check=True, capture_output=True, text=True).stdout


class Keychain:
    """MACOS_CERTIFICATE's identity in a keychain of its own (a CI runner's),
    or none: the user's keychains."""

    def __init__(self) -> None:
        self.path = None
        certificate = os.environ.get("MACOS_CERTIFICATE")
        if not certificate:
            return
        self.directory = tempfile.mkdtemp()
        self.path = Path(self.directory) / "signing.keychain-db"
        password = base64.b16encode(os.urandom(16)).decode()
        p12 = Path(self.directory) / "certificate.p12"
        p12.write_bytes(base64.b64decode(certificate))
        run(["security", "create-keychain", "-p", password, self.path])
        run(["security", "set-keychain-settings", "-lut", "21600", self.path])
        run(["security", "unlock-keychain", "-p", password, self.path])
        run(["security", "import", p12, "-k", self.path, "-P", os.environ.get("MACOS_CERTIFICATE_PASSWORD", ""),
             "-T", "/usr/bin/codesign"])
        run(["security", "set-key-partition-list", "-S", "apple-tool:,apple:,codesign:", "-s", "-k", password,
             self.path], stdout=subprocess.DEVNULL)
        # (searched along with the user's)
        existing = [line.strip().strip('"') for line in output(["security", "list-keychains", "-d", "user"]).splitlines()]
        run(["security", "list-keychains", "-d", "user", "-s", self.path, *existing])
        p12.unlink()

    def close(self) -> None:
        if self.path:
            subprocess.run(["security", "delete-keychain", str(self.path)], check=False)
            shutil.rmtree(self.directory, ignore_errors=True)


def find_identity() -> str:
    wanted = os.environ.get("MACOS_SIGN_IDENTITY")
    lines = output(["security", "find-identity", "-v", "-p", "codesigning"]).splitlines()
    names = [line.split('"')[1] for line in lines if '"' in line]
    developer_ids = [name for name in names if name.startswith("Developer ID Application:")]
    # ("-": ad hoc, to check the hardened runtime locally; not notarizable)
    if wanted == "-":
        return wanted
    if wanted:
        if wanted not in names:
            sys.exit(f"no code signing identity {wanted!r} in the keychain (security find-identity -v -p codesigning)")
        return wanted
    if len(developer_ids) == 1:
        return developer_ids[0]
    if not developer_ids:
        sys.exit("no Developer ID Application certificate in the keychain: one comes from an Apple Developer "
                 "Program membership (developer.apple.com/account, Certificates), port/macos/README.md")
    sys.exit(f"several Developer ID Application certificates; name one in MACOS_SIGN_IDENTITY: {developer_ids}")


def sign(path: Path, identity: str, entitlements: bool) -> None:
    command = ["codesign", "--force", "--options", "runtime", "--sign", identity]
    if identity != "-":
        command.append("--timestamp")
    if entitlements:
        command += ["--entitlements", ENTITLEMENTS]
    run([*command, path])
    run(["codesign", "--verify", "--strict", "--verbose=2", path])


def notary_arguments():
    if os.environ.get("MACOS_NOTARY_KEY"):
        key = os.environ["MACOS_NOTARY_KEY"]
        # (the key itself rather than its file, as a CI secret gives it)
        if not Path(key).is_file():
            handle, name = tempfile.mkstemp(suffix=".p8")
            os.write(handle, key.encode())
            os.close(handle)
            atexit.register(os.unlink, name)
            key = name
        return ["--key", key, "--key-id", os.environ["MACOS_NOTARY_KEY_ID"],
                "--issuer", os.environ["MACOS_NOTARY_ISSUER"]]
    if os.environ.get("MACOS_NOTARY_PROFILE"):
        return ["--keychain-profile", os.environ["MACOS_NOTARY_PROFILE"]]
    return None


def notarize(path: Path, credentials: list) -> None:
    """Sends path (an app, zipped, or a disk image) and staples the ticket."""
    with tempfile.TemporaryDirectory() as directory:
        upload = path
        if path.suffix == ".app":
            upload = Path(directory) / f"{path.stem}.zip"
            run(["ditto", "-c", "-k", "--keepParent", path, upload])
        result = subprocess.run(["xcrun", "notarytool", "submit", str(upload), "--wait", "--output-format", "json",
                                 *credentials], capture_output=True, text=True)
        print(result.stdout, result.stderr, flush=True)
        try:
            answer = json.loads(result.stdout)
        except json.JSONDecodeError:
            sys.exit(f"notarytool failed: {result.stderr.strip()}")
        if answer.get("status") != "Accepted":
            if answer.get("id"):
                subprocess.run(["xcrun", "notarytool", "log", answer["id"], *credentials], check=False)
            sys.exit(f"Apple did not notarize {path.name}: {answer.get('status')} ({answer.get('message')})")
    run(["xcrun", "stapler", "staple", path])
    run(["xcrun", "stapler", "validate", path])


def package(app: Path, image: Path) -> None:
    with tempfile.TemporaryDirectory() as directory:
        stage = Path(directory) / VOLUME_NAME
        stage.mkdir()
        run(["ditto", app, stage / app.name])
        (stage / "Applications").symlink_to("/Applications")
        if image.exists():
            image.unlink()
        run(["hdiutil", "create", "-volname", VOLUME_NAME, "-srcfolder", stage, "-fs", "HFS+", "-format", "UDZO",
             "-ov", image])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("app", type=Path)
    parser.add_argument("--sign-only", action="store_true", help="sign and pack, without notarizing")
    parser.add_argument("--dmg", type=Path, help="the disk image (default: OpenCE-macos.dmg beside the app)")
    args = parser.parse_args()
    app = args.app.resolve()
    if not (app / "Contents" / "Info.plist").is_file():
        sys.exit(f"{app} is not an app")
    image = (args.dmg or app.parent / "OpenCE-macos.dmg").resolve()

    keychain = Keychain()
    try:
        identity = find_identity()
        print(f"signing with {identity}", flush=True)
        sign(app, identity, True)
        credentials = None if args.sign_only or identity == "-" else notary_arguments()
        if credentials:
            notarize(app, credentials)
        elif not args.sign_only and identity != "-":
            print("::warning::no notarization credentials (MACOS_NOTARY_KEY or MACOS_NOTARY_PROFILE): "
                  "signed, not notarized", flush=True)
        package(app, image)
        sign(image, identity, False)
        if credentials:
            notarize(image, credentials)
            run(["spctl", "--assess", "--type", "execute", "--verbose", app])
            run(["spctl", "--assess", "--type", "open", "--context", "context:primary-signature", "--verbose",
                 image])
        print(f"{image.relative_to(ROOT) if image.is_relative_to(ROOT) else image}: "
              f"{'notarized' if credentials else 'signed only'}", flush=True)
    finally:
        keychain.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
