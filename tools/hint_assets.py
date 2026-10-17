#!/usr/bin/env python3
"""Makes high-res textures of the HUD's hint icons (port/assets/hints/*.png):
the controller's sticks, triggers and buttons that the HUD's messages and
the menus' text draw beside their words ("Press LEFT [stick] to move"), and
the warnings by the reticle (Reload, No Ammo, Low Battery, No Grenades,
No Weapon), from hand-drawn SVG redraws (port/assets/hints/svg) and words
set in Overpass (port/assets/fonts):

    python tools/hint_assets.py layout --map assets/maps/a10.map
    python tools/hint_assets.py build
    python tools/hint_assets.py check --map assets/maps/a10.map --out /tmp/hint_check

The sheets are bitmaps of every map: ui\\hud\\bitmaps\\combined\\hud_msg_icons
(the HUD's messages: hud_globals' messaging icons), hud_msg_icons_sm (the
menus' text: draw_string_and_hack_in_icons, ui_widget.c) and
combined\\hud_reticle_warnings (the weapons' crosshairs). Their icons carry
English words, so they draw only on the maps their pixels are the English
maps' (the game checks a CRC of them, as for the rest of the high-res HUD:
port/linux/src/hud_hires.c); another language's keep the map's art.

The message icons' sheets also have the buttons (A, B, X, Y: on the small
sheet tools/button_assets.py's sprites, on the large one drawn here from
the same redraws, as stencils) and the weapons' and grenades' pictures (not
redrawn), so each texture here stands only for the
sprites it draws, through a placeholder bitmap (port/linux/game/
hud_hires_tags.c). The warnings' sheet is drawn whole.

Every icon is a white drawing (the game tints it) in a sprite's cell: a word
above a picture. The words are set in Overpass, their size, width, spacing
and place fitted to the map's word by least squares; each picture is a
redraw drawn over the large sheet's picture (whose sizes were fitted to it),
placed on each cell by the shape of the map's picture there (its area and
centre, then least squares). A thumbstick's click is its stick pressed: the
cap moved down onto its stem by as much as the map's frame moves it.

layout: reads the sheets from the map (their sprites, sizes and pixels'
    CRCs) and fits each word and picture to its cell; writes
    port/assets/hints/hints.json (numbers only).
build: draws port/assets/hints/*.png as hints.json says (committed; the
    builds embed them: tools/embed_assets.py).
check: compares each PNG, reduced to its bitmap's size, with the map's
    bitmap and writes side-by-side images into --out.

Needs rsvg-convert, Pillow, NumPy, SciPy and fontTools.
"""

import argparse
import json
import re
import sys
import tempfile
import xml.etree.ElementTree as ElementTree
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hud_assets import (FORMATS, XboxMap, decode_bitmap, level0_size, overlap,  # noqa: E402
                        pixel_rectangle, render_svg)

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "port/assets/hints"
LIST = ASSETS / "hints.json"
FONTS = ROOT / "port/assets/fonts"
SCALE = 8

HUD = "ui\\hud\\bitmaps\\"
LARGE = HUD + "combined\\hud_msg_icons"
SMALL = HUD + "hud_msg_icons_sm"
WARNINGS = HUD + "combined\\hud_reticle_warnings"

