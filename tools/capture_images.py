"""Turn mw4-rr captures (Pause/Break) into PNG images and an index.html to look at them.

CLI: python tools/capture_images.py FOLDER... [--frames 0,7] [--out DIR]
     python tools/capture_images.py --self-test
FOLDER is a capture folder (holding manifest.json) or a folder of them (mw4-rr-captures). Each
capture gets <capture>/images/ (or DIR/<capture name>/) with one PNG per input and frame and an
index.html; a folder of captures also gets an index.html linking them. Only numpy is needed.
"""
import argparse
import html
import json
from pathlib import Path
import struct
import sys
import tempfile
import zlib

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from read_capture import input_states, lighting_label, read_input  # noqa: E402

STATUS_COLORS = np.array([[0, 0, 0], [110, 110, 110], [70, 140, 255], [255, 150, 30],
                          [255, 40, 40]], np.uint8)  # padding, ordinary, sky, special, invalid
HDR = ('color', 'color_before_transparency', 'rr_output')
NOTES = {
    'color': 'RR colour input, tone mapped (same exposure as rr_output)',
    'color_before_transparency': 'colour before transparency, tone mapped',
    'rr_output': "RR's output, tone mapped (same exposure as color)",
    'depth': 'depth: bright = near, black = sky or far',
    'motion': 'motion vectors: hue = direction (right red, down yellow-green, left cyan, up '
              'purple), brightness = length',
    'normal_roughness': 'normals (xyz as rgb)',
    'roughness': 'roughness (normal_roughness alpha): white = rough',
    'diffuse_albedo': 'diffuse albedo',
    'specular_albedo': 'specular albedo',
    'status': 'Status guide: grey ordinary, blue sky, orange special, red invalid, black padding',
    'sss_guide': 'SSS guide: white = 1',
    'responsivity_mask': 'responsivity mask: white = 1',
    'specular_hit_distance': 'specular hit distance: bright = far (scaled to the 99th '
                             'percentile)',
    'disocclusion_mask': 'water disocclusion mask: white = discard history',
    'animated_texture_mask': 'water animated texture mask: white = animated',
}


def png_bytes(rgb):
    """rgb: uint8 (H, W, 3) or (H, W). Returns a PNG file's bytes."""
    rgb = np.ascontiguousarray(rgb, np.uint8)
    if rgb.ndim == 2:
        rgb = np.repeat(rgb[..., None], 3, axis=2)
    height, width = rgb.shape[:2]
    rows = np.concatenate([np.zeros((height, 1), np.uint8), rgb.reshape(height, width * 3)],
                          axis=1)

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF))
    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
            chunk(b'IDAT', zlib.compress(rows.tobytes(), 3)) + chunk(b'IEND', b''))


def srgb(linear):
    linear = np.clip(np.nan_to_num(linear, nan=0.0, posinf=1.0, neginf=0.0), 0, 1)
    encoded = np.where(linear <= 0.0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - 0.055)
    return (encoded * 255 + 0.5).astype(np.uint8)


def gray(value):
    return (np.clip(np.nan_to_num(value), 0, 1) * 255 + 0.5).astype(np.uint8)


