"""Redraw the place names on the Avernum 6 world map (G200.jpg) in Japanese.

usage: python -I make_map_a6.py ORIGINAL_DIR OUTPUT_DIR [PREVIEW_DIR] [--boxes]

The map is the Avernum 4 world map with some names added or moved. Boxes are in image
coordinates; "v" boxes hold a label rotated 90 degrees in the original and are written top to
bottom in Japanese. "|" breaks a label into lines.
"""
import os
import sys

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.stdout.reconfigure(encoding='utf-8')
FONT = 'C:/Windows/Fonts/yumindb.ttf'
INK = (52, 36, 22)

LABELS = [
    ((350, 47, 422, 78), '修道院砦'),
    ((536, 117, 606, 132), 'ドレイコ砦'),
    ((128, 172, 184, 189), 'バルガ'),
    ((702, 142, 770, 159), 'フォルメロ'),
    ((536, 171, 680, 192), '占領された地'),
    ((92, 210, 178, 230), '深淵'),
    ((353, 214, 424, 232), 'グラインドストーン'),
    ((688, 267, 775, 284), '角の門'),
    ((124, 287, 167, 304), '尖塔'),
    ((580, 310, 684, 348), '東の|回廊'),
    ((403, 339, 492, 354), 'ドランロン砦'),
    ((695, 346, 783, 363), 'ドゥヴノ砦'),
    ((588, 353, 639, 371), 'コトラ'),
    ((44, 373, 95, 403), '尖塔の|要塞'),
    ((709, 412, 765, 430), 'シルヴァー'),
    ((58, 452, 134, 468), 'サフラン砦'),
    ((234, 459, 316, 475), 'エメラルド砦'),
    ((26, 460, 50, 616), '西部荒野', 'v'),
    ((184, 497, 236, 515), 'グナス'),
    ((128, 513, 178, 530), 'ブロスク'),
    ((601, 516, 701, 553), '蜂の巣|洞窟'),
    ((116, 553, 192, 570), 'ダーモン'),
    ((217, 545, 315, 581), '大洞窟'),
    ((339, 579, 432, 599), '城'),
    ((711, 567, 760, 584), 'メルティス'),
    ((308, 619, 354, 649), '転移門の|城塞'),
    ((460, 638, 526, 657), 'アルマリア'),
    ((356, 639, 426, 668), '城の|食料倉庫'),
    ((54, 647, 138, 665), 'リモート砦'),
    ((646, 666, 692, 695), '大転移門'),
    ((728, 678, 824, 695), '塔のコロニー'),
    ((218, 687, 319, 703), 'パトリックの塔'),
]


def fit(lines, w, h, start):
    size = start
    while size > 10:
        f = ImageFont.truetype(FONT, size)
        lh = size * 1.1
        if max(f.getlength(t) for t in lines) <= w * 1.25 and lh * len(lines) <= h + 4:
            return f, size, lh
        size -= 1
    return ImageFont.truetype(FONT, 10), 10, 11.0


def main(argv):
    show = '--boxes' in argv
    argv = [a for a in argv if a != '--boxes']
    src, out = argv[0], argv[1]
    preview = argv[2] if len(argv) > 2 else None
    im = Image.open(os.path.join(src, 'G200.jpg')).convert('RGB')
    rgb = cv2.cvtColor(np.array(im), cv2.COLOR_RGB2BGR)
    a = rgb.astype(int)
    mask = np.zeros(rgb.shape[:2], np.uint8)
    for L in LABELS:
        x0, y0, x1, y1 = L[0]
        sub = a[y0:y1, x0:x1]   # BGR
        lum = sub.sum(2)
        bg = np.median(lum)
        hit = (lum < bg - 90) & (sub[:, :, 0] < 140)   # letters are much darker than the parchment
        mask[y0:y1, x0:x1] |= hit.astype(np.uint8) * 255
    mask = cv2.dilate(mask, np.ones((3, 3), np.uint8), iterations=2)
    rgb = cv2.inpaint(rgb, mask, 5, cv2.INPAINT_TELEA)
    img = Image.fromarray(cv2.cvtColor(rgb, cv2.COLOR_BGR2RGB))
    layer = Image.new('L', img.size, 0)
    d = ImageDraw.Draw(layer)
    for L in LABELS:
        (x0, y0, x1, y1), text = L[0], L[1]
        w, h = x1 - x0, y1 - y0
        if len(L) > 2 and L[2] == 'v':
            chars = list(text)
            size = min(w - 2, int(h / len(chars) / 1.05), 18)
            f = ImageFont.truetype(FONT, size)
            y = y0 + (h - size * 1.05 * len(chars)) / 2
            for ch in chars:
                d.text((x0 + (w - f.getlength(ch)) / 2, y), ch, font=f, fill=255)
                y += size * 1.05
            continue
        lines = text.split('|')
        f, size, lh = fit(lines, w, h, 15)
        y = y0 + (h - lh * len(lines)) / 2
        for t in lines:
            bb = f.getbbox(t)
            d.text((x0 + (w - f.getlength(t)) / 2, y - bb[1] + (size - (bb[3] - bb[1])) / 2), t, font=f, fill=255)
            y += lh
    img = Image.composite(Image.new('RGB', img.size, INK), img, layer)
    img.save(os.path.join(out, 'G200.jpg'), quality=95)
    if preview:
        p = img.copy()
        if show:
            dd = ImageDraw.Draw(p)
            for L in LABELS:
                dd.rectangle(L[0], outline=(255, 0, 0))
        p.save(os.path.join(preview, 'G200_jp.png'))
    print('wrote G200.jpg')


if __name__ == '__main__':
    main(sys.argv[1:])
