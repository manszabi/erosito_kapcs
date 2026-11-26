"""Ikon rajzolása: bekapcsoló szimbólum sötét alapon.
Kimenet: icon_png.h (180 px, a firmware weboldalához) és ios/Erosito/Assets.xcassets/AppIcon.appiconset/icon-1024.png
Futtatás a repó gyökeréből: python3 tools/make_icon.py
"""
import io
import math
from PIL import Image, ImageDraw

BG = (38, 50, 56)        # sötét palaszürke
FG = (255, 255, 255)
ACCENT = (76, 175, 80)   # zöld


def draw(size):
    s = size * 4  # túlmintavételezés az élsimításhoz
    img = Image.new("RGB", (s, s), BG)
    d = ImageDraw.Draw(img)
    c = s / 2
    r = s * 0.30
    w = int(s * 0.075)
    # zöld gyűrű a háttérben
    rr = s * 0.40
    d.ellipse([c - rr, c - rr, c + rr, c + rr], outline=ACCENT, width=int(s * 0.025))
    # bekapcsoló ív (felül nyitott)
    box = [c - r, c - r + s * 0.03, c + r, c + r + s * 0.03]
    d.arc(box, start=-58, end=238, fill=FG, width=w)
    # függőleges vonal
    d.rounded_rectangle([c - w / 2, c - r - s * 0.02, c + w / 2, c + s * 0.02], radius=w / 2, fill=FG)
    return img.resize((size, size), Image.LANCZOS)


def png_bytes(img, palette=True):
    if palette:
        img = img.convert("P", palette=Image.ADAPTIVE, colors=32)
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def main():
    data = png_bytes(draw(180))
    lines = []
    for i in range(0, len(data), 20):
        lines.append("  " + ", ".join(f"0x{b:02X}" for b in data[i:i + 20]) + ",")
    with open("icon_png.h", "w", encoding="utf-8") as f:
        f.write("// A weboldal ikonja (apple-touch-icon, 180 px). Generálja: tools/make_icon.py\n")
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write(f"const uint8_t ICON_PNG[{len(data)}] = {{\n" + "\n".join(lines) + "\n};\n")
    import os
    out = "ios/Erosito/Assets.xcassets/AppIcon.appiconset"
    os.makedirs(out, exist_ok=True)
    draw(1024).save(f"{out}/icon-1024.png", "PNG", optimize=True)   # RGB, alfa nélkül (App Store szabály)
    print(f"icon_png.h: {len(data)} bajt; {out}/icon-1024.png")


if __name__ == "__main__":
    main()