# each icon: its sprites' sequence, a word (or none) and a picture (a redraw,
# or none). A picture's "press" is a thumbstick's click (its frames: the cap
# pushed down, and the arrow above the right stick's). The icons of the
# buttons (A, B, X, Y), the weapons and the grenades are not here.
WORD_FONT = "Overpass-900"
ICONS = {
    # (the A, B, X and Y buttons: stencils of tools/button_assets.py's
    # redraws, as on the small sheet)
    0: {"button": "a_butn.svg"},
    1: {"button": "b_butn.svg"},
    2: {"button": "x_butn.svg"},
    3: {"button": "y_butn.svg"},
    13: {"picture": "button_round.svg"},
    15: {"word": "LEFT", "picture": "stick_left.svg"},
    16: {"word": "RIGHT", "picture": "stick_right.svg"},
    17: {"word": "BACK", "picture": "oval.svg"},
    18: {"word": "START", "picture": "oval.svg"},
    19: {"word": "LEFT", "picture": "stick_left.svg", "press": True},
    20: {"word": "RIGHT", "picture": "stick_right.svg", "press": True},
    21: {"picture": "trigger_left.svg"},
    22: {"picture": "trigger_right.svg"},
    23: {"picture": "flashlight.svg"},
}
# the small sheet's: the same icons, smaller, each word beside its picture
# rather than above it (its Black/White button also hollow, sequence 14),
# and its triggers with their word and without their letter
SMALL_ICONS = {
    13: {"picture": "button_round.svg"},
    14: {"picture": "button_ring.svg"},
    15: {"word": "LEFT", "picture": "stick_left.svg"},
    16: {"word": "RIGHT", "picture": "stick_right.svg"},
    19: {"word": "LEFT", "picture": "stick_left.svg", "press": True},
    20: {"word": "RIGHT", "picture": "stick_right.svg", "press": True},
    21: {"word": "LEFT", "picture": "trigger_left.svg", "no_letter": True},
    22: {"word": "RIGHT", "picture": "trigger_right.svg", "no_letter": True},
}
# the warnings, by sequence: two lines of words, set in Overpass's Bold
# (its weight is the map's), and the sheet's arrows and triangle
WARNING_FONT = "Overpass-750"
WARNING_WORDS = {
    0: ("Low", "Battery"),
    1: ("Reload",),
    2: ("No", "Ammo"),
    3: ("Low", "Fuel"),
    4: ("No", "Fuel"),
    5: ("No", "Grenades"),
    6: ("No", "Weapon"),
    7: ("Low", "Ammo"),
    10: ("Battery", "Depleted"),
}
WARNING_SHAPES = (8, 9)

# a picture is found in the map's cell below the word: the rows from the
# first one, after the word's, that has any of the picture's texels
WORD_GAP = 1


# ---------- drawing


def document(width: float, height: float, body: str) -> str:
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:g}" height="{height:g}" '
            f'viewBox="0 0 {width:g} {height:g}">\n{body}\n</svg>\n')


def render(text: str, scale: float) -> np.ndarray:
    """An SVG document at scale times its size, as RGBA."""
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "drawing.svg"
        path.write_text(text)
        return render_svg(path, scale)


def reduced(text: str, width: int, height: int, scale: int = 4) -> np.ndarray:
    """A document's alpha (0-1) at texels of its size, drawn at scale."""
    image = render(text, scale)[..., 3].astype(float) / 255
    full = np.zeros((height * scale, width * scale))
    rows, columns = min(image.shape[0], height * scale), min(image.shape[1], width * scale)
    full[:rows, :columns] = image[:rows, :columns]
    return full.reshape(height, scale, width, scale).mean(axis=(1, 3))


def soften(image: np.ndarray, sigma: float = 0.6) -> np.ndarray:
    from scipy import ndimage

    return ndimage.gaussian_filter(image, sigma)


def number(value: float) -> str:
    text = f"{value:.4f}".rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


_faces = {}


def glyph_outlines(font: str, text: str) -> tuple:
    """Each letter's outline (an SVG path, in font units, y up) and advance,
    and the font's capital height."""
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.ttLib import TTFont

    if font not in _faces:
        _faces[font] = TTFont(str(FONTS / f"{font}.ttf"))
    face = _faces[font]
    glyphs = face.getGlyphSet()
    cmap = face.getBestCmap()
    letters = []
    for character in text:
        name = cmap[ord(character)]
        pen = SVGPathPen(glyphs)
        glyphs[name].draw(pen)
        letters.append((pen.getCommands(), face["hmtx"][name][0]))
    return letters, face["OS/2"].sCapHeight


def word_body(font: str, text: str, place: list) -> str:
    """text in white, its baseline's left at place[0:2] (texels), capitals
    place[2] texels high, letters widened by place[3] and spaced place[4]
    texels apart."""
    x, y, capitals, widen, spacing = place
    letters, capital_height = glyph_outlines(font, text)
    size = capitals / capital_height
    paths, pen = [], 0.0
    for outline, advance in letters:
        if outline:
            paths.append(f'<path transform="matrix({number(size * widen)} 0 0 {number(-size)} '
                         f'{number(x + pen)} {number(y)})" d="{outline}"/>')
        pen += advance * size * widen + spacing
    return '<g fill="#ffffff">' + "".join(paths) + "</g>"


