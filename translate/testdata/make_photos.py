# Synthetic "phone camera pointed at a label" test pictures for pm_translate.
#
# Renders Japanese / Korean / English / Chinese packaging labels, signs and
# menus onto paper textures, puts them on a table at an angle (perspective),
# adds uneven light, glare, blur, sensor noise and JPEG compression, and
# frames the result like the iPhone camera app mirrored to the PC
# (994x2160, viewfinder + zh-Hant camera UI labels, as in the owner's test).
#
#   py -3 translate/testdata/make_photos.py OUTDIR
#
# NAME_1x (ja_food_label_1x, ko_food_label_1x, en_label_1x, ja_menu_1x): the
# same scene at 75 % size (the camera at 1x: the label fills a third of the
# frame) with clutter around it (a snack box "Pocky / ポッキー / share happi",
# a 福袋 sticker) that must not be translated.
#
# Writes NAME.png + NAME.gt.tsv per scene: one line per text line,
# "x0 y0 x1 y1 qx0 qy0 qx1 qy1 qx2 qy2 qx3 qy3<TAB>text" (box and the
# rotated quad, 0..1 of the picture).  Fonts: Windows' own (Yu Gothic, Malgun
# Gothic, Microsoft YaHei / JhengHei, Arial, Georgia).  Needs numpy, opencv,
# Pillow.  Deterministic (fixed seeds).
import math
import os
import sys

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

FONTS = r"C:\Windows\Fonts"
F = {
    "ja": "YuGothM.ttc", "jab": "YuGothB.ttc", "jag": "msgothic.ttc",
    "ko": "malgun.ttf", "kob": "malgunbd.ttf",
    "zhs": "msyh.ttc", "zhsb": "msyhbd.ttc", "zht": "msjh.ttc", "zhtb": "msjhbd.ttc",
    "en": "arial.ttf", "enb": "arialbd.ttf", "ens": "georgia.ttf", "enn": "ARIALN.TTF",
}
_cache = {}


def font(key, size):
    k = (key, size)
    if k not in _cache:
        _cache[k] = ImageFont.truetype(os.path.join(FONTS, F[key]), size)
    return _cache[k]


