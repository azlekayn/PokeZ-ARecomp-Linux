"""ZA Recomp's icon: docs/icon/icon_1024.png (window icon at run time) and docs/icon/icon.ico (the .exe's).
An original drawing - a lit tower over a city skyline at night and "ZA" - in the style of LAS Recomp's icon (rounded
square, night gradient). Lettering: Baloo 2 SemiBold (OFL, third_party/fonts/Baloo2). Needs Pillow.
Usage: python tools/za/make_icon.py
"""
import os
import random

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
S = 4096  # drawn at 4x, downsampled (anti-aliasing)
M, R = 400, 720  # margin and corner radius of the rounded square (as in the old icon: 100 / 180 at 1024)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def main():
    w = h = S - 2 * M
    art = Image.new('RGBA', (w, h))
    px = art.load()
    top, mid, bot = (10, 14, 46), (40, 34, 92), (186, 92, 120)  # night sky to a warm city glow at the horizon
    for y in range(h):
        t = y / h
        c = lerp(top, mid, t / 0.6) if t < 0.6 else lerp(mid, bot, (t - 0.6) / 0.4)
        for x in range(w):
            px[x, y] = c + (255,)
    d = ImageDraw.Draw(art)
    rnd = random.Random(7)
    for _ in range(40):  # stars
        x, y, r = rnd.randrange(w), rnd.randrange(int(h * 0.45)), rnd.choice((6, 8, 10, 14))
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 250, 230, rnd.randrange(150, 255)))

    # the tower: a glow behind, then a tall tapering spire with a lit band
    cx = int(w * 0.5)
    glow = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - 900, int(h * 0.12), cx + 900, int(h * 0.95)), fill=(255, 196, 120, 120))
    art.alpha_composite(glow.filter(ImageFilter.GaussianBlur(260)))
    d = ImageDraw.Draw(art)
    base_y, tip_y = int(h * 0.80), int(h * 0.10)
    d.polygon([(cx - 230, base_y), (cx - 60, int(h * 0.30)), (cx, tip_y), (cx + 60, int(h * 0.30)), (cx + 230, base_y)],
              fill=(28, 24, 58, 255))
    d.polygon([(cx - 22, int(h * 0.16)), (cx, tip_y), (cx + 22, int(h * 0.16)), (cx + 40, int(h * 0.52)),
               (cx - 40, int(h * 0.52))], fill=(255, 214, 140, 255))  # its lit edge
    for i, y in enumerate((0.36, 0.48, 0.60)):  # lit rings
        yy = int(h * y)
        half = 70 + i * 50
        d.rounded_rectangle((cx - half, yy - 18, cx + half, yy + 18), 18, fill=(255, 226, 160, 255))

    # skyline in front, with lit windows
    rnd = random.Random(3)
    x = 0
    while x < w:
        bw = rnd.randrange(220, 420)
        if abs(x + bw / 2 - cx) < 330:  # (the tower's base stays visible)
            x += bw
            continue
        bh = rnd.randrange(int(h * 0.16), int(h * 0.34))
        y0 = h - bh - int(h * 0.12)
        d.rectangle((x, y0, x + bw - 30, h), fill=(20, 18, 44, 255))
        for wy in range(y0 + 60, h - int(h * 0.12), 110):
            for wx in range(x + 50, x + bw - 80, 90):
                if rnd.random() < 0.45:
                    d.rectangle((wx, wy, wx + 40, wy + 56), fill=(255, 210, 130, 220))
        x += bw
    d.rectangle((0, h - int(h * 0.12), w, h), fill=(16, 14, 36, 255))  # ground

    # "ZA": gold to orange, a soft shadow, in the lower band
    font = ImageFont.truetype(os.path.join(ROOT, 'third_party', 'fonts', 'Baloo2', 'Baloo2-SemiBold.ttf'), 1150)
    text = 'ZA'
    box = d.textbbox((0, 0), text, font=font)
    tw, th = box[2] - box[0], box[3] - box[1]
    tx, ty = (w - tw) // 2 - box[0], int(h * 0.97) - th - box[1]
    shadow = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((tx + 30, ty + 40), text, font=font, fill=(0, 0, 0, 200))
    art.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(40)))
    mask = Image.new('L', (w, h), 0)
    ImageDraw.Draw(mask).text((tx, ty), text, font=font, fill=255)
    grad = Image.new('RGBA', (w, h))
    gp = grad.load()
    for y in range(h):
        c = lerp((255, 236, 130), (255, 132, 44), max(0.0, min(1.0, (y - ty) / max(1, th))))
        for xx in range(w):
            gp[xx, y] = c + (255,)
    art.paste(grad, (0, 0), mask)

    # rounded square, on a transparent canvas
    shape = Image.new('L', (w, h), 0)
    ImageDraw.Draw(shape).rounded_rectangle((0, 0, w - 1, h - 1), R, fill=255)
    out = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    out.paste(art, (M, M), shape)
    out = out.resize((1024, 1024), Image.LANCZOS)
    icon_dir = os.path.join(ROOT, 'docs', 'icon')
    out.save(os.path.join(icon_dir, 'icon_1024.png'))
    out.save(os.path.join(icon_dir, 'icon.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128),
                                                       (256, 256)])
    print('docs/icon/icon_1024.png, docs/icon/icon.ico')


if __name__ == '__main__':
    main()
