#!/usr/bin/env python3
"""Makes the game's icons from its artwork, port/assets/icon:

    python tools/app_icon.py
    python tools/app_icon.py --macos   # the Mac app's icon only (on a Mac)
    python tools/app_icon.py --tvos    # the Apple TV app's icon and Top Shelf images only

- opence-icon.png, the helmet on its background (512x512): the Windows
  executable's icon (port/windows/opence-icon.ico, which port/windows/halo.rc
  puts in halo.exe) and the desktop builds' window icon (opence-icon-256.png,
  which tools/embed_assets.py embeds and sdl_platform.c gives the window);
- opence-icon-render.png, the helmet alone on transparency (1024x1024, the same
  framing): the Android app's launcher icon;
- opence-icon.png again: the web page's icons (port/web/site/icons), its tab's,
  the installed web app's and the home screen's.

The launcher icon is an adaptive one (Android 8 and later): the render as its
foreground layer, the background's gradient as its background layer, and the
render's light parts (the armour, not the visor) as its monochrome (themed
icon) layer. Launchers mask adaptive icons to their own shapes and show only
the middle 72 dp of the 108 dp layers, so the render fills those 72 dp, as
the picture fills the other icons, and its edges are carried out to the
layers' edges for the launchers that move the layers. Writes the layers for
every screen density into port/android/app/src/main/res (mipmap-*), and the
icon's definition (mipmap-anydpi-v26/ic_launcher.xml) and background
(drawable/ic_launcher_background.xml). Run it again when the artwork
changes. Needs Pillow and NumPy.
"""

from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ARTWORK = ROOT / "port/assets/icon"
ICON = ARTWORK / "opence-icon.png"
RENDER = ARTWORK / "opence-icon-render.png"
WINDOW_ICON = ARTWORK / "opence-icon-256.png"
WINDOWS_ICON = ROOT / "port/windows/opence-icon.ico"
MACOS_ICON = ROOT / "port/macos/OpenCE.icns"
RESOURCES = ROOT / "port/android/app/src/main/res"

# the layers' size, and the part of them a launcher shows, in dp
LAYER_DP = 108
VISIBLE_DP = 72
# pixels per dp of each density
DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}
WINDOWS_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
# the web page's: its tab's (32), iOS's home screen's (180) and the web app
# manifest's (192, 512)
WEB_ICONS = ROOT / "port/web/site/icons"
WEB_SIZES = [32, 180, 192, 512]


def background_gradient(icon: Image.Image, render: Image.Image) -> tuple:
    """the icon's background as a line through its rows' colours (where the
    render leaves it bare): the colours at the top and the bottom of the
    launcher's layer, which reaches (LAYER_DP - VISIBLE_DP) / 2 beyond the
    picture at each side"""
    pixels = np.asarray(icon.convert("RGB"), dtype=np.float64)
    bare = np.asarray(render.resize(icon.size, Image.LANCZOS).getchannel("A")) == 0
    rows, colours = [], []
    for y in range(icon.height):
        if bare[y].any():
            rows.append(y)
            colours.append(pixels[y][bare[y]].mean(axis=0))
    fit = np.polyfit(np.array(rows, dtype=np.float64), np.array(colours), 1)
    margin = (LAYER_DP - VISIBLE_DP) / 2 / VISIBLE_DP * icon.height

    def at(y: float) -> tuple:
        return tuple(int(round(min(255.0, max(0.0, fit[0][c] * y + fit[1][c])))) for c in range(3))

    return at(-margin), at(icon.height + margin)


def carried_out(image: Image.Image, size: int) -> Image.Image:
    """image in the middle of a square of size, its edge pixels carried out
    to the square's edges"""
    pad = (size - image.width) // 2
    data = np.asarray(image)
    padded = np.pad(data, ((pad, size - image.height - pad), (pad, size - image.width - pad), (0, 0)), mode="edge")
    return Image.fromarray(padded, "RGBA")


