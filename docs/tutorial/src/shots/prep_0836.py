# 0.8.3.6 (rel-0836): run after prep_0834.py and prep_0835.py. The pictures 0.8.3.6 changed:
#   phone-login / phone-login-guest  the sign-in page with 投投 and three friends (tools/shots_0833.mjs after --only A,
#                                    <lang>_A6-phone-login.png / <lang>_A6b-phone-login-guest.png; same crop as 0.8.3.3:
#                                    the friends sit above the title, inside it)
#   phone-settings                   the settings sheet with 每秒畫面 and 解析度跟手機一樣 (remote/tools/phone_shots_0834.mjs
#                                    <dir>, touch emulated: the phone default; after/<lang>_P2-settings.png copied to
#                                    _shots/after/<lang>_P2-settings-0836.png)
#   python docs/tutorial/src/shots/prep_0836.py zh-TW en ja ko
import cv2, numpy as np, os, sys
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', '_shots')
SPEC = {
    'phone-login': ('A6-phone-login.png', 330, (0, 0.2, 1, 0.78)),
    'phone-login-guest': ('A6b-phone-login-guest.png', 330, (0, 0.2, 1, 0.78)),
    'phone-settings': ('P2-settings-0836.png', 360, (0, 0.0, 1, 1)),
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
