"""Extract the 9 rounded-square icons from the ChatGPT mockup grid.

This version:
  - Saves with TRANSPARENT background instead of white pad.
  - Outputs at exactly 28x28 (matches kIconW) so AlphaBlend is 1:1.

Writes into WORKING/assets/icons/, NOT the top-level folder.
"""

import sys
import os
import time
from collections import deque

print("[DEBUG] extract_icons: start", flush=True)

try:
    from PIL import Image
    import numpy as np
except ImportError as e:
    print(f"[DEBUG] missing dep: {e} -- pip install pillow numpy", flush=True)
    sys.exit(1)

# CONFIGURE: SRC = path to the source mockup PNG containing the icon
# grid. OUT = repo's assets/icons directory (relative to this script).
import os
SRC = r"PATH\TO\YOUR\MOCKUP.png"
OUT = os.path.normpath(os.path.join(os.path.dirname(__file__),
                                    "..", "assets", "icons"))

# Grid layout of the mockup we're extracting from. NAMES is laid out
# left-to-right, top-to-bottom. ROWS * COLS == len(NAMES).
# May 20 11:55 mockup: 1-row horizontal pair, redrawn bolder by user.
# Left visual (stacked folders) = Open in New WINDOW (multiple windows).
# Right visual (single folder with +) = Open in New TAB (one new tab).
ROWS, COLS = 1, 2
NAMES = [
    "open_in_new_window",
    "open_in_new_tab",
]

CROP_MARGIN = 2
TARGET = 56   # 2x kIconW — runtime downscale via GDI+ bicubic
              # produces a smoother result than direct 28x28.

def main():
    t0 = time.time()
    print(f"[DEBUG] opening {SRC}", flush=True)
    img = Image.open(SRC).convert("RGB")
    W, H = img.size
    print(f"[DEBUG] image size = {W}x{H}", flush=True)

    arr = np.asarray(img).astype(np.int16)
    spread = arr.max(axis=2) - arr.min(axis=2)
    mask = spread > 8

    labels = np.zeros(mask.shape, dtype=np.int32)
    next_id = 0
    blobs = []

    h, w = mask.shape
    for y in range(h):
        if y % 200 == 0:
            print(f"[DEBUG] scan row {y}/{h}", flush=True)
        for x in range(w):
            if not mask[y, x] or labels[y, x]:
                continue
            next_id += 1
            stack = deque()
            stack.append((y, x))
            count = 0
            ymin = ymax = y
            xmin = xmax = x
            while stack:
                cy, cx = stack.pop()
                if cy < 0 or cy >= h or cx < 0 or cx >= w: continue
                if labels[cy, cx] or not mask[cy, cx]: continue
                labels[cy, cx] = next_id
                count += 1
                if cy < ymin: ymin = cy
                if cy > ymax: ymax = cy
                if cx < xmin: xmin = cx
                if cx > xmax: xmax = cx
                stack.append((cy+1, cx))
                stack.append((cy-1, cx))
                stack.append((cy, cx+1))
                stack.append((cy, cx-1))
            blobs.append({
                "id": next_id, "count": count,
                "ymin": ymin, "ymax": ymax,
                "xmin": xmin, "xmax": xmax,
            })

    def is_squareish(b, tol=0.4):
        bw = b["xmax"] - b["xmin"] + 1
        bh = b["ymax"] - b["ymin"] + 1
        if bw == 0 or bh == 0: return False
        ar = bw / bh
        return 1.0 - tol <= ar <= 1.0 + tol

    expected = ROWS * COLS
    candidates = [b for b in blobs if b["count"] > 1500 and is_squareish(b)]
    candidates.sort(key=lambda b: -b["count"])
    keepers = candidates[:expected]

    keepers.sort(key=lambda b: (b["ymin"] + b["ymax"]) / 2)
    rows = [keepers[i*COLS:(i+1)*COLS] for i in range(ROWS)]
    ordered = []
    for r in rows:
        r.sort(key=lambda b: (b["xmin"] + b["xmax"]) / 2)
        ordered.extend(r)

    print(f"[DEBUG] ordered keepers:", flush=True)
    for i, b in enumerate(ordered):
        print(f"  {i:2d} {NAMES[i]:11s} "
              f"x={b['xmin']}..{b['xmax']} y={b['ymin']}..{b['ymax']} "
              f"area={b['count']}", flush=True)

    os.makedirs(OUT, exist_ok=True)
    rgb_arr = np.asarray(img).astype(np.int16)

    for name, b in zip(NAMES, ordered):
        x0 = max(0, b["xmin"] - CROP_MARGIN)
        y0 = max(0, b["ymin"] - CROP_MARGIN)
        x1 = min(W, b["xmax"] + CROP_MARGIN + 1)
        y1 = min(H, b["ymax"] + CROP_MARGIN + 1)

        crop_rgb = rgb_arr[y0:y1, x0:x1, :]
        ch, cw, _ = crop_rgb.shape

        size = max(ch, cw)
        canvas_rgb = np.full((size, size, 3), 255, dtype=np.int16)
        oy = (size - ch) // 2
        ox = (size - cw) // 2
        canvas_rgb[oy:oy+ch, ox:ox+cw, :] = crop_rgb

        # Alpha curve: distance-from-white, threshold split. Strokes go
        # fully opaque past the threshold; pastel rounded-square BG
        # stays softly visible below. Threshold dropped from 80 to 50
        # because some newer mockups draw outlines at lighter
        # saturation (distance ~60-80) — those were hitting the linear
        # ramp instead of the full-opaque branch and reading as faint.
        #   distance >= 50  -> 255 (outline / strong colored region)
        #   distance <  50  -> distance * 3 (pastel background, light)
        min_chan = canvas_rgb.min(axis=2)
        distance = (255 - min_chan).astype(np.int32)
        alpha = np.where(
            distance >= 50, 255, distance * 3
        ).clip(0, 255).astype(np.uint8)

        rgb_uint8 = canvas_rgb.astype(np.uint8)
        rgba = np.dstack([rgb_uint8, alpha])

        out_img = Image.fromarray(rgba, mode="RGBA").resize(
            (TARGET, TARGET), Image.LANCZOS)
        out_path = os.path.join(OUT, f"{name}.png")
        out_img.save(out_path)
        print(f"  wrote {out_path}", flush=True)

    print(f"[DEBUG] total elapsed = {time.time()-t0:.2f}s", flush=True)

if __name__ == "__main__":
    main()
