"""Boxes and texts of the Avernum 6 UI images with English text (used by make_images.py).

Coordinates were measured on the original images (orig_tree/Avernum 6 Files/Graphics Core).
Images whose content is identical to an image already translated for Escape From the Pit are
copied from that mod (COPY below) instead of being redrawn. Most BMPs here are 8-bit with pure
white as the transparent colour.
"""
import os

from PIL import Image

FONTS = 'C:/Windows/Fonts/'
MINCHO_B = FONTS + 'yumindb.ttf'       # Yu Mincho Demibold
GOTHIC_B = FONTS + 'YuGothB.ttc'       # Yu Gothic Bold

PARCH = (242, 237, 230)
INK = (62, 22, 22)          # help legends: dark red-brown
STONE_INK = (40, 34, 28)    # labels engraved in the stone frames
PAGES = {}
PAGE_BOX = (0, 0, 0, 0)
PAGE_INK = (0, 0, 0)

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.join(HERE, '..', 'orig_tree', 'Avernum 6 Files', 'Graphics Core')
EFTP_OUT = os.path.join(HERE, '..', '..', '..', 'Avernum Escape From the Pit', '_jp_mod', 'img_out')
# same original content as these Escape From the Pit images
COPY = {n: os.path.join(EFTP_OUT, n) for n in ('G168.bmp', 'G169.bmp', 'G188.bmp', 'G196.bmp', 'G202.bmp', 'G216.bmp')}


def _size(name):
    return Image.open(os.path.join(CORE, name)).size


# headings: grey stone letters on white (transparent)
HEAD = {'style': 'a4head', 'edge': (238, 238, 230), 'top': (112, 112, 106), 'bottom': (38, 38, 38)}
BUTTON = {'style': 'a4button', 'edge': (238, 238, 230), 'top': (96, 96, 92), 'bottom': (30, 30, 30)}


def heading(name, text, pad=1):
    w, h = _size(name)
    return dict(HEAD, erase='white', key_white=True,
                texts=[((pad, pad, w - pad - 1, h - pad - 1), [text], MINCHO_B, h - 4, None, 'left')])


SPEC = {
    'G150.bmp': heading('G150.bmp', 'パーティーの編成'),
    'G153.bmp': heading('G153.bmp', '能力一覧'),
    'G155.bmp': heading('G155.bmp', 'ゲームをセーブ'),
    'G156.bmp': heading('G156.bmp', 'セーブデータのロード'),
    'G159.bmp': heading('G159.bmp', '記録'),
    'G160.bmp': heading('G160.bmp', 'アヴァーナム6 操作説明'),
    'G162.bmp': heading('G162.bmp', 'パーティーの作成'),
    'G163.bmp': heading('G163.bmp', '依頼掲示板'),
    'G164.bmp': heading('G164.bmp', '製品登録'),
    'G184.bmp': heading('G184.bmp', '転移門の使用'),
    'G189.bmp': heading('G189.bmp', 'ゲーム設定'),
}


def rows(texts, x0, x1, y0, step, align='left', size=20):
    out, fills = [], []
    for i, t in enumerate(texts):
        y = y0 + i * step
        fills.append(((x0, y + 1, x1, y + step), (255, 255, 255, 255)))
        if t:
            out.append(((x0 + 2, y + 2, x1 - 2, y + step - 1), [t], MINCHO_B, size, None, align))
    return out, fills


_t203l, _f203l = rows(['兵士', '狂戦士', '司祭', '妖術師', 'ならず者', '射手', '反逆者', '田舎魔術師', 'シャーマン',
                       'カスタム'], 1, 223, 0, 24, align='center', size=19)
_t203r, _f203r = rows(['ゲームに戻る', '', 'ゲームをセーブ', 'セーブデータのロード', '', '設定', 'キー操作の一覧',
                       'アヴァーナム6について', 'タイトル画面に戻る', '終了'], 224, 446, 0, 24, align='center', size=19)

