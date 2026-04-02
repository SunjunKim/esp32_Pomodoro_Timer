from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw


def draw_tomato_icon(size: int = 1024) -> Image.Image:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    cx, cy = size // 2, size // 2

    # Tomato body
    red = (229, 57, 53, 255)      # #E53935
    red3 = (198, 40, 40, 255)     # darker edge

    r = int(size * 0.36)
    body_center = (cx, int(size * 0.55))

    # Slightly flattened circle for a tomato feel (scale y by drawing multiple circles)
    for i in range(-10, 11):
        rr = r + (abs(i) // 3)
        y = body_center[1] + int(i * (size * 0.003))
        d.ellipse((body_center[0] - rr, y - rr, body_center[0] + rr, y + rr), fill=red)

    # Subtle darker lower edge
    d.ellipse(
        (
            body_center[0] - int(r * 0.95),
            body_center[1] + int(r * 0.35),
            body_center[0] + int(r * 0.95),
            body_center[1] + int(r * 0.35) + int(r * 0.75),
        ),
        fill=red3,
    )
    d.ellipse(
        (
            body_center[0] - int(r * 0.95),
            body_center[1] + int(r * 0.30),
            body_center[0] + int(r * 0.95),
            body_center[1] + int(r * 0.30) + int(r * 0.70),
        ),
        fill=red,
    )

    # Highlight
    hl = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    hd.ellipse(
        (
            body_center[0] - int(r * 0.55),
            body_center[1] - int(r * 0.55),
            body_center[0] - int(r * 0.55) + int(r * 0.70),
            body_center[1] - int(r * 0.55) + int(r * 1.05),
        ),
        fill=(255, 255, 255, 70),
    )
    img.alpha_composite(hl)

    # Calyx / leaves
    green = (46, 125, 50, 255)    # #2E7D32
    green2 = (27, 94, 32, 255)

    top_y = int(size * 0.25)
    calyx_center = (cx, int(size * 0.33))

    # Stem
    stem = (
        cx - int(size * 0.02),
        top_y,
        cx - int(size * 0.02) + int(size * 0.04),
        top_y + int(size * 0.14),
    )
    d.rounded_rectangle(stem, radius=int(size * 0.02), fill=green2)

    # 5-point leaf star
    leaf_r_outer = int(size * 0.16)
    leaf_r_inner = int(size * 0.07)
    pts: list[tuple[int, int]] = []
    for k in range(10):
        ang = (-90 + k * 36) * math.pi / 180.0
        rr = leaf_r_outer if (k % 2 == 0) else leaf_r_inner
        x = calyx_center[0] + int(rr * math.cos(ang))
        y = calyx_center[1] + int(rr * math.sin(ang))
        pts.append((x, y))
    d.polygon(pts, fill=green)
    d.line(pts + [pts[0]], fill=green2, width=max(2, size // 256))

    return img


def main():
    out = Path(__file__).resolve().parents[1] / "assets" / "tomato.png"
    out.parent.mkdir(parents=True, exist_ok=True)

    surf = draw_tomato_icon(1024)
    surf.save(out, format="PNG")
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()

