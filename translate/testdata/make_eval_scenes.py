# Translation-quality evaluation scenes for pm_translate (12 scenes: ja/ko/en x
# label, manual, menu-or-sign, app screen).
#
#   py -3 translate/testdata/make_eval_scenes.py OUTDIR [scene ...]
#
# Writes into OUTDIR, per scene NAME:
#   NAME.png        photo-like scenes (labels, manuals, menus, signs) go through the
#                   same camera path as make_photos.py (table, perspective, light,
#                   blur, noise, JPEG, iPhone camera frame 994x2160) but with a mild
#                   tilt and large text; app scenes are flat 1170x2532 screenshots.
#   NAME.gt.tsv     one line per rendered text line, same format as make_photos.py:
#                   "x0 y0 x1 y1 qx0 qy0 .. qy3<TAB>text" (0..1 of the picture).
#   NAME.ref.json   the translation spec (also written to translate/testdata/eval/,
#                   which is the committed copy).  Units are the pieces a reader
#                   needs (table row, sentence, menu item, UI row / button); every
#                   text line of the picture belongs to exactly one unit; unit "src"
#                   = its GT lines joined in reading order (ja/ko: no separator,
#                   en: one space; a wrapped Korean line keeps its trailing space so
#                   that "".join() restores the sentence).  "ref" is the human zh-Hant
#                   (Taiwan) reference, "must" = groups of acceptable words (one word
#                   of every group must appear), "keys" = critical facts (neg / num /
#                   date / phone / price / temp / time / pct) with the accepted
#                   spellings in "any"; "optional" = text a reader need not translate
#                   (brand logo, status-bar clock, camera-app UI).
# The spec is generated from the same code that draws the text, so src never drifts.
# Deterministic (fixed seeds).  Needs numpy, opencv, Pillow, Windows fonts.
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cv2  # noqa: E402
import numpy as np  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

import make_photos as mp  # noqa: E402

EVAL_DIR = os.path.join(HERE, "eval")
INK = (30, 30, 30)

# ---------------------------------------------------------------------------
# accepted-spelling helpers


def uniq(seq):
    out = []
    for x in seq:
        if x not in out:
            out.append(x)
    return out


def fm(x, *units):
    out = []
    for u in units:
        out += ["%s%s" % (x, u), "%s %s" % (x, u)]
    return uniq(out)


def rng(a, b, us=("°C", "℃"), extra=()):
    seps = ["~", "～", "〜", "-", "–", "—", "至", "到", " ~ ", " - ", " 至 ", " 到 ", " – "]
    out = []
    for u1 in ("",) + tuple(us):
        for u2 in (us or ("",)):
            for sp in seps:
                out.append("%s%s%s%s%s" % (a, u1, sp, b, u2))
    return uniq(out + list(extra))


def G(x): return fm(x, "g", "G", "公克", "克")
def MG(x): return fm(x, "mg", "毫克")
def ML(x): return fm(x, "mL", "ml", "毫升", "cc")
def KCAL(x): return fm(x, "kcal", "Kcal", "大卡", "千卡", "卡", "Cal", "卡路里")
def PCT(x): return uniq(["%s%%" % x, "%s %%" % x, "%s％" % x])
def VOLT(x): return fm(x, "V", "伏", "伏特")
def AMP(x): return fm(x, "A", "安", "安培")
def WATT(x): return fm(x, "W", "瓦")
def HZ(x): return fm(x, "Hz", "hz", "赫茲", "赫")


def _price(n, units, prefixes):
    out = []
    for s in uniq(["{:,}".format(n), str(n)]):
        for u in units:
            out += ["%s%s" % (s, u), "%s %s" % (s, u)]
        for p in prefixes:
            out.append("%s%s" % (p, s))
    return out


def JPY(n): return _price(n, ("日圓", "日元", "日幣", "圓", "元", "円", "JPY", "yen"), ("¥", "JPY "))
def KRW(n): return _price(n, ("韓元", "韓圜", "韓幣", "圜", "元", "원", "won", "KRW"), ("₩", "KRW "))


NEG = ["不", "勿", "別", "禁止", "避免", "不要", "不可", "無法", "未", "沒有", "不得", "嚴禁", "切勿"]
NEG_AVOID = ["避免", "避開", "遠離", "勿", "不要", "不可", "禁止", "別", "不得", "請勿"]


def K(type_, src, any_):
    return {"type": type_, "src": src, "any": list(any_)}


def S(ref, must, keys=(), opt=False):
    return {"ref": ref, "must": must, "keys": list(keys), "opt": opt}


# ---------------------------------------------------------------------------
# text wrapping

NOHEAD = set("、。，．,.)）」』】!?！？ー〜～・:：;；")
OPEN = set("「『（(【")


def split_lines(s, f, maxw, kind):
    if kind == "en":
        lines, cur = [], ""
        for w in s.split(" "):
            t = (cur + " " + w) if cur else w
            if cur and f.getlength(t) > maxw:
                lines.append(cur)
                cur = w
            else:
                cur = t
        return lines + ([cur] if cur else [])
    if kind == "ko":
        words = s.split(" ")
        lines, cur = [], ""
        for i, w in enumerate(words):
            tok = w + (" " if i < len(words) - 1 else "")
            if cur and f.getlength((cur + tok).rstrip()) > maxw:
                lines.append(cur)
                cur = tok
            else:
                cur += tok
        return lines + ([cur] if cur else [])
    lines, cur = [], ""
    for c in s:
        if cur and f.getlength(cur + c) > maxw and c not in NOHEAD:
            if cur[-1] in OPEN:
                lines.append(cur[:-1])
                cur = cur[-1] + c
            else:
                lines.append(cur)
                cur = c
        else:
            cur += c
    return lines + ([cur] if cur else [])


