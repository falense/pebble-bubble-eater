"""Generate BubbleEater appstore assets: 144x144 + 48x48 icons and 720x320 banner."""
from PIL import Image, ImageDraw, ImageFont

OUT = "/home/sondre/Repositories/BubbleEater/store-assets"

BG = (10, 16, 28)
GRID = (32, 44, 64)
CYAN = (85, 255, 255)
CYAN_EDGE = (255, 255, 255)
GREEN = (0, 170, 85)
RED = (255, 60, 60)
YELLOW = (255, 220, 100)
WHITE = (255, 255, 255)


def grid(draw, w, h, step):
    for x in range(0, w + 1, step):
        draw.line([(x, 0), (x, h)], fill=GRID, width=1)
    for y in range(0, h + 1, step):
        draw.line([(0, y), (w, y)], fill=GRID, width=1)


def bubble(draw, cx, cy, r, fill, outline=CYAN_EDGE, width=2):
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill, outline=outline, width=width)
    # small specular highlight
    hr = max(2, r // 4)
    hx, hy = cx - r // 2, cy - r // 2
    draw.ellipse([hx - hr, hy - hr, hx + hr, hy + hr], fill=(255, 255, 255, 90))


def pellet(draw, cx, cy, r=3):
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=YELLOW)


def make_icon(size, path):
    s = size / 144.0
    img = Image.new("RGB", (size, size), BG)
    d = ImageDraw.Draw(img, "RGBA")
    grid(d, size, size, max(12, int(36 * s)))
    # player bubble eating toward a small green bubble
    bubble(d, int(58 * s), int(78 * s), int(40 * s), CYAN, width=max(2, int(4 * s)))
    bubble(d, int(112 * s), int(44 * s), int(16 * s), GREEN, width=max(1, int(3 * s)))
    pellet(d, int(120 * s), int(104 * s), max(2, int(6 * s)))
    pellet(d, int(96 * s), int(126 * s), max(2, int(5 * s)))
    if size >= 96:
        pellet(d, int(20 * s), int(22 * s), int(5 * s))
    img.save(path)


def load_font(px):
    for p in [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    ]:
        try:
            return ImageFont.truetype(p, px)
        except OSError:
            continue
    return ImageFont.load_default()


def make_banner(path):
    w, h = 720, 320
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img, "RGBA")
    grid(d, w, h, 60)
    # scene: red lurker top-left, player center-right chasing a green bubble
    bubble(d, 80, 60, 70, RED, width=4)
    bubble(d, 634, 252, 62, CYAN, width=5)
    bubble(d, 676, 118, 28, GREEN, width=3)
    for cx, cy in [(420, 90), (480, 280), (640, 280), (360, 220), (300, 60), (680, 40)]:
        pellet(d, cx, cy, 6)
    title_font = load_font(64)
    sub_font = load_font(26)
    d.text((48, 130), "BubbleEater", font=title_font, fill=WHITE)
    d.text((52, 210), "Tilt. Eat. Grow.", font=sub_font, fill=CYAN)
    d.text((52, 248), "Eat smaller bubbles, dodge bigger\nones — steered by your wrist.", font=sub_font, fill=(170, 185, 205))
    img.save(path)


make_icon(144, f"{OUT}/icon-large-144.png")
make_icon(48, f"{OUT}/icon-small-48.png")
make_banner(f"{OUT}/banner-720x320.png")
print("done")