ElementTree.register_namespace("", "http://www.w3.org/2000/svg")
SVG = "{http://www.w3.org/2000/svg}"


def redraw(svg: str, press: float = 0.0, arrow: float = 0.0, no_letter: bool = False) -> tuple:
    """A redraw's elements and its size: a thumbstick pressed by press texels
    (its cap and the cap's silhouette moved down), with the arrow above a
    right stick's click arrow (0: none, up to 1: whole); a trigger without
    its letter."""
    root = ElementTree.parse(ASSETS / "svg" / svg).getroot()
    width, height = float(root.get("width")), float(root.get("height"))
    for element in root.iter():
        if element.get("id") in ("cap", "cap-silhouette") and press:
            element.set("transform", f"translate(0 {number(press)}) {element.get('transform', '')}".strip())
        if element.get("id") == "face" and no_letter:
            del element.attrib["mask"]
    body = "".join(ElementTree.tostring(child, encoding="unicode") for child in root
                   if not child.tag.endswith("}title"))
    body = re.sub(r"<!--.*?-->", "", body, flags=re.S)
    if arrow:
        arrow_root = ElementTree.parse(ASSETS / "svg" / "press_arrow.svg").getroot()
        arrow_body = "".join(ElementTree.tostring(child, encoding="unicode") for child in arrow_root)
        # (above the cap, its point at the dish; drawn small, then whole)
        scale = 0.55 + 0.45 * arrow
        body += (f'<g transform="translate({number(width / 2 - 3 * scale)} {number(-3.4 * scale + press - 1.6)}) '
                 f'scale({number(scale)})">{arrow_body}</g>')
    return body, width, height


def placed(body: str, place: list) -> str:
    scale, x, y = place
    return f'<g transform="translate({number(x)} {number(y)}) scale({number(scale)})">{body}</g>'


# ---------- fitting


def fit(error, start: list, bounds: list = None, evaluations: int = 1500) -> list:
    from scipy import optimize

    result = optimize.minimize(error, start, method="Powell", bounds=bounds,
                               options={"maxfev": evaluations, "xtol": 1e-3, "ftol": 1e-8})
    return [round(float(value), 4) for value in result.x]


def shape(alpha: np.ndarray) -> tuple:
    """The area and centre of a shape's alpha (0-1)."""
    rows, columns = np.mgrid[0:alpha.shape[0], 0:alpha.shape[1]]
    area = alpha.sum()
    return area, ((columns + 0.5) * alpha).sum() / area, ((rows + 0.5) * alpha).sum() / area


def word_rows(alpha: np.ndarray) -> tuple:
    """The rows of a cell's word (the first run of rows with texels), or None."""
    covered = (alpha > 0.25).any(axis=1)
    rows = np.nonzero(covered)[0]
    if not len(rows):
        return None
    top = rows[0]
    bottom = top
    while bottom + 1 < len(covered) and covered[bottom + 1]:
        bottom += 1
    return top, bottom + 1


def fit_word(font: str, text: str, alpha: np.ndarray) -> list:
    """The place of a word set in font that matches alpha (texels, 0-1)."""
    height, width = alpha.shape
    columns = np.nonzero((alpha > 0.5).any(axis=0))[0]
    # (the capitals: from the word's top to its baseline, the last row with
    # much of the word, above its descenders)
    weight = alpha.sum(axis=1)
    top = np.nonzero((alpha > 0.5).any(axis=1))[0].min()
    baseline = np.nonzero(weight > weight.max() * 0.3)[0].max()
    rows = np.array([top, baseline])
    capitals = baseline - top + 1.0

    def error_at(sigma):
        target = soften(alpha, sigma)

        def error(place):
            drawn = reduced(document(width, height, word_body(font, text, list(place))), width, height)
            return float(((soften(drawn, sigma) - target) ** 2).sum())
        return error

    # (from a few sizes, roughly, then the best of them closely)
    coarse = error_at(1.2)
    starts = [fit(coarse, [columns.min() - 0.3, rows.max() + 1.0, capitals * factor, 1.0, 0.0], evaluations=500)
              for factor in (0.9, 1.0, 1.1)]
    best = min(starts, key=coarse)
    return fit(error_at(0.5), best, evaluations=2000)