class Sc(mp.Card):
    """mp.Card that knows which unit every text line belongs to."""

    def __init__(self, w, h, paper=(246, 242, 232), seed=0, lang="ja", flat=None):
        super().__init__(w, h, paper, seed)
        if flat is not None:
            self.img = Image.new("RGB", (w, h), flat)
            self.d = ImageDraw.Draw(self.img)
        self.lang = lang
        self.units = []
        self.tags = []
        self.cur = None

    def u(self, ref, must, keys=(), opt=False):
        self.cur = "u%d" % (len(self.units) + 1)
        self.units.append({"id": self.cur, "ref": ref, "must": must, "keys": list(keys), "optional": opt})

    def text(self, x, y, s, key, size, color=INK, gt=True, spacing=0):
        x1 = super().text(x, y, s, key, size, color, gt, spacing)
        if gt:
            assert self.cur, s
            self.tags.append(self.cur)
        return x1

    def wrap(self, x, y, s, key, size, maxw, color=INK, lead=1.4, cx=None):
        f = mp.font(key, size)
        for ln in split_lines(s, f, maxw, self.lang):
            t = ln.rstrip()
            xx = x if cx is None else cx - f.getlength(t) / 2
            self.text(xx, y, t, key, size, color)
            if t != ln:  # Korean: keep the word space in the GT text
                q, _ = self.lines[-1]
                self.lines[-1] = (q, ln)
            y += size * lead
        return y

    def ctext(self, y, s, key, size, color=INK, spacing=0, cx=None):
        f = mp.font(key, size)
        w = sum(f.getlength(c) for c in s) + spacing * (len(s) - 1) if spacing else f.getlength(s)
        self.text((cx if cx is not None else self.w / 2) - w / 2, y, s, key, size, color, spacing=spacing)

    def rtext(self, xr, y, s, key, size, color=INK):
        self.text(xr - mp.font(key, size).getlength(s), y, s, key, size, color)

    def trow(self, y, label, value, size=30, lkey="ja", vkey="ja", x0=40, xs=270, x1=960, minh=62,
             col=(80, 65, 55), lfill=(238, 230, 214)):
        f = mp.font(vkey, size)
        n = len(split_lines(value, f, x1 - xs - 32, self.lang))
        h = max(minh, int(26 + n * size * 1.4))
        self.rect(x0, y, xs, y + h, fill=lfill)
        self.text(x0 + 16, y + (h - size * 1.25) / 2, label, lkey, size)
        self.wrap(xs + 16, y + (h - n * size * 1.4) / 2 + 3, value, vkey, size, x1 - xs - 32)
        self.d.rectangle((x0, y, x1, y + h), outline=col, width=2)
        self.d.line((xs, y, xs, y + h), fill=col, width=2)
        return y + h

    def tri(self, x, y, sz, color):
        self.d.polygon([(x + sz / 2, y), (x + sz, y + sz * 0.88), (x, y + sz * 0.88)], fill=color)
        f = mp.font("enb", int(sz * 0.6))
        self.d.text((x + sz / 2 - f.getlength("!") / 2, y + sz * 0.2), "!", font=f, fill=(255, 255, 255))

    # ---- app-screen parts -------------------------------------------------
    def status_bar(self):
        self.u("9:41", [], opt=True)
        self.text(90, 36, "9:41", "enb", 46, (10, 10, 10))
        for i in range(4):
            self.rect(870 + i * 24, 82 - (i + 1) * 10, 886 + i * 24, 82, fill=(10, 10, 10))
        self.rect(992, 48, 1080, 86, outline=(10, 10, 10), width=3, radius=11)
        self.rect(998, 54, 1066, 80, fill=(10, 10, 10), radius=6)

    def nav(self, title, key):
        self.ctext(130, title, key, 52, (10, 10, 10))
        self.d.line((0, 222, self.w, 222), fill=(200, 200, 205), width=2)

    def srow(self, y, title, tkey, sub=None, skey=None, toggle=None, value=None, tcolor=(20, 20, 20),
             size=44, ssize=36):
        sl = split_lines(sub, mp.font(skey, ssize), 760, self.lang) if sub else []
        h = int(40 + size * 1.25 + (10 + len(sl) * ssize * 1.4 if sl else 0) + 36)
        self.rect(0, y, self.w, y + h, fill=(255, 255, 255))
        self.d.line((60, y + h, self.w, y + h), fill=(222, 222, 227), width=2)
        ty = y + 38
        self.text(60, ty, title, tkey, size, tcolor)
        if value:
            self.rtext(1110, ty + 2, value, tkey, size - 4, (130, 130, 135))
        if sub:
            self.wrap(60, ty + size * 1.25 + 8, sub, skey, ssize, 760, (120, 120, 125), 1.4)
        if toggle is not None:
            cy, x = y + h / 2, 960
            self.rect(x, cy - 36, x + 120, cy + 36, fill=(52, 199, 89) if toggle else (205, 205, 210), radius=36)
            k = x + 84 if toggle else x + 36
            self.d.ellipse((k - 30, cy - 30, k + 30, cy + 30), fill=(255, 255, 255))
        return y + h

    def dim(self, a):
        self.img = Image.blend(self.img, Image.new("RGB", (self.w, self.h), (0, 0, 0)), a)
        self.d = ImageDraw.Draw(self.img)

    def dialog(self, y, parts, btns, w=880):
        """parts: (text, key, size, color, S-dict); btns: (label, key, color, S-dict)."""
        x0, pad, cx = (self.w - w) // 2, 50, self.w / 2
        meas, h = [], pad
        for t, key, sz, col, uk in parts:
            ls = split_lines(t, mp.font(key, sz), w - 2 * pad, self.lang)
            meas.append(ls)
            h += int(len(ls) * sz * 1.4) + 18
        h += 20 + 130
        self.rect(x0, y, x0 + w, y + h, fill=(250, 250, 252), radius=40)
        cy = y + pad
        for t, key, sz, col, uk in parts:
            self.u(uk["ref"], uk["must"], uk["keys"], uk["opt"])
            cy = self.wrap(x0 + pad, cy, t, key, sz, w - 2 * pad, col, 1.4, cx=cx) + 18
        by = y + h - 130
        self.d.line((x0, by, x0 + w, by), fill=(200, 200, 205), width=2)
        bw = w / len(btns)
        for i, (lab, key, col, uk) in enumerate(btns):
            if i:
                self.d.line((x0 + i * bw, by, x0 + i * bw, y + h), fill=(200, 200, 205), width=2)
            self.u(uk["ref"], uk["must"], uk["keys"], uk["opt"])
            f = mp.font(key, 46)
            self.text(x0 + i * bw + (bw - f.getlength(lab)) / 2, by + 40, lab, key, 46, col)
        return y + h

    def toast(self, y, t, key, size=42):
        w = mp.font(key, size).getlength(t) + 110
        x0 = (self.w - w) / 2
        self.rect(x0, y, x0 + w, y + 112, fill=(50, 50, 54), radius=56)
        self.text(x0 + 55, y + 30, t, key, size, (255, 255, 255))


# ---------------------------------------------------------------------------
# photo scenes: card 1000x1320 -> viewfinder 994x1325 with a mild tilt

Q1 = [[24, 24], [966, 8], [982, 1192], [12, 1204]]
Q2 = [[14, 12], [975, 28], [966, 1198], [26, 1184]]
Q3 = [[30, 18], [960, 10], [978, 1186], [16, 1200]]
Q4 = [[18, 10], [978, 22], [970, 1200], [22, 1190]]


def ja_label_cupnoodle():
    s = Sc(1000, 1320, (248, 242, 226), seed=101, lang="ja")
    s.rect(0, 0, 1000, 104, fill=(196, 40, 36))
    s.u("濃郁醬油拉麵", [["醬油"], ["拉麵"]])
    s.text(40, 20, "濃厚醤油ラーメン", "jab", 60, (255, 255, 255))
    y = 126
    s.u("名稱：即食杯麵", [["即食", "速食", "泡麵"], ["杯麵", "碗麵", "杯裝麵", "杯"]])
    y = s.trow(y, "名称", "即席カップめん")
    s.u("原料名稱：油炸麵（小麥粉、植物油脂、食鹽）、湯包（醬油、豬油、砂糖、香辛料）、配料（蔥、調味豬肉）（部分含有小麥、大豆、豬肉）",
        [["小麥"], ["醬油"], ["豬"]])
    y = s.trow(y, "原材料名", "油揚げめん（小麦粉、植物油脂、食塩）、スープ（しょうゆ、豚脂、砂糖、香辛料）、かやく（ねぎ、味付豚肉）（一部に小麦・大豆・豚肉を含む）")
    s.u("內容量：78公克（麵量65公克）", [["內容量", "容量", "淨重", "重量", "內容物"]],
        [K("num", "78g", G("78")), K("num", "65g", G("65"))])
    y = s.trow(y, "内容量", "78g（めん65g）")
    s.u("賞味期限：標示於包裝外框上方", [["賞味期限", "有效期限", "保存期限", "賞味期", "期限"], ["標示", "標註", "標明", "記載", "印"]])
    y = s.trow(y, "賞味期限", "枠外上部に記載")
    s.u("保存方法：請避免陽光直射，於常溫下保存。", [["避免", "避開", "遠離", "勿", "不要"], ["常溫"], ["保存", "存放", "貯存"]],
        [K("neg", "避け", NEG_AVOID)])
    y = s.trow(y, "保存方法", "直射日光を避け、常温で保存してください。")
    y += 14
    s.u("營養標示（每份78公克）", [["營養"], ["每份", "每一份", "一份", "1份", "每1份", "每食"]], [K("num", "78g", G("78"))])
    s.text(40, y, "栄養成分表示（1食（78g）当たり）", "jab", 34, (196, 40, 36))
    y += 54
    for lab, val, any_, must in [("エネルギー", "338kcal", KCAL("338"), ["熱量", "能量"]),
                                 ("たんぱく質", "7.2g", G("7.2"), ["蛋白質"]),
                                 ("脂質", "14.8g", G("14.8"), ["脂質", "脂肪"]),
                                 ("炭水化物", "43.6g", G("43.6"), ["碳水化合物", "碳水"]),
                                 ("食塩相当量", "5.9g", G("5.9"), ["食鹽", "鹽", "鈉"])]:
        zh = {"エネルギー": "熱量", "たんぱく質": "蛋白質", "脂質": "脂質", "炭水化物": "碳水化合物", "食塩相当量": "食鹽相當量"}[lab]
        s.u("%s %s" % (zh, val.replace("g", "公克").replace("kcal", "大卡")), [must], [K("num", val, any_)])
        y = s.trow(y, lab, val, size=31, xs=520, minh=46)
    y += 16
    s.u("調理方法：① 將蓋子掀開至虛線處，倒入粉末湯包，注入熱水至內側線，蓋上蓋子等待3分鐘。",
        [["蓋"], ["熱水", "沸水", "開水"], ["虛線", "點線", "線"], ["等", "靜置", "燜", "3分"]],
        [K("num", "3分", ["3分鐘", "3 分鐘", "三分鐘", "3分", "三分"])])
    s.text(40, y, "調理方法", "jab", 34, (196, 40, 36))
    y += 54
    y = s.wrap(40, y, "① ふたを点線まで開けて粉末スープを入れ、内側の線まで熱湯を注ぎ、ふたをして3分待ちます。", "ja", 31, 920, lead=1.45)
    y += 14
    s.rect(30, y, 970, y + 140, outline=(196, 40, 36), width=4)
    s.u("請小心，勿被熱水燙傷。", [["燙傷", "燙到", "燒傷", "燙"], ["熱水", "沸水", "開水", "熱湯"]],
        [K("neg", "やけどをしないよう", NEG + ["小心", "注意", "以免", "謹防"])])
    s.text(54, y + 18, "熱湯でやけどをしないようご注意ください。", "jab", 32, (196, 40, 36))
    s.u("請勿使用微波爐烹調。", [["微波"], ["調理", "烹調", "加熱", "烹煮", "料理", "泡"]], [K("neg", "しないでください", NEG)])
    s.text(54, y + 78, "電子レンジで調理しないでください。", "jab", 32, (196, 40, 36))
    return s, "wood", dict(shadow=0.22, glare=0.15, glare_at=(0.7, 0.3)), 0.85, Q1, 101, "label"


