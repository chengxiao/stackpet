"""Compose the PPM frames written by usage_preview into labelled PNG sheets: one per style (every scene) and an
overview (every style in the first scenes)."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

OUT = Path(sys.argv[1])
FONT = str(Path(__file__).resolve().parents[2] / "managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf")
STYLES = [("claude", "暖黑"), ("rings", "表盘"), ("terminal", "终端"), ("sleepy", "睡觉")]
SCENES = [("work", "工作中"), ("wait", "每周 86% · 等你确认"), ("done", "超过 90% · 已完成"), ("night", "夜里"),
          ("stale", "Mac 12 分钟没回"), ("setup", "未配置"), ("real", "Mac 上的真实快照")]
W, H, PAD, LABEL = 320, 240, 14, 30


def load(name):
    p = OUT / f"{name}.ppm"
    if not p.exists():
        return None
    frame = Image.open(p).convert("RGB")
    frame.save(OUT / f"{name}.png")
    return frame


def sheet(title, cells, cols, path):
    rows = (len(cells) + cols - 1) // cols
    img = Image.new("RGB", (cols * (W + PAD) + PAD, 54 + rows * (H + LABEL + PAD) + PAD), (40, 42, 48))
    d = ImageDraw.Draw(img)
    d.text((PAD, 14), title, font=ImageFont.truetype(FONT, 24), fill=(240, 240, 240))
    for i, (label, frame) in enumerate(cells):
        r, c = divmod(i, cols)
        x, y = PAD + c * (W + PAD), 54 + PAD + r * (H + LABEL + PAD)
        img.paste(frame, (x, y))
        d.rectangle([x - 1, y - 1, x + W, y + H], outline=(90, 94, 102))
        d.text((x + W / 2, y + H + 15), label, font=ImageFont.truetype(FONT, 18), fill=(225, 225, 225), anchor="mm")
    img.save(path)
    print("saved", path)


overview = []
for key, name in STYLES:
    cells = [(label, f) for scene, label in SCENES if (f := load(f"{key}_{scene}")) is not None]
    sheet(f"屏保风格「{name}」（设备实际渲染 320x240，背光未调暗）", cells, 4, OUT / f"sheet_{key}.png")
    overview += [(f"{name} · {label}", f) for scene, label in SCENES[:2] if (f := load(f"{key}_{scene}")) is not None]
sheet("主屏屏保的四种风格（屏保上摇杆左右换，按一下回主屏）", overview, 4, OUT / "overview.png")