def fit_picture(svg: str, alpha: np.ndarray, press: float = 0.0, no_letter: bool = False) -> list:
    """The scale and place of a redraw that matches alpha (texels, 0-1):
    first by area and centre, then by least squares."""
    height, width = alpha.shape
    body, svg_width, svg_height = redraw(svg, press, 0.0, no_letter)
    own = reduced(document(svg_width, svg_height, body), int(np.ceil(svg_width)), int(np.ceil(svg_height)), 8)
    own_area, own_x, own_y = shape(own)
    area, x, y = shape(alpha)
    scale = float(np.sqrt(area / own_area))
    start = [scale, x - scale * own_x, y - scale * own_y]
    target = soften(alpha)

    def error(place):
        drawn = reduced(document(width, height, placed(body, list(place))), width, height)
        return float(((soften(drawn) - target) ** 2).sum())

    return fit(error, start, [(scale * 0.8, scale * 1.25), (start[1] - 3, start[1] + 3),
                              (start[2] - 3, start[2] + 3)])


def fit_press(svg: str, picture: list, alpha: np.ndarray, arrow: float) -> float:
    """How far a stick's cap is pushed down on a click's frame."""
    height, width = alpha.shape
    target = soften(alpha)

    def error(values):
        body, _, _ = redraw(svg, float(values[0]), arrow)
        drawn = reduced(document(width, height, placed(body, picture)), width, height)
        return float(((soften(drawn) - target) ** 2).sum())

    return fit(error, [1.0], [(0.0, 4.0)], 200)[0]


def likeness(a: np.ndarray, b: np.ndarray) -> float:
    a, b = a - a.mean(), b - b.mean()
    norm = np.sqrt((a * a).sum() * (b * b).sum())
    return round(float((a * b).sum() / norm), 4) if norm else 0.0


# ---------- layout


def sheet_sprites(group: dict, sequence: int) -> list:
    """A sequence's sprites: (bitmap, cell in texels)."""
    sprites = []
    for sprite in group["sequences"][sequence]:
        bitmap = group["bitmaps"][sprite[0]]
        sprites.append((sprite[0], pixel_rectangle(sprite, bitmap["width"], bitmap["height"])))
    return sprites


def split_word(alpha: np.ndarray, beside: bool) -> tuple:
    """A cell's alpha as its word's and its picture's: the word above the
    picture (its first run of rows), or beside it, left of the picture (the
    largest shape)."""
    word_alpha, picture_alpha = np.zeros_like(alpha), alpha.copy()
    if beside:
        from scipy import ndimage

        labels, count = ndimage.label(alpha > 0.05, structure=np.ones((3, 3), bool))
        sizes = ndimage.sum(alpha, labels, range(1, count + 1))
        columns = np.nonzero((labels == int(np.argmax(sizes)) + 1).any(axis=0))[0]
        split = columns.min()
        word_alpha[:, :split] = alpha[:, :split]
        picture_alpha[:, :split] = 0
    else:
        rows = word_rows(alpha)
        word_alpha[rows[0]:rows[1]] = alpha[rows[0]:rows[1]]
        picture_alpha[:rows[1] + WORD_GAP] = 0
    return word_alpha, picture_alpha


