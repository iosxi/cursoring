"""cursoring のアイコン (src/cursoring.ico と無効時の src/cursoring-off.ico) を作る。

図柄: 大小 2 枚のモニタを下端そろえで接して並べ、継ぎ目の上に矢印カーソルを重ねる。
16/20/24px はにじまないよう画素単位で塗り (アンチエイリアスなし)、
32px 以上は 16 倍で描いてから縮小する。

使い方: python tools/make_icon.py [プレビュー PNG の出力先]
必要なもの: Pillow
"""
import struct
import sys
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent

PALETTES = {
    "on":  dict(frame=(0x1E, 0x3A, 0x5F), big=(0x4F, 0xA3, 0xE0), small=(0x7C, 0xD6, 0x7C),
                arrow=(0xFF, 0xFF, 0xFF), outline=(0x10, 0x10, 0x10)),
    "off": dict(frame=(0x50, 0x50, 0x50), big=(0xA0, 0xA0, 0xA0), small=(0xB8, 0xB8, 0xB8),
                arrow=(0xF0, 0xF0, 0xF0), outline=(0x30, 0x30, 0x30)),
}

# ---- 32px 以上: 64 単位の座標で描く ----
BIG = (3, 9, 37, 35)      # 大きいモニタの外形 (左, 上, 右, 下)
SMALL = (37, 17, 61, 35)  # 小さいモニタ。左辺が大きいモニタの右辺に接し、下端がそろう
FRAME = 4
RADIUS = 3
# Windows の矢印カーソル (高さ 20 単位) と、その置き場所・倍率
ARROW = [(0, 0), (0, 17), (4, 13.2), (6.8, 19.6), (9.6, 18.4), (6.9, 12.2), (12, 12.2)]
ARROW_AT, ARROW_SCALE, ARROW_LINE = (21, 17), 1.9, 3.4


def draw_smooth(size, pal):
    ss = 16
    k = size * ss / 64
    im = Image.new("RGBA", (size * ss, size * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for rect, fill in ((BIG, pal["big"]), (SMALL, pal["small"])):
        l, t, r, b = (v * k for v in rect)
        d.rounded_rectangle((l, t, r - 1, b - 1), RADIUS * k, fill=pal["frame"])
        f = FRAME * k
        d.rounded_rectangle((l + f, t + f, r - f - 1, b - f - 1), max(RADIUS - FRAME / 2, 0.5) * k, fill=fill)
    pts = [((ARROW_AT[0] + x * ARROW_SCALE) * k, (ARROW_AT[1] + y * ARROW_SCALE) * k) for x, y in ARROW]
    d.polygon(pts, fill=pal["arrow"])
    d.line(pts + pts[:2], fill=pal["outline"], width=round(ARROW_LINE * k), joint="curve")
    return im.resize((size, size), Image.LANCZOS)


# ---- 16/20/24px: 画素で描く ----
# 矢印の型 (Windows 標準の矢印を各サイズに合わせて描いたもの)。W=白 X=縁 空白=透明
ARROW_PX = {
    16: """
X
XX
XWX
XWWX
XWWWX
XWWWWX
XWWWWWX
XWWXXXX
XWXXWX
XX XWX
X   XWX
     X
""",
    20: """
X
XX
XWX
XWWX
XWWWX
XWWWWX
XWWWWWX
XWWWWWWX
XWWWXXXXX
XWWXWWX
XWX XWWX
XX  XWWX
X    XX
""",
    24: """
X
XX
XWX
XWWX
XWWWX
XWWWWX
XWWWWWX
XWWWWWWX
XWWWWWWWX
XWWWWXXXXX
XWWXWWX
XWX XWWX
XX  XWWX
X    XWWX
     XWWX
      XX
""",
}
# サイズごとの配置: 大きいモニタ, 小さいモニタ (左, 上, 右, 下 / 画素、両端含む), 矢印の先端
LAYOUT_PX = {
    16: ((0, 1, 9, 8), (10, 4, 15, 8), (5, 3)),
    20: ((0, 3, 11, 10), (12, 6, 19, 10), (6, 5)),
    24: ((1, 3, 13, 12), (14, 6, 22, 12), (8, 6)),
}


def draw_pixels(size, pal):
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    big, small, (ax, ay) = LAYOUT_PX[size]
    for (l, t, r, b), fill in ((big, pal["big"]), (small, pal["small"])):
        for y in range(t, b + 1):
            for x in range(l, r + 1):
                edge = x in (l, r) or y in (t, b)
                im.putpixel((x, y), (pal["frame"] if edge else fill) + (255,))
    for dy, row in enumerate(ARROW_PX[size].strip("\n").split("\n")):
        for dx, c in enumerate(row):
            if c != " ":
                im.putpixel((ax + dx, ay + dy), pal["arrow" if c == "W" else "outline"] + (255,))
    return im


SIZES = [16, 20, 24, 32, 48, 64, 256]


def make(pal):
    return [draw_pixels(s, pal) if s in LAYOUT_PX else draw_smooth(s, pal) for s in SIZES]


def write_ico(path, images):
    """全サイズを PNG で格納した ico を書く (Vista 以降の形式。Pillow の ico 保存は
    画素で描いた小さい版を使わず縮小で作り直すため、自前で組み立てる)"""
    blobs = []
    for im in images:
        b = BytesIO()
        im.save(b, "PNG", optimize=True)
        blobs.append(b.getvalue())
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for im, blob in zip(images, blobs):
        w = im.width % 256
        out += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    path.write_bytes(out + b"".join(blobs))


def preview(path, sets):
    """各サイズを白地・黒地に並べ、8 倍に拡大したものも付けたプレビュー"""
    pad, z = 8, 8
    zoom = [16, 20, 24]
    w = pad + sum(s + pad for s in SIZES) + sum(s * z + pad for s in zoom)
    h = pad + (256 + pad) * len(sets) * 2
    sheet = Image.new("RGB", (w, h), (128, 128, 128))
    y = pad
    for images in sets:
        for bg in ((255, 255, 255), (32, 32, 32)):
            x = pad
            for im in images:
                tile = Image.new("RGBA", im.size, bg + (255,))
                tile.alpha_composite(im)
                sheet.paste(tile.convert("RGB"), (x, y))
                x += im.width + pad
            for s in zoom:
                im = images[SIZES.index(s)]
                tile = Image.new("RGBA", im.size, bg + (255,))
                tile.alpha_composite(im)
                sheet.paste(tile.convert("RGB").resize((s * z, s * z), Image.NEAREST), (x, y))
                x += s * z + pad
            y += 256 + pad
    sheet.save(path)


if __name__ == "__main__":
    on, off = make(PALETTES["on"]), make(PALETTES["off"])
    write_ico(ROOT / "src" / "cursoring.ico", on)
    write_ico(ROOT / "src" / "cursoring-off.ico", off)
    if len(sys.argv) > 1:
        preview(sys.argv[1], [on, off])
    print("OK: src/cursoring.ico, src/cursoring-off.ico")
