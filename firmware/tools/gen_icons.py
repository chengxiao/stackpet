#!/usr/bin/env python3
"""Draw the StackPet launcher icons and write them as LVGL v9 binary images.

Output: main/assets/assets_bin/icon_{remote,freeze,spin}.bin, 188x150, color format RGB565A8 (cf=0x14:
an RGB565 plane followed by an A8 plane), same as the official launcher icons. The style follows the
official icons: chunky black strokes, white fills, pink/cyan accents, transparent background (the launcher
paints the app theme color behind it).

Usage (from the firmware directory):
  uv run --with pillow python tools/gen_icons.py [--preview DIR]
"""
import argparse
import math
import os
import struct

from PIL import Image, ImageDraw

W, H = 188, 150
SS = 4  # supersampling factor for anti-aliasing

BLACK = (0, 0, 0, 255)
WHITE = (255, 255, 255, 255)
LIGHT = (230, 230, 230, 255)
PINK = (255, 48, 156, 255)
CYAN = (0, 206, 255, 255)

STROKE = 10  # stroke width at the final size, matches the official icons


def s(v):
    """Scale a final-size coordinate/length to the supersampled canvas."""
    return int(round(v * SS))


def box(x0, y0, x1, y1):
    return [s(x0), s(y0), s(x1), s(y1)]


def circle(draw, cx, cy, r, fill=None, outline=None, width=0):
    draw.ellipse(box(cx - r, cy - r, cx + r, cy + r), fill=fill, outline=outline, width=s(width))


def round_line(draw, points, width, fill=BLACK):
    pts = [(s(x), s(y)) for x, y in points]
    draw.line(pts, fill=fill, width=s(width), joint='curve')
    for x, y in points:
        circle(draw, x, y, width / 2, fill=fill)


def round_arc(draw, cx, cy, r, start, end, width, fill=BLACK):
    draw.arc(box(cx - r, cy - r, cx + r, cy + r), start=start, end=end, fill=fill, width=s(width))
    # Round caps: the arc stroke is drawn inside the bounding box, so the cap sits half a stroke inwards
    rm = r - width / 2
    for a in (start, end):
        rad = math.radians(a)
        circle(draw, cx + rm * math.cos(rad), cy + rm * math.sin(rad), width / 2, fill=fill)


def new_canvas():
    img = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def finish(img):
    # Downsample in premultiplied alpha so the anti-aliased edges keep their color
    return img.convert('RGBa').resize((W, H), Image.LANCZOS).convert('RGBA')


def draw_remote():
    img, d = new_canvas()

    # Phone body
    d.rounded_rectangle(box(30, 12, 106, 142), radius=s(16), fill=WHITE, outline=BLACK, width=s(STROKE))
    # Camera notch with a pink "live" lens, and the home bar
    d.rounded_rectangle(box(54, 20, 82, 30), radius=s(5), fill=BLACK)
    circle(d, 75, 25, 3, fill=PINK)
    round_line(d, [(58, 130), (78, 130)], 5)

    # StackChan face on the screen
    d.rounded_rectangle(box(44, 50, 92, 100), radius=s(12), fill=BLACK)
    circle(d, 58, 71, 6, fill=WHITE)
    circle(d, 78, 71, 6, fill=WHITE)
    circle(d, 51, 86, 3.5, fill=PINK)
    circle(d, 85, 86, 3.5, fill=PINK)
    round_arc(d, 68, 82, 8, 30, 150, 4, fill=WHITE)

    # Wi-Fi waves coming out of the phone
    cx, cy = 118, 62
    for r in (22, 40, 58):
        round_arc(d, cx, cy, r, -80, -10, 9)
    circle(d, cx + 1, cy - 1, 8, fill=CYAN, outline=BLACK, width=4)
    return finish(img)


def draw_freeze():
    img, d = new_canvas()
    cx = 94

    # Pole
    d.rounded_rectangle(box(cx - 7, 126, cx + 7, 146), radius=s(4), fill=BLACK)
    # Body
    d.rounded_rectangle(box(cx - 34, 8, cx + 34, 134), radius=s(24), fill=BLACK)

    # Lamps: red (pink) is on, yellow is off, green (cyan) dim
    circle(d, cx, 34, 17, fill=PINK)
    circle(d, cx, 72, 15, fill=(70, 70, 70, 255))
    circle(d, cx, 110, 15, fill=CYAN)
    # Glints
    round_arc(d, cx, 34, 11, 200, 260, 4, fill=WHITE)
    round_arc(d, cx, 110, 9, 200, 260, 3, fill=WHITE)

    # "Stop!" shock lines next to the red lamp
    round_line(d, [(146, 18), (158, 8)], 7)
    round_line(d, [(150, 36), (166, 34)], 7)
    round_line(d, [(42, 18), (30, 8)], 7)
    round_line(d, [(38, 36), (22, 34)], 7)
    return finish(img)


