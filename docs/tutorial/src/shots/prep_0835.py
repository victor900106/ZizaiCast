# 0.8.3.5 (rel-0835): run after prep.py, prep_0833.py and prep_0834.py. The two new pictures of the guides, from
# remote/tools/pcmouse0835_test.mjs --only shots --shots <dir> --lang N (chrome-headless-shell, spike --inject-stub,
# a made-up desktop picture), copied to _shots/after/<lang>_S1-pc-bar.png / <lang>_S2-phone-mode.png:
#   pc-bar      another computer's browser with the mouse at the top edge (the PC bar)
#   phone-mode  a phone's first connection (the 直接點 card and the six buttons)
# guest-new / guest-made / phone-bar come from prep_0834.py (tools/shots_0833.mjs after --only AP, the 臨時帳號
# with the made-up password ABC123 from PM_TEST_GUEST_PW).
#   python docs/tutorial/src/shots/prep_0835.py zh-TW en ja ko
import cv2, numpy as np, os, sys
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', '_shots')
SPEC = {
    'pc-bar': ('S1-pc-bar.png', 1280, (0, 0, 1, 0.30)),  # shown 640 CSS px wide (sharp on HiDPI)
    'phone-mode': ('S2-phone-mode.png', 600, (0, 0.64, 1, 1)),  # the card and the six buttons, shown 260 CSS px
}
for lang in sys.argv[1:]:
    out = f'{root}/tut/web/{lang}'
    os.makedirs(out, exist_ok=True)
    for name, (src, w, crop) in SPEC.items():
        p = f'{root}/after/{lang}_{src}'
        if not os.path.exists(p):
            print('missing', lang, src); continue
        im = cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
        h, ww = im.shape[:2]
        im = im[int(crop[1] * h):int(crop[3] * h), int(crop[0] * ww):int(crop[2] * ww)]
        s = w / im.shape[1]
        im = cv2.resize(im, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)
        ok, buf = cv2.imencode('.webp', im, [cv2.IMWRITE_WEBP_QUALITY, 86])
        buf.tofile(f'{out}/{name}.webp')
    print(lang, 'done')