def ja_manual():
    s = Sc(1000, 1320, (250, 250, 246), seed=102, lang="ja")
    s.u("行動電源 使用說明書", [["行動電源", "充電寶", "移動電源"], ["說明書", "使用手冊", "操作手冊"]])
    s.ctext(44, "モバイルバッテリー", "jab", 44, (40, 40, 40))
    s.ctext(104, "取扱説明書", "jab", 72, (20, 20, 20))
    s.d.line((60, 206, 940, 206), fill=(40, 40, 40), width=5)
    y = 236
    s.rect(50, y, 950, y + 330, outline=(200, 30, 30), width=5)
    s.tri(76, y + 22, 64, (200, 30, 30))
    s.u("警告", [["警告"]])
    s.text(160, y + 28, "警告", "jab", 50, (200, 30, 30))
    items = [("分解・改造しないでください。", "請勿拆解或改裝。", [["拆解", "拆開", "拆卸", "分解"], ["改裝", "改造"]], "しないでください"),
             ("ぬれた手で触らないでください。", "請勿用濕手觸摸。", [["濕"], ["手"], ["摸", "觸", "碰"]], "触らないでください"),
             ("充電中は布などで覆わないでください。", "充電時請勿用布等物品覆蓋。", [["充電"], ["布"], ["覆蓋", "蓋住", "遮蓋", "蓋", "覆"]], "覆わないでください")]
    iy = y + 116
    for t, ref, must, ks in items:
        s.u(ref, must, [K("neg", ks, NEG)])
        s.d.ellipse((76, iy + 14, 90, iy + 28), fill=(200, 30, 30))
        s.text(108, iy, t, "ja", 35)
        iy += 66
    y = 606
    s.rect(50, y, 950, y + 236, outline=(230, 140, 20), width=5)
    s.tri(76, y + 18, 56, (230, 140, 20))
    s.u("注意", [["注意"]])
    s.text(160, y + 22, "注意", "jab", 46, (210, 120, 10))
    s.u("請勿放置於陽光直射的車內等高溫場所。", [["高溫"], ["車內", "車裡", "車上"], ["放置", "放", "擺放", "留"]],
        [K("neg", "置かないでください", NEG)])
    s.wrap(80, y + 98, "直射日光の当たる車内など高温になる場所に置かないでください。", "ja", 34, 820, lead=1.45)
    y = 880
    s.u("使用溫度 0～40°C", [["使用溫度", "操作溫度", "適用溫度", "使用環境溫度", "工作溫度", "使用環境"]], [K("temp", "0〜40℃", rng("0", "40"))])
    y = s.trow(y, "使用温度", "0〜40℃", size=34, x0=50, xs=330, x1=950, minh=72)
    s.u("額定輸出 5V 2A", [["額定"], ["輸出", "輸入", "電壓", "電流"]], [K("num", "5V", VOLT("5")), K("num", "2A", AMP("2"))])
    y = s.trow(y, "定格出力", "5V 2A", size=34, x0=50, xs=330, x1=950, minh=72)
    s.u("洽詢專線 0120-555-012", [["洽詢", "諮詢", "詢問", "聯絡", "客服", "服務"]],
        [K("phone", "0120-555-012", ["0120-555-012", "0120555012", "0120 555 012"])])
    y = s.trow(y, "お問い合わせ", "0120-555-012", size=34, x0=50, xs=330, x1=950, minh=72)
    s.u("受理時間 9:00～17:00（週六、週日及國定假日除外）", [["受理", "服務時間", "接聽", "營業時間", "受付", "客服時間"], ["週六", "星期六", "六日", "週末", "假日", "週日", "星期日", "例假日", "國定假日"]],
        [K("time", "9:00〜17:00", rng("9:00", "17:00", ("",), ["上午9點", "9點", "9時", "09:00", "9:00"]))])
    y = s.trow(y, "受付時間", "9:00〜17:00（土日祝日を除く）", size=34, x0=50, xs=330, x1=950, minh=72)
    return s, "dark", dict(shadow=0.22, glare=0.12, glare_at=(0.3, 0.6)), 0.85, Q2, 102, "manual"


def ja_menu_izakaya():
    s = Sc(1000, 1320, (234, 216, 182), seed=103, lang="ja")
    s.rect(60, 34, 300, 100, fill=(170, 30, 30), radius=10)
    s.u("居酒屋 ひので", [], opt=True)
    s.text(84, 48, "居酒屋 ひので", "jab", 32, (255, 255, 255))
    s.u("菜單", [["菜單"]])
    s.ctext(70, "お品書き", "jab", 78, (60, 30, 20), spacing=12)
    items = [("枝豆", 380, "毛豆", [["毛豆"]], []),
             ("焼き鳥盛り合わせ（5本）", 780, "綜合烤雞肉串（5串）", [["烤雞", "雞肉串", "烤串", "燒鳥", "串燒", "雞串"], ["綜合", "拼盤", "盛合", "什錦", "拼"]],
              [K("num", "5本", ["5串", "5支", "5根", "5條", "五串", "5隻", "5本", "五支"])]),
             ("刺身三点盛り", 1280, "生魚片三點拼盤", [["生魚片", "刺身"], ["拼盤", "盛合", "三種", "3種", "三款", "三點", "綜合", "三樣"]], []),
             ("鶏の唐揚げ", 680, "日式炸雞", [["炸雞", "唐揚", "炸"]], []),
             ("だし巻き玉子", 580, "高湯玉子燒", [["玉子燒", "蛋捲", "蛋卷", "煎蛋", "高湯蛋", "厚蛋燒", "蛋"]], []),
             ("冷奴", 350, "涼拌豆腐", [["豆腐"]], []),
             ("生ビール（中）", 590, "生啤酒（中杯）", [["啤酒"], ["中"]], []),
             ("日本酒（一合）", 720, "日本清酒（一合）", [["清酒", "日本酒"], ["一合", "1合", "180毫升", "一杯", "合"]], [])]
    y = 190
    for name, price, zh, must, ks in items:
        s.u("%s %s日圓" % (zh, "{:,}".format(price)), must, ks + [K("price", "{:,}円".format(price), JPY(price))])
        s.text(80, y, name, "jab", 38, (40, 25, 15))
        s.rtext(920, y, "{:,}円".format(price), "jab", 38, (40, 25, 15))
        s.d.line((80, y + 64, 920, y + 64), fill=(150, 120, 90), width=2)
        y += 90
    y += 14
    s.u("※價格均已含稅", [["含稅", "稅內", "內含稅", "稅込"]])
    s.text(80, y, "※価格はすべて税込です", "ja", 32, (150, 30, 20))
    y += 56
    s.u("酌收小菜費（お通し）300日圓", [["小菜", "前菜", "開胃菜", "お通し", "座位", "桌邊"], ["收", "酌收", "加收", "另收", "需", "計"]],
        [K("price", "300円", JPY(300))])
    s.text(80, y, "お通し代 300円をいただきます", "ja", 32, (150, 30, 20))
    y += 56
    s.u("未滿20歲者，恕不提供酒類。", [["未滿20歲", "20歲以下", "未滿二十歲", "未成年", "二十歲未滿", "20歲未滿"], ["酒"], ["不提供", "恕不", "不供應", "不販售", "不得提供", "不會提供", "不售"]],
        [K("neg", "いたしません", NEG + ["恕不", "不提供", "不供應", "拒絕", "不會"]),
         K("num", "20歳", ["20歲", "二十歲", "20 歲"])])
    s.text(80, y, "20歳未満の方への酒類の提供はいたしません", "ja", 32, (150, 30, 20))
    y += 56
    s.u("最後點餐時間 23:00", [["最後點餐", "最後點單", "最後下單", "最終點餐", "點餐截止", "點餐時間"]],
        [K("time", "23:00", ["23:00", "23點", "晚上11點", "下午11點", "晚間11點", "23時", "晚上十一點"])])
    s.text(80, y, "ラストオーダー 23:00", "ja", 32, (150, 30, 20))
    return s, "wood", dict(shadow=0.25, glare=0.1, glare_at=(0.5, 0.2)), 0.85, Q3, 103, "menu"


