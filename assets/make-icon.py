#!/usr/bin/env python3
import sys
from PIL import Image, ImageDraw

SIZE = 512
SCALE = 4

BACKGROUND = "#18181c"
YELLOW = "#ffe680"
CYAN = "#90fcff"
GREEN = "#a0ff9a"
PINK = "#ffc4ff"
LIGHT = "#f0f0f0"


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "Usage: python chiripy_icon.py ICON_512.png ICON_256.png"
        )

    image = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    def box(bounds):
        return tuple(round(value * SCALE) for value in bounds)

    def rounded(bounds, radius, fill, outline=None, width=1):
        draw.rounded_rectangle(
            box(bounds),
            radius=round(radius * SCALE),
            fill=fill,
            outline=outline,
            width=round(width * SCALE),
        )

    # Transparent outer corners; exact 96-pixel background radius.
    rounded((0, 0, 512, 512), 96, BACKGROUND)

    # Quiet inset surface gives the translucent chat panel depth.
    rounded((64, 72, 448, 440), 66, "#202028")
    rounded((112, 112, 448, 408), 40, "#101014")

    # Composite a translucent panel over the inset surface.
    panel = Image.new("RGBA", image.size, (0, 0, 0, 0))
    panel_draw = ImageDraw.Draw(panel)
    panel_draw.rounded_rectangle(
        box((100, 100, 436, 396)),
        radius=36 * SCALE,
        fill=(24, 24, 28, 224),
        outline=(112, 126, 138, 180),
        width=3 * SCALE,
    )
    image = Image.alpha_composite(image, panel)
    draw = ImageDraw.Draw(image)

    # Four fixed rows: short name-like bars plus longer message bars.
    rows = (
        (144, YELLOW, 374),
        (204, CYAN, 398),
        (264, GREEN, 354),
        (324, PINK, 384),
    )
    for y, color, end in rows:
        rounded((128, y, 164, y + 20), 7, color)
        rounded((180, y, end, y + 20), 7, color)
        rounded((180, y + 28, end - 38, y + 34), 3, LIGHT)

    # Two restrained speed streaks sit outside the chat panel.
    rounded((54, 215, 88, 227), 6, CYAN)
    rounded((38, 245, 88, 257), 6, YELLOW)

    icon = image.resize((512, 512), Image.Resampling.LANCZOS)
    small = icon.resize((256, 256), Image.Resampling.LANCZOS)
    icon.save(sys.argv[1], format="PNG")
    small.save(sys.argv[2], format="PNG")


if __name__ == "__main__":
    main()
