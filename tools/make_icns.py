from __future__ import annotations

from pathlib import Path

from PIL import Image


def main():
    root = Path(__file__).resolve().parents[1]
    src = root / "assets" / "tomato.png"
    out = root / "assets" / "tomato.icns"
    out.parent.mkdir(parents=True, exist_ok=True)

    img = Image.open(src).convert("RGBA")

    # Pillow will embed multiple sizes when given `sizes`.
    # Include standard macOS icon sizes up to 1024.
    img.save(
        out,
        format="ICNS",
        sizes=[
            (16, 16),
            (32, 32),
            (64, 64),
            (128, 128),
            (256, 256),
            (512, 512),
            (1024, 1024),
        ],
    )
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()