def ja_sign_station():
    s = Sc(1000, 1320, (250, 250, 247), seed=104, lang="ja")
    s.rect(0, 0, 1000, 112, fill=(30, 80, 160))
    s.u("公告", [["公告", "通知", "告示", "提醒", "須知"]])
    s.ctext(18, "お知らせ", "jab", 70, (255, 255, 255), spacing=14)
    s.rect(24, 134, 976, 394, fill=(255, 214, 40), outline=(20, 20, 20), width=8)
    s.u("請勿衝刺搶上車", [["衝", "搶", "趕", "奔"], ["上車", "乘車", "搭車", "登車"]], [K("neg", "おやめください", NEG_AVOID + ["請不要"])])
    s.ctext(176, "駆け込み乗車は", "jab", 76, (190, 20, 20))
    s.ctext(282, "おやめください", "jab", 76, (190, 20, 20))
    s.rect(24, 424, 976, 548, outline=(30, 80, 160), width=6)
    s.u("月台上請勿奔跑", [["月台"], ["奔跑", "跑"]], [K("neg", "走らないでください", NEG)])
    s.ctext(462, "ホームでは走らないでください", "jab", 50, (20, 20, 20))
    s.rect(24, 578, 976, 704, outline=(200, 30, 30), width=8)
    s.u("前方 非相關人員禁止進入", [["前方", "前面", "往前", "此處往前"], ["相關人員", "工作人員", "相關人士", "關係人員", "閒雜人"], ["禁止", "不得", "嚴禁", "謝絕"]],
        [K("neg", "立入禁止", ["禁止", "不得", "嚴禁", "勿", "不可", "謝絕", "不准"])])
    s.ctext(618, "この先 関係者以外立入禁止", "jab", 54, (200, 30, 30))
    s.rect(24, 734, 976, 944, fill=(200, 30, 30))
    s.u("禁止穿鞋入內", [["鞋"], ["禁止", "嚴禁", "勿", "不可", "不得"]], [K("neg", "厳禁", ["禁止", "嚴禁", "請勿", "不可", "不得", "不准"])])
    s.ctext(770, "土足厳禁", "jab", 130, (255, 255, 255), spacing=20)
    s.rect(24, 974, 976, 1150, outline=(20, 20, 20), width=5)
    s.u("營業時間 10:00～20:00", [["營業時間", "營業", "開放時間", "服務時間"]],
        [K("time", "10:00〜20:00", rng("10:00", "20:00", ("",), ["上午10點", "10點", "10時"]))])
    s.text(70, 992, "営業時間 10:00〜20:00", "jab", 48, (20, 20, 20))
    s.d.line((24, 1062, 976, 1062), fill=(20, 20, 20), width=3)
    s.u("公休日 星期三", [["公休", "休息", "休館", "固定休", "定休", "公休日"], ["星期三", "週三", "禮拜三", "周三"]],
        [K("date", "水曜日", ["星期三", "週三", "禮拜三", "周三"])])
    s.text(70, 1080, "定休日 水曜日", "jab", 48, (20, 20, 20))
    s.u("遺失物品請洽詢站務人員", [["遺失", "失物", "遺忘", "忘記", "忘了"], ["站務", "車站人員", "站員", "車站工作人員", "服務台", "站務員", "站務人員"]])
    s.wrap(40, 1190, "忘れ物のお問い合わせは駅係員までお願いします。", "ja", 36, 920, lead=1.4)
    return s, "dark", dict(shadow=0.2, glare=0.15, glare_at=(0.75, 0.25)), 0.85, Q4, 104, "sign"


def ko_label_snack():
    s = Sc(1000, 1320, (252, 249, 238), seed=201, lang="ko")
    s.rect(0, 0, 1000, 112, fill=(240, 180, 30))
    s.u("蜂蜜奶油洋芋片", [["蜂蜜"], ["奶油"], ["洋芋片", "薯片", "馬鈴薯片"]])
    s.ctext(22, "허니버터 감자칩", "kob", 62, (80, 30, 10))
    s.u("營養資訊", [["營養"]])
    s.text(50, 138, "영양정보", "kob", 38, (30, 30, 30))
    y = 192
    rows = [("총 내용량", "120g", "", "總內容量", "120公克", G("120"), None, ["總內容量", "內容量", "總重量", "淨重", "總量"]),
            ("열량", "520kcal", "", "熱量", "520大卡", KCAL("520"), None, ["熱量", "能量"]),
            ("나트륨", "350mg", "18%", "鈉", "350毫克 18%", MG("350"), "18", ["鈉"]),
            ("탄수화물", "62g", "19%", "碳水化合物", "62公克 19%", G("62"), "19", ["碳水化合物", "碳水"]),
            ("당류", "12g", "12%", "糖類", "12公克 12%", G("12"), "12", ["糖類", "糖"]),
            ("단백질", "6g", "11%", "蛋白質", "6公克 11%", G("6"), "11", ["蛋白質"])]
    for lab, val, pct, zhl, zhv, any_, p, must in rows:
        ks = [K("num", val, any_)]
        if p:
            ks.append(K("pct", pct, PCT(p)))
        s.u("%s %s" % (zhl, zhv), [must], ks)
        s.text(60, y + 8, lab, "kob", 33)
        s.text(520, y + 8, val, "ko", 33)
        if pct:
            s.rtext(940, y + 8, pct, "ko", 33)
        s.d.line((40, y + 58, 960, y + 58), fill=(120, 120, 120), width=2)
        y += 60
    s.d.line((40, 184, 960, 184), fill=(30, 30, 30), width=5)
    s.u("占每日營養素參考值的百分比", [["每日", "一日", "1日", "每天"], ["參考值", "基準值", "基準", "標準", "營養素"], ["比例", "百分比", "比率", "%", "占", "佔"]])
    s.text(50, y + 12, "1일 영양성분 기준치에 대한 비율", "ko", 31, (60, 60, 60))
    y += 76
    s.u("原料名稱：馬鈴薯（國產）、棕櫚油（馬來西亞產）、砂糖、精製鹽、奶油粉、蜂蜜粉", [["馬鈴薯"], ["棕櫚"], ["蜂蜜", "蜜"]])
    y = s.trow(y, "원재료명", "감자(국산), 팜유(말레이시아산), 설탕, 정제소금, 버터분말, 꿀분말", size=31, lkey="kob", vkey="ko", xs=250, minh=70,
               x1=960, lfill=(244, 232, 190))
    s.u("食用期限 至2027.03.15", [["食用期限", "有效期限", "保存期限", "期限", "消費期限", "有效日期"]],
        [K("date", "2027.03.15", ["2027.03.15", "2027/03/15", "2027-03-15", "2027年3月15日", "2027年03月15日", "2027.3.15", "2027/3/15"])])
    y = s.trow(y, "소비기한", "2027.03.15까지", size=31, lkey="kob", vkey="ko", xs=250, minh=70, lfill=(244, 232, 190))
    s.u("保存方法：請避免陽光直射，存放於陰涼處。", [["避免", "避開", "遠離", "勿"], ["陰涼", "涼爽", "涼處", "低溫", "涼"], ["保存", "存放", "保管", "貯存", "儲存"]],
        [K("neg", "피하고", NEG_AVOID)])
    y = s.trow(y, "보관방법", "직사광선을 피하고 서늘한 곳에 보관하십시오", size=31, lkey="kob", vkey="ko", xs=250, minh=70, lfill=(244, 232, 190))
    y += 20
    s.rect(40, y, 960, y + 170, outline=(200, 40, 40), width=4)
    s.u("本產品與使用花生、大豆的產品在同一製造設施中生產。", [["花生"], ["大豆", "黃豆"], ["同一", "相同", "同樣", "共用", "同"], ["設施", "工廠", "廠房", "產線", "生產線", "場所"]])
    s.wrap(64, y + 18, "이 제품은 땅콩, 대두를 사용한 제품과 같은 제조시설에서 제조하고 있습니다", "ko", 33, 872, lead=1.45)
    return s, "wood", dict(shadow=0.22, glare=0.15, glare_at=(0.3, 0.5)), 0.85, Q1, 201, "label"