def draw_spin():
    img, d = new_canvas()
    cx, cy, r = 94, 84, 56

    # Wheel segments
    colors = [WHITE, PINK, WHITE, CYAN, WHITE, PINK, WHITE, CYAN]
    for i, color in enumerate(colors):
        start = -90 + i * 45 - 22.5
        d.pieslice(box(cx - r, cy - r, cx + r, cy + r), start=start, end=start + 45, fill=color)
    # Spokes
    for i in range(8):
        a = math.radians(-90 + i * 45 - 22.5)
        round_line(d, [(cx, cy), (cx + (r - 2) * math.cos(a), cy + (r - 2) * math.sin(a))], 5)
    # Rim and hub
    circle(d, cx, cy, r, outline=BLACK, width=STROKE)
    circle(d, cx, cy, 12, fill=BLACK)
    circle(d, cx, cy, 4, fill=WHITE)

    # Pointer at the top
    d.polygon([(s(cx - 15), s(8)), (s(cx + 15), s(8)), (s(cx), s(38))], fill=BLACK)
    circle(d, cx, 9, 9, fill=BLACK)
    circle(d, cx, 9, 4, fill=PINK)

    # Motion arcs around the wheel
    round_arc(d, cx, cy, r + 18, 150, 205, 8)
    round_arc(d, cx, cy, r + 18, -25, 30, 8)
    return finish(img)


def draw_radar():
    img, d = new_canvas()
    cx, cy, r = 94, 77, 64

    # Scope with a range ring and a cross hair
    circle(d, cx, cy, r, fill=WHITE)
    circle(d, cx, cy, 34, outline=BLACK, width=5)
    round_line(d, [(cx - r + 6, cy), (cx + r - 6, cy)], 4)
    round_line(d, [(cx, cy - r + 6), (cx, cy + r - 6)], 4)
    # Sweep sector (the afterglow behind the beam), on top of the grid
    d.pieslice(box(cx - r, cy - r, cx + r, cy + r), start=-115, end=-35, fill=CYAN)
    # Beam: the leading edge
    a = math.radians(-35)
    round_line(d, [(cx, cy), (cx + (r - 3) * math.cos(a), cy + (r - 3) * math.sin(a))], 9)
    # Rim and hub
    circle(d, cx, cy, r, outline=BLACK, width=STROKE)
    circle(d, cx, cy, 9, fill=BLACK)
    # Targets: one caught in the beam, one fading behind
    b = math.radians(-62)
    circle(d, cx + 40 * math.cos(b), cy + 40 * math.sin(b), 10, fill=PINK, outline=BLACK, width=4)
    c = math.radians(158)
    circle(d, cx + 44 * math.cos(c), cy + 44 * math.sin(c), 6, fill=PINK)
    return finish(img)


def _face_heart(draw, cx, cy, w, fill):
    # Two circles on the upper sides of a 45 degree diamond, like the hearts of the big-eyes face
    dd = w / 2.4142
    c0 = cy + 0.1035 * dd
    draw.polygon([(s(cx - dd), s(c0)), (s(cx + dd), s(c0)), (s(cx), s(c0 + dd))], fill=fill)
    circle(draw, cx - dd / 2, c0 - dd / 2, 0.7071 * dd, fill=fill)
    circle(draw, cx + dd / 2, c0 - dd / 2, 0.7071 * dd, fill=fill)


