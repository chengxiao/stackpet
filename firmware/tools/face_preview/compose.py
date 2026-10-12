"""Compose the PPM frames written by face_preview into labelled PNG sheets."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

OUT = Path(sys.argv[1])
FONT = str(Path(__file__).resolve().parents[2] / "managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf")
STYLES = [("official", "官方原版"), ("classic", "经典加强"), ("bigeyes", "大眼萌系")]
EMOTIONS = [("neutral", "平静"), ("happy", "开心"), ("laughing", "大笑"), ("angry", "生气"), ("sad", "难过"),
            ("surprised", "惊讶"), ("doubt", "疑惑"), ("sleepy", "困了"), ("shy", "害羞"), ("love", "喜欢")]
EXTRAS = [("x_blink", "眨眼"), ("x_speaking", "说话"), ("x_look_left", "看左边"), ("x_look_up", "看上面"), ("x_speech", "气泡")]
W, H, PAD, LABEL = 320, 240, 12, 28


def load(name):
    p = OUT / f"{name}.ppm"
    return Image.open(p).convert("RGB") if p.exists() else None


def sheet(title, cells, cols, path):
    rows = (len(cells) + cols - 1) // cols
    img = Image.new("RGB", (cols * (W + PAD) + PAD, 50 + rows * (H + LABEL + PAD) + PAD), (34, 36, 42))
    d = ImageDraw.Draw(img)
    d.text((PAD, 12), title, font=ImageFont.truetype(FONT, 24), fill=(240, 240, 240))
    for i, (label, frame) in enumerate(cells):
        r, c = divmod(i, cols)
        x, y = PAD + c * (W + PAD), 50 + PAD + r * (H + LABEL + PAD)
        if frame is not None:
            img.paste(frame, (x, y))
        d.rectangle([x - 1, y - 1, x + W, y + H], outline=(80, 84, 92))
        d.text((x + W / 2, y + H + 14), label, font=ImageFont.truetype(FONT, 18), fill=(220, 220, 220), anchor="mm")
    img.save(path)
    print("saved", path)


for key, name in STYLES:
    cells = [(label, load(f"{key}_{e}")) for e, label in EMOTIONS] + [(label, load(f"{key}_{x}")) for x, label in EXTRAS]
    sheet(f"{name}：10 种表情 + 动作状态（设备实际渲染，320x240）", cells, 5, OUT / f"sheet_{key}.png")