def ko_manual():
    s = Sc(1000, 1320, (248, 248, 250), seed=202, lang="ko")
    s.u("無線加濕器 使用說明書", [["加濕器", "增濕器", "水氧機", "加溼器"], ["說明書", "使用手冊", "操作手冊"]])
    s.ctext(44, "무선 가습기", "kob", 50, (40, 40, 40))
    s.ctext(112, "사용 설명서", "kob", 74, (20, 20, 20))
    s.d.line((60, 220, 940, 220), fill=(40, 40, 40), width=5)
    y = 248
    s.rect(50, y, 950, y + 330, outline=(200, 30, 30), width=5)
    s.tri(76, y + 22, 64, (200, 30, 30))
    s.u("警告", [["警告"]])
    s.text(160, y + 30, "경고", "kob", 50, (200, 30, 30))
    items = [("물에 담그지 마십시오.", "請勿浸入水中。", [["水"], ["浸", "泡"]], "담그지 마십시오"),
             ("어린이의 손이 닿지 않는 곳에 보관하세요.", "請放置於孩童無法觸及之處。",
              [["兒童", "孩童", "小孩", "幼兒", "小朋友"], ["觸及", "碰不到", "碰觸不到", "拿不到", "搆不到", "構不到", "接觸不到", "遠離", "無法"], ["保管", "放置", "存放", "保存", "收納", "放"]], "닿지 않는 곳"),
             ("임의로 분해하지 마세요.", "請勿擅自拆解。", [["拆解", "拆開", "拆卸", "分解"], ["擅自", "隨意", "私自", "自行"]], "분해하지 마세요")]
    iy = y + 120
    for t, ref, must, ks in items:
        anyk = NEG + ["碰不到", "拿不到", "搆不到", "構不到", "遠離"] if "닿" in ks else NEG
        s.u(ref, must, [K("neg", ks, anyk)])
        s.d.ellipse((76, iy + 14, 90, iy + 28), fill=(200, 30, 30))
        s.text(108, iy, t, "ko", 34)
        iy += 66
    y = 608
    s.rect(50, y, 950, y + 250, outline=(230, 140, 20), width=5)
    s.tri(76, y + 18, 56, (230, 140, 20))
    s.u("注意", [["注意", "小心"]])
    s.text(160, y + 22, "주의", "kob", 46, (210, 120, 10))
    s.u("使用後請將水倒掉並晾乾。", [["水"], ["倒", "排", "清空", "清除"], ["乾", "晾", "擦乾"]])
    s.text(108, y + 100, "사용 후에는 물을 비우고 말려 주세요.", "ko", 34)
    s.u("請勿用濕手觸碰插頭。", [["濕"], ["插頭"], ["摸", "觸", "碰"]], [K("neg", "만지지 마세요", NEG)])
    s.text(108, y + 168, "젖은 손으로 플러그를 만지지 마세요.", "ko", 34)
    y = 888
    kw = dict(size=34, lkey="kob", vkey="ko", x0=50, xs=330, x1=950, minh=70, lfill=(228, 232, 240), col=(70, 70, 80))
    s.u("輸入 220V 60Hz", [["輸入", "電源"]], [K("num", "220V", VOLT("220")), K("num", "60Hz", HZ("60"))])
    y = s.trow(y, "입력", "220V 60Hz", **kw)
    s.u("耗電量 25W", [["耗電", "功率", "消耗"]], [K("num", "25W", WATT("25"))])
    y = s.trow(y, "소비전력", "25W", **kw)
    s.u("使用溫度 5～35°C", [["使用溫度", "操作溫度", "適用溫度", "使用環境", "工作溫度"]], [K("temp", "5~35℃", rng("5", "35"))])
    y = s.trow(y, "사용 온도", "5~35℃", **kw)
    s.u("客服中心 1588-0123", [["客服", "客戶服務", "服務中心", "顧客中心", "客戶中心"]],
        [K("phone", "1588-0123", ["1588-0123", "15880123", "1588 0123"])])
    y = s.trow(y, "고객센터", "1588-0123", **kw)
    s.u("服務時間 平日 09:00～18:00", [["平日", "週一至週五", "星期一至星期五", "週一到週五", "工作日", "週一~週五", "週一～週五"]],
        [K("time", "09:00~18:00", rng("09:00", "18:00", ("",), ["上午9點", "9點", "9時", "9:00"]))])
    y = s.trow(y, "운영시간", "평일 09:00~18:00", **kw)
    return s, "dark", dict(shadow=0.22, glare=0.12, glare_at=(0.7, 0.4)), 0.85, Q2, 202, "manual"


def ko_menu():
    s = Sc(1000, 1320, (238, 224, 196), seed=203, lang="ko")
    s.rect(50, 40, 270, 104, fill=(150, 40, 30), radius=12)
    s.u("味道食堂", [], opt=True)
    s.text(74, 52, "맛나식당", "kob", 36, (255, 255, 255))
    s.u("菜單", [["菜單"]])
    s.ctext(80, "메뉴", "kob", 86, (60, 30, 20), spacing=16)
    items = [("김치찌개", "9,000원", "泡菜鍋", [["泡菜"], ["鍋", "湯"]], 9000, "9,000원"),
             ("된장찌개", "8,000원", "大醬湯", [["大醬", "味噌", "豆醬"]], 8000, "8,000원"),
             ("제육볶음", "11,000원", "辣炒豬肉", [["豬肉", "豬"], ["炒"]], 11000, "11,000원"),
             ("불고기 정식", "13,000원", "韓式烤肉定食", [["烤肉", "烤牛肉", "불고기", "bulgogi", "Bulgogi", "燒肉"], ["定食", "套餐", "套餐組合"]], 13000, "13,000원"),
             ("해물파전", "14,000원", "海鮮煎餅", [["海鮮"], ["煎餅", "蔥餅", "蔥煎餅", "煎"]], 14000, "14,000원"),
             ("소주", "₩5,000", "燒酒", [["燒酒", "韓國燒酒", "酒"]], 5000, "₩5,000"),
             ("맥주", "₩5,000", "啤酒", [["啤酒"]], 5000, "₩5,000")]
    y = 230
    for name, ptxt, zh, must, n, psrc in items:
        s.u("%s %s韓元" % (zh, "{:,}".format(n)), must, [K("price", psrc, KRW(n))])
        s.text(80, y, name, "kob", 40, (40, 25, 15))
        s.rtext(920, y, ptxt, "kob", 40, (40, 25, 15))
        s.d.line((80, y + 66, 920, y + 66), fill=(150, 120, 90), width=2)
        y += 96
    y += 24
    s.u("湯鍋類無法單點一人份", [["一人份", "1人份", "單人", "一人", "1人", "單點", "一份"], ["無法", "不能", "不可", "不接受", "不提供", "不行", "恕不", "不"]],
        [K("neg", "불가", ["無法", "不能", "不可", "不接受", "不提供", "不行", "恕不", "不得"])])
    s.text(80, y, "찌개류는 1인분 주문 불가", "ko", 34, (150, 30, 20))
    y += 58
    s.u("白飯另計", [["白飯", "飯"], ["另", "額外", "單點", "另計", "另購", "加收", "加價"]])
    s.text(80, y, "공기밥 별도", "ko", 34, (150, 30, 20))
    y += 58
    s.u("可外帶", [["外帶", "打包", "帶走", "外賣"], ["可", "可以", "接受", "提供", "歡迎"]])
    s.text(80, y, "포장 가능", "ko", 34, (150, 30, 20))
    y += 58
    s.u("所有價格均含加值稅", [["含稅", "含加值稅", "含增值稅", "含附加價值稅", "已含稅", "稅金"]])
    s.text(80, y, "모든 가격은 부가세 포함", "ko", 34, (150, 30, 20))
    return s, "wood", dict(shadow=0.25, glare=0.1, glare_at=(0.5, 0.25)), 0.85, Q3, 203, "menu"


