# 0.8.3 whole-picture timing scenes (pm_translate_test --bench): flat phone
# screenshots, 1170x2532, drawn with Windows fonts (deterministic, no browser).
#
#   py -3 translate/testdata/make_bench_083.py OUTDIR
#
# Writes OUTDIR/ja_menu.png (a Japanese restaurant menu app), en_web.png (an
# English shop / article page) and dense_web.png (a dense Japanese news page,
# ~70 text blocks: the case where the local LLM has the most to do).
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 1170, 2532
FONTS = "C:/Windows/Fonts/"


def font(size, latin=False, bold=False):
    if latin:
        return ImageFont.truetype(FONTS + ("segoeuib.ttf" if bold else "segoeui.ttf"), size)
    return ImageFont.truetype(FONTS + ("YuGothB.ttc" if bold else "YuGothM.ttc"), size)


def status_bar(d, latin=True):
    d.text((60, 30), "9:41", font=font(44, True, True), fill=(20, 20, 20))
    d.text((W - 230, 30), "5G  86%", font=font(40, True), fill=(20, 20, 20))


def wrap(d, text, f, maxw, cjk):
    lines, cur = [], ""
    units = list(text) if cjk else text.split(" ")
    for u in units:
        cand = cur + u if cjk else (cur + " " + u if cur else u)
        if d.textlength(cand, font=f) > maxw and cur:
            lines.append(cur)
            cur = u
        else:
            cur = cand
    if cur:
        lines.append(cur)
    return lines


def para(d, x, y, text, f, maxw, cjk, fill=(30, 30, 30), gap=1.45):
    for ln in wrap(d, text, f, maxw, cjk):
        d.text((x, y), ln, font=f, fill=fill)
        y += int(f.size * gap)
    return y


