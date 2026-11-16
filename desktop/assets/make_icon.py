"""Generate assets/icon.ico from the same geometry as icon.svg / icon-small.svg.

Two variants are rendered: the full icon (stacked terminal windows) for 48px and
above, and a heavier small variant for 16-32px where the thin window outline and
the back card would disappear. Each frame is drawn at 8x and downsampled with
LANCZOS, which is what keeps the rounded corners and round line caps clean.
"""

import io

from PIL import Image, ImageDraw

SS = 8
SIZES_FULL = (32, 48, 64, 128, 256)
SIZES_SMALL = (16, 24)

BG = (21, 26, 36, 255)
BG_EDGE = (44, 54, 72, 255)
CARD_BACK = (91, 124, 247, 77)
WIN_FILL = (13, 18, 27, 255)
WIN_EDGE = (108, 138, 245, 255)
WIN_TITLE = (108, 138, 245, 82)
PROMPT = (61, 216, 168, 255)
CARET = (245, 184, 76, 255)


def _rr(draw, k, xy, radius, **kw):
    draw.rounded_rectangle([v * k for v in xy], radius=radius * k, **kw)


def _line(draw, k, pts, color, width, caps=True):
    w = max(1, round(width * k))
    draw.line([(x * k, y * k) for x, y in pts], fill=color, width=w, joint="curve")
    if caps:
        r = w / 2
        for x, y in (pts[0], pts[-1]):
            draw.ellipse([x * k - r, y * k - r, x * k + r, y * k + r], fill=color)


def render(size, variant):
    s = size * SS
    k = s / 160.0
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    _rr(d, k, (3, 3, 157, 157), 33, fill=BG,
        outline=BG_EDGE if variant == "full" else None,
        width=max(1, round(2 * k)))

    if variant == "full":
        back = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        _rr(ImageDraw.Draw(back), k, (54, 42, 128, 88), 11, fill=None,
            outline=CARD_BACK, width=max(1, round(3 * k)))
        img = Image.alpha_composite(img, back)
        d = ImageDraw.Draw(img)

        _rr(d, k, (30, 56, 130, 114), 11, fill=WIN_FILL, outline=WIN_EDGE,
            width=max(1, round(3.5 * k)))

        title = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        _line(ImageDraw.Draw(title), k, [(30, 73), (130, 73)], WIN_TITLE, 2, caps=False)
        img = Image.alpha_composite(img, title)
        d = ImageDraw.Draw(img)

        _line(d, k, [(53, 86), (64, 94), (53, 102)], PROMPT, 5.5)
        _line(d, k, [(74, 102), (107, 102)], CARET, 5.5)
    else:
        _rr(d, k, (26, 48, 134, 112), 14, fill=None, outline=WIN_EDGE,
            width=max(1, round(5.5 * k)))
        _line(d, k, [(47, 70), (64, 80), (47, 90)], PROMPT, 9)
        _line(d, k, [(76, 90), (113, 90)], CARET, 9)

    return img.resize((size, size), Image.LANCZOS)


def build_ico(frames):
    """Pack PNG frames into an .ico by hand: Pillow's ICO writer derives every
    size from one source image, so it cannot mix the two variants."""
    blobs = []
    for img in frames:
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        blobs.append(buf.getvalue())

    head = bytearray()
    head += (0).to_bytes(2, "little")
    head += (1).to_bytes(2, "little")
    head += len(frames).to_bytes(2, "little")

    offset = 6 + 16 * len(frames)
    entries = bytearray()
    for img, blob in zip(frames, blobs):
        w = 0 if img.width >= 256 else img.width
        h = 0 if img.height >= 256 else img.height
        entries += bytes([w, h, 0, 0])
        entries += (1).to_bytes(2, "little")
        entries += (32).to_bytes(2, "little")
        entries += len(blob).to_bytes(4, "little")
        entries += offset.to_bytes(4, "little")
        offset += len(blob)

    return bytes(head + entries + b"".join(blobs))


if __name__ == "__main__":
    frames = [render(s, "small") for s in SIZES_SMALL]
    frames += [render(s, "full") for s in SIZES_FULL]
    data = build_ico(frames)
    with open("assets/icon.ico", "wb") as f:
        f.write(data)
    print("assets/icon.ico", len(data), "bytes,", len(frames), "frames")