def en_label_nutrition():
    s = Sc(1000, 1320, (252, 252, 250), seed=301, lang="en")
    s.u("濃郁番茄羅勒湯", [["番茄"], ["羅勒"], ["湯"]])
    s.text(50, 22, "Creamy Tomato Basil Soup", "enb", 58, (150, 30, 20))
    bx0, bx1, y = 50, 950, 112
    s.d.rectangle((bx0, y, bx1, y + 640), outline=(0, 0, 0), width=6)
    s.u("營養標示", [["營養"]])
    s.text(bx0 + 24, y + 8, "Nutrition Facts", "enb", 62)
    y += 84
    s.d.line((bx0, y, bx1, y), fill=(0, 0, 0), width=3)
    s.u("每份 1杯（240毫升）", [["每份", "一份", "份量", "食用份量", "每一份"], ["杯"]], [K("num", "240mL", ML("240"))])
    s.text(bx0 + 24, y + 8, "Serving size", "enb", 36)
    s.rtext(bx1 - 24, y + 8, "1 cup (240mL)", "enb", 36)
    y += 58
    s.d.line((bx0, y, bx1, y), fill=(0, 0, 0), width=14)
    s.u("熱量 150大卡", [["熱量", "卡路里", "大卡"]], [K("num", "150", KCAL("150"))])
    s.text(bx0 + 24, y + 22, "Calories", "enb", 46)
    s.rtext(bx1 - 24, y + 8, "150", "enb", 70)
    y += 96
    s.d.line((bx0, y, bx1, y), fill=(0, 0, 0), width=6)
    s.u("每日參考值百分比*", [["每日"], ["參考值", "營養素參考值", "建議攝取量", "需求量", "基準", "百分比", "%"]])
    s.rtext(bx1 - 24, y + 6, "% Daily Value*", "enb", 34)
    y += 50
    for lab, amt, pct, zh, zhamt, ak, must in [
            ("Total Fat", "8g", "10%", "總脂肪", "8公克", G("8"), ["脂肪"]),
            ("Sodium", "125mg", "5%", "鈉", "125毫克", MG("125"), ["鈉"]),
            ("Total Carbohydrate", "18g", "7%", "總碳水化合物", "18公克", G("18"), ["碳水化合物", "碳水"])]:
        s.d.line((bx0, y, bx1, y), fill=(0, 0, 0), width=2)
        s.u("%s %s %s" % (zh, zhamt, pct), [must], [K("num", amt, ak), K("pct", pct, PCT(pct[:-1]))])
        x1 = s.text(bx0 + 24, y + 8, lab, "enb", 36)
        s.text(x1 + 14, y + 8, amt, "en", 36)
        s.rtext(bx1 - 24, y + 8, pct, "enb", 36)
        y += 54
    s.d.line((bx0, y, bx1, y), fill=(0, 0, 0), width=6)
    s.u("*每日參考值（%DV）表示一份食物中的營養素占每日飲食的比例。一般營養建議以每日2,000大卡為基準。",
        [["每日"], ["2,000", "2000"], ["大卡", "卡路里", "熱量", "千卡", "卡"]],
        [K("num", "2,000 calories", [x for v in ("2,000", "2000") for x in fm(v, "大卡", "卡", "卡路里", "千卡", "kcal", "Cal", "calories")])])
    s.wrap(bx0 + 24, y + 12, "*The % Daily Value tells you how much a nutrient in a serving contributes to a daily diet. 2,000 calories a day is used for general nutrition advice.",
           "en", 34, 850, lead=1.32)
    y = 790
    s.u("成分：水、番茄、鮮奶油、洋蔥、奶油、羅勒、糖、海鹽、大豆卵磷脂。", [["成分", "原料", "配料", "內容物"], ["番茄"], ["奶油", "鮮奶油"]])
    y = s.wrap(50, y, "Ingredients: Water, tomatoes, cream, onion, butter, basil, sugar, sea salt, soy lecithin.", "en", 35, 900, lead=1.35) + 14
    s.u("含有：牛奶、大豆", [["含有", "內含", "含", "過敏原"], ["牛奶", "乳", "奶"], ["大豆", "黃豆"]])
    s.text(50, y, "Contains: milk, soy", "enb", 38)
    y += 66
    s.u("開封後請冷藏保存。", [["開封", "開啟", "打開", "開罐"], ["冷藏"]])
    s.text(50, y, "Keep refrigerated after opening", "en", 36)
    y += 62
    s.u("請勿連同容器微波加熱。", [["微波"], ["容器", "罐", "盒"]], [K("neg", "Do not", NEG)])
    s.text(50, y, "Do not microwave in container", "enb", 36, (170, 20, 20))
    y += 62
    s.u("最佳食用期限 2027年3月15日", [["最佳", "建議", "賞味", "食用期限", "有效期限", "期限", "之前食用", "前食用"]],
        [K("date", "03/15/2027", ["2027/03/15", "2027年3月15日", "2027-03-15", "03/15/2027", "2027.03.15", "3/15/2027", "2027年03月15日", "2027/3/15"])])
    s.text(50, y, "Best by 03/15/2027", "enb", 36)
    return s, "wood", dict(shadow=0.22, glare=0.15, glare_at=(0.6, 0.45)), 0.85, Q4, 301, "label"


def en_manual():
    s = Sc(1000, 1320, (248, 249, 251), seed=302, lang="en")
    s.rect(50, 36, 290, 104, fill=(20, 60, 140), radius=10)
    s.u("VOLTEX", [], opt=True)
    s.text(74, 48, "VOLTEX", "enb", 46, (255, 255, 255))
    s.u("行動電源 使用手冊", [["行動電源", "充電器", "充電寶", "移動電源", "隨身電源"], ["說明書", "手冊", "使用手冊", "使用說明"]])
    s.text(50, 140, "Portable Charger", "enb", 62, (20, 20, 20))
    s.text(50, 218, "User Manual", "en", 52, (60, 60, 60))
    s.d.line((50, 296, 950, 296), fill=(40, 40, 40), width=5)
    y = 322
    s.rect(50, y, 950, y + 330, outline=(200, 30, 30), width=5)
    s.tri(76, y + 20, 64, (200, 30, 30))
    s.u("警告", [["警告"]])
    s.text(160, y + 26, "WARNING", "enb", 50, (200, 30, 30))
    iy = y + 112
    for t, ref, must, ks, anyk in [
            ("Do not expose to rain or moisture.", "請勿暴露於雨水或潮濕環境中。", [["雨"], ["潮濕", "濕氣", "水氣", "受潮", "濕"]], "Do not", NEG),
            ("Never leave the charger unattended.", "切勿讓充電器處於無人看管的狀態。", [["充電器"], ["無人", "離開", "看管", "照看", "看護", "看顧", "不在", "獨自"]], "Never",
             NEG + ["絕不", "千萬不要", "千萬別", "不准"]),
            ("Keep away from children under 3.", "請放置於3歲以下兒童無法取得之處。", [["3歲", "三歲"], ["兒童", "小孩", "幼兒", "孩童", "嬰幼兒"]], "Keep away",
             ["遠離", "避開", "避免", "勿", "不要", "不可", "禁止", "別", "無法", "不得", "不能"])]:
        ks_list = [K("neg", ks, anyk)]
        if ks == "Keep away":
            ks_list.append(K("num", "under 3", ["3歲以下", "三歲以下", "未滿3歲", "未滿三歲", "3歲以內", "不滿3歲", "小於3歲", "3 歲以下", "未滿 3 歲"]))
        s.u(ref, must, ks_list)
        s.d.ellipse((76, iy + 14, 90, iy + 28), fill=(200, 30, 30))
        s.text(108, iy, t, "en", 36)
        iy += 66
    y = 690
    kw = dict(size=34, lkey="en", vkey="en", x0=50, xs=430, x1=950, minh=72, lfill=(228, 232, 240), col=(70, 70, 80))
    s.u("輸入：100-240V ~ 50/60Hz", [["輸入"]],
        [K("num", "100-240V", [x for sp in ("-", "–", "~", "～", "至", "到", " - ", " ~ ", " 至 ", " 到 ") for x in
                              ("100%s240V" % sp, "100V%s240V" % sp, "100%s240 V" % sp, "100 V%s240 V" % sp, "100%s240伏" % sp)]),
         K("num", "50/60Hz", ["50/60Hz", "50/60 Hz", "50/60赫茲", "50/60hz", "50Hz/60Hz", "50 Hz/60 Hz", "50或60Hz", "50或60 Hz", "50-60Hz", "50~60Hz",
                              "50 / 60 Hz", "50/60赫", "50 / 60Hz"])])
    y = s.trow(y, "Input", "100-240V ~ 50/60Hz", **kw)
    s.u("輸出：5V 3A", [["輸出"]], [K("num", "5V", VOLT("5")), K("num", "3A", AMP("3"))])
    y = s.trow(y, "Output", "5V 3A", **kw)
    s.u("操作溫度 32°F-104°F（0°C-40°C）", [["操作溫度", "使用溫度", "運作溫度", "工作溫度", "運轉溫度", "適用溫度", "使用環境溫度", "操作環境溫度"]],
        [K("temp", "32°F-104°F", rng("32", "104", ("°F", "℉"))), K("temp", "0°C-40°C", rng("0", "40"))])
    y = s.trow(y, "Operating temperature", "32°F-104°F (0°C-40°C)", **kw)
    s.u("僅限室內使用。", [["室內"], ["僅", "只", "限", "專用"]])
    s.text(60, y + 28, "For indoor use only.", "en", 36)
    y += 100
    s.u("客服專線：1-800-555-0142", [["客服", "客戶服務", "支援", "服務專線", "客戶支援", "客戶支持", "技術支援", "服務"]],
        [K("phone", "1-800-555-0142", ["1-800-555-0142", "1800-555-0142", "18005550142", "1 800 555 0142", "1-800-5550142", "1800-5550142", "1800 555 0142"])])
    s.text(60, y, "Customer support: 1-800-555-0142", "enb", 36)
    return s, "wood", dict(shadow=0.22, glare=0.12, glare_at=(0.3, 0.3)), 0.85, Q2, 302, "manual"


