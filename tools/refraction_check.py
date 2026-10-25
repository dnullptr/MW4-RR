#!/usr/bin/env python3
"""Check the copied row pairs that screen-space refraction leaves in RR's colour input.

Behind window glass the game's refraction resamples a noisy half-resolution image, so adjacent rows
of RR's colour come in copied pairs. Per T x T tile and row parity (ph0 = rows 2k,2k+1 "aligned",
ph1 = rows 2k+1,2k+2 "offset"): share of bit-equal packed texels among pairs with both pixels
nonzero, share of exact-zero pixels, correlation of the pairs' high-pass linear luminance
((r+2g+b)/4 minus its 3x3 box mean). Refraction tiles (over all frames): one parity's equality
>= --eq-min and >= --asym x the other's (flat colour such as sky is equal in both), or zeros >=
--zero-min with mean luminance > 0; the rest is the control. With --compare (e.g. refraction dropout
off vs on, same spot) both use the union of their refraction tiles. Reads only color and
color_before_transparency.
Usage:  python3 -I tools/refraction_check.py CAPTURE [--compare OTHER] [--tile 32] [--eq-min 0.15]
                                             [--asym 2] [--zero-min 0.05]   |   ... --selftest
"""
import argparse
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I ignores cwd and PYTHONPATH
from read_capture import decode_image, decode_r11g11b10, load  # noqa: E402

PARITIES = (0, 1)
SUMS = ('nz', 'eq', 'n', 'a', 'b', 'aa', 'bb', 'ab')  # per-parity sums kept per tile
COLS = ('eq0', 'eq1', 'r0', 'r1', 'zero', 'cbt')


def same(x, y):
    """Per-pixel equality of keys: raw packed uint32 (H, W) or decoded floats (H, W, C)."""
    return np.all(x == y, axis=tuple(range(2, np.ndim(x))))


