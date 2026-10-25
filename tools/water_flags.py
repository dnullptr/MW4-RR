"""Where an RR capture's special and water pixels are, to see whether the game marks a river.

The Status guide is 3 on the game's special surfaces (PackedSurface w&2), which carry either the
prepass water flag (w&4: lakes, distant water) or the class override (w&8: the river in Andi's
captures). With a water test on, the disocclusion or animated texture mask is the add-on's water
classification. Builds before 2026-10-25 took only w&4 as water, so there red (special, not water)
is override pixels. Later builds take every special pixel, so red only shows on a frame the add-on
rejected whole; a special pixel rejected on its own is status 4, magenta like every invalid pixel.
Without a water mask every special pixel is red. Untinted pixels are ordinary surfaces.

CLI: python tools/water_flags.py FOLDER [--frame N]   |   python tools/water_flags.py --self-test
Prints per-frame shares and a coarse screen grid, and writes FOLDER/images/water_flags_frameN.png:
colour with water cyan, special non-water red and invalid (status 4) magenta.
"""
import argparse
from pathlib import Path
import struct
import sys
import tempfile
import zlib

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from read_capture import load  # noqa: E402

GRID = (6, 4)  # columns, rows


def png_bytes(rgb):
    """uint8 (H, W, 3) -> PNG file bytes."""
    height, width = rgb.shape[:2]
    rows = np.concatenate([np.zeros((height, 1), np.uint8), rgb.reshape(height, width * 3)], axis=1)

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack(
            '>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
            chunk(b'IDAT', zlib.compress(rows.tobytes(), 6)) + chunk(b'IEND', b''))


def water_mask(arrays):
    """The add-on's water classification for this frame, or None without a water mask."""
    if 'disocclusion_mask' in arrays:
        return arrays['disocclusion_mask'] > 0
    if 'animated_texture_mask' in arrays:
        return arrays['animated_texture_mask'] > 0.5
    return None


def pct(part, whole):
    return 100.0 * part / whole if whole else 0.0


def grid_text(mask, label):
    height, width = mask.shape
    columns, rows = GRID
    lines = [f'  {label} % per screen cell ({columns}x{rows}, top row first):']
    for r in range(rows):
        y0, y1 = r * height // rows, (r + 1) * height // rows
        cells = []
        for c in range(columns):
            x0, x1 = c * width // columns, (c + 1) * width // columns
            cells.append(f'{pct(int(mask[y0:y1, x0:x1].sum()), (y1 - y0) * (x1 - x0)):5.1f}')
        lines.append('    ' + ' '.join(cells))
    return lines


def frame_report(index, arrays):
    status = arrays.get('status')
    if status is None:
        return [f'frame {index}: no status guide']
    status = status.astype(np.int64)
    total = status.size
    special = status == 3
    water = water_mask(arrays)
    shares = ' '.join(f'{v}={pct(int((status == v).sum()), total):.2f}%' for v in range(5))
    lines = [f'frame {index}: status {shares}']
    if water is None:
        lines.append('  no water mask in this capture (turn on a water discard or animated test)')
        water = np.zeros_like(special)
    else:
        lines.append(f'  water {pct(int(water.sum()), total):.2f}%, special and water '
                     f'{pct(int((special & water).sum()), total):.2f}%, special not water '
                     f'{pct(int((special & ~water).sum()), total):.2f}%, water not special '
                     f'{pct(int((water & ~special).sum()), total):.2f}%')
    if special.any():
        parts = []
        if 'normal_roughness' in arrays:
            roughness = arrays['normal_roughness'][..., 3][special]
            parts.append(f'roughness {float(roughness.mean()):.3f}')
        for name in ('specular_albedo', 'diffuse_albedo'):
            if name in arrays:
                parts.append(f'{name} {float(arrays[name][..., :3][special].mean()):.3f}')
        if parts:
            lines.append('  special pixels mean: ' + ', '.join(parts))
    lines += grid_text(special & ~water, 'special not water')
    lines += grid_text(water, 'water')
    return lines


def overlay(arrays):
    status = arrays['status'].astype(np.int64)
    color = arrays.get('color')
    if color is None:
        base = np.full(status.shape + (3,), 0.5, np.float32)
    else:
        rgb = np.nan_to_num(color[..., :3].astype(np.float32), nan=0, posinf=0, neginf=0)
        lum = 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]
        scale = np.percentile(lum, 99) or 1.0
        base = np.clip(rgb / scale, 0, 1) ** (1 / 2.2)
    water = water_mask(arrays)
    if water is None:
        water = np.zeros(status.shape, bool)
    out = base.copy()
    tints = (((status == 3) & ~water, (1, 0, 0)), (water, (0, 1, 1)), (status == 4, (1, 0, 1)))
    for mask, tint in tints:
        out[mask] = 0.4 * out[mask] + 0.6 * np.array(tint, np.float32)
    return (np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8)


def run(folder, frame):
    manifest, frames = load(folder)
    lines = [f'{folder}: {len(frames)} frames, render {manifest["render_width"]}x'
             f'{manifest["render_height"]}, settings {manifest.get("settings")}']
    for i, arrays in enumerate(frames):
        lines += frame_report(manifest['frames'][i]['index'], arrays)
    if frames and 0 <= frame < len(frames) and 'status' in frames[frame]:
        out = Path(folder) / 'images'
        out.mkdir(exist_ok=True)
        path = out / f'water_flags_frame{frame}.png'
        path.write_bytes(png_bytes(overlay(frames[frame])))
        lines.append(f'image: {path} (cyan water, red special not water, magenta invalid)')
    return lines


def self_test():
    import json
    height, width = 8, 12
    status = np.ones((height, width), '<u4')
    status[:2, :6] = 3                  # special, top left
    status[6:, 6:] = 3                  # special, bottom right: water
    status[0, 11] = 4
    discard = np.zeros((height, width), '<f2')
    discard[6:, 6:] = 10000
    color = np.full((height, width, 4), 0.5, '<f2')
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        inputs = {}
        for name, array, fmt in (('status', status, 42), ('disocclusion_mask', discard, 54),
                                 ('color', color, 10)):
            (folder / f'frame0_{name}.bin').write_bytes(array.tobytes())
            inputs[name] = {'file': f'frame0_{name}.bin', 'dxgi_format': fmt, 'width': width,
                            'height': height, 'bytes_per_pixel': array.nbytes // (width * height)}
        manifest = {'format': 'mw4-rr-capture', 'version': 1, 'frame_count': 1,
                    'render_width': width, 'render_height': height, 'settings': {},
                    'frames': [{'index': 0, 'inputs': inputs}]}
        (folder / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
        text = '\n'.join(run(folder, 0))
        assert 'status 0=0.00% 1=73.96% 2=0.00% 3=25.00% 4=1.04%' in text, text
        assert ('water 12.50%, special and water 12.50%, special not water 12.50%, '
                'water not special 0.00%') in text, text
        # Top-left cells hold the special pixels outside the mask; the bottom-right ones the water.
        assert '    100.0 100.0 100.0   0.0   0.0   0.0' in text, text
        assert '      0.0   0.0   0.0 100.0 100.0 100.0' in text, text
        png = (folder / 'images' / 'water_flags_frame0.png').read_bytes()
        assert png.startswith(b'\x89PNG') and b'IEND' in png
    print('self-test passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('folder', nargs='?', type=Path)
    parser.add_argument('--frame', type=int, default=0, help='frame for the image (default 0)')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.folder is None:
        parser.error('folder is required unless --self-test is given')
    print('\n'.join(run(args.folder, args.frame)))


if __name__ == '__main__':
    main()
