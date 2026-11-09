"""Иконки POSCommander 2 в стиле Total Commander.

Скрипт рисует res/folder.ico, res/folder_hidden.ico, res/arrow_up.ico и res/app.ico.
Геометрия и цвета сняты попиксельно с эталонных вырезок из TC (16 px), затем
повторены в векторном виде: каждая иконка рисуется в 8x и уменьшается LANCZOS,
поэтому нет грязи на 16/20/24/32/48 px.

Запуск:  python tools/make_icons.py [--preview]
"""
import io
import math
import os
import struct
import sys

from PIL import Image, ImageChops, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, 'ref_folders.png')   # вырезка из TC для сравнения (--compare)
RES = os.path.join(os.path.dirname(HERE), 'res')
SS = 8                      # суперсэмплинг
FOLDER_SIZES = [16, 20, 24, 32, 48]
APP_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

# ---------- утилиты ----------


def lerp(c1, c2, t):
    return tuple(int(round(c1[i] + (c2[i] - c1[i]) * t)) for i in range(3)) + (255,)


def new_canvas(size):
    s = size * SS
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img), s


def vgrad(d, box, top, bottom):
    x0, y0, x1, y1 = box
    h = max(1, int(y1 - y0))
    for i in range(h):
        d.rectangle([x0, y0 + i, x1 - 1, y0 + i], fill=lerp(top, bottom, i / h))


def masked_paste(img, layer, mask):
    """Наложить слой, обрезав его по маске силуэта."""
    layer.putalpha(ImageChops.multiply(layer.getchannel('A'), mask))
    img.alpha_composite(layer)


# ---------- папка ----------
# Координаты ниже — в пикселях эталонной вырезки (23x35): тело x 4..18, y 6..16,
# язычок x 4..11, y 4..6. В сетку 16x16 переносим линейно.
def fx(x):
    return x - 3.5


def fy(y):
    return (y - 4.0) * 0.93 + 2.5


