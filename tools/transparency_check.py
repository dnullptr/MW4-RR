"""Where a capture has transparent surfaces, and how RR's inputs and output behave there.

CLI: python tools/transparency_check.py CAPTURE [--threshold 0.05]   |   --selftest
The transparent region T is where colour and colour before transparency differ (relative luminance
difference above --threshold). For T and for the rest of the frame (C, sky excluded when the
status guide marks it) it prints: share and bounding box, transmittance (colour / colour before
transparency), spatial grain (high-pass luminance of colour and of RR's output, relative to the
local mean), frame-to-frame flicker of colour and output (pixels whose motion is under 0.5 px),
copied-row equality, guide statistics (albedo, normal, roughness detail, motion, depth, hit
distance) and whether the albedo and normal guides carry the detail seen through the glass.
Needs numpy only.
"""
import argparse
from pathlib import Path
import sys
import tempfile

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from read_capture import load  # noqa: E402


def luma(rgb):
    rgb = np.nan_to_num(np.asarray(rgb, np.float64)[..., :3], nan=0.0, posinf=0.0)
    return rgb @ np.array([0.2126, 0.7152, 0.0722])


def box3(a):
    p = np.pad(a, [(1, 1), (1, 1)] + [(0, 0)] * (a.ndim - 2), mode='edge')
    h, w = a.shape[:2]
    return sum(p[y:y + h, x:x + w] for y in range(3) for x in range(3)) / 9.0


def highpass_rel(y):
    """|y - box3(y)| / (box3(y) + eps): local grain relative to brightness."""
    m = box3(y)
    return np.abs(y - m) / (np.abs(m) + 1e-4)


def to_render(a, h, w):
    """Block-mean a larger image (RR's output) down to the render size."""
    if a.shape[:2] == (h, w):
        return a
    fy, fx = a.shape[0] / h, a.shape[1] / w
    ys = (np.arange(h) * fy).astype(int)
    xs = (np.arange(w) * fx).astype(int)
    return a[ys][:, xs]


def stat(values, mask):
    v = values[mask]
    v = v[np.isfinite(v)]
    return (float(np.median(v)), float(np.mean(v))) if v.size else (float('nan'),) * 2