def ja_menu(out):
    im = Image.new("RGB", (W, H), (250, 246, 238))
    d = ImageDraw.Draw(im)
    status_bar(d)
    d.rectangle((0, 110, W, 300), fill=(120, 40, 30))
    d.text((60, 150), "居酒屋　まるやま", font=font(76, bold=True), fill=(255, 255, 255))
    d.text((60, 250), "本日のおすすめメニュー", font=font(40), fill=(255, 230, 210))
    items = [
        ("焼き鳥盛り合わせ（5本）", "780円", "塩・タレからお選びいただけます"),
        ("刺身三点盛り", "1,280円", "仕入れにより内容が変わります"),
        ("鶏の唐揚げ", "680円", "ジューシーで外はカリッと仕上げました"),
        ("だし巻き玉子", "580円", "注文を受けてから焼き上げます"),
        ("マンゴーチャツネのサラダ", "650円", "季節限定の人気メニュー"),
        ("とんこつラーメン", "900円", "替え玉は一回まで無料です"),
        ("生ビール（中）", "590円", "ハッピーアワーは半額"),
        ("日本酒（一合）", "720円", "二十歳未満の方にはお酒を提供できません"),
    ]
    y = 360
    for name, price, note in items:
        d.text((60, y), name, font=font(54, bold=True), fill=(40, 30, 20))
        pw = d.textlength(price, font=font(54, bold=True))
        d.text((W - 60 - pw, y), price, font=font(54, bold=True), fill=(150, 40, 30))
        d.text((60, y + 80), note, font=font(38), fill=(110, 100, 90))
        d.line((60, y + 150, W - 60, y + 150), fill=(220, 210, 195), width=2)
        y += 185
    y += 20
    y = para(d, 60, y, "※アレルギーをお持ちの方は、ご注文の前に必ずスタッフにお申し付けください。", font(38), W - 120, True,
             fill=(150, 40, 30))
    y = para(d, 60, y + 10, "※お会計はレジにてお願いします。クレジットカードはご利用いただけません。", font(38), W - 120, True)
    d.rectangle((60, H - 220, W - 60, H - 90), fill=(120, 40, 30))
    d.text((W // 2 - 190, H - 195), "注文を確定する", font=font(56, bold=True), fill=(255, 255, 255))
    im.save(os.path.join(out, "ja_menu.png"))


def en_web(out):
    im = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(im)
    status_bar(d)
    d.rectangle((0, 110, W, 230), fill=(35, 47, 62))
    d.text((60, 135), "Northwind Outdoor", font=font(60, True, True), fill=(255, 255, 255))
    d.text((60, 270), "Trail Runner 3 Waterproof Jacket", font=font(64, True, True), fill=(20, 20, 20))
    d.text((60, 370), "$89.99", font=font(60, True, True), fill=(180, 30, 30))
    d.text((300, 380), "Free shipping on orders over $35", font=font(42, True), fill=(30, 110, 50))
    d.text((60, 470), "In Stock. Only 3 left - order soon.", font=font(42, True), fill=(30, 110, 50))
    d.rectangle((60, 560, W - 60, 680), fill=(255, 196, 40))
    d.text((W // 2 - 150, 580), "Add to Cart", font=font(56, True, True), fill=(20, 20, 20))
    y = 740
    d.text((60, y), "About this item", font=font(52, True, True), fill=(20, 20, 20))
    y += 90
    for t in [
        "Lightweight shell that keeps you dry in heavy rain without trapping heat.",
        "Do not tumble dry. Wash cold with similar colors and hang to dry.",
        "Packs into its own chest pocket, so it fits easily in a small bag.",
        "Reflective details make you visible to drivers after dark.",
    ]:
        d.ellipse((66, y + 22, 82, y + 38), fill=(60, 60, 60))
        y = para(d, 110, y, t, font(42, True), W - 170, False) + 18
    y += 30
    d.text((60, y), "Customer reviews", font=font(52, True, True), fill=(20, 20, 20))
    y += 90
    for who, t in [
        ("Maria K.", "Great fit and it really is waterproof. I wore it on a six hour hike in the rain and stayed dry."),
        ("James T.", "The hood is a little small if you wear a cap underneath, but otherwise a very good jacket for the price."),
    ]:
        d.text((60, y), who, font=font(42, True, True), fill=(40, 40, 40))
        y = para(d, 60, y + 64, t, font(42, True), W - 120, False) + 30
    d.text((60, H - 160), "Returns are accepted within 30 days of delivery.", font=font(38, True), fill=(90, 90, 90))
    im.save(os.path.join(out, "en_web.png"))


def dense_web(out):
    im = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(im)
    status_bar(d)
    d.rectangle((0, 100, W, 190), fill=(20, 60, 120))
    d.text((40, 115), "ニュース速報　トップ　経済　国際　スポーツ　天気", font=font(40), fill=(255, 255, 255))
    stories = [
        ("台風10号、週末に九州へ接近の見込み", "気象庁は、不要不急の外出を控えるよう呼びかけています。交通機関の計画運休にも注意してください。"),
        ("日経平均、3日ぶりに反発", "円安を背景に輸出関連株が買われ、終値は前日比412円高となりました。"),
        ("新型スマホ、来月発売へ", "バッテリーの持ちが改善され、価格は据え置きとなる見通しです。予約は今週金曜日から始まります。"),
        ("駅前の再開発、計画を見直し", "住民説明会では、緑地を減らさないでほしいという意見が相次ぎました。"),
        ("高校野球、決勝は延長戦に", "両チーム無得点のまま延長十回に入り、最後はサヨナラ勝ちで決着しました。"),
        ("電気料金、来月から値上げ", "燃料価格の高騰を受け、標準的な家庭で月額およそ300円の負担増となります。"),
        ("美術館で特別展が開幕", "期間中は月曜日も開館しますが、写真撮影は一部の作品を除いてできません。"),
        ("食品の自主回収のお知らせ", "一部の商品に表示されていない原材料が含まれていたため、対象商品は食べずに返品してください。"),
        ("週間天気予報", "週の前半は晴れる日が多いものの、後半は前線の影響で雨が降りやすくなるでしょう。"),
        ("市役所の窓口、土曜日も開設", "マイナンバーカードの申請や受け取りができます。予約がなくても利用可能です。"),
    ]
    y = 220
    for title, body in stories:
        d.text((40, y), title, font=font(44, bold=True), fill=(20, 40, 90))
        y = para(d, 40, y + 62, body, font(34), W - 80, True, fill=(50, 50, 50), gap=1.4)
        d.text((40, y), "2時間前・コメント 128件", font=font(28), fill=(130, 130, 130))
        y += 52
        d.line((40, y, W - 40, y), fill=(225, 225, 225), width=2)
        y += 18
    im.save(os.path.join(out, "dense_web.png"))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    ja_menu(out)
    en_web(out)
    dense_web(out)
    for n in ("ja_menu", "en_web", "dense_web"):
        print(os.path.join(out, n + ".png"))


if __name__ == "__main__":
    main()
