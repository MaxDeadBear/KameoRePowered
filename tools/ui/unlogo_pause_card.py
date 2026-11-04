# Paints the game's logo out of the custom pause card, so the card can ship and the logo be drawn from the player's copy.
import sys, numpy as np
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
im = np.asarray(Image.open(src).convert('RGBA')).astype(float)
rgb = im[..., :3]
Y0, Y1 = 54, 234          # the logo, its glow and a margin
LINE = (120, 141)         # the ornament's double line
PANEL = 168               # first row inside the inner panel, below its top edge
SHIFT = 70                # the panel parchment copied in comes from this far below
F = 7                     # feather
band = rgb[:, 140:260]
detail = band - band.mean(axis=1, keepdims=True)
def span(y): return (289, 594) if y < 117 else (287, 618)   # leaves the two dots and the line's ends alone
def pingpong(i, n):
    k = i % (2 * n)
    return k if k < n else 2 * n - 1 - k
fill = rgb.copy()
w = np.zeros(rgb.shape[:2])
for y in range(Y0, Y1):
    x0, x1 = span(y)
    xs = np.arange(x0, x1)
    if LINE[0] <= y < LINE[1]:
        # the line's own colour for the row, with the parchment's grain over it, so it runs on without seams
        fill[y, x0:x1] = np.median(rgb[y, 270:298], axis=0) + 0.5 * detail[y, [pingpong(i, 120) for i in range(len(xs))]]
    elif y < PANEL:
        L = np.median(rgb[y - 1:y + 2, x0 - 30:x0 - 4].reshape(-1, 3), axis=0)
        R = np.median(rgb[y - 1:y + 2, x1 + 4:x1 + 30].reshape(-1, 3), axis=0)
        t = ((xs - x0) / (x1 - x0))[:, None]
        fill[y, x0:x1] = L * (1 - t) + R * t + detail[y, [pingpong(i, 120) for i in range(len(xs))]]
    else:
        copy = rgb[y + SHIFT, x0 - 12:x1 + 12]
        dL = np.median(rgb[y, x0 - 12:x0] - copy[:12], axis=0)
        dR = np.median(rgb[y, x1:x1 + 12] - copy[-12:], axis=0)
        t = ((xs - x0) / (x1 - x0))[:, None]
        fill[y, x0:x1] = copy[12:-12] + dL * (1 - t) + dR * t
    ramp = np.ones(len(xs))
    ramp[:F] = (np.arange(F) + 0.5) / F
    right = 3 if y < 117 else F   # the right-hand dot sits close to the logo's edge
    ramp[-right:] = ((np.arange(right) + 0.5) / right)[::-1]
    w[y, x0:x1] = ramp
# the bottom fades back into the original panel
for i, y in enumerate(range(Y1 - 16, Y1)): w[y] *= 1 - (i + 0.5) / 16
# the bottom rows' brightness correction must also blend toward the original below
res = im.copy()
res[..., :3] = np.clip(rgb * (1 - w[..., None]) + fill * w[..., None], 0, 255)
Image.fromarray(res.astype(np.uint8)).save(out, optimize=True)
