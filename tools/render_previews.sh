#!/bin/sh
# Renders host previews (identical pixels to the device frame buffer) into docs/screens/*.png.
set -e
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
MT_SCREENS_DIR="$OUT" pio test -e native -f test_ui -f test_admin
python3 - "$OUT" docs/screens <<'PY'
import sys, glob, os
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
for f in sorted(glob.glob(os.path.join(src, "*.pbm"))):
    name = os.path.splitext(os.path.basename(f))[0]
    Image.open(f).convert("L").save(os.path.join(dst, name + ".png"))
    print(os.path.join(dst, name + ".png"))
PY
rm -rf "$OUT"