def folder_image(size, hidden=False):
    """Папка по пиксельной сетке эталона, без масштабирования.

    Тело: строки 2..12, столбцы 0..14; углы скошены на 1 px (справа сверху/снизу
    и слева снизу), справа внутри — фаска темнее заливки, над нижней кромкой —
    тёмная полоса. Язычок: строки 0..1, верх x 2..6, основание x 1..7.

    Порядок слоёв: корпус с контуром, затем язычок — иначе верхняя кромка корпуса
    рисуется поверх заливки язычка и тот выглядит отдельной «ручкой».
    """
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    k = size / 16.0

    def S(v):
        return int(math.floor(v * k + 0.5))

    ow = max(1, S(1))

    if hidden:
        tab_fill, tab_edge = (0xF8, 0xF1, 0xC6), (0xD6, 0xB6, 0x79)
        body_top, body_bottom = (0xF9, 0xF5, 0xDC), (0xF8, 0xE8, 0xBB)
        edge_top, edge_left = (0xC9, 0xAE, 0x7C), (0xD4, 0xB5, 0x7A)
        edge_right, edge_bottom = (0xBD, 0xA7, 0x7F), (0xBD, 0xA7, 0x7F)
        bevel_top, bevel_bottom, shade_row = (0xF7, 0xE3, 0xB9), (0xDD, 0xC9, 0xA3), (0xDD, 0xC9, 0xA4)
    else:
        tab_fill, tab_edge = (0xFE, 0xF2, 0x9D), (0xB9, 0x7A, 0x00)
        body_top, body_bottom = (0xFF, 0xF8, 0xC5), (0xFD, 0xDE, 0x84)
        edge_top, edge_left = (0xA4, 0x6D, 0x05), (0xB9, 0x7A, 0x00)
        edge_right, edge_bottom = (0x87, 0x5B, 0x0C), (0x8E, 0x5F, 0x0A)
        bevel_top, bevel_bottom, shade_row = (0xF0, 0xC7, 0x71), (0xC8, 0x9F, 0x54), (0xC7, 0x9F, 0x55)

    y0, y1 = S(2), S(13)
    x0, x1 = S(0), S(15)

    # 1. заливка с фаской справа и полосой над нижней кромкой
    top_row, bottom_row = y0 + ow, y1 - ow
    for y in range(y0 + ow, y1 - ow):
        t = (y - top_row) / max(1, bottom_row - top_row)
        left_col = lerp(body_top, body_bottom, min(1.0, max(0.0, t)))[:3] + (255,)
        if y >= y1 - 2 * ow:                       # полоса над нижней кромкой
            for x in range(x0 + ow, x1 - 2 * ow):
                t2 = min(1.0, max(0.0, (x - x0 - ow) / 5.0))
                rc = lerp(left_col[:3], shade_row, t2)
                d.rectangle([x, y, x, y], fill=rc[:3] + (255,))
        else:
            d.rectangle([x0 + ow, y, x1 - 2 * ow - 1, y], fill=left_col)
        bevel = lerp(bevel_top, bevel_bottom, min(1.0, max(0.0, t)) ** 0.55)[:3] + (255,)
        d.rectangle([x1 - 2 * ow, y, x1 - 2 * ow + ow - 1, y], fill=bevel)

    # 2. контур: верх уже на 1 px справа, низ начинается с 1 px слева (скосы)
    d.rectangle([x0, y0, x1 - 2 * ow, y0 + ow - 1], fill=edge_top + (255,))
    d.rectangle([x0, y0 + ow, x0 + ow - 1, y1 - ow - 1], fill=edge_left + (255,))
    d.rectangle([x1 - ow, y0 + ow, x1 - 1, y1 - ow - 1], fill=edge_right + (255,))
    d.rectangle([x0 + ow, y1 - ow, x1 - 2 * ow, y1 - 1], fill=edge_bottom + (255,))

    # 3. язычок поверх верхней кромки корпуса
    tab_y0, tab_y1 = S(0), S(2)
    for y in range(tab_y0, tab_y1 + ow):
        t = (y - tab_y0) / max(1, (tab_y1 - tab_y0 - 1))
        left = int(math.floor(S(2) + (S(1) - S(2)) * t + 0.5))
        right = int(math.floor(S(6) + (S(7) - S(6)) * t + 0.5))
        d.rectangle([left + ow, y, right - ow - 1, y], fill=tab_fill + (255,))

    # 4. контур язычка: верх и скосы (нижнюю строку основания не обводим)
    d.rectangle([S(2), tab_y0, S(6) - 1, tab_y0 + ow - 1], fill=tab_edge + (255,))
    for y in range(tab_y0, tab_y1):
        t = (y - tab_y0) / max(1, (tab_y1 - tab_y0 - 1))
        left = int(math.floor(S(2) + (S(1) - S(2)) * t + 0.5))
        right = int(math.floor(S(6) + (S(7) - S(6)) * t + 0.5))
        d.rectangle([left, y, left + ow - 1, y], fill=tab_edge + (255,))
        d.rectangle([right - ow, y, right - 1, y], fill=tab_edge + (255,))

    if hidden:
        # 5. белая полоса 2 px (столбцы 7..8) и красный пунктир с окантовкой 1 px
        sx0, sx1 = S(7), S(9)
        d.rectangle([sx0, y0, sx1 - 1, y1 - ow - 1], fill=(255, 255, 255, 255))
        hx0, hx1 = S(6), S(10)
        red = (0xC4, 0x3D, 0x47, 255)
        for dy0, dy1 in ((3, 7), (9, 11)):
            yy0, yy1 = S(dy0), S(dy1)
            d.rectangle([hx0, yy0, hx1 - 1, yy1 - 1], fill=(255, 255, 255, 255))
            d.rectangle([sx0, yy0, sx1 - 1, yy1 - 1], fill=red)

    return img


