"""Decodes a QR image with OpenCV and prints the text (exit 1 if none).

Used by pm_android_test to prove the generated pairing QR is scannable:
    py verify_qr.py qr.bmp

OpenCV 4.10's classic QRCodeDetector misses some perfectly valid, perfectly
sharp renderings (8 px/module, no blur), which made the self-test fail at
random.  So a few decoders are tried: the classic one and the ArUco-based one
(QRCodeDetectorAruco), each on the image as is and on a downscaled copy.  The
first non-empty result wins; pm_android_test still compares it with the exact
text that was encoded.
"""
import sys

import cv2


def detectors():
    yield cv2.QRCodeDetector()
    if hasattr(cv2, "QRCodeDetectorAruco"):
        yield cv2.QRCodeDetectorAruco()


def images(img):
    yield img
    h, w = img.shape[:2]
    for f in (0.5, 0.75):
        yield cv2.resize(img, (max(1, int(w * f)), max(1, int(h * f))), interpolation=cv2.INTER_AREA)


def decode(img) -> str:
    for im in images(img):
        for det in detectors():
            try:
                text, _, _ = det.detectAndDecode(im)
            except cv2.error:
                continue
            if text:
                return text
    return ""


def main() -> int:
    img = cv2.imread(sys.argv[1])
    if img is None:
        print("cannot read image", file=sys.stderr)
        return 2
    text = decode(img)
    if not text:
        print("no QR decoded", file=sys.stderr)
        return 1
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
