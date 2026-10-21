# 0.8.3.4 (rel-0834): prep_0833.py with the crops of the 0.8.3.4 remote page (it starts right under the tabs) and
# the settings sheet (from 操作方式); same sources (tools/shots_0833.mjs after). 0.8.3.3: the guides' remote-control / pairing pictures from the off-screen shots of tools/shots_0833.mjs
# (_shots/after/<lang>_<run><n>-<name>_<file>.png) -> _shots/tut/web/<lang>/<name>.webp. The other pictures
# stay as prep.py made them (0.8.3 ux-tutorial). Made-up data only (203.0.113.x, example addresses).
#   python docs/tutorial/src/shots/prep_0834.py zh-TW en ja ko
import cv2, numpy as np, os, sys
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', '_shots')
# name: (source suffix after "<lang>_", width px, crop (x0, y0, x1, y1) as fractions or None)
REMOTE = (0, 0.085, 1, 0.92)  # 0.8.3.4: the page starts right under the tabs
SPEC = {
    'remote-off': ('E1-remote-off_window.png', 520, REMOTE),
    'remote-lan': ('E2-remote-lan_window.png', 520, REMOTE),
    'remote-allow': ('A5-remote-guests_window.png', 520, REMOTE),
    'remote-fixed': ('A1-remote-allow_window.png', 520, REMOTE),
    'remote-preparing': ('B1-fixed-start_window.png', 520, REMOTE),
    'remote-enable': ('H1-fixed-enable_window.png', 520, REMOTE),
    'guest-new': ('A3-guest-form_guest_panel_96.png', 360, None),
    'guest-made': ('A4-guest-made_guest_panel_96.png', 360, None),
    'phone-login': ('A6-phone-login.png', 330, (0, 0.2, 1, 0.78)),
    'phone-login-guest': ('A6b-phone-login-guest.png', 330, (0, 0.2, 1, 0.78)),
    'phone-bar': ('P1-phone-bar.png', 360, (0, 0.8, 1, 1)),
    'phone-settings': ('P2-settings-0834.png', 360, (0, 0.05, 1, 1)),  # 0.8.3.4: the whole sheet (電腦喇叭靜音 in), remote/tools/phone_shots_0834.mjs after/<lang>_P2-settings.png copied as <lang>_P2-settings-0834.png
    'pair': ('D2-pair_pair.png', 460, None),
    'pair-steps': ('D3-pair-steps_pair.png', 460, None),
    'idle': ('D1-idle_window.png', 520, (0, 0, 1, 0.62)),
    'snapshot': ('F1-snapshot_window.png', 360, None),
}
for lang in sys.argv[1:]:
    out = f'{root}/tut/web/{lang}'
    os.makedirs(out, exist_ok=True)
    for name, (src, w, crop) in SPEC.items():
        p = f'{root}/after/{lang}_{src}'
        if not os.path.exists(p):
            print('missing', lang, src); continue
        im = cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_UNCHANGED)
        if im.ndim == 3 and im.shape[2] == 4:  # flatten on the guide's card colour
            a = im[:, :, 3:4].astype(np.float32) / 255
            bg = np.zeros_like(im[:, :, :3]); bg[:] = (0x1C, 0x1A, 0x23)
            im = (im[:, :, :3] * a + bg * (1 - a)).astype(np.uint8)
        if crop:
            h, ww = im.shape[:2]
            im = im[int(crop[1] * h):int(crop[3] * h), int(crop[0] * ww):int(crop[2] * ww)]
        s = w / im.shape[1]
        im = cv2.resize(im, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)
        ok, buf = cv2.imencode('.webp', im, [cv2.IMWRITE_WEBP_QUALITY, 86])
        buf.tofile(f'{out}/{name}.webp')
    print(lang, 'done', sum(os.path.getsize(f'{out}/{f}') for f in os.listdir(out)) // 1024, 'KB')
