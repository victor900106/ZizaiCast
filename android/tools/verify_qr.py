"""Decodes a QR image with OpenCV and prints the text (exit 1 if none).

Used by pm_android_test to prove the generated pairing QR is scannable:
    py verify_qr.py qr.bmp
"""
import sys

import cv2


def main() -> int:
    img = cv2.imread(sys.argv[1])
    if img is None:
        print("cannot read image", file=sys.stderr)
        return 2
    text, points, _ = cv2.QRCodeDetector().detectAndDecode(img)
    if not text:
        print("no QR decoded", file=sys.stderr)
        return 1
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
