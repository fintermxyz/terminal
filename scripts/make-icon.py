#!/usr/bin/env python3
# usage: python3 scripts/make-icon.py assets/icon.png   (needs Pillow)
# Renders the app icon: a lone backtick on the terminal's dark background.
import sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

out = sys.argv[1]
S = 1024 * 2                       # supersample, downscale at the end
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

# macOS icon grid: 824px body in a 1024 canvas, ~185px corner radius
m, r = 100 * 2, 185 * 2
box = (m, m, S - m, S - m)

# soft drop shadow
sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(sh).rounded_rectangle((m, m + 24, S - m, S - m + 24), r, fill=(0, 0, 0, 110))
img = Image.alpha_composite(img, sh.filter(ImageFilter.GaussianBlur(36)))

# body: vertical gradient #24283b -> #16161e, clipped to the squircle
grad = Image.new("RGBA", (S, S))
top, bot = (0x24, 0x28, 0x3b), (0x16, 0x16, 0x1e)
gd = ImageDraw.Draw(grad)
for y in range(S):
    t = y / (S - 1)
    gd.line([(0, y), (S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bot)) + (255,))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle(box, r, fill=255)
img.paste(grad, (0, 0), mask)

# hairline rim
ImageDraw.Draw(img).rounded_rectangle(box, r, outline=(255, 255, 255, 28), width=4)

# the backtick: a tapered stroke, heavy at the top, swept down to the right
layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ld = ImageDraw.Draw(layer)
ax, ay, ra = 0.405 * S, 0.345 * S, 0.068 * S   # head
bx, by, rb = 0.595 * S, 0.625 * S, 0.030 * S   # tail
N = 600
for i in range(N + 1):
    t = i / N
    x, y, rr = ax + (bx - ax) * t, ay + (by - ay) * t, ra + (rb - ra) * t
    ld.ellipse((x - rr, y - rr, x + rr, y + rr), fill=(0xc0, 0xca, 0xf5, 255))

glow = layer.filter(ImageFilter.GaussianBlur(40))
glow.putalpha(glow.getchannel("A").point(lambda a: a * 0.45))
img = Image.alpha_composite(img, glow)
img = Image.alpha_composite(img, layer)

img.resize((1024, 1024), Image.LANCZOS).save(out)