def icon_layout(group: dict, sequence: int, icon: dict, alphas: list, beside: bool) -> list:
    """Each of an icon's sprites, with its word and picture fitted."""
    sprites = []
    pressed = None
    for frame, (bitmap, cell) in enumerate(sheet_sprites(group, sequence)):
        left, top, right, bottom = cell
        alpha = alphas[bitmap][top:bottom, left:right]
        entry = {"sequence": sequence, "frame": frame, "bitmap": bitmap, "cell": cell}
        if "button" in icon:
            import button_assets

            texels = np.round(alpha * 255)
            place = button_assets.stencil_place(icon["button"], texels)
            entry["button"] = {"svg": icon["button"], "place": place}
            entry["button"]["letter"] = button_assets.sprite_letter(
                {"cell": cell, "svg": icon["button"], "place": place}, texels)
            drawn = reduced(document(right - left, bottom - top, icon_body(entry)), right - left, bottom - top)
            entry["likeness"] = likeness(soften(drawn), soften(alpha))
            sprites.append(entry)
            print(f"  sequence {sequence} frame {frame}: cell {cell} of bitmap {bitmap}, likeness {entry['likeness']}")
            continue
        picture_alpha = alpha
        if "word" in icon:
            word_alpha, picture_alpha = split_word(alpha, beside)
            entry["word"] = {"text": icon["word"], "font": WORD_FONT, "place": fit_word(WORD_FONT, icon["word"],
                                                                                         word_alpha)}
        arrow = 0.0
        # (the large sheet's right stick's click has an arrow above it)
        if icon.get("press") and icon["word"] == "RIGHT" and frame and not beside:
            arrow = 1.0 if frame == 2 else 0.4
        press = 0.0
        if icon.get("press") and frame:
            # (the click's frames: the stick's place of its first frame, the
            # cap pushed down; the arrow's texels are left to the arrow)
            press = fit_press(icon["picture"], pressed, picture_alpha, arrow)
            place = pressed
        else:
            place = fit_picture(icon["picture"], picture_alpha, 0.0, icon.get("no_letter", False))
            pressed = place
        entry["picture"] = {"svg": icon["picture"], "place": place}
        if press:
            entry["picture"]["press"] = press
        if arrow:
            entry["picture"]["arrow"] = arrow
        if icon.get("no_letter"):
            entry["picture"]["no_letter"] = True
        drawn = reduced(document(right - left, bottom - top, icon_body(entry)), right - left, bottom - top)
        entry["likeness"] = likeness(soften(drawn), soften(alpha))
        sprites.append(entry)
        print(f"  sequence {sequence} frame {frame}: cell {cell} of bitmap {bitmap}, likeness {entry['likeness']}")
    return sprites


def icon_body(sprite: dict) -> str:
    """A sprite's drawing in texels of its cell."""
    parts = []
    if "button" in sprite:
        import button_assets

        left, top, right, bottom = sprite["cell"]
        button = sprite["button"]
        parts.append(button_assets.stencil(button["svg"], [0, 0, right - left, bottom - top], button["place"], 0,
                                           button["letter"]))
    if "word" in sprite:
        parts.append(word_body(sprite["word"]["font"], sprite["word"]["text"], sprite["word"]["place"]))
    if "picture" in sprite:
        picture = sprite["picture"]
        body, _, _ = redraw(picture["svg"], picture.get("press", 0.0), picture.get("arrow", 0.0),
                            picture.get("no_letter", False))
        parts.append(placed(body, picture["place"]))
    if "lines" in sprite:
        parts.extend(word_body(line["font"], line["text"], line["place"]) for line in sprite["lines"])
    if "shape" in sprite:
        parts.append(sprite["shape"])
    return "\n".join(parts)


def sheet_entries(xbox_map: XboxMap, tag: str, icons: dict, beside: bool) -> list:
    """The textures of a message icons' sheet: one for each bitmap that has
    any of the icons' sprites."""
    group = xbox_map.bitmap_group(tag)
    alphas = [decode_bitmap(bitmap)[..., 3].astype(float) / 255 for bitmap in group["bitmaps"]]
    sprites = []
    for sequence, icon in icons.items():
        print(f"{tag} sequence {sequence}: {icon.get('word', '')} {icon.get('picture', icon.get('button'))}")
        sprites += icon_layout(group, sequence, icon, alphas, beside)
    entries = []
    for index, bitmap in enumerate(group["bitmaps"]):
        own = [sprite for sprite in sprites if sprite["bitmap"] == index]
        if not own:
            continue
        entries.append({
            "name": tag.split("\\")[-1] + f"__{index}",
            "tag": tag,
            "bitmap": index,
            "width": bitmap["width"],
            "height": bitmap["height"],
            "format": FORMATS[bitmap["format"]],
            "scale": SCALE,
            # (the map's, which the game checks before drawing this in its
            # place: other languages' maps have their own words)
            "crc": zlib.crc32(bitmap["pixels"][:level0_size(bitmap)]),
            "sprites": own,
        })
    return entries