# ---------------------------------------------------------------------------
# app screens (1170x2532, flat)

def ja_app_error():
    s = Sc(1170, 2532, lang="ja", flat=(242, 242, 247), seed=401)
    s.status_bar()
    s.u("帳號設定", [["帳號", "帳戶"], ["設定", "設置"]])
    s.nav("アカウント設定", "jab")
    y = 222
    s.u("推播通知 關閉後將不會收到通知", [["推播", "通知"], ["關閉", "關掉", "停用"], ["不會", "不", "收不到"]],
        [K("neg", "届きません", NEG + ["收不到", "不會收到"])])
    y = s.srow(y, "プッシュ通知", "ja", "オフにすると通知は届きません", "ja", toggle=True)
    s.u("電子郵件通知 每週一次，彙整後寄送", [["電子郵件", "郵件", "Email", "email", "信件", "e-mail"], ["彙整", "整理", "匯總", "統整", "彙總", "合併"]],
        [K("num", "週1回", ["每週一次", "每週1次", "每週 1 次", "一週一次", "每星期一次", "每星期1次", "每週一封", "每週1封", "一週1次", "每週寄", "每週"])])
    y = s.srow(y, "メール通知", "ja", "週1回、お知らせをまとめて送信します", "ja", toggle=True)
    s.u("深色模式", [["深色", "暗色", "夜間", "黑暗", "暗黑"]])
    y = s.srow(y, "ダークモード", "ja", toggle=False)
    s.u("語言 日文", [["語言"], ["日文", "日語", "日本語"]])
    y = s.srow(y, "言語", "ja", value="日本語")
    s.u("登出", [["登出", "登出帳號", "登出帳戶", "退出", "登出系統"]])
    y = s.srow(y, "ログアウト", "ja", tcolor=(220, 40, 40))
    s.dim(0.45)
    yd = s.dialog(1190, [
        ("変更の破棄", "jab", 48, (20, 20, 20), S("捨棄變更", [["捨棄", "放棄", "丟棄", "取消"], ["變更", "更動", "修改"]])),
        ("保存されていない変更があります。破棄しますか？", "ja", 42, (50, 50, 55),
         S("有尚未儲存的變更。要捨棄嗎？", [["尚未儲存", "未儲存", "沒有儲存", "未存檔", "未保存", "尚未保存", "未被儲存"], ["變更", "更動", "修改"], ["捨棄", "放棄", "丟棄", "不儲存"]])),
        ("この操作は元に戻せません", "ja", 40, (200, 40, 40),
         S("此操作無法復原。", [["無法", "不能", "不可"], ["復原", "還原", "恢復", "撤銷", "撤回", "回復", "取消"]],
           [K("neg", "戻せません", ["無法", "不能", "不可", "不會", "沒辦法"])]))],
        [("破棄", "jab", (220, 40, 40), S("捨棄", [["捨棄", "放棄", "丟棄", "刪除"]])),
         ("キャンセル", "jab", (20, 110, 240), S("取消", [["取消"]]))])
    s.u("無法連線至網路", [["網路", "網際網路", "連線", "連接"], ["無法", "不能", "未能"]],
        [K("neg", "できません", ["無法", "不能", "沒辦法", "未能", "失敗"])])
    s.toast(2200, "ネットワークに接続できません", "ja")
    return s, "app", "app"


def ko_app():
    s = Sc(1170, 2532, lang="ko", flat=(242, 242, 247), seed=402)
    s.status_bar()
    s.u("聊天設定", [["聊天", "對話", "聊天室"], ["設定", "設置"]])
    s.nav("채팅 설정", "kob")
    y = 222
    s.u("通知 關閉通知後，將不會收到新訊息通知", [["通知"], ["關閉", "關掉", "停用"], ["不會", "不", "收不到"], ["新訊息", "新消息", "新的訊息", "新簡訊"]],
        [K("neg", "받지 않습니다", NEG + ["收不到", "不會收到", "不再"])])
    y = s.srow(y, "알림", "kob", "알림을 끄면 새 메시지 알림을 받지 않습니다", "ko", toggle=True)
    s.u("備份對話內容 僅在連上Wi-Fi時備份", [["備份"], ["僅", "只", "只有", "唯有", "才"], ["Wi-Fi", "WiFi", "Wi‑Fi", "wifi", "Wifi", "無線網路", "wi-fi"]])
    y = s.srow(y, "대화 내용 백업", "kob", "Wi-Fi 연결 시에만 백업합니다", "ko", toggle=True)
    s.u("已讀標示", [["已讀", "讀取", "已讀取"]])
    y = s.srow(y, "읽음 표시", "kob", toggle=False)
    s.u("刪除對話 已刪除的訊息無法復原", [["刪除"], ["無法", "不能", "不可", "沒辦法"], ["復原", "恢復", "還原", "找回", "回復", "救回"]],
        [K("neg", "복구할 수 없습니다", ["無法", "不能", "不可", "沒辦法", "不會"])])
    y = s.srow(y, "대화 삭제", "kob", "삭제한 메시지는 복구할 수 없습니다", "ko", tcolor=(220, 40, 40))
    s.dim(0.45)
    s.dialog(1150, [
        ("결제 실패", "kob", 48, (20, 20, 20), S("付款失敗", [["付款", "支付", "結帳", "扣款"], ["失敗", "不成功", "未成功", "無法完成"]])),
        ("결제를 완료할 수 없습니다. 카드 정보를 확인해 주세요", "ko", 42, (50, 50, 55),
         S("無法完成付款，請確認您的卡片資訊。", [["完成"], ["付款", "支付", "結帳"], ["卡", "信用卡", "卡片", "銀行卡"], ["確認", "檢查", "核對", "查看", "核實"]],
           [K("neg", "완료할 수 없습니다", ["無法", "不能", "未能", "沒辦法", "失敗"])]))],
        [("다시 시도", "kob", (20, 110, 240), S("重試", [["重試", "重新嘗試", "再試一次", "再次嘗試", "重新", "再試"]])),
         ("확인", "kob", (20, 110, 240), S("確認", [["確定", "確認", "好", "OK"]]))])
    s.u("網路連線不穩定", [["網路", "連線", "網絡"], ["不穩", "不穩定", "不佳", "不良", "不太穩"]])
    s.toast(2200, "네트워크 연결이 불안정합니다", "kob")
    return s, "app", "app"