def monochrome(render: Image.Image) -> Image.Image:
    """the render's light parts, white with their lightness as opacity: the
    armour without the darker visor"""
    data = np.asarray(render, dtype=np.float64)
    lightness = (0.2126 * data[..., 0] + 0.7152 * data[..., 1] + 0.0722 * data[..., 2]) / 255.0
    alpha = np.clip(data[..., 3] / 255.0 * np.clip((lightness - 0.25) / 0.5, 0.0, 1.0), 0.0, 1.0)
    out = np.zeros(data.shape, dtype=np.uint8)
    out[..., :3] = 255
    out[..., 3] = np.round(alpha * 255).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def macos_icon(icon: Image.Image, render: Image.Image) -> None:
    """the Mac app's icon (port/macos/OpenCE.icns, which tools/macos_build.py
    puts in OpenCE.app): the render on the background's gradient, in the
    rounded square Mac icons have (824 of the 1024 pixels, the rest left
    clear for the shadow the Dock and Finder draw), at each of the sizes an
    icon set has. Needs iconutil (macOS)."""
    import shutil
    import subprocess
    import tempfile

    top, bottom = background_gradient(icon, render)
    side, inner, radius = 1024, 824, 185
    rows = np.linspace(0.0, 1.0, inner)[:, None]
    gradient = np.zeros((inner, inner, 4), np.float64)
    for channel in range(3):
        gradient[..., channel] = top[channel] + (bottom[channel] - top[channel]) * rows
    gradient[..., 3] = 255
    tile = Image.fromarray(gradient.astype(np.uint8))
    # (the render fills the square as it fills the other icons)
    tile.alpha_composite(render.resize((inner, inner), Image.LANCZOS))
    # (the corners, drawn at four times the size for smooth edges)
    mask = Image.new("L", (inner * 4, inner * 4), 0)
    from PIL import ImageDraw

    ImageDraw.Draw(mask).rounded_rectangle((0, 0, inner * 4 - 1, inner * 4 - 1), radius * 4, fill=255)
    tile.putalpha(mask.resize((inner, inner), Image.LANCZOS))
    picture = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    picture.alpha_composite(tile, ((side - inner) // 2, (side - inner) // 2))
    with tempfile.TemporaryDirectory() as directory:
        iconset = Path(directory) / "OpenCE.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            picture.resize((size, size), Image.LANCZOS).save(iconset / f"icon_{size}x{size}.png")
            picture.resize((size * 2, size * 2), Image.LANCZOS).save(iconset / f"icon_{size}x{size}@2x.png")
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(Path(directory) / "OpenCE.icns")],
                       check=True)
        shutil.copyfile(Path(directory) / "OpenCE.icns", MACOS_ICON)
    print(f"{MACOS_ICON.relative_to(ROOT)}: 16 to 1024")


TVOS_ASSETS = ROOT / "port/tvos/Assets.xcassets"
TVOS_BRAND = "App Icon & Top Shelf Image"
FONT = ROOT / "port/assets/fonts/OpenCE-Regular.ttf"