def warning_entry(xbox_map: XboxMap) -> dict:
    """The warnings' sheet, drawn whole: each warning's two lines of words
    fitted to the map's; the arrows and the triangle as the map draws them,
    in solid shapes."""
    group = xbox_map.bitmap_group(WARNINGS)
    bitmap = group["bitmaps"][0]
    pixels = decode_bitmap(bitmap)
    alpha = pixels[..., 3].astype(float) / 255
    sprites = []
    for sequence, lines in WARNING_WORDS.items():
        (_, cell), = sheet_sprites(group, sequence)
        left, top, right, bottom = cell
        cell_alpha = alpha[top:bottom, left:right]
        # (the words' lines: split at the faintest row between them, as one
        # line's descenders and the next one's ascenders can touch)
        weight = cell_alpha.sum(axis=1)
        rows = np.nonzero(weight > 0.5)[0]
        bounds = [0, len(weight)]
        if len(lines) == 2:
            middle = range(rows.min() + 5, rows.max() - 4)
            bounds = [0, min(middle, key=lambda row: weight[row]), len(weight)]
        fitted = []
        for index, text in enumerate(lines):
            line_alpha = np.zeros_like(cell_alpha)
            line_alpha[bounds[index]:bounds[index + 1]] = cell_alpha[bounds[index]:bounds[index + 1]]
            fitted.append({"text": text, "font": WARNING_FONT, "place": fit_word(WARNING_FONT, text, line_alpha)})
        sprite = {"sequence": sequence, "frame": 0, "bitmap": 0, "cell": cell, "lines": fitted}
        drawn = reduced(document(right - left, bottom - top, icon_body(sprite)), right - left, bottom - top)
        sprite["likeness"] = likeness(soften(drawn), soften(cell_alpha))
        print(f"{WARNINGS} sequence {sequence}: {' '.join(lines)}, likeness {sprite['likeness']}")
        sprites.append(sprite)
    grey = pixels[..., 0].astype(float) / 255
    for sequence in WARNING_SHAPES:
        for frame, (_, cell) in enumerate(sheet_sprites(group, sequence)):
            left, top, right, bottom = cell
            sprite = {"sequence": sequence, "frame": frame, "bitmap": 0, "cell": cell,
                      "shape": pixel_shape(alpha[top:bottom, left:right])}
            # (the arrows' cells' grey is not their alpha: a row of dots)
            cell_grey = grey[top:bottom, left:right]
            if np.abs(cell_grey - alpha[top:bottom, left:right]).max() > 0.1:
                sprite["grey"] = dots(cell_grey)
            sprites.append(sprite)
    return {
        "name": WARNINGS.split("\\")[-1] + "__0",
        "tag": WARNINGS,
        "bitmap": 0,
        "width": bitmap["width"],
        "height": bitmap["height"],
        "format": FORMATS[bitmap["format"]],
        "scale": SCALE,
        "crc": zlib.crc32(bitmap["pixels"][:level0_size(bitmap)]),
        "whole": True,
        "sprites": sprites,
    }


def pixel_shape(alpha: np.ndarray) -> str:
    """The arrows and the triangle: each a run of rows of 1, 3, 5... texels
    on the map, so a triangle with its point at the top texel's top centre
    and its base along the bottom texels' bottom (pointing up, drawn crisp)."""
    from scipy import ndimage

    labels, count = ndimage.label(alpha > 0.5)
    paths = []
    for index in range(1, count + 1):
        rows, columns = np.nonzero(labels == index)
        top, bottom = rows.min(), rows.max() + 1
        apex = (columns[rows == top].min() + columns[rows == top].max() + 1) / 2
        left, right = columns[rows == bottom - 1].min(), columns[rows == bottom - 1].max() + 1
        paths.append(f"M{number(apex)},{top} L{right},{bottom} L{left},{bottom} Z")
    return f'<path d="{" ".join(paths)}" fill="#ffffff"/>'


def dots(grey: np.ndarray) -> str:
    """A cell's grey dots as circles: each one's centre and area the map's."""
    from scipy import ndimage

    labels, count = ndimage.label(grey > 0.05)
    circles = []
    for index in range(1, count + 1):
        weight = grey * (labels == index)
        area, x, y = shape(weight)
        circles.append(f'<circle cx="{number(x)}" cy="{number(y)}" r="{number(np.sqrt(area / np.pi))}"/>')
    return '<g fill="#ffffff">' + "".join(circles) + "</g>"


def layout(arguments) -> None:
    xbox_map = XboxMap(Path(arguments.map))
    entries = []
    entries += sheet_entries(xbox_map, LARGE, ICONS, False)
    entries += sheet_entries(xbox_map, SMALL, SMALL_ICONS, True)
    entries.append(warning_entry(xbox_map))
    LIST.write_text(json.dumps({"assets": entries}, indent=1) + "\n")
    print(f"{LIST.relative_to(ROOT)}: {len(entries)} textures")


