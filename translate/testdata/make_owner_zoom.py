# Test pictures from the owner's window shots of the 八ッ橋ショコラ label
# (0.7.0 real test; the shots are not in the repository).
#
#   py -3 translate/testdata/make_owner_zoom.py SHOTS_DIR OUTDIR
#
# SHOTS_DIR has 18.png (label at 2x, two old cards on it) and 19.png (camera
# at 1x, label covered by old cards).  Writes, at 2x the shot size (closer to
# the phone picture's resolution):
#   owner_2x.png  18.png with the old cards painted over and their text put
#                 back (Yu Gothic, as on the label; 「海夫」 -> HEIF)
#   owner_1x.png  19.png with its label replaced by that cleaned 2x label,
#                 scaled to where the label is at 1x, and the old cards left
#                 outside the label (「巧克力…」 at the left edge, 「福奇」 /
#                 「分享 happi」 on the package) inpainted away
import os
import sys

import cv2
import numpy as np

from PIL import Image, ImageDraw, ImageFont

FONT = r"C:\Windows\Fonts\YuGothM.ttc"


def clean18(img):
    """18.png with the old 「株式會社美十…」 / 「食塩相當…」 cards replaced by the label text."""
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT, 10)
    # Row 製造者 (value cell) and the nutrition row under 炭水化物.
    d.rectangle((112, 448, 346, 468), fill=(236, 232, 228))
    d.text((116, 451), "株式会社美十　〒601-8446 京都市南区西九条高畠町35-2", font=f, fill=(40, 40, 40))
    d.line((100, 447, 346, 447), fill=(90, 80, 80), width=1)
    d.rectangle((347, 448, 492, 476), fill=(236, 232, 228))
    d.text((390, 451), "食塩相当量  0.05g", font=f, fill=(40, 40, 40))
    d.text((352, 464), "※この表示値は目安です。", font=ImageFont.truetype(FONT, 9), fill=(40, 40, 40))
    return img


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    a = clean18(Image.open(os.path.join(src, "18.png")).convert("RGB"))
    a2 = a.resize((a.width * 2, a.height * 2), Image.LANCZOS)
    # The old 「海夫」 card on the camera's HEIF label: the label put back.
    d2 = ImageDraw.Draw(a2)
    d2.rectangle((570, 182, 656, 226), fill=(0, 0, 0))
    d2.text((578, 186), "HEIF", font=ImageFont.truetype("arialbd.ttf", 30), fill=(245, 245, 245))
    a2.save(os.path.join(out, "owner_2x.png"))
    b = Image.open(os.path.join(src, "19.png")).convert("RGB")
    b2 = b.resize((b.width * 2, b.height * 2), Image.LANCZOS)
    # 18's label (incl. the pink lines above / below it) -> 19's label area.
    label = a2.crop((80, 680, 980, 1400))
    dst = (96, 600, 780, 1150)
    b2.paste(label.resize((dst[2] - dst[0], dst[3] - dst[1]), Image.LANCZOS), dst[:2])
    # Old 0.7.0 cards outside the label (2x coordinates): painted out.
    arr = cv2.cvtColor(np.asarray(b2), cv2.COLOR_RGB2BGR)
    mask = np.zeros(arr.shape[:2], np.uint8)
    for x0, y0, x1, y1 in ((52, 876, 97, 1008), (684, 1205, 858, 1292), (698, 1286, 816, 1322)):
        mask[y0:y1, x0:x1] = 255
    arr = cv2.inpaint(arr, mask, 9, cv2.INPAINT_TELEA)
    b2 = Image.fromarray(cv2.cvtColor(arr, cv2.COLOR_BGR2RGB))
    b2.save(os.path.join(out, "owner_1x.png"))
    print("owner_2x.png", a2.size, "owner_1x.png", b2.size)


if __name__ == "__main__":
    main()