def tvos_icons(icon: Image.Image, render: Image.Image) -> None:
    """the Apple TV app's icon and Top Shelf images, as an asset catalog
    (port/tvos/Assets.xcassets, which tools/tvos_build.py compiles into
    OpenCETV.app with actool). A TV icon is a stack of layers the home screen
    moves for its parallax: the background's gradient as the back layer
    (opaque) and the helmet render as the front one, at 400x240 and 800x480,
    and 1280x768 for the App Store. The Top Shelf images (1920x720 and
    2320x720, with their @2x) are the same gradient with the helmet and the
    name."""
    import json
    import shutil
    from PIL import ImageDraw, ImageFont

    top, bottom = background_gradient(icon, render)
    info = {"author": "xcode", "version": 1}

    def gradient(width: int, height: int) -> Image.Image:
        rows = np.linspace(0.0, 1.0, height)[:, None]
        data = np.zeros((height, width, 4), np.float64)
        for channel in range(3):
            data[..., channel] = top[channel] + (bottom[channel] - top[channel]) * rows
        data[..., 3] = 255
        return Image.fromarray(np.round(data).astype(np.uint8), "RGBA")

    def helmet(width: int, height: int, fraction: float, centre_x: float = 0.5) -> Image.Image:
        """the render on transparency, fraction of the height tall"""
        side = int(round(height * fraction))
        layer = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        layer.alpha_composite(render.resize((side, side), Image.LANCZOS),
                              (int(round(width * centre_x - side / 2)), (height - side) // 2))
        return layer

    def write_json(path: Path, data: dict) -> None:
        path.mkdir(parents=True, exist_ok=True)
        (path / "Contents.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    if TVOS_ASSETS.exists():
        shutil.rmtree(TVOS_ASSETS)
    write_json(TVOS_ASSETS, {"info": info})
    brand = TVOS_ASSETS / f"{TVOS_BRAND}.brandassets"

    def stack(name: str, sizes: list, idiom: str) -> None:
        """an image stack of a back and a front layer at each (scale, width, height)"""
        directory = brand / f"{name}.imagestack"
        write_json(directory, {"info": info, "layers": [{"filename": "Front.imagestacklayer"},
                                                        {"filename": "Back.imagestacklayer"}]})
        for layer, draw in (("Front", lambda w, h: helmet(w, h, 0.86)), ("Back", gradient)):
            imageset = directory / f"{layer}.imagestacklayer" / "Content.imageset"
            write_json(imageset.parent, {"info": info})
            images = []
            for scale, width, height in sizes:
                filename = f"{layer.lower()}{'' if scale == 1 else f'@{scale}x'}.png"
                picture = draw(width, height)
                if layer == "Back":
                    picture = picture.convert("RGB")
                imageset.mkdir(parents=True, exist_ok=True)
                picture.save(imageset / filename, optimize=True)
                images.append({"filename": filename, "idiom": idiom, "scale": f"{scale}x"})
            write_json(imageset, {"images": images, "info": info})

    stack("App Icon", [(1, 400, 240), (2, 800, 480)], "tv")
    stack("App Icon - App Store", [(1, 1280, 768)], "tv")

    def shelf(name: str, width: int, height: int) -> None:
        imageset = brand / f"{name}.imageset"
        images = []
        for scale in (1, 2):
            w, h = width * scale, height * scale
            picture = gradient(w, h)
            picture.alpha_composite(helmet(w, h, 0.78, centre_x=0.30))
            font = ImageFont.truetype(str(FONT), int(round(h * 0.22)))
            draw = ImageDraw.Draw(picture)
            text = "OpenCE"
            box = draw.textbbox((0, 0), text, font=font)
            draw.text((int(w * 0.47), (h - (box[3] - box[1])) // 2 - box[1]), text, font=font,
                      fill=(255, 255, 255, 255))
            filename = f"top_shelf{'' if scale == 1 else f'@{scale}x'}.png"
            imageset.mkdir(parents=True, exist_ok=True)
            picture.convert("RGB").save(imageset / filename, optimize=True)
            images.append({"filename": filename, "idiom": "tv", "scale": f"{scale}x"})
        write_json(imageset, {"images": images, "info": info})

    shelf("Top Shelf Image", 1920, 720)
    shelf("Top Shelf Image Wide", 2320, 720)
    write_json(brand, {"assets": [
        {"filename": "App Icon - App Store.imagestack", "idiom": "tv", "role": "primary-app-icon",
         "size": "1280x768"},
        {"filename": "App Icon.imagestack", "idiom": "tv", "role": "primary-app-icon", "size": "400x240"},
        {"filename": "Top Shelf Image Wide.imageset", "idiom": "tv", "role": "top-shelf-image-wide",
         "size": "2320x720"},
        {"filename": "Top Shelf Image.imageset", "idiom": "tv", "role": "top-shelf-image", "size": "1920x720"},
    ], "info": info})
    print(f"{TVOS_ASSETS.relative_to(ROOT)}: icon 400x240 (@2x), App Store 1280x768, Top Shelf 1920/2320x720 (@2x)")


def web_icons(icon: Image.Image) -> None:
    WEB_ICONS.mkdir(parents=True, exist_ok=True)
    for size in WEB_SIZES:
        icon.resize((size, size), Image.LANCZOS).save(WEB_ICONS / f"icon-{size}.png", optimize=True)
    print(f"{WEB_ICONS.relative_to(ROOT)}: {', '.join(str(size) for size in WEB_SIZES)}")


def main() -> None:
    import sys

    icon = Image.open(ICON).convert("RGBA")
    render = Image.open(RENDER).convert("RGBA")
    # (--macos: the Mac app's icon only, which needs a Mac's iconutil)
    if "--macos" in sys.argv[1:]:
        macos_icon(icon, render)
        return
    # (--tvos: the Apple TV app's icon and Top Shelf images only)
    if "--tvos" in sys.argv[1:]:
        tvos_icons(icon, render)
        return

    # the desktop's icons
    icon.resize((256, 256), Image.LANCZOS).save(WINDOW_ICON, optimize=True)
    print(f"{WINDOW_ICON.relative_to(ROOT)}: 256x256")
    icon.save(WINDOWS_ICON, sizes=[(size, size) for size in WINDOWS_SIZES])
    print(f"{WINDOWS_ICON.relative_to(ROOT)}: {', '.join(str(size) for size in WINDOWS_SIZES)}")
    web_icons(icon)

    # the launcher's layers
    themed = monochrome(render)
    for density, scale in DENSITIES.items():
        size = round(LAYER_DP * scale)
        visible = round(VISIBLE_DP * scale)
        directory = RESOURCES / f"mipmap-{density}"
        directory.mkdir(parents=True, exist_ok=True)
        for name, layer in (("ic_launcher_foreground", render), ("ic_launcher_monochrome", themed)):
            carried_out(layer.resize((visible, visible), Image.LANCZOS), size).save(
                directory / f"{name}.png", optimize=True)
        print(f"{directory.relative_to(ROOT)}: {size}x{size}")

    top, bottom = background_gradient(icon, render)
    drawable = RESOURCES / "drawable"
    drawable.mkdir(parents=True, exist_ok=True)
    (drawable / "ic_launcher_background.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<!-- the launcher icon's background (tools/app_icon.py) -->\n"
        '<shape xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <gradient android:angle="270" android:startColor="#{:02X}{:02X}{:02X}"'
        ' android:endColor="#{:02X}{:02X}{:02X}" />\n'.format(*top, *bottom) +
        "</shape>\n", encoding="utf-8")
    adaptive = RESOURCES / "mipmap-anydpi-v26"
    adaptive.mkdir(parents=True, exist_ok=True)
    (adaptive / "ic_launcher.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<!-- the launcher icon (tools/app_icon.py, from port/assets/icon) -->\n"
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@drawable/ic_launcher_background" />\n'
        '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
        '    <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />\n'
        "</adaptive-icon>\n", encoding="utf-8")
    print("background #{:02X}{:02X}{:02X} to #{:02X}{:02X}{:02X}".format(*top, *bottom))


if __name__ == "__main__":
    main()