def _face_card(w, h, draw_screen):
    """A StackChan screen on its own layer: white body, thick black outline, black display."""
    img = Image.new('RGBA', (s(w), s(h)), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # PIL strokes the outline inside the box: black 0..STROKE, then a white bezel, then the display
    d.rounded_rectangle([0, 0, s(w) - 1, s(h) - 1], radius=s(22), fill=WHITE, outline=BLACK, width=s(STROKE))
    d.rounded_rectangle(box(20, 20, w - 20, h - 20), radius=s(8), fill=BLACK)
    draw_screen(d, w, h)
    return img


def draw_face():
    img, d = new_canvas()
    eye_cyan = (60, 225, 255, 255)

    # Back face, tilted: heart eyes
    def back_screen(dd, w, h):
        _face_heart(dd, w * 0.36, h * 0.5, 19, PINK)
        _face_heart(dd, w * 0.64, h * 0.5, 19, PINK)

    back = _face_card(94, 80, back_screen).rotate(-13, resample=Image.BICUBIC, expand=True)
    img.alpha_composite(back, (s(80), s(0)))

    # Front face: the big glowing eyes
    def front_screen(dd, w, h):
        for cx in (w * 0.35, w * 0.65):
            dd.rounded_rectangle(box(cx - 11, h * 0.5 - 15, cx + 11, h * 0.5 + 15), radius=s(8), fill=eye_cyan)

    img.alpha_composite(_face_card(116, 100, front_screen), (s(10), s(44)))

    # Swap arrow from the back face to the front one
    cx, cy, r = 146, 104, 24
    round_arc(d, cx, cy, r, -10, 105, 8)
    a = math.radians(105)
    ex, ey = cx + (r - 4) * math.cos(a), cy + (r - 4) * math.sin(a)
    d.polygon([(s(ex - 13), s(ey + 1)), (s(ex + 3), s(ey - 11)), (s(ex + 4), s(ey + 11))], fill=BLACK)

    # Sparkles
    for x, y, r in ((34, 22, 8), (178, 92, 6)):
        round_line(d, [(x - r, y), (x + r, y)], 4, fill=WHITE)
        round_line(d, [(x, y - r), (x, y + r)], 4, fill=WHITE)
    return finish(img)


def encode_rgb565a8(img):
    stride = W * 2
    color = bytearray(stride * H)
    alpha = bytearray(W * H)
    px = img.load()
    for y in range(H):
        for x in range(W):
            r, g, b, a = px[x, y]
            if a == 0:
                r = g = b = 0
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            color[y * stride + 2 * x] = v & 0xFF
            color[y * stride + 2 * x + 1] = v >> 8
            alpha[y * W + x] = a
    # LVGL v9 image header: magic, cf, flags, w, h, stride, reserved
    header = struct.pack('<BBHHHHH', 0x19, 0x14, 0, W, H, stride, 0)
    return header + bytes(color) + bytes(alpha)


def decode_rgb565a8(data):
    _, cf, _, w, h, stride, _ = struct.unpack('<BBHHHHH', data[:12])
    assert cf == 0x14
    color = data[12:12 + stride * h]
    alpha = data[12 + stride * h:12 + stride * h + w * h]
    img = Image.new('RGBA', (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            v = color[y * stride + 2 * x] | (color[y * stride + 2 * x + 1] << 8)
            px[x, y] = (((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31,
                        alpha[y * w + x])
    return img


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--preview', help='also write a PNG preview sheet into this directory')
    args = parser.parse_args()

    fw_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(fw_dir, 'main', 'assets', 'assets_bin')
    icons = {'icon_remote.bin': draw_remote(), 'icon_freeze.bin': draw_freeze(), 'icon_spin.bin': draw_spin()}
    icons['icon_radar.bin'] = draw_radar()
    icons['icon_face.bin'] = draw_face()

    for name, img in icons.items():
        data = encode_rgb565a8(img)
        with open(os.path.join(out_dir, name), 'wb') as f:
            f.write(data)
        print(f'wrote {name} ({len(data)} bytes)')

    if args.preview:
        os.makedirs(args.preview, exist_ok=True)
        # Decode the written files (not the source images) to check the encoding, on the launcher colors
        themes = {'icon_remote.bin': (0x3F, 0xA9, 0xF5), 'icon_freeze.bin': (0xFF, 0x5A, 0x5F),
                  'icon_spin.bin': (0xFF, 0xC5, 0x3D)}
        themes['icon_radar.bin'] = (0x3D, 0xDC, 0x84)
        themes['icon_face.bin'] = (0xB2, 0x8C, 0xFF)
        sheet = Image.new('RGBA', (len(icons) * (W + 12) + 12, H + 24), (40, 40, 40, 255))
        for i, name in enumerate(icons):
            with open(os.path.join(out_dir, name), 'rb') as f:
                decoded = decode_rgb565a8(f.read())
            tile = Image.new('RGBA', (W, H), themes[name] + (255,))
            tile.alpha_composite(decoded)
            sheet.alpha_composite(tile, (12 + i * (W + 12), 12))
        path = os.path.join(args.preview, 'stackpet_icons_preview.png')
        sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(path)
        print(f'preview: {path}')


if __name__ == '__main__':
    main()