def en_app():
    s = Sc(1170, 2532, lang="en", flat=(242, 242, 247), seed=403)
    s.status_bar()
    s.u("設定", [["設定", "設置"]])
    s.nav("Settings", "enb")
    s.rect(50, 270, 1120, 590, fill=(255, 244, 214), outline=(240, 190, 60), width=3, radius=30)
    s.u("您有尚未儲存的變更。要捨棄嗎？", [["未儲存", "尚未儲存", "沒有儲存", "未保存", "尚未保存", "未存檔"], ["變更", "更動", "修改"], ["捨棄", "放棄", "丟棄", "不儲存"]])
    s.text(90, 310, "You have unsaved changes. Discard them?", "enb", 40, (60, 40, 0))
    s.u("此操作無法復原。", [["無法", "不能", "不可"], ["復原", "還原", "恢復", "撤銷", "撤回", "回復", "取消"]],
        [K("neg", "can't", ["無法", "不能", "不可", "沒辦法", "不會"])])
    s.text(90, 380, "This can't be undone.", "en", 38, (110, 80, 20))
    s.rect(780, 450, 1080, 550, outline=(200, 40, 40), width=4, radius=50)
    s.u("捨棄", [["捨棄", "放棄", "丟棄"]])
    s.ctext(472, "Discard", "enb", 44, (200, 40, 40), cx=930)
    y = 640
    s.u("推播通知 接收新訊息與更新的提醒", [["推播", "通知"], ["提醒", "通知", "警示", "提示"]])
    y = s.srow(y, "Push notifications", "enb", "Get alerts for new messages and updates", "en", toggle=True)
    s.u("電子郵件摘要 每週一寄送每週摘要", [["摘要", "彙整", "整理"], ["電子郵件", "郵件", "Email", "email", "信件"]],
        [K("date", "Monday", ["週一", "星期一", "禮拜一", "周一", "每週一"])])
    y = s.srow(y, "Email digest", "enb", "A weekly summary every Monday", "en", toggle=True)
    s.u("深色模式", [["深色", "暗色", "夜間", "黑暗", "暗黑"]])
    y = s.srow(y, "Dark mode", "enb", toggle=False)
    s.dim(0.45)
    s.dialog(1250, [
        ("Turn off notifications?", "enb", 48, (20, 20, 20), S("要關閉通知嗎？", [["關閉", "關掉", "停用"], ["通知"]])),
        ("You won't receive alerts.", "en", 42, (50, 50, 55),
         S("您將不會收到提醒。", [["提醒", "通知", "警示", "提示"], ["收到", "收"]], [K("neg", "won't", NEG + ["收不到"])]))],
        [("Cancel", "enb", (20, 110, 240), S("取消", [["取消"]])),
         ("Turn off", "enb", (220, 40, 40), S("關閉", [["關閉", "關掉", "停用"]]))])
    s.u("沒有網路連線", [["網路", "網際網路", "連線", "網絡"], ["沒有", "無", "未連線", "未連接", "斷線", "未"]],
        [K("neg", "No", ["沒有", "無", "未", "斷線", "沒"])])
    s.toast(2200, "No internet connection", "enb")
    return s, "app", "app"


SCENES = [("ja_label_cupnoodle", ja_label_cupnoodle, "ja"), ("ja_manual", ja_manual, "ja"),
          ("ja_menu_izakaya", ja_menu_izakaya, "ja"), ("ja_app_error", ja_app_error, "ja"),
          ("ja_sign_station", ja_sign_station, "ja"), ("ko_label_snack", ko_label_snack, "ko"),
          ("ko_manual", ko_manual, "ko"), ("ko_menu", ko_menu, "ko"), ("ko_app", ko_app, "ko"),
          ("en_label_nutrition", en_label_nutrition, "en"), ("en_manual", en_manual, "en"),
          ("en_app", en_app, "en")]

# ---------------------------------------------------------------------------


def finish(s, name, lang, kind, lines, tags, pic_wh, out):
    W, H = pic_wh
    sep = " " if lang == "en" else ""
    units = []
    used = set()
    for u in s.units:
        texts = [t for tg, (q, t) in zip(tags, lines) if tg == u["id"]]
        assert texts, (name, u)
        used.add(u["id"])
        for k in u["keys"]:
            assert k["src"] in sep.join(texts), (name, k["src"], sep.join(texts))
        units.append({"id": u["id"], "src": sep.join(texts), "ref": u["ref"], "must": u["must"], "keys": u["keys"],
                      "optional": u["optional"]})
    assert len(tags) == len(lines) and set(tags) == used, name
    with open(os.path.join(out, name + ".gt.tsv"), "w", encoding="utf-8", newline="\n") as f:
        for q, t in lines:
            q = q / np.array([W, H], np.float32)
            x0, y0 = q.min(0)
            x1, y1 = q.max(0)
            f.write("%.4f %.4f %.4f %.4f %s\t%s\n" % (x0, y0, x1, y1, " ".join("%.4f" % v for v in q.reshape(-1)), t))
    head = json.dumps({"name": name, "lang": lang, "kind": kind}, ensure_ascii=False)[:-1]
    body = head + ', "units": [\n' + ",\n".join(" " + json.dumps(u, ensure_ascii=False) for u in units) + "\n]}\n"
    for d in (out, EVAL_DIR):
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name + ".ref.json"), "w", encoding="utf-8", newline="\n") as f:
            f.write(body)
    nk = sum(len(u["keys"]) for u in units)
    print(name, len(lines), "lines", len(units), "units", nk, "keys")


def render_photo(s, name, lang, kind, bg, light, blur, quad, seed, out):
    assert max(q[:, 1].max() for q, _ in s.lines) <= s.h - 8, ("card overflow", name, max(q[:, 1].max() for q, _ in s.lines))
    vw, vh = mp.VF[2] - mp.VF[0], mp.VF[3] - mp.VF[1]
    view = mp.table(vh, vw, seed, bg)
    quads = mp.place(s, quad, view, seed)
    view = mp.lighting(view, seed + 1, **light)
    view = cv2.GaussianBlur(view, (0, 0), blur)
    rng_ = np.random.default_rng(seed + 2)
    view += rng_.normal(0, 4.0, view.shape).astype(np.float32)
    full = np.zeros((mp.H, mp.W, 3), np.float32) + 8
    full[mp.VF[1]:mp.VF[3], mp.VF[0]:mp.VF[2]] = np.clip(view, 0, 255)
    lines = [(q + np.array([mp.VF[0], mp.VF[1]], np.float32), t) for q, t in quads]
    ui = []
    full = mp.camera_ui(full, ui)
    s.u("錄影 拍照 人像", [], opt=True)  # camera-app mode labels
    tags = s.tags + [s.cur] * len(ui)
    lines += ui
    ok, enc = cv2.imencode(".jpg", cv2.cvtColor(np.clip(full, 0, 255).astype(np.uint8), cv2.COLOR_RGB2BGR),
                           [cv2.IMWRITE_JPEG_QUALITY, 62])
    cv2.imwrite(os.path.join(out, name + ".png"), cv2.imdecode(enc, cv2.IMREAD_COLOR))
    finish(s, name, lang, kind, lines, tags, (mp.W, mp.H), out)


def render_screen(s, name, lang, kind, out):
    assert max(q[:, 1].max() for q, _ in s.lines) <= s.h, name
    s.img.save(os.path.join(out, name + ".png"))
    finish(s, name, lang, kind, list(s.lines), s.tags, (s.w, s.h), out)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "eval_scenes"
    os.makedirs(out, exist_ok=True)
    want = sys.argv[2:]
    for name, fn, lang in SCENES:
        if want and name not in want:
            continue
        r = fn()
        s = r[0]
        if r[1] == "app":
            render_screen(s, name, lang, "app", out)
        else:
            _, bg, light, blur, quad, seed, kind = r
            render_photo(s, name, lang, kind, bg, light, blur, quad, seed, out)


if __name__ == "__main__":
    main()