def luminance(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def exposure(rgb):
    """Scale that maps the image's log-average luminance to 0.18."""
    lum = luminance(np.nan_to_num(rgb[..., :3], nan=0.0, posinf=0.0, neginf=0.0))
    lum = lum[lum > 0]
    if not lum.size:
        return 1.0
    key = float(np.exp(np.mean(np.log(lum + 1e-6))))
    return 0.18 / key if key > 0 else 1.0


def tonemap(rgb, scale):
    x = np.maximum(np.nan_to_num(rgb[..., :3], nan=0.0, posinf=65504.0, neginf=0.0), 0) * scale
    return srgb(x / (1 + x))


def percentile_scale(value, q=99.0):
    finite = value[np.isfinite(value) & (value > 0)]
    top = float(np.percentile(finite, q)) if finite.size else 1.0
    return value / top if top > 0 else value


def motion_image(mv):
    x, y = np.nan_to_num(mv[..., 0]), np.nan_to_num(mv[..., 1])
    length = np.hypot(x, y)
    top = float(np.percentile(length, 99)) if length.size else 1.0
    value = np.clip(length / top, 0, 1) if top > 0 else np.zeros_like(length)
    hue = (np.arctan2(y, x) / (2 * np.pi)) % 1.0
    h6 = hue * 6
    c = value
    xx = c * (1 - np.abs(h6 % 2 - 1))
    zero = np.zeros_like(c)
    sector = np.floor(h6).astype(int) % 6
    r = np.choose(sector, [c, xx, zero, zero, xx, c])
    g = np.choose(sector, [xx, c, c, xx, zero, zero])
    b = np.choose(sector, [zero, zero, xx, c, c, xx])
    return gray(np.stack([r, g, b], axis=-1)), top


def images_for(name, array, scale):
    """[(suffix, uint8 image, extra caption)] for one decoded input."""
    if name in HDR:
        return [('', tonemap(array, scale), '')]
    if name == 'depth':
        return [('', gray(percentile_scale(array, 99.5) ** 0.5), '')]
    if name == 'motion':
        image, top = motion_image(array)
        return [('', image, f'full brightness = {top:.3g} or more')]
    if name == 'normal_roughness':
        n = array[..., :3]
        rgb = n * 0.5 + 0.5 if float(np.nanmin(n)) < -0.01 else n
        return [('', gray(rgb), ''), ('_roughness', gray(array[..., 3]), '')]
    if name in ('diffuse_albedo', 'specular_albedo'):
        return [('', srgb(array[..., :3]), '')]
    if name == 'status':
        return [('', STATUS_COLORS[np.clip(array, 0, len(STATUS_COLORS) - 1)], '')]
    if name == 'specular_hit_distance':
        return [('', gray(percentile_scale(array) ** 0.5), '')]
    if array.dtype == np.uint8:  # An unknown format, raw bytes: show the first byte.
        return [('', array[..., 0], 'raw bytes (unknown format)')]
    first = array if array.ndim == 2 else array[..., 0]
    if array.dtype == np.uint32:
        first = first.astype(np.float32)
    if name.endswith('_mask') or name == 'sss_guide':
        return [('', gray(first), '')]
    return [('', gray(percentile_scale(first)), 'scaled to the 99th percentile')]


def capture_settings(manifest, frame):
    lobe = frame.get('lighting_input', manifest.get('settings', {}).get('lighting_input'))
    parts = [f"lighting input {lobe}: {lighting_label(lobe)}" if lobe is not None else '']
    for key, off in (('color_scale', 1), ('guide_test', 0), ('depth_floor', 0),
                     ('responsivity_source', 'none'), ('hit_distance_test', 0),
                     ('water_masks', False), ('water_guide', 0)):
        if frame.get(key, off) != off:
            parts.append(f'{key}={frame[key]}')
    states = input_states(frame)
    if states:
        parts.append(f'input states: {states}')
    return '; '.join(p for p in parts if p)


def camera_motion(manifest):
    positions = [f.get('camera', {}).get('position') for f in manifest.get('frames', [])]
    positions = [p for p in positions if p]
    if len(positions) < 2:
        return ''
    a, b = np.array(positions[0]), np.array(positions[-1])
    forward = [f.get('camera', {}).get('forward') for f in manifest['frames']]
    turn = ''
    if forward[0] and forward[-1]:
        cos = float(np.clip(np.dot(forward[0], forward[-1]) /
                            (np.linalg.norm(forward[0]) * np.linalg.norm(forward[-1]) + 1e-12),
                            -1, 1))
        turn = f', turned {np.degrees(np.arccos(cos)):.1f} degrees'
    return f'camera moved {float(np.linalg.norm(b - a)):.3g} units over the frames{turn}'


def convert(folder, frames_wanted, out):
    folder = Path(folder)
    manifest = json.loads((folder / 'manifest.json').read_text(encoding='utf-8'))
    out.mkdir(parents=True, exist_ok=True)
    frames = manifest['frames']
    wanted = [i for i in frames_wanted if 0 <= i < len(frames)]
    title = folder.name
    body = [f'<h1>{html.escape(title)}</h1>',
            f"<p>{html.escape(manifest.get('created', ''))}, render "
            f"{manifest.get('render_width')}x{manifest.get('render_height')}, output "
            f"{manifest.get('output_width')}x{manifest.get('output_height')}. "
            f"{html.escape(camera_motion(manifest))}</p>"]
    for i in wanted:
        frame = frames[i]
        body.append(f"<h2>Frame {i} (serial {frame.get('serial')})</h2>"
                    f"<p>{html.escape(capture_settings(manifest, frame))}</p><div class=grid>")
        decoded = {name: read_input(folder, info) for name, info in frame['inputs'].items()
                   if info is not None}
        color_scale = frame.get('color_scale', 1) or 1
        for name in HDR:  # Undo the colour brightness test so images compare.
            if name in decoded and name != 'color_before_transparency' and color_scale != 1:
                decoded[name] = decoded[name] / color_scale
        scale = exposure(decoded['color']) if 'color' in decoded else 1.0
        for name, array in decoded.items():
            info = frame['inputs'][name]
            for suffix, image, extra in images_for(name, array, scale):
                file = f'frame{i}_{name}{suffix}.png'
                (out / file).write_bytes(png_bytes(image))
                note = NOTES.get(name + suffix if suffix else name, name)
                caption = (f"<b>{html.escape(name + suffix)}</b> {info['width']}x"
                           f"{info['height']} {html.escape(info['format'])}<br>"
                           f"{html.escape(note)}{'; ' + html.escape(extra) if extra else ''}")
                body.append(f'<figure><a href="{file}"><img src="{file}" loading=lazy></a>'
                            f'<figcaption>{caption}</figcaption></figure>')
        missing = [name for name, info in frame['inputs'].items() if info is None]
        body.append('</div>')
        if missing:
            body.append(f"<p>Not sent this frame: {html.escape(', '.join(missing))}</p>")
    (out / 'index.html').write_text(page(title, '\n'.join(body)), encoding='utf-8')
    return out / 'index.html'


def page(title, body):
    return f"""<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>{html.escape(title)}</title>
<style>body{{background:#111;color:#ddd;font:14px system-ui,sans-serif;margin:16px}}
.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(420px,1fr));gap:12px}}
figure{{margin:0}}img{{width:100%;image-rendering:pixelated;background:#000}}
figcaption{{padding:4px 0}}a{{color:#8bf}}</style>
{body}
"""


def captures_in(folder):
    folder = Path(folder)
    if (folder / 'manifest.json').is_file():
        return [folder]
    return sorted(p for p in folder.iterdir() if (p / 'manifest.json').is_file())


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        capture = root / '20261025-120000'
        capture.mkdir()
        h, w = 4, 6
        rng = np.random.default_rng(1)
        color = rng.random((h, w, 4), np.float32) * 4
        files = {
            'color': (color, 2, 16), 'depth': (rng.random((h, w), np.float32), 41, 4),
            'motion': (rng.standard_normal((h, w, 2)).astype(np.float16), 34, 4),
            'normal_roughness': ((rng.random((h, w, 4)) * 2 - 1).astype(np.float16), 10, 8),
            'status': (np.arange(h * w, dtype=np.uint32).reshape(h, w) % 6, 42, 4),
            'disocclusion_mask': ((rng.random((h, w)) * 255).astype(np.uint8), 61, 1),
        }
        names = {2: 'R32G32B32A32_FLOAT', 41: 'R32_FLOAT', 34: 'R16G16_FLOAT',
                 10: 'R16G16B16A16_FLOAT', 42: 'R32_UINT', 61: 'R8_UNORM'}
        inputs = {}
        for name, (array, dxgi, bpp) in files.items():
            (capture / f'frame0_{name}.bin').write_bytes(array.tobytes())
            inputs[name] = {'file': f'frame0_{name}.bin', 'format': names[dxgi],
                            'dxgi_format': dxgi, 'width': w, 'height': h,
                            'bytes_per_pixel': bpp, 'source_offset': [0, 0],
                            'source_size': [w, h], 'state': 'contract', 'state_before': 64}
        inputs['sss_guide'] = None
        manifest = {'format': 'mw4-rr-capture', 'created': '2026-10-25T12:00:00',
                    'render_width': w, 'render_height': h, 'output_width': w,
                    'output_height': h, 'settings': {'lighting_input': 3},
                    'frames': [{'index': 0, 'serial': 7, 'lighting_input': 3,
                                'camera': {'position': [0, 0, 0], 'forward': [1, 0, 0]},
                                'inputs': inputs},
                               {'index': 1, 'serial': 8, 'lighting_input': 3,
                                'camera': {'position': [3, 4, 0], 'forward': [0, 1, 0]},
                                'inputs': {'sss_guide': None}}]}
        (capture / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
        out = root / 'out'
        index = convert(capture, [0, 1, 9], out)
        text = index.read_text(encoding='utf-8')
        pngs = sorted(p.name for p in out.glob('*.png'))
        expected = [f'frame0_{n}.png' for n in files] + ['frame0_normal_roughness_roughness.png']
        assert pngs == sorted(expected), pngs
        for p in out.glob('*.png'):
            data = p.read_bytes()
            assert data[:8] == b'\x89PNG\r\n\x1a\n', p
            width, height = struct.unpack('>II', data[16:24])
            assert (width, height) == (w, h), (p, width, height)
            idat = data.index(b'IDAT')
            size = struct.unpack('>I', data[idat - 4:idat])[0]
            raw = zlib.decompress(data[idat + 4:idat + 4 + size])
            assert len(raw) == h * (1 + w * 3), (p, len(raw))
        assert 'Lobe 2 fill gaps' in text, 'lighting label in the page'
        assert 'input states: color contract, depth contract' in text, 'input states shown'
        assert 'camera moved 5 units' in text and 'turned 90.0 degrees' in text, text
        assert 'Not sent this frame: sss_guide' in text, 'missing inputs listed'
        assert 'Frame 9' not in text, 'frames past the end are skipped'
        assert np.array_equal(STATUS_COLORS[np.clip(np.array([0, 4, 9]), 0, 4)][:, 0],
                              [0, 255, 255]), 'status palette clamps'
        listing = captures_in(root)
        assert listing == [capture], listing
    print('self-test passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('folders', nargs='*', type=Path)
    parser.add_argument('--frames', default='0', help='comma-separated frame numbers (0-7)')
    parser.add_argument('--out', type=Path, help='write DIR/<capture name>/ instead')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.folders:
        parser.error('give a capture folder or a folder of captures')
    wanted = [int(f) for f in args.frames.split(',') if f.strip()]
    for folder in args.folders:
        captures = captures_in(folder)
        if not captures:
            print(f'{folder}: no capture with manifest.json found')
            continue
        links = []
        for capture in captures:
            out = (args.out / capture.name) if args.out else capture / 'images'
            index = convert(capture, wanted, out)
            print(f'{capture.name}: {index}')
            links.append((capture.name, index))
        if len(captures) > 1 or captures[0] != Path(folder):
            base = args.out if args.out else Path(folder)
            items = ''.join(
                f'<li><a href="{html.escape(Path(i).relative_to(base).as_posix())}">'
                f'{html.escape(n)}</a></li>' for n, i in links)
            top = base / 'index.html'
            top.write_text(page('RR captures', f'<h1>RR captures</h1><ul>{items}</ul>'),
                           encoding='utf-8')
            print(f'all captures: {top}')


if __name__ == '__main__':
    main()