def analyse(frames, threshold=0.05):
    first = frames[0]
    color = luma(first['color'])
    h, w = color.shape
    lines = []
    if 'color_before_transparency' not in first:
        return ['no color_before_transparency in this capture: the transparency guide was off']
    masks = []
    for f in frames:
        c, b = luma(f['color']), luma(f['color_before_transparency'])
        masks.append(np.abs(c - b) / (np.abs(c) + np.abs(b) + 1e-4) > threshold)
    T = np.logical_and.reduce(masks)
    sky = np.zeros_like(T)
    if 'status' in first:
        s = np.asarray(first['status'])
        s = s[..., 0] if s.ndim == 3 else s
        sky = s == 2 if s.dtype.kind in 'ui' else np.isclose(s, 2)
    C = ~np.logical_or.reduce(masks) & ~sky
    lines.append(f'render {w}x{h}, {len(frames)} frames; T = transparent in every frame, C = '
                 'never transparent and not sky')
    lines.append(f'T: {T.mean() * 100:.1f}% of the frame ({int(T.sum())} px); C: {C.mean() * 100:.1f}%')
    if not T.any():
        return lines + ['no transparent pixels found; try a lower --threshold']
    ys, xs = np.nonzero(T)
    lines.append(f'T bounding box x {xs.min()}-{xs.max()}, y {ys.min()}-{ys.max()}; '
                 f'5-95% x {int(np.percentile(xs, 5))}-{int(np.percentile(xs, 95))}, '
                 f'y {int(np.percentile(ys, 5))}-{int(np.percentile(ys, 95))}')
    rows = []

    def row(name, values, fmt='{:.3f}'):
        t, c = stat(values, T), stat(values, C)
        rows.append((name, fmt.format(t[0]), fmt.format(t[1]), fmt.format(c[0]), fmt.format(c[1])))

    c0, b0 = luma(first['color']), luma(first['color_before_transparency'])
    row('transmittance colour/before', c0 / (b0 + 1e-4))
    row('luminance colour', c0)
    row('grain colour (rel high-pass)', np.mean([highpass_rel(luma(f['color'])) for f in frames], 0))
    row('grain before-transparency', np.mean([highpass_rel(luma(f['color_before_transparency']))
                                              for f in frames], 0))
    out = None
    if 'rr_output' in first:
        out = [to_render(luma(f['rr_output']), h, w) for f in frames]
        row('grain RR output', np.mean([highpass_rel(o) for o in out], 0))
    still = np.ones((h, w), bool)
    if 'motion' in first:
        mv = np.stack([np.hypot(*np.moveaxis(np.asarray(f['motion'], np.float64)[..., :2], -1, 0))
                       for f in frames])
        still = (mv < 0.5).all(0)
        row('motion length (px)', mv.mean(0))
    if len(frames) > 1:
        cs = np.stack([luma(f['color']) for f in frames])
        flick = cs.std(0) / (np.abs(cs.mean(0)) + 1e-4)
        flick[~still] = np.nan
        row('flicker colour (rel std, still px)', flick)
        if out is not None:
            os_ = np.stack(out)
            fo = os_.std(0) / (np.abs(os_.mean(0)) + 1e-4)
            fo[~still] = np.nan
            row('flicker RR output (rel std)', fo)
    eq0 = np.zeros((h, w))
    eq0[0:h - 1:2] = (first['color'][0:h - 1:2, :, :3] == first['color'][1:h:2, :, :3]).all(-1)
    eq1 = np.zeros((h, w))
    eq1[1:h - 1:2] = (first['color'][1:h - 1:2, :, :3] == first['color'][2:h:2, :, :3]).all(-1)
    row('copied rows (2k,2k+1) share', eq0 * 2)
    row('copied rows (2k+1,2k+2) share', eq1 * 2)
    for name in ('diffuse_albedo', 'specular_albedo'):
        if name in first:
            a = luma(first[name])
            row(f'{name} mean', a)
            row(f'{name} detail (rel high-pass)', highpass_rel(a))
    if 'normal_roughness' in first:
        n = np.asarray(first['normal_roughness'], np.float64)
        row('normal detail (high-pass length)',
            np.linalg.norm(n[..., :3] - box3(n[..., :3]), axis=-1))
        row('roughness mean', n[..., 3])
    if 'depth' in first:
        d = np.asarray(first['depth'], np.float64)
        d = d[..., 0] if d.ndim == 3 else d
        row('depth mean', d, '{:.5f}')
        row('depth detail (rel high-pass)', highpass_rel(d))
    if 'specular_hit_distance' in first:
        hd = np.asarray(first['specular_hit_distance'], np.float64)
        row('specular hit distance', hd[..., 0] if hd.ndim == 3 else hd)
    # Do the guides follow what is seen through the glass (colour before transparency) or the
    # glass itself? Correlation of high-pass luminance inside T.
    hb = b0 - box3(b0)
    for name in ('diffuse_albedo', 'normal_roughness', 'depth'):
        if name in first:
            g = np.asarray(first[name], np.float64)
            g = luma(g) if g.ndim == 3 and g.shape[-1] >= 3 else (g[..., 0] if g.ndim == 3 else g)
            hg = g - box3(g)
            a, b = hb[T], hg[T]
            ok = np.isfinite(a) & np.isfinite(b)
            r = np.corrcoef(a[ok], b[ok])[0, 1] if ok.sum() > 2 and a[ok].std() and b[ok].std() else float('nan')
            rows.append((f'detail corr: {name} vs scene behind', f'{r:.3f}', '', '', ''))
    width = max(len(r[0]) for r in rows)
    lines.append(f'{"":{width}}  {"T med":>9} {"T mean":>9} {"C med":>9} {"C mean":>9}')
    lines += [f'{r[0]:{width}}  {r[1]:>9} {r[2]:>9} {r[3]:>9} {r[4]:>9}' for r in rows]
    return lines


def selftest():
    rng = np.random.default_rng(1)
    h, w = 64, 96
    frames = []
    for i in range(4):
        before = rng.uniform(0.2, 0.4, (h, w, 3)).astype(np.float32)
        color = before.copy()
        color[16:48, 24:72] *= 0.5  # a pane that halves the light
        color[16:48, 24:72] += rng.normal(0, 0.02, (32, 48, 3)).astype(np.float32) * (i + 1)
        frames.append({'color': color, 'color_before_transparency': before,
                       'rr_output': np.repeat(np.repeat(color, 2, 0), 2, 1),
                       'motion': np.zeros((h, w, 2), np.float32),
                       'diffuse_albedo': np.full((h, w, 3), 0.5, np.float32),
                       'depth': np.full((h, w), 0.5, np.float32)})
    lines = analyse(frames)
    text = '\n'.join(lines)
    T = [l for l in lines if l.startswith('T:')][0]
    assert 23 <= float(T.split()[1].rstrip('%')) <= 25, T
    assert 'T bounding box x 24-71, y 16-47' in text, text
    trans = [l for l in lines if l.startswith('transmittance')][0].split()
    assert abs(float(trans[2]) - 0.5) < 0.1 and abs(float(trans[4]) - 1) < 0.01, trans
    print(text)
    print('selftest PASS')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture', nargs='?')
    ap.add_argument('--threshold', type=float, default=0.05)
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.capture:
        ap.error('CAPTURE is required')
    manifest, frames = load(a.capture)
    try:
        from analyze_sky_edges import undo_tests
        frames, notes = undo_tests(manifest, frames)
        for n in notes or []:
            print('note:', n)
    except Exception as e:  # weapon depth without a mapping: depth stays linear
        print('note: test passes not undone:', e)
    print('\n'.join(analyse(frames, a.threshold)))


if __name__ == '__main__':
    main()
