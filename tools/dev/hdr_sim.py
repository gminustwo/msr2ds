"""HDR output worked out offline from the HDR pass's inputs (video.c PWR_HDR_DUMP), for choosing HDR settings without
judging an HDR10 window from screen captures (they look washed out).

  PWR_HDR_DUMP=9,12 ... (the game, HDR on) writes hdr_dump_<n>.txt, _game.rgba and _ui.rgba
  python -I tools/dev/hdr_sim.py <folder or dump prefix ...> --out <folder> [--set name:paper:peak:knee:desat ...]

For each dump and each setting it reproduces hdr.frag (the picture in nits before the PQ encoding; BT.709 to BT.2020
keeps the luminance) and writes:
  - <name>_view.png: the picture as an HDR display shows it, scaled so paper white is SDR's 80% gray level and the
    brightest the setting reaches compressed into what is left (the same scale for every setting, so brighter
    highlights look brighter)
  - <name>_heat.png: the luminance over paper white in false color: gray under 1x, green 1-1.5x, yellow 1.5-2.5x,
    orange 2.5-4x, red 4x and over
and prints the share of the picture over 1.5x and 3x paper white, the mean light level against SDR's and the brightest.
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image


def load(prefix):
    meta = {}
    with open(prefix + ".txt") as f:
        for line in f:
            k, v = line.split()
            meta[k] = float(v)
    w, h = int(meta["width"]), int(meta["height"])
    def rgba(part):
        a = np.fromfile(prefix + "_" + part + ".rgba", dtype=np.uint8).reshape(h, w, 4).astype(np.float32) / 255.0
        if meta.get("bgra"):
            a = a[..., [2, 1, 0, 3]]
        return a
    return meta, rgba("game"), rgba("ui")


def linear(c):
    return np.power(np.clip(c, 0.0, None), 2.2)


LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def expand(lin, paper, peak, knee, desat=0.0, shape=2.0):
    """hdr.frag's expand(): luminance over the knee rises to peak/paper at SDR white (t^shape), keeping its hue while
    the color moves toward white by desat * t (bright light desaturates)"""
    r = peak / paper
    y = lin @ LUMA
    if r <= 1.0:
        return lin
    t = np.clip((y - knee) / (1.0 - knee), 0.0, 1.0)
    gain = np.where(y > knee, (y + (r - 1.0) * t ** shape) / np.maximum(y, 1e-6), 1.0)
    out = lin * gain[..., None]
    if desat > 0:
        yo = (y * gain)[..., None]
        s = (desat * t)[..., None]
        out = out * (1 - s) + yo * s
    return out


def hdr_nits(game, ui, paper, peak, knee, desat=0.0, shape=2.0):
    lin = linear(game[..., :3])
    a = ui[..., 3:4]
    cover = np.clip(a / 0.25, 0, 1); cover = cover * cover * (3 - 2 * cover)   # smoothstep(0, 0.25, a)
    g = (expand(lin, paper, peak, knee, desat, shape) * (1 - cover) + lin * cover) * paper
    ul = np.where(a > 0, linear(ui[..., :3] / np.maximum(a, 1e-6)) * paper, 0.0)
    return g * (1 - a) + ul * a


def view(nits, paper, top):
    """paper white at SDR's 80% gray level (0.8 encoded), above it a smooth roll-off that reaches white at `top` nits"""
    v = nits / paper
    knee_v = 1.0                          # paper white
    lo = 0.8 ** 2.2                       # paper white's linear level on the preview
    over = np.clip(v - knee_v, 0, None)
    span = max(top / paper - knee_v, 1e-3)
    hi = lo + (1 - lo) * (1 - np.exp(-3.0 * over / span)) / (1 - np.exp(-3.0))
    out = np.where(v <= knee_v, v * lo, hi)
    return np.clip(out, 0, 1) ** (1 / 2.2)


def heat(nits, paper):
    y = (nits @ LUMA) / paper
    out = np.empty(y.shape + (3,), np.float32)
    gray = np.clip(y, 0, 1) ** (1 / 2.2) * 0.6
    out[:] = gray[..., None]
    for lo, hi, col in ((1.0, 1.5, (0.2, 0.8, 0.2)), (1.5, 2.5, (0.95, 0.9, 0.1)), (2.5, 4.0, (1.0, 0.55, 0.0)), (4.0, 1e9, (0.95, 0.1, 0.1))):
        m = (y > lo) & (y <= hi)
        out[m] = col
    return out


def stats(nits, game, paper):
    y = (nits @ LUMA) / paper
    sdr = linear(game[..., :3]) @ LUMA
    return {
        "over1.5": float((y > 1.5).mean() * 100), "over3": float((y > 3.0).mean() * 100),
        "mean": float(y.mean()), "sdr_mean": float(sdr.mean()), "max_nits": float((nits @ LUMA).max()),
    }


def save(img, path):
    Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dumps", nargs="+", help="dump prefixes (…/hdr_dump_0) or folders holding them")
    ap.add_argument("--out", required=True)
    ap.add_argument("--set", action="append", default=[],
                    help="name:paper:peak:knee[:desat[:shape]] (any of them empty or 'dump': the dump's own)")
    ap.add_argument("--top", type=float, default=0, help="the preview's white in nits (default: the highest peak asked)")
    args = ap.parse_args()
    prefixes = []
    for d in args.dumps:
        if os.path.isdir(d):
            prefixes += sorted(os.path.join(d, f[:-4]) for f in os.listdir(d) if f.endswith(".txt") and "hdr_dump" in f)
        else:
            prefixes.append(d)
    os.makedirs(args.out, exist_ok=True)
    sets = args.set or ["dump:dump:dump:dump"]
    for p in prefixes:
        meta, game, ui = load(p)
        base = os.path.basename(p)
        parsed = []
        for s in sets:
            f = s.split(":") + [""] * 6
            name = f[0]
            paper = meta["paper"] if f[1] in ("", "dump") else float(f[1])
            peak = meta["peak"] if f[2] in ("", "dump") else float(f[2])
            knee = meta["knee"] if f[3] in ("", "dump") else float(f[3])
            desat = meta.get("desat", 0.0) if f[4] in ("", "dump") else float(f[4])
            shape = float(f[5]) if f[5] else 2.0
            parsed.append((name, paper, peak, knee, desat, shape))
        top = args.top or max(x[2] for x in parsed)
        for name, paper, peak, knee, desat, shape in parsed:
            nits = hdr_nits(game, ui, paper, peak, knee, desat, shape)
            save(view(nits, paper, top), os.path.join(args.out, f"{base}_{name}_view.png"))
            save(heat(nits, paper), os.path.join(args.out, f"{base}_{name}_heat.png"))
            st = stats(nits, game, paper)
            print(f"{base} {name:>10} paper {paper:5.0f} peak {peak:5.0f} knee {knee:.2f} desat {desat:.2f}: "
                  f"over 1.5x {st['over1.5']:5.1f}%  over 3x {st['over3']:5.1f}%  mean {st['mean']:.3f}x paper "
                  f"(SDR {st['sdr_mean']:.3f})  brightest {st['max_nits']:6.0f} nits")


if __name__ == "__main__":
    sys.exit(main())