def frame_stats(colour, key, cbt, tile):
    """Per-tile sums of one frame: name -> float64 array (tiles,), tiles in row-major order."""
    h, w = key.shape[:2]
    ntx = -(-w // tile)
    count = ntx * -(-h // tile)

    def per_tile(rows, weights=None):
        ids = ((rows // tile)[:, None] * ntx + np.arange(w)[None, :] // tile).ravel()
        return np.bincount(ids, None if weights is None else weights.ravel(), minlength=count)

    rgb = colour[..., :3].astype(np.float64)
    lum = (rgb[..., 0] + 2 * rgb[..., 1] + rgb[..., 2]) / 4
    lum = np.where(np.isfinite(lum), lum, 0)
    pad = np.pad(lum, 1, mode='edge')
    high = lum - sum(pad[y:y + h, x:x + w] for y in range(3) for x in range(3)) / 9
    zero, rows = same(key, 0), np.arange(h)
    s = {'pix': per_tile(rows), 'zero': per_tile(rows, zero), 'lum': per_tile(rows, lum),
         'cbt_pix': np.zeros(count), 'cbt_zero': np.zeros(count)}
    if cbt is not None and cbt.shape[:2] == (h, w):
        s['cbt_pix'], s['cbt_zero'] = s['pix'], per_tile(rows, same(cbt, 0))
    for p in PARITIES:
        top = np.arange(p, h - 1, 2)  # a pair belongs to the tile of its top row
        a, b = high[top], high[top + 1]
        nz = ~zero[top] & ~zero[top + 1]
        eq = same(key[top], key[top + 1]) & nz
        for name, v in zip(SUMS, (nz, eq, None, a, b, a * a, b * b, a * b)):
            s[f'{name}{p}'] = per_tile(top, v)
    return s


def analyse(folder, tile):
    """Reads the capture one frame at a time; the key of R11G11B10 colour is the raw uint32."""
    path = Path(folder) / 'manifest.json'
    if not path.is_file():
        raise FileNotFoundError(f'{path}: missing (not a capture, or still being written)')
    manifest = json.loads(path.read_text(encoding='utf-8'))
    if manifest.get('format') != 'mw4-rr-capture' or not manifest.get('frames'):
        raise ValueError(f'{path}: not an mw4-rr capture manifest, or no frames')
    per, shape = [], None
    for i, frame in enumerate(manifest['frames']):
        got = {}
        for name in ('color', 'color_before_transparency'):
            info = frame['inputs'].get(name)
            if info is not None:
                data = (Path(folder) / info['file']).read_bytes()
                image = decode_image(data, info, Path(folder) / info['file'])
                got[name] = (image, np.frombuffer(data, '<u4').reshape(image.shape[:2])
                             if info['dxgi_format'] == 26 else image)
        if 'color' not in got or shape not in (None, got['color'][0].shape[:2]):
            raise ValueError(f'{folder}: frame {i} has no color input, or another colour size')
        shape = got['color'][0].shape[:2]
        per.append(frame_stats(*got['color'], got.get('color_before_transparency', [None])[-1],
                               tile))
    stats = {name: np.stack([s[name] for s in per]) for name in per[0]}
    return {'folder': folder, 'manifest': manifest, 'shape': shape, 'stats': stats}


def ratio(a, b):
    return a / np.where(b > 0, b, np.nan)  # nan where nothing was counted


def mean(x):
    x = np.asarray(x)[np.isfinite(x)]
    return float(x.mean()) if x.size else float('nan')


def refraction_tiles(stats, opts):
    tot = {k: v.sum(0) for k, v in stats.items()}
    eq = [np.nan_to_num(ratio(tot[f'eq{p}'], tot[f'nz{p}'])) for p in PARITIES]
    hi, lo = np.maximum(*eq), np.minimum(*eq)
    copied = (hi >= opts.eq_min) & (hi >= opts.asym * lo)
    dropped = (np.nan_to_num(ratio(tot['zero'], tot['pix'])) >= opts.zero_min) & (tot['lum'] > 0)
    return copied | dropped


def summarise(stats, mask):
    """Per-frame values over the tiles in mask: name -> array (frames,)."""
    s = {k: v[:, mask].sum(1) for k, v in stats.items()}
    out = {'zero': ratio(s['zero'], s['pix']), 'cbt': ratio(s['cbt_zero'], s['cbt_pix'])}
    for p in PARITIES:
        n = s[f'n{p}']
        ma, mb = ratio(s[f'a{p}'], n), ratio(s[f'b{p}'], n)
        va, vb = ratio(s[f'aa{p}'], n) - ma * ma, ratio(s[f'bb{p}'], n) - mb * mb
        out[f'eq{p}'] = ratio(s[f'eq{p}'], s[f'nz{p}'])
        out[f'r{p}'] = ratio(ratio(s[f'ab{p}'], n) - ma * mb, np.sqrt(np.maximum(va * vb, 0)))
    return out


def cells(values, names):
    return ''.join(f'{"-":>7}' if not np.isfinite(values[n]) else
                   f'{values[n]:7.2f}' if n[0] == 'r' else f'{100 * values[n]:7.1f}' for n in names)


def report(caps, opts):
    """Print the report for one capture, or two with --compare; returns per-capture summaries."""
    own = [refraction_tiles(cap['stats'], opts) for cap in caps]
    region = np.logical_or.reduce(own)
    print('R = refraction region, C = control (rest of frame); eq = bit-equal share of both-nonzero'
          ' pairs,\nr = high-pass luminance correlation of the pairs, cbt0 = zeros in '
          'color_before_transparency')
    results = []
    for tag, cap, mine in zip('AB', caps, own):
        stats, (h, w), pix = cap['stats'], cap['shape'], cap['stats']['pix'][0]
        flags = {k: v for k, v in cap['manifest'].get('settings', {}).items()
                 if 'dropout' in k or 'refract' in k}
        print(f'== {tag}: {cap["folder"]}  ({len(stats["pix"])} frames, colour {w}x{h}, '
              f'tile {opts.tile}{", " + json.dumps(flags) if flags else ""})')
        union = f' (union of both captures; this one alone: {int(mine.sum())})' * (len(caps) > 1)
        print(f'region: {int(region.sum())} tiles, {int(pix[region].sum())} px{union}; '
              f'control: {int((~region).sum())} tiles, {int(pix[~region].sum())} px')
        reg, ctl = summarise(stats, region), summarise(stats, ~region)
        copied = int(mean(reg['eq1']) > mean(reg['eq0'])) if region.any() else None
        if copied is not None:
            print(f'copied pairs (higher equality in region): ph{copied} = '
                  f'{["aligned rows (2k,2k+1)", "offset rows (2k+1,2k+2)"][copied]}')
        head = ''.join(f'{c:>7}' for c in ('eq0%', 'eq1%', 'r0', 'r1', 'zero%', 'cbt0%'))
        print(f'{"frame":>7}  R{head} | C{head[:35]}')
        frames = [(i, lambda v, i=i: v[i]) for i in range(len(stats['pix']))]
        for label, pick in frames + [('mean', mean)]:
            print(f'{label:>7}   ' + cells({k: pick(v) for k, v in reg.items()}, COLS) + ' |  '
                  + cells({k: pick(v) for k, v in ctl.items()}, COLS[:5]))
        results.append({'own': mine, 'region': reg, 'control': ctl, 'copied': copied})
    p = results[0]['copied']
    if p is None:
        print('verdict: no refraction tiles found (no copied row pairs, no dropout zeros)')
        return results
    rs = ' -> '.join(f'{mean(r["region"][f"r{p}"]):.2f}' for r in results)
    zs = ' -> '.join(f'{100 * mean(r["region"]["zero"]):.1f}%' for r in results)
    print(f'copied-pair correlation in refraction region: {rs} (ph{p}); zero pixels {zs}')
    if any(r['copied'] != p for r in results):
        print('note: the captures disagree on which parity holds the copied pairs')
    return results


# ---- self-test on synthetic captures ---------------------------------------------------------
H, W, WIN = 128, 256, (slice(32, 96), slice(64, 160))  # window 96x64, aligned to 32 px tiles
NAMES = ('color depth motion normal_roughness diffuse_albedo specular_albedo status sss_guide '
         'color_before_transparency responsivity_mask specular_hit_distance rr_output').split()


def encode_r11g11b10(rgb):
    """float HxWx3 (finite, 0 <= v < 65536) -> packed uint32; truncates, flushes v < 2**-14 to 0."""
    out = np.zeros(rgb.shape[:-1], np.uint32)
    for ch, (bits, shift) in enumerate([(6, 0), (6, 11), (5, 22)]):
        v = rgb[..., ch].astype(np.float64)
        f, e = np.frexp(v)  # v = f * 2**e, 0.5 <= f < 1, so the biased exponent is e - 1 + 15
        field = ((e + 14) << bits) | np.floor((2 * f - 1) * (1 << bits)).astype(np.int64)
        out |= np.where(v >= 2.0 ** -14, field, 0).astype(np.uint32) << shift
    return out


def synth_frames(rng, count=8):
    """Independent noise, except the window: a half-res field upsampled 2x with a one-row offset
    (rows 2k+1 and 2k+2 copy half-res row k+1; odd columns interpolate their two neighbours)."""
    frames, cols = [], np.arange(W)
    for _ in range(count):
        image = rng.uniform(0.05, 1.0, (H, W, 3))
        half = rng.uniform(0.05, 1.0, (H // 2 + 1, W // 2 + 1, 3))[(np.arange(H) + 1) // 2]
        image[WIN] = ((half[:, cols // 2] + half[:, (cols + 1) // 2]) / 2)[WIN]
        frames.append(encode_r11g11b10(image))
    return frames


def write_capture(folder, frames):
    folder.mkdir()
    entries = []
    for i, arrays in enumerate(frames):
        inputs = dict.fromkeys(NAMES)
        for name, packed in arrays.items():
            (folder / f'frame{i}_{name}.bin').write_bytes(packed.astype('<u4').tobytes())
            inputs[name] = {'file': f'frame{i}_{name}.bin', 'format': 'R11G11B10_FLOAT',
                            'dxgi_format': 26, 'width': W, 'height': H, 'bytes_per_pixel': 4}
        entries.append({'index': i, 'serial': 100 + i, 'delivered': True, 'reset': False,
                        'reset_reasons': 0, 'inputs': inputs})
    manifest = {'format': 'mw4-rr-capture', 'version': 1, 'frame_count': len(frames),
                'render_width': W, 'render_height': H, 'settings': {}, 'frames': entries}
    (folder / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')  # last


def selftest():
    opts = argparse.Namespace(tile=32, eq_min=0.15, asym=2.0, zero_min=0.05)
    base = synth_frames(np.random.default_rng(1))
    rng, dropped = np.random.default_rng(2), []
    for packed in base:  # dropout P = 0.5 in the window (white noise), survivors x2 (exact)
        rgb = decode_r11g11b10(packed)
        rgb[WIN] = np.where(rng.random((H, W, 1))[WIN] >= 0.5, rgb[WIN] * 2, 0)
        dropped.append(encode_r11g11b10(rgb))
    with tempfile.TemporaryDirectory() as tmp:
        off, on = Path(tmp) / 'dropout_off', Path(tmp) / 'dropout_on'
        write_capture(off, [{'color': f} for f in base])
        write_capture(on, [{'color': f, 'color_before_transparency': f} for f in dropped])
        _, frames = load(on)  # the real reader accepts the synthetic capture
        reader_ok = (sorted(frames[0]) == ['color', 'color_before_transparency'] and
                     np.array_equal(frames[5]['color'], decode_r11g11b10(dropped[5])))
        caps = [analyse(off, opts.tile), analyse(on, opts.tile)]
        results = report(caps, opts)
    window = np.isin(np.arange(32), [10, 11, 12, 18, 19, 20])  # 8x4 tiles, row-major
    a, b = (summarise(cap['stats'], window) for cap in caps)
    control = max(abs(mean(summarise(cap['stats'], ~window)[f'r{p}']))
                  for cap in caps for p in PARITIES)
    checks = [('the reader loads the synthetic capture', reader_ok),
              (f'A offset-parity equality in window {mean(a["eq1"]):.3f} > 0.25',
               mean(a['eq1']) > 0.25 and results[0]['copied'] == 1),
              ('refraction region == window in A and in B',
               all(np.array_equal(r['own'], window) for r in results)),
              (f'copied-pair correlation {mean(a["r1"]):.2f} -> {mean(b["r1"]):.2f} drops >= 0.3',
               mean(a['r1']) - mean(b['r1']) >= 0.3),
              (f'window zeros A {mean(a["zero"]):.4f} ~ 0, B {mean(b["zero"]):.3f} ~ 0.5',
               mean(a['zero']) < 0.005 and abs(mean(b['zero']) - 0.5) < 0.05),
              (f'B color_before_transparency zeros {mean(b["cbt"]):.3f} ~ 0.5',
               abs(mean(b['cbt']) - 0.5) < 0.05),
              (f'control max |r| {control:.3f} < 0.2', control < 0.2)]
    for text, ok in checks:
        print(f'{"PASS" if ok else "FAIL"}  {text}')
    passed = all(ok for _, ok in checks)
    print('selftest PASS' if passed else 'selftest FAIL')
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('folder', nargs='?', type=Path, help='capture folder (has manifest.json)')
    parser.add_argument('--compare', type=Path, metavar='OTHER_FOLDER',
                        help='second capture from the same spot (e.g. dropout on); same size')
    parser.add_argument('--tile', type=int, default=32, help='tile size in pixels (32)')
    parser.add_argument('--eq-min', type=float, default=0.15, help='refraction: equality (0.15)')
    parser.add_argument('--asym', type=float, default=2.0, help='... and x other parity (2)')
    parser.add_argument('--zero-min', type=float, default=0.05, help='or zero share (0.05)')
    parser.add_argument('--selftest', action='store_true', help='run on synthetic captures')
    opts = parser.parse_args()
    if opts.selftest:
        return selftest()
    if opts.folder is None or opts.tile < 2:
        parser.error('a capture folder is required unless --selftest is given; --tile >= 2')
    try:
        caps = [analyse(f, opts.tile) for f in [opts.folder] + [opts.compare] * bool(opts.compare)]
        if len({cap['shape'] for cap in caps}) > 1:
            raise ValueError('--compare needs two captures with the same colour size')
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 2
    report(caps, opts)
    return 0


if __name__ == '__main__':
    sys.exit(main())