# ---------- стрелка «на уровень вверх» ----------
def arrow_image(size):
    """Серая стрелка с загибом вправо — снята попиксельно с вырезки TC."""
    img, d, s = new_canvas(size)
    k = s / 16.0

    def S(v):
        return v * k

    pts = [(6.6, 1.2), (12.1, 6.1), (12.1, 7.0), (9.0, 7.0), (9.0, 10.1),
           (13.1, 10.1), (13.1, 14.0), (4.0, 14.0), (4.0, 7.0), (1.0, 7.0)]
    poly = [(S(x), S(y)) for x, y in pts]

    d.polygon([(x + S(0.5), y + S(0.5)) for x, y in poly], fill=(0, 0, 0, 60))

    mask = Image.new('L', (s, s), 0)
    ImageDraw.Draw(mask).polygon(poly, fill=255)
    grad = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    vgrad(ImageDraw.Draw(grad), [0, S(1.2), s, S(14.0)], (0xD6, 0xD6, 0xD6), (0x88, 0x88, 0x88))
    img.paste(grad, (0, 0), mask)

    layer = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    dl = ImageDraw.Draw(layer)
    dl.line([(S(4.2), S(6.6)), (S(6.4), S(2.6))], fill=(0xEE, 0xEE, 0xEE, 255), width=max(1, int(S(0.5))))
    dl.line([(S(4.6), S(13.4)), (S(4.6), S(7.6))], fill=(0xE2, 0xE2, 0xE2, 170), width=max(1, int(S(0.4))))
    dl.line([(S(12.4), S(10.8)), (S(12.4), S(13.4))], fill=(0x70, 0x70, 0x70, 200), width=max(1, int(S(0.5))))
    dl.line([(S(9.6), S(13.4)), (S(12.4), S(13.4))], fill=(0x70, 0x70, 0x70, 200), width=max(1, int(S(0.5))))
    masked_paste(img, layer, mask)

    d = ImageDraw.Draw(img)
    d.line(poly + [poly[0]], fill=(0x3E, 0x3E, 0x3E, 255), width=max(1, int(S(0.8))), joint='curve')
    return img.resize((size, size), Image.LANCZOS)


