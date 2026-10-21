# _shots/tut/<lang>/*.png -> _shots/tut/web/<lang>/<name>.webp (resized / cropped for the guides)
import cv2, numpy as np, os, sys
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', '_shots', 'tut')
# name: (source, width px, crop (x0, y0, x1, y1) as fractions or None)
SPEC = {
    'idle': ('a_idle_window.png', 520, (0, 0, 1, 0.62)),
    'pair': ('a_pair_pair.png', 460, None),
    'toolbar': ('b_toolbar_window.png', 900, (0.03, 0.012, 0.97, 0.068)),
    'toolbar-android': ('d_android_window.png', 900, (0.03, 0.012, 0.97, 0.068)),
    'snapshot': ('b_snapshot_window.png', 360, None),
    'recording': ('b_recording_window.png', 360, None),
    'ink': ('b_ink_window.png', 360, None),
    'translate': ('c_translate_window.png', 360, None),
    'android': ('d_android_window.png', 360, None),
    'drop': ('d_drop_window.png', 360, None),
    'picker': ('b_picker_picker.png', 520, None),
    'share': ('b_share_share.png', 420, None),
    'remote-off': ('e_remote-off_window.png', 520, (0, 0.2, 1, 0.88)),
    'remote-lan': ('e_remote-lan_window.png', 520, (0, 0.2, 1, 0.88)),
    'remote-fixed': ('f_remote-fixed_window.png', 520, (0, 0.2, 1, 0.88)),
    'phone-login': ('f_phone-login.png', 330, (0, 0.2, 1, 0.62)),
    'phone-settings': ('h_phone-settings.png', 360, (0, 0.27, 1, 1)),
    'phone-bar': ('h_phone-bar.png', 360, (0, 0.82, 1, 1)),
    'update': ('g_update_update.png', 520, None),
}
for lang in sys.argv[1:]:
    out = f'{root}/web/{lang}'
    os.makedirs(out, exist_ok=True)
    for name, (src, w, crop) in SPEC.items():
        p = f'{root}/{lang}/{src}'
        if not os.path.exists(p):
            print('missing', lang, src); continue
        im = cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_UNCHANGED)
        if im.shape[2] == 4:  # flatten on the guide's card colour
            a = im[:, :, 3:4].astype(np.float32) / 255
            bg = np.zeros_like(im[:, :, :3]); bg[:] = (0x1C, 0x1A, 0x23)
            im = (im[:, :, :3] * a + bg * (1 - a)).astype(np.uint8)
        if crop:
            h, ww = im.shape[:2]
            im = im[int(crop[1] * h):int(crop[3] * h), int(crop[0] * ww):int(crop[2] * ww)]
        s = w / im.shape[1]
        im = cv2.resize(im, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)
        ok, buf = cv2.imencode('.webp', im, [cv2.IMWRITE_WEBP_QUALITY, 84])
        buf.tofile(f'{out}/{name}.webp')
    print(lang, 'done', sum(os.path.getsize(f'{out}/{f}') for f in os.listdir(out)) // 1024, 'KB')