class Card:
    """A label / sign / menu drawn flat; remembers every text line's quad."""

    def __init__(self, w, h, paper=(246, 242, 232), seed=0):
        self.w, self.h = w, h
        rng = np.random.default_rng(seed)
        base = np.ones((h, w, 3), np.float32) * np.array(paper, np.float32)
        low = cv2.resize(rng.normal(0, 1, (max(2, h // 60), max(2, w // 60))).astype(np.float32), (w, h),
                         interpolation=cv2.INTER_CUBIC)
        grain = rng.normal(0, 1, (h, w)).astype(np.float32)
        fib = cv2.GaussianBlur(rng.normal(0, 1, (h, w)).astype(np.float32), (0, 0), sigmaX=6, sigmaY=0.6)
        tex = base + (low * 5 + grain * 3 + fib * 9)[..., None]
        self.img = Image.fromarray(np.clip(tex, 0, 255).astype(np.uint8))
        self.d = ImageDraw.Draw(self.img)
        self.lines = []  # (quad 4x2 in card px, text)

    def rect(self, x0, y0, x1, y1, fill=None, outline=None, width=2, radius=0):
        if radius:
            self.d.rounded_rectangle((x0, y0, x1, y1), radius, fill=fill, outline=outline, width=width)
        else:
            self.d.rectangle((x0, y0, x1, y1), fill=fill, outline=outline, width=width)

    def text(self, x, y, s, key, size, color=(30, 30, 30), gt=True, spacing=0):
        f = font(key, size)
        if spacing:
            cx = x
            for ch in s:
                self.d.text((cx, y), ch, font=f, fill=color)
                cx += f.getlength(ch) + spacing
            x1 = cx - spacing
        else:
            self.d.text((x, y), s, font=f, fill=color)
            x1 = x + f.getlength(s)
        asc, desc = f.getmetrics()
        bb = self.d.textbbox((x, y), s, font=f)
        y0, y1 = bb[1], bb[3]
        if gt:
            self.lines.append((np.array([[x, y0], [x1, y0], [x1, y1], [x, y1]], np.float32), s))
        return x1

    def vtext(self, x, y, s, key, size, color=(30, 30, 30), gap=0.08):
        """Vertical (top to bottom) text, one character under the other."""
        f = font(key, size)
        cy = y
        for ch in s:
            w = f.getlength(ch)
            self.d.text((x + (size - w) / 2, cy), ch, font=f, fill=color)
            cy += size * (1 + gap)
        self.lines.append((np.array([[x, y], [x + size, y], [x + size, cy], [x, cy]], np.float32), s))

    def para(self, x, y, lines, key, size, color=(30, 30, 30), lead=1.45):
        for s in lines:
            self.text(x, y, s, key, size, color)
            y += size * lead
        return y


def place(card, dst_quad, out, seed, shadow=0.25, glare=0.0, glare_at=(0.6, 0.3)):
    """Warps the card onto out (float32 HxWx3) at dst_quad; returns the quads of its lines."""
    h, w = out.shape[:2]
    src = np.array([[0, 0], [card.w, 0], [card.w, card.h], [0, card.h]], np.float32)
    dst = np.array(dst_quad, np.float32)
    M = cv2.getPerspectiveTransform(src, dst)
    img = np.asarray(card.img).astype(np.float32)
    warped = cv2.warpPerspective(img, M, (w, h), flags=cv2.INTER_AREA if False else cv2.INTER_LINEAR)
    mask = cv2.warpPerspective(np.ones((card.h, card.w), np.float32), M, (w, h))
    # Soft drop shadow under the card.
    sh = cv2.GaussianBlur(cv2.warpPerspective(np.ones((card.h, card.w), np.float32), M, (w, h)), (0, 0), 18)
    out *= (1 - 0.45 * sh[..., None] * (1 - mask[..., None]))
    out[:] = out * (1 - mask[..., None]) + warped * mask[..., None]
    quads = []
    for q, s in card.lines:
        p = cv2.perspectiveTransform(q.reshape(-1, 1, 2), M).reshape(-1, 2)
        quads.append((p, s))
    return quads


def lighting(img, seed, shadow=0.3, glare=0.0, glare_at=(0.6, 0.3), glare_r=0.18):
    h, w = img.shape[:2]
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    a = rng.uniform(0, 2 * math.pi)
    grad = (np.cos(a) * (xx / w - 0.5) + np.sin(a) * (yy / h - 0.5))
    img *= (1 - shadow * (grad + 0.5))[..., None]
    if glare > 0:
        gx, gy = glare_at[0] * w, glare_at[1] * h
        r = glare_r * w
        g = np.exp(-(((xx - gx) / (r * 1.6)) ** 2 + ((yy - gy) / r) ** 2))
        img += (glare * 255 * g)[..., None]
    return img


def table(h, w, seed, kind="wood"):
    rng = np.random.default_rng(seed)
    if kind == "wood":
        base = np.array([92, 118, 150], np.float32)  # BGR-ish brown in RGB order below
        base = base[::-1]
        stripes = cv2.resize(rng.normal(0, 1, (h // 4, 6)).astype(np.float32), (w, h), interpolation=cv2.INTER_CUBIC)
        n = rng.normal(0, 1, (h, w)).astype(np.float32)
        img = base + (stripes * 14 + n * 4)[..., None]
    else:  # dark counter
        n = cv2.GaussianBlur(rng.normal(0, 1, (h, w)).astype(np.float32), (0, 0), 3)
        img = np.array([48, 46, 44], np.float32) + (n * 10)[..., None]
    return img.astype(np.float32)


# ---------------------------------------------------------------------------
# Scenes (each returns a list of cards with destination quads in the viewfinder)

def ja_food_label():
    c = Card(1500, 1250, (247, 240, 232), seed=11)
    c.rect(0, 0, 1500, 70, fill=(40, 22, 30))
    c.text(30, 14, "うすく焼き上げた香り高いカカオ風味の八ッ橋をコーティングチョコレートで仕上げました", "jab", 36,
           (236, 120, 160))
    c.rect(0, 70, 1500, 84, fill=(236, 140, 170))
    c.text(40, 108, "商品名：八ッ橋ショコラ", "ja", 34, (60, 50, 50))
    rows = [("名　　称", ["八ッ橋菓子"]),
            ("原材料名", ["チョコレートコーチング（タイ製造）（砂糖、食用精製加工油脂、ココアパウ",
                          "ダー）、砂糖、米粉、カカオマス、小麦粉、食用精製加工油脂、にっき／着色",
                          "料（カラメル）、膨張剤、乳化剤、香料、（一部に小麦・乳成分・大豆を含む）"]),
            ("内 容 量", ["8袋（16枚）"]),
            ("保存方法", ["直射日光、高温多湿な場所を避けて常温で保存してください。"]),
            ("製 造 者", ["株式会社美十　〒601-8446 京都市南区西九条高畠町35-2"])]
    y = 160
    c.rect(40, y, 1460, y + 470, outline=(70, 60, 60), width=3)
    for label, vals in rows:
        hgt = 26 + 46 * len(vals)
        c.text(58, y + (hgt - 34) / 2, label, "ja", 32, (40, 40, 40))
        c.d.line((250, y, 250, y + hgt), fill=(70, 60, 60), width=3)
        yy = y + 12
        for v in vals:
            c.text(268, yy, v, "ja", 32, (30, 30, 30))
            yy += 46
        y += hgt
        c.d.line((40, y, 1460, y), fill=(70, 60, 60), width=2)
    y += 24
    c.text(58, y, "製造所　〒919-1552　福井県三方上中郡若狭町若狭テクノバレー1-3-1", "ja", 30, (40, 40, 40))
    y += 64
    c.rect(58, y, 640, y + 110, fill=(30, 30, 30), radius=10)
    c.text(140, y + 10, "特定原材料および", "jab", 34, (245, 245, 245))
    c.text(140, y + 56, "それに準ずるもの", "jab", 34, (245, 245, 245))
    c.rect(680, y + 22, 960, y + 88, fill=(60, 30, 40), radius=8)
    c.text(700, y + 30, "小麦・乳・大豆", "jab", 36, (255, 255, 255))
    y += 140
    c.para(58, y, ["※本品製造工場では卵を含む製品を生産しています。",
                   "●賞味期限は未開封状態の期限です。開封後はお早めにお召し上がりください。",
                   "●チョコレート製品は温度にデリケートです。直射日光の当たらない涼しい場所に",
                   "保管してください。高温でやわらかくなったチョコレートは、冷えて固まると白く",
                   "なることがあります（ブルーム現象）。風味は劣りますが、お召し上がりいただけます。"],
           "ja", 31, (30, 30, 30), lead=1.5)
    return [(c, [[60, 120], [960, 150], [985, 840], [40, 860]])]


def ja_sign():
    c = Card(1100, 800, (252, 252, 250), seed=12)
    c.rect(0, 0, 1100, 150, fill=(200, 40, 40))
    c.text(330, 28, "お知らせ", "jab", 86, (255, 255, 255), spacing=12)
    c.text(90, 210, "本日は臨時休業とさせていただきます。", "jab", 50, (20, 20, 20))
    c.text(90, 300, "ご迷惑をおかけして申し訳ございません。", "ja", 46, (20, 20, 20))
    c.text(90, 400, "営業時間　10:00〜19:00（水曜定休）", "ja", 44, (20, 20, 20))
    c.text(90, 490, "店内でのご飲食はご遠慮ください。", "ja", 44, (20, 20, 20))
    c.rect(90, 590, 1010, 720, outline=(200, 40, 40), width=4)
    c.text(130, 625, "足元にご注意ください", "jab", 54, (200, 40, 40))
    return [(c, [[90, 240], [920, 190], [950, 820], [70, 860]])]


def ja_menu():
    c = Card(1000, 1300, (240, 226, 196), seed=13)
    c.text(330, 40, "お品書き", "jab", 80, (60, 30, 20), spacing=10)
    items = [("醤油ラーメン", "850円"), ("味噌ラーメン", "900円"), ("とんこつラーメン", "950円"),
             ("餃子（6個）", "400円"), ("唐揚げ定食", "980円"), ("ライス", "200円"), ("生ビール", "550円")]
    y = 200
    for n, p in items:
        c.text(90, y, n, "jab", 52, (40, 25, 15))
        c.text(720, y, p, "jab", 52, (40, 25, 15))
        c.d.line((90, y + 76, 910, y + 76), fill=(150, 120, 90), width=2)
        y += 120
    c.text(90, y + 20, "本日のおすすめ：季節の天ぷら盛り合わせ", "ja", 40, (150, 30, 20))
    c.text(90, y + 90, "大盛り無料・お子様メニューあり", "ja", 40, (40, 25, 15))
    return [(c, [[120, 110], [880, 140], [900, 1190], [80, 1170]])]


def ja_vertical():
    c = Card(900, 1200, (250, 246, 236), seed=14)
    c.rect(20, 20, 880, 1180, outline=(40, 40, 40), width=6)
    c.vtext(700, 90, "天ぷらそば", "jab", 120, (20, 20, 20))
    c.vtext(520, 90, "季節の野菜と海老", "ja", 70, (20, 20, 20))
    c.vtext(400, 90, "毎日手打ち", "ja", 70, (20, 20, 20))
    c.text(90, 1040, "一杯 1,200円（税込）", "jab", 56, (160, 30, 20))
    return [(c, [[150, 120], [860, 160], [840, 1160], [130, 1120]])]


def ko_food_label():
    c = Card(1500, 1150, (250, 248, 240), seed=21)
    c.rect(0, 0, 1500, 80, fill=(30, 60, 120))
    c.text(40, 14, "부드러운 초콜릿과 마시멜로가 어우러진 정(情) 초코파이", "kob", 40, (255, 255, 255))
    rows = [("제품명", "초코파이"), ("식품유형", "과자"), ("내용량", "468g(39g×12개)"),
            ("원재료명", "밀가루(밀:미국산), 백설탕, 쇼트닝, 물엿, 코코아분말, 전란액"),
            ("보관방법", "직사광선을 피하고 서늘하고 건조한 곳에 보관하십시오."),
            ("소비기한", "포장지 뒷면 별도 표기일까지")]
    y = 130
    for k, v in rows:
        c.rect(40, y, 1460, y + 82, outline=(60, 60, 60), width=2)
        c.rect(40, y, 280, y + 82, fill=(225, 228, 236), outline=(60, 60, 60), width=2)
        c.text(60, y + 20, k, "kob", 36, (30, 30, 30))
        c.text(300, y + 20, v, "ko", 36, (30, 30, 30))
        y += 82
    y += 40
    c.para(50, y, ["이 제품은 우유, 대두, 밀, 달걀을 사용한 제품과 같은 제조시설에서",
                   "제조하고 있습니다. 부정·불량식품 신고는 국번 없이 1399",
                   "본 제품은 공정거래위원회 고시 소비자분쟁해결기준에 의거",
                   "교환 또는 보상받을 수 있습니다."], "ko", 34, (30, 30, 30), lead=1.5)
    return [(c, [[70, 160], [950, 120], [980, 820], [50, 860]])]


def ko_sign():
    c = Card(1100, 760, (255, 214, 40), seed=22)
    c.rect(30, 30, 1070, 730, outline=(20, 20, 20), width=10)
    c.text(250, 70, "출입금지", "kob", 130, (20, 20, 20), spacing=20)
    c.text(110, 290, "관계자 외 출입을 금합니다", "kob", 66, (20, 20, 20))
    c.text(110, 410, "화장실은 2층에 있습니다", "ko", 58, (20, 20, 20))
    c.text(110, 520, "문을 꼭 닫아 주세요", "ko", 58, (20, 20, 20))
    return [(c, [[110, 250], [900, 280], [930, 830], [80, 790]])]


def ko_menu():
    c = Card(1000, 1240, (32, 32, 34), seed=23)
    c.text(360, 40, "메  뉴", "kob", 84, (250, 220, 120))
    items = [("김치찌개", "8,000원"), ("된장찌개", "8,000원"), ("비빔밥", "9,000원"), ("불고기 정식", "12,000원"),
             ("제육볶음", "10,000원"), ("공기밥 추가", "1,000원")]
    y = 210
    for n, p in items:
        c.text(90, y, n, "kob", 56, (245, 245, 245))
        c.text(680, y, p, "ko", 56, (245, 245, 245))
        y += 128
    c.text(90, y + 30, "모든 메뉴는 포장 가능합니다", "ko", 44, (250, 220, 120))
    return [(c, [[110, 120], [880, 100], [910, 1180], [90, 1200]])]


def en_label():
    c = Card(1400, 1150, (252, 252, 252), seed=31)
    c.text(50, 40, "Dark Chocolate Sea Salt Crackers", "enb", 56, (90, 40, 20))
    c.d.line((50, 120, 1350, 120), fill=(90, 40, 20), width=4)
    c.para(50, 150, ["INGREDIENTS: Wheat flour, sugar, cocoa mass, palm oil,",
                     "cocoa butter, sea salt, emulsifier (soy lecithin), natural",
                     "vanilla flavouring. Cocoa solids 55% minimum."], "en", 40, (20, 20, 20), lead=1.4)
    c.text(50, 360, "Allergy advice: contains wheat and soy. May contain milk.", "enb", 38, (20, 20, 20))
    c.text(50, 440, "Store in a cool, dry place away from direct sunlight.", "en", 40, (20, 20, 20))
    c.text(50, 510, "Best before: see bottom of pack.", "en", 40, (20, 20, 20))
    c.rect(50, 600, 900, 1080, outline=(20, 20, 20), width=4)
    c.text(70, 615, "Nutrition Facts", "enb", 54, (20, 20, 20))
    for i, (k, v) in enumerate([("Energy", "2180 kJ / 522 kcal"), ("Fat", "29 g"), ("of which saturates", "16 g"),
                                ("Carbohydrate", "55 g"), ("Protein", "7.1 g"), ("Salt", "0.9 g")]):
        y = 700 + i * 62
        c.text(70, y, k, "en", 38, (20, 20, 20))
        c.text(620, y, v, "en", 38, (20, 20, 20))
        c.d.line((60, y + 52, 890, y + 52), fill=(80, 80, 80), width=2)
    return [(c, [[60, 150], [940, 110], [960, 800], [40, 830]])]


def en_sign():
    c = Card(1000, 800, (250, 250, 250), seed=32)
    c.rect(0, 0, 1000, 200, fill=(240, 190, 20))
    c.text(260, 40, "CAUTION", "enb", 120, (20, 20, 20))
    c.text(120, 250, "Wet Floor", "enb", 96, (20, 20, 20))
    c.text(120, 400, "Please keep the door closed", "en", 54, (20, 20, 20))
    c.text(120, 490, "Opening hours: Mon-Fri 9am-6pm", "en", 54, (20, 20, 20))
    c.text(120, 590, "No smoking on these premises", "en", 54, (180, 20, 20))
    return [(c, [[120, 250], [880, 210], [900, 840], [100, 860]])]


def zhs_label():
    c = Card(1400, 1000, (250, 248, 244), seed=41)
    c.text(50, 40, "巧克力夹心饼干", "zhsb", 70, (120, 30, 30))
    c.para(50, 170, ["产品名称：巧克力夹心饼干", "配料：小麦粉、白砂糖、植物油、可可粉、食用盐",
                     "净含量：200克", "贮存条件：请置于阴凉干燥处，避免阳光直射",
                     "保质期：12个月", "生产日期：见包装底部", "致敏物质提示：含有小麦、大豆制品"],
           "zhs", 42, (30, 30, 30), lead=1.6)
    return [(c, [[70, 160], [930, 140], [950, 820], [60, 850]])]


SCENES = {
    "ja_food_label": (ja_food_label, "wood", dict(shadow=0.35, glare=0.45, glare_at=(0.62, 0.42)), 1.3),
    "ja_sign": (ja_sign, "dark", dict(shadow=0.25, glare=0.25, glare_at=(0.3, 0.3)), 1.0),
    "ja_menu": (ja_menu, "wood", dict(shadow=0.3, glare=0.0), 1.1),
    "ja_vertical": (ja_vertical, "dark", dict(shadow=0.3, glare=0.2, glare_at=(0.7, 0.7)), 1.0),
    "ko_food_label": (ko_food_label, "wood", dict(shadow=0.3, glare=0.4, glare_at=(0.3, 0.5)), 1.2),
    "ko_sign": (ko_sign, "dark", dict(shadow=0.2, glare=0.2, glare_at=(0.75, 0.35)), 1.0),
    "ko_menu": (ko_menu, "wood", dict(shadow=0.25, glare=0.15, glare_at=(0.5, 0.2)), 1.1),
    "en_label": (en_label, "wood", dict(shadow=0.3, glare=0.35, glare_at=(0.55, 0.45)), 1.2),
    "en_sign": (en_sign, "dark", dict(shadow=0.25, glare=0.0), 0.9),
    "zhs_label": (zhs_label, "wood", dict(shadow=0.3, glare=0.3, glare_at=(0.4, 0.4)), 1.1),
}

W, H = 994, 2160          # the owner's iPhone picture
VF = (0, 400, 994, 1725)  # camera viewfinder (3:4)


def camera_ui(img, lines):
    """iPhone camera app chrome (zh-Hant UI, like the owner's phone)."""
    pil = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(pil)
    f = font("zht", 34)
    fb = font("zhtb", 36)
    for x, s in [(320, ".5"), (430, "1"), (530, "2×"), (650, "4"), (760, "8")]:
        d.text((x, 1620), s, font=f, fill=(240, 240, 240))
    y = 1890
    for x, s, fo, col in [(330, "錄影", f, (235, 235, 235)), (470, "拍照", fb, (255, 204, 0)), (620, "人像", f, (235, 235, 235))]:
        d.text((x, y), s, font=fo, fill=col)
        bb = d.textbbox((x, y), s, font=fo)
        lines.append((np.array([[bb[0], bb[1]], [bb[2], bb[1]], [bb[2], bb[3]], [bb[0], bb[3]]], np.float32), s))
    d.ellipse((420, 1960, 574, 2114), outline=(255, 255, 255), width=8)
    d.ellipse((434, 1974, 560, 2100), fill=(255, 255, 255))
    return np.asarray(pil).astype(np.float32)


def clutter():
    """Objects around a label seen at 1x (camera further away): a snack box
    with a Latin brand + slogan and a katakana brand name, a sticker with a
    lone kanji word.  None of them should get a card."""
    box = Card(700, 420, (200, 30, 40), seed=51)
    box.text(60, 40, "Pocky", "enb", 150, (255, 255, 255))
    box.text(70, 230, "ポッキー", "jab", 80, (255, 235, 120))
    box.text(430, 340, "share happi", "en", 40, (255, 255, 255))
    tag = Card(360, 200, (240, 240, 235), seed=52)
    tag.text(40, 50, "福袋", "jab", 90, (180, 20, 20))
    return [(box, [[540, 1010], [960, 1060], [930, 1300], [500, 1250]]),
            (tag, [[40, 40], [250, 20], [270, 140], [50, 160]])]


def render(name, out_dir):
    base, scale = name, 1.0
    if name.endswith("_1x"):  # the same scene seen from further away (camera at 1x): label 30-40 % of the frame
        base, scale = name[:-3], 0.75
    fn, bg, light, blur = SCENES[base]
    seed = abs(hash(name)) % 1000 if False else sum(map(ord, base))
    vw, vh = VF[2] - VF[0], VF[3] - VF[1]
    view = table(vh, vw, seed, bg)
    quads = []
    scene = fn()
    if scale != 1.0:
        cx, cy = vw * 0.42, vh * 0.36
        scene = [(c, [[cx + (x - cx) * scale, cy + (y - cy) * scale] for x, y in q]) for c, q in scene] + clutter()
    for card, q in scene:
        quads += place(card, q, view, seed)
    view = lighting(view, seed + 1, **light)
    view = cv2.GaussianBlur(view, (0, 0), blur)
    rng = np.random.default_rng(seed + 2)
    view += rng.normal(0, 4.0, view.shape).astype(np.float32)
    full = np.zeros((H, W, 3), np.float32) + 8
    full[VF[1]:VF[3], VF[0]:VF[2]] = np.clip(view, 0, 255)
    lines = [(q + np.array([VF[0], VF[1]], np.float32), s) for q, s in quads]
    ui = []
    full = camera_ui(full, ui)
    lines += ui
    # JPEG round trip (the phone's video stream / photo compression).
    ok, enc = cv2.imencode(".jpg", cv2.cvtColor(np.clip(full, 0, 255).astype(np.uint8), cv2.COLOR_RGB2BGR),
                           [cv2.IMWRITE_JPEG_QUALITY, 62])
    dec = cv2.imdecode(enc, cv2.IMREAD_COLOR)
    cv2.imwrite(os.path.join(out_dir, name + ".png"), dec)
    with open(os.path.join(out_dir, name + ".gt.tsv"), "w", encoding="utf-8", newline="\n") as f:
        for q, s in lines:
            q = q / np.array([W, H], np.float32)
            x0, y0 = q.min(0)
            x1, y1 = q.max(0)
            f.write("%.4f %.4f %.4f %.4f %s\t%s\n" % (x0, y0, x1, y1, " ".join("%.4f" % v for v in q.reshape(-1)), s))
    print(os.path.join(out_dir, name + ".png"), len(lines), "lines")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "photos"
    os.makedirs(out, exist_ok=True)
    for n in (sys.argv[2:] or list(SCENES) + [k + "_1x" for k in ("ja_food_label", "ko_food_label", "en_label", "ja_menu")]):
        render(n, out)