# title menu: cream letters with a dark edge on the (transparent) inside of a stone frame
_menu = [(54, 89, '新しいゲーム'), (101, 136, 'ゲームをロード'), (148, 183, '購入方法'), (194, 236, '製品登録'),
         (242, 276, '操作説明'), (288, 330, 'ゲーム設定'), (335, 370, 'キャラクターエディター'), (382, 423, '終了')]

SPEC.update({
    'G203.bmp': dict(BUTTON, erase='none', key_white=True, fill_rects=_f203l + _f203r, texts=_t203l + _t203r),
    'G158.png': {'erase': 'none', 'style': 'a4menu', 'edge': (24, 18, 12), 'top': (246, 241, 222),
                 'bottom': (214, 200, 164), 'bold': 1,
                 'clear_rects': [(38, y0 - 3, 404, y1 + 3) for y0, y1, _ in _menu],
                 'texts': [((40, y0 - 2, 402, y1 + 2), [t], MINCHO_B, 30, None, 'center') for y0, y1, t in _menu]},
    'G128.png': {'erase': 'inpaint', 'texts': [
        ((727, 6, 799, 22), ['地図'], GOTHIC_B, 13, STONE_INK, 'left'),
        ((775, 503, 835, 519), ['メンバー'], GOTHIC_B, 13, STONE_INK, 'left')]},
    'G133.png': {'erase': 'inpaint', 'texts': [
        ((42, 2, 79, 18), ['購入'], GOTHIC_B, 12, (60, 36, 20), 'center'),
        ((81, 2, 118, 18), ['購入'], GOTHIC_B, 12, (60, 36, 20), 'center'),
        ((1, 112, 45, 126), ['人間'], GOTHIC_B, 12, (255, 255, 255), 'center', 'outline'),
        ((47, 112, 91, 126), ['ネフィル'], GOTHIC_B, 11, (255, 255, 255), 'center', 'outline'),
        ((93, 112, 137, 126), ['スリス'], GOTHIC_B, 12, (255, 255, 255), 'center', 'outline'),
        ((132, 465, 206, 482), ['読み込み中'], MINCHO_B, 13, STONE_INK, 'center')]},
})

_g166 = []
for (x0, x1), names in zip([(14, 78), (91, 154), (169, 246), (259, 363)],
                           [['呪い', '祝福', '減速', '加速', '毒', '酸', '雷', '死の呪い'],
                            ['結界', '弱体化', '棘の盾', '朦朧', '恐慌', '放心', '魅了', '刃の盾'],
                            ['悟り', '凍結', '持久の盾', '持久の鎧', '再生', '狂乱', '無敵', 'スクリバン'],
                            ['思考の守護', '元素の守護', '鋼鉄の守護', '呪いの外套', '刃の外套', '稲妻の外套', '秘術の外套']]):
    for k, n in enumerate(names):
        y = 24 + k * 13
        _g166.append(((x0, y, x1, y + 12), [n], GOTHIC_B, 11, INK, 'left'))

SPEC.update({
    'G166.bmp': {'erase': 'fill', 'no_grid': True,
                 'texts': [((100, 3, 266, 18), ['魔法の効果'], MINCHO_B, 15, INK, 'center')] + _g166},
    'G167.bmp': {'erase': 'none', 'fill_rects': [((222, 26, 383, 130), (244, 244, 244, 255))],
                 'texts': [((225, y0 - 1, 380, y1 + 1), [t], GOTHIC_B, 12, INK, 'left') for (y0, y1), t in zip(
                     [(31, 41), (46, 56), (62, 72), (79, 89), (96, 106), (113, 123)],
                     ['A - 地形', 'B - パーティー一覧', 'C - 自動地図', 'D - ゲームボタン', 'E - サイドボタン', 'F - 文字欄'])]},
    'G288.bmp': {'erase': 'none', 'fill_rects': [((4, 2, 556, 33), (255, 255, 255, 255))],
                 'texts': [((10, 6, 300, 31), ['冒険は続く……'], MINCHO_B, 20, (110, 24, 24), 'left')]},
})