# ---------- иконка приложения ----------
def app_image(size):
    img, d, s = new_canvas(size)
    k = s / 256.0

    def S(v):
        return v * k

    box = [S(6), S(6), S(250), S(250)]
    mask = Image.new('L', (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=S(40), fill=255)
    grad = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    vgrad(ImageDraw.Draw(grad), [0, S(6), s, S(250)], (0x2A, 0x63, 0xA8), (0x10, 0x2B, 0x55))
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(box, radius=S(40), outline=(0x08, 0x1B, 0x36, 255), width=max(1, int(S(6))))

    for x0, x1 in ((44, 118), (138, 212)):
        d.rounded_rectangle([S(x0), S(58), S(x1), S(178)], radius=S(8), fill=(255, 255, 255, 255),
                            outline=(0x93, 0xB4, 0xD8, 255), width=max(1, int(S(4))))
        for i, y in enumerate(range(78, 168, 22)):
            d.rectangle([S(x0 + 10), S(y), S(x1 - 12), S(y + 8)],
                        fill=(0x9A, 0xB6, 0xD2, 255) if i % 2 == 0 else (0xC2, 0xD4, 0xE6, 255))

    fbox = [S(84), S(150), S(196), S(226)]
    d.rounded_rectangle([fbox[0], fbox[1] - S(20), fbox[0] + S(52), fbox[1] + S(6)], radius=S(7),
                        fill=(0xF3, 0xC4, 0x45, 255), outline=(0x7A, 0x5A, 0x0A, 255), width=max(1, int(S(4))))
    d.rounded_rectangle(fbox, radius=S(8), fill=(0xFF, 0xDD, 0x77, 255), outline=(0x7A, 0x5A, 0x0A, 255),
                        width=max(1, int(S(4))))
    d.line([S(fbox[0] + 10), S(fbox[1] + 8), S(fbox[2] - 10), S(fbox[1] + 8)],
           fill=(0xFF, 0xF3, 0xC9, 255), width=max(1, int(S(5))))
    return img.resize((size, size), Image.LANCZOS)


def app_image_small(size):
    """16-24 px: только две панели (на 20-24 ещё и папка), иначе детали сливаются."""
    img, d, s = new_canvas(size)
    k = s / 16.0

    def S(v):
        return v * k

    box = [S(0.4), S(0.4), S(15.6), S(15.6)]
    mask = Image.new('L', (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=S(2.6), fill=255)
    grad = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    vgrad(ImageDraw.Draw(grad), [0, S(0.4), s, S(15.6)], (0x2A, 0x63, 0xA8), (0x0E, 0x27, 0x4E))
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(box, radius=S(2.6), outline=(0x07, 0x18, 0x30, 255), width=max(1, int(S(0.5))))

    with_folder = size >= 20
    bottom = 8.6 if with_folder else 13.2
    for x0, x1 in ((2.6, 6.8), (9.2, 13.4)):
        d.rectangle([S(x0), S(2.4), S(x1), S(bottom)], fill=(0xF7, 0xFB, 0xFF, 255))
    if with_folder:
        d.rounded_rectangle([S(3.4), S(8.2), S(9.4), S(10.0)], radius=S(0.5), fill=(0xF3, 0xC0, 0x3C, 255),
                            outline=(0x6E, 0x50, 0x06, 255), width=max(1, int(S(0.35))))
        d.rounded_rectangle([S(3.0), S(9.4), S(13.0), S(13.4)], radius=S(0.8), fill=(0xFF, 0xDD, 0x77, 255),
                            outline=(0x6E, 0x50, 0x06, 255), width=max(1, int(S(0.4))))
        d.line([S(4.2), S(10.3), S(11.8), S(10.3)], fill=(0xFF, 0xF4, 0xCE, 255), width=max(1, int(S(0.4))))
    return img.resize((size, size), Image.LANCZOS)


# ---------- ICO ----------
def write_ico(path, images):
    """Многоразмерный ICO: до 96 px — BMP-записи, крупнее — PNG (Windows понимает)."""
    blobs = []
    for im in images:
        w, h = im.size
        rgba = im.convert('RGBA')
        if w >= 128:
            buf = io.BytesIO()
            rgba.save(buf, format='PNG', optimize=True)
            blobs.append(buf.getvalue())
            continue
        rows = []
        px = rgba.load()
        for y in range(h - 1, -1, -1):
            row = bytearray()
            for x in range(w):
                r, g, b, a = px[x, y]
                row += bytes((b, g, r, a))
            rows.append(bytes(row))
        pixels = b''.join(rows)
        and_mask = bytes(((w + 31) // 32) * 4 * h)
        header = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0, len(pixels) + len(and_mask), 0, 0, 0, 0)
        blobs.append(header + pixels + and_mask)

    out = bytearray(struct.pack('<HHH', 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for im, blob in zip(images, blobs):
        w, h = im.size
        out += struct.pack('<BBBBHHII', w if w < 256 else 0, h if h < 256 else 0, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    for blob in blobs:
        out += blob
    with open(path, 'wb') as f:
        f.write(out)


def main():
    os.makedirs(RES, exist_ok=True)
    write_ico(os.path.join(RES, 'folder.ico'), [folder_image(s) for s in FOLDER_SIZES])
    write_ico(os.path.join(RES, 'folder_hidden.ico'), [folder_image(s, hidden=True) for s in FOLDER_SIZES])
    write_ico(os.path.join(RES, 'arrow_up.ico'), [arrow_image(s) for s in FOLDER_SIZES])
    write_ico(os.path.join(RES, 'app.ico'),
              [app_image_small(s) if s <= 24 else app_image(s) for s in APP_SIZES])
    for name in ('folder.ico', 'folder_hidden.ico', 'arrow_up.ico', 'app.ico'):
        print('  ', name, os.path.getsize(os.path.join(RES, name)), 'байт')

    if '--compare' in sys.argv:
        ref = Image.open(REF)
        # верхняя папка вырезки: x 4..19, y 4..17; нижняя: y 21..34
        top = ref.crop((4, 4, 20, 17))
        bottom = ref.crop((4, 21, 20, 34))
        sheet = Image.new('RGB', (620, 300), (0xF0, 0xF0, 0xF0))
        for i, (crop, ours) in enumerate(((top, folder_image(16)), (bottom, folder_image(16, hidden=True)))):
            y = 10 + i * 150
            ref_big = crop.convert('RGB').resize((192, 156), Image.NEAREST)
            sheet.paste(ref_big, (10, y))
            o = ours.resize((192, 192), Image.NEAREST)
            sheet.paste(o, (230, y - 18), o)
        sheet.save(os.path.join(HERE, 'compare.png'))
        print('сравнение: tools/compare.png')

    if '--preview' in sys.argv:
        sheet = Image.new('RGBA', (4 * 140 + 20, 170), (0xF0, 0xF0, 0xF0, 255))
        x = 10
        for im in (folder_image(32), folder_image(32, hidden=True), arrow_image(32), app_image(32)):
            big = im.resize((128, 128), Image.LANCZOS)
            sheet.paste(big, (x, 20), big)
            x += 140
        sheet.save(os.path.join(HERE, 'preview.png'))
        print('превью: tools/preview.png')


if __name__ == '__main__':
    main()