# ---------- building


def build_asset(entry: dict) -> np.ndarray:
    """A texture of 8x the bitmap: its icons' drawings, each in its cell,
    white with the drawing's coverage as alpha (the sheets are a8y8: the
    grey is the alpha, which the HUD blends premultiplied)."""
    width, height = entry["width"], entry["height"]
    # (each sprite drawn on its own, kept to its cell: the redraws' ids
    # repeat between sprites)
    image = np.zeros((height * entry["scale"], width * entry["scale"], 4), np.uint8)
    for sprite in entry["sprites"]:
        left, top, right, bottom = sprite["cell"]
        scale = entry["scale"]
        drawn = render(document(right - left, bottom - top, icon_body(sprite)), scale)
        rows, columns = min(drawn.shape[0], (bottom - top) * scale), min(drawn.shape[1], (right - left) * scale)
        cell = image[top * scale:top * scale + rows, left * scale:left * scale + columns]
        cell[..., 3] = np.maximum(cell[..., 3], drawn[:rows, :columns, 3])
        # (the grey: the drawing's coverage, or the cell's own grey drawing)
        grey = drawn[:rows, :columns, 3]
        if "grey" in sprite:
            grey = render(document(right - left, bottom - top, sprite["grey"]), scale)[:rows, :columns, 3]
        cell[..., :3] = np.maximum(cell[..., :3], grey[..., None])
    return image


def build(arguments) -> None:
    description = json.loads(LIST.read_text())
    names = {f"{entry['name']}.png" for entry in description["assets"]}
    for stale in ASSETS.glob("*.png"):
        if stale.name not in names:
            stale.unlink()
    for entry in description["assets"]:
        image = build_asset(entry)
        Image.fromarray(image).save(ASSETS / f"{entry['name']}.png", optimize=True)
        print(f"{entry['name']}.png: {image.shape[1]}x{image.shape[0]}")


# ---------- checking


def check(arguments) -> None:
    xbox_map = XboxMap(Path(arguments.map))
    output = Path(arguments.out)
    output.mkdir(parents=True, exist_ok=True)
    for entry in json.loads(LIST.read_text())["assets"]:
        if ("bitm", entry["tag"]) not in xbox_map.tags:
            print(f"{entry['name']}: not in this map")
            continue
        bitmap = xbox_map.bitmap_group(entry["tag"])["bitmaps"][entry["bitmap"]]
        if zlib.crc32(bitmap["pixels"][:level0_size(bitmap)]) != entry["crc"]:
            print(f"{entry['name']}: this map's bitmap is not the one laid out")
        xbox = decode_bitmap(bitmap)
        image = Image.open(ASSETS / f"{entry['name']}.png").convert("RGBA")
        small = np.asarray(image.resize((entry["width"], entry["height"]), Image.BOX))
        scale = entry["scale"]
        for sprite in entry["sprites"]:
            left, top, right, bottom = sprite["cell"]
            score = overlap(small[top:bottom, left:right, 3].astype(float), xbox[top:bottom, left:right, 3].astype(float))
            name = f"{entry['name']}__sequence{sprite['sequence']}_{sprite['frame']}"
            print(f"{name}: alpha overlap {score:.2f}")
            ours = image.crop((left * scale, top * scale, right * scale, bottom * scale))
            panels = [Image.fromarray(np.ascontiguousarray(picture[top:bottom, left:right])).resize(
                ours.size, Image.NEAREST) for picture in (xbox, small)]
            panels.append(ours)
            width, height = ours.size
            sheet = Image.new("RGB", (width * 3 + 16, height), (255, 0, 0))
            for place, picture in enumerate(panels):
                background = Image.new("RGBA", picture.size, (16, 26, 60, 255))
                sheet.paste(Image.alpha_composite(background, picture).convert("RGB"), (place * (width + 8), 0))
            sheet.save(output / f"{name}.png")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("layout")
    command.add_argument("--map", required=True)
    commands.add_parser("build")
    command = commands.add_parser("check")
    command.add_argument("--map", required=True)
    command.add_argument("--out", required=True)
    arguments = parser.parse_args()
    {"layout": layout, "build": build, "check": check}[arguments.command](arguments)


if __name__ == "__main__":
    sys.exit(main())
