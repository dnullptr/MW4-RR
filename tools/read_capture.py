"""Read raw frame captures written by the mw4-rr addon (F7 hotkey).

A capture folder holds manifest.json (written last) and frame<i>_<name>.bin raw texel files.
From another script:  sys.path.insert(0, 'tools'); from read_capture import load
    manifest, frames = load(folder)   # frames[i] = {input_name: numpy array}, null inputs omitted
Arrays are float32 (H, W[, C]) for float formats, uint32 (H, W) for R32_UINT, else uint8 (H, W, bpp).
CLI: python tools/read_capture.py FOLDER [--npz OUT.npz]   |   python tools/read_capture.py --self-test
"""
import argparse
import json
from pathlib import Path
import tempfile

import numpy as np

# dxgi_format -> (little-endian dtype, channels); 26 (R11G11B10_FLOAT) is unpacked from uint32.
FORMATS = {2: ('<f4', 4), 10: ('<f2', 4), 16: ('<f4', 2), 26: ('<u4', 1), 34: ('<f2', 2),
           41: ('<f4', 1), 42: ('<u4', 1), 54: ('<f2', 1)}


def small_float(bits, mantissa_bits):
    """Unsigned 5-bit-exponent float (bias 15) from integer arrays holding exponent|mantissa."""
    exponent = (bits >> mantissa_bits).astype(np.int32)
    mantissa = (bits & ((1 << mantissa_bits) - 1)).astype(np.float64) / (1 << mantissa_bits)
    value = np.where(exponent == 0, np.ldexp(mantissa, -14), np.ldexp(1.0 + mantissa, exponent - 15))
    return np.where(exponent == 31, np.where(mantissa == 0, np.inf, np.nan), value)


def decode_r11g11b10(packed):
    """uint32 array (any shape) -> float32 array with a trailing RGB axis."""
    packed = np.asarray(packed, dtype=np.uint32)
    channels = (small_float(packed & 0x7FF, 6), small_float((packed >> 11) & 0x7FF, 6),
                small_float(packed >> 22, 5))
    return np.stack(channels, axis=-1).astype(np.float32)


def decode_image(data, info, path):
    width, height, bpp = info['width'], info['height'], info['bytes_per_pixel']
    if len(data) != width * height * bpp:
        raise ValueError(f'{path}: {len(data)} bytes, expected {width}x{height}x{bpp} = '
                         f'{width * height * bpp}')
    fmt = info['dxgi_format']
    if fmt not in FORMATS:
        return np.frombuffer(data, np.uint8).reshape(height, width, bpp).copy()
    dtype, channels = FORMATS[fmt]
    if np.dtype(dtype).itemsize * channels != bpp:
        raise ValueError(f'{path}: bytes_per_pixel {bpp} does not match DXGI format {fmt}')
    raw = np.frombuffer(data, dtype).reshape(height, width, channels)
    if fmt == 26:
        return decode_r11g11b10(raw[..., 0])
    out = raw.astype(np.uint32 if dtype == '<u4' else np.float32)
    return out[..., 0] if channels == 1 else out


def read_input(folder, info):
    path = folder / info['file']
    return decode_image(path.read_bytes(), info, path)


def load(folder):
    folder = Path(folder)
    path = folder / 'manifest.json'
    if not path.is_file():
        raise FileNotFoundError(f'{path}: missing; the capture is incomplete or still being '
                                'written (manifest.json is written last)')
    manifest = json.loads(path.read_text(encoding='utf-8'))
    if manifest.get('format') != 'mw4-rr-capture':
        raise ValueError(f'{path}: not an mw4-rr capture manifest')
    frames = [{name: read_input(folder, info) for name, info in frame['inputs'].items()
               if info is not None} for frame in manifest['frames']]
    return manifest, frames


def npz_arrays(frames):
    return {f'frame{i}_{name}': array for i, frame in enumerate(frames)
            for name, array in frame.items()}


def range_text(array):
    finite = array[np.isfinite(array)]
    bad = array.size - finite.size
    if finite.size == 0:
        return 'no finite values'
    return (f'{finite.min():.6g}/{finite.mean(dtype=np.float64):.6g}/{finite.max():.6g}' +
            (f' ({bad} non-finite)' if bad else ''))


def summary(manifest, frames):
    m = manifest
    lines = [f'{m["frame_count"]} frames, render {m["render_width"]}x{m["render_height"]}, '
             f'output {m["output_width"]}x{m["output_height"]}, quality {m["quality"]}, '
             f'preset {m["preset"]}, settings {json.dumps(m["settings"], separators=(",", ":"))}']
    for frame, arrays in zip(m['frames'], frames):
        names = [n for bit, n in m['reset_reason_bits'].items() if frame['reset_reasons'] & int(bit)]
        reasons = f'({"|".join(names)})' if names else ''
        lines.append(f'frame {frame["index"]} serial={frame["serial"]} sl_frame={frame["sl_frame"]} '
                     f'delivered={frame["delivered"]} reset={frame["reset"]} '
                     f'reset_reasons={frame["reset_reasons"]}{reasons} jitter={frame["jitter"]}')
        for name, array in arrays.items():
            if name == 'status':
                counts = [int((array == v).sum()) for v in range(5)]  # any dtype, no big allocation
                other = array.size - sum(counts)
                lines.append('  status: ' + ' '.join(f'{v}={n}' for v, n in enumerate(counts)) +
                             (f' other={other}' if other else ''))
            else:
                lines.append(f'  {name}: {range_text(array)}')
    return lines


def self_test():
    def expect(condition, message):
        if not condition:
            raise AssertionError(message)

    def pack(r, g, b):
        return r | (g << 11) | (b << 22)

    width, height = 4, 2
    color = np.zeros(8, np.uint32)
    color[:5] = [pack(0x3C0, 0x380, 0x200),   # 1.0, 0.5, 2.0
                 pack(1, 63, 1),              # denormals
                 pack(0x7C0, 0x7C1, 0x3E0),   # inf, nan, inf
                 pack(0x7BF, 0, 0x3DF),       # largest finite R and B
                 pack(0, 0, 0x3E1)]           # nan in B
    color = color.reshape(height, width)
    depth = (np.arange(8, dtype='<f4') / 4).reshape(height, width)
    depth[0, 3] = np.inf
    motion = np.arange(16, dtype='<f2').reshape(height, width, 2) / 8
    status = np.array([0, 1, 1, 2, 2, 3, 4, 0xFFFFFFFF], '<u4').reshape(height, width)
    normal = (np.arange(32, dtype='<f2') / 16).reshape(height, width, 4)
    pair = (np.arange(16, dtype='<f4') * 0.1).reshape(height, width, 2)
    sss = (np.arange(8, dtype='<f2') - 4).reshape(height, width)
    odd = np.arange(24, dtype=np.uint8).reshape(height, width, 3)
    names = ['color', 'depth', 'motion', 'normal_roughness', 'diffuse_albedo', 'specular_albedo',
             'status', 'sss_guide', 'color_before_transparency', 'responsivity_mask',
             'specular_hit_distance']
    images = [('color', color, 26), ('depth', depth, 41), ('motion', motion, 34),
              ('normal_roughness', normal, 10), ('specular_albedo', pair, 16),
              ('status', status, 42), ('sss_guide', sss, 54), ('specular_hit_distance', odd, 99)]

    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        frames_json = []
        for i, used in enumerate([images, images[:1]]):
            inputs = dict.fromkeys(names)
            for name, array, fmt in used:
                (folder / f'frame{i}_{name}.bin').write_bytes(array.tobytes())
                inputs[name] = {'file': f'frame{i}_{name}.bin', 'dxgi_format': fmt, 'width': width,
                                'height': height, 'bytes_per_pixel': array.nbytes // (width * height)}
            frames_json.append({'index': i, 'serial': 10 + i, 'sl_frame': 20 + i, 'delivered': True,
                                'reset': bool(i), 'reset_reasons': 3 * i, 'jitter': [0.25, -0.125],
                                'inputs': inputs})
        manifest = {'format': 'mw4-rr-capture', 'version': 1, 'frame_count': 2,
                    'render_width': width, 'render_height': height, 'output_width': 8,
                    'output_height': 4, 'quality': 2, 'preset': 6, 'settings': {'sss_enabled': True},
                    'reset_reason_bits': {'1': 'rejected', '2': 'cut', '4': 'generation', '8': 'gap'},
                    'frames': frames_json}
        (folder / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')

        loaded, frames = load(folder)
        expect(loaded == manifest and len(frames) == 2, 'manifest or frame count')
        expect(sorted(frames[0]) == sorted(n for n, _, _ in images), 'null inputs must be omitted')
        expect(list(frames[1]) == ['color'], 'frame 1 holds only color')
        got = frames[0]['color']
        expect(got.dtype == np.float32 and got.shape == (height, width, 3), 'color dtype/shape')
        want = np.zeros((8, 3), np.float32)
        want[:5] = [[1, 0.5, 2], [2 ** -20, 63 * 2 ** -20, 2 ** -19], [np.inf, np.nan, np.inf],
                    [65024, 0, 64512], [0, 0, np.nan]]
        np.testing.assert_array_equal(got.reshape(8, 3), want)
        np.testing.assert_array_equal(decode_r11g11b10(color), got)
        np.testing.assert_array_equal(frames[0]['depth'], depth)
        for name, array in [('motion', motion), ('normal_roughness', normal),
                            ('specular_albedo', pair), ('sss_guide', sss)]:
            expect(frames[0][name].dtype == np.float32, f'{name} decodes to float32')
            np.testing.assert_array_equal(frames[0][name], array.astype(np.float32))
        expect(frames[0]['depth'].dtype == np.float32, 'depth decodes to float32')
        expect(frames[0]['normal_roughness'].shape == (height, width, 4), 'rgba shape')
        expect(frames[0]['status'].dtype == np.uint32 and frames[0]['status'].shape == (height, width),
               'status dtype/shape')
        np.testing.assert_array_equal(frames[0]['status'], status)
        np.testing.assert_array_equal(frames[0]['specular_hit_distance'], odd)
        expect(frames[0]['specular_hit_distance'].dtype == np.uint8, 'unknown format is uint8')

        text = '\n'.join(summary(manifest, frames))
        expect('  status: 0=1 1=2 2=2 3=1 4=1 other=1' in text, 'status histogram with garbage texel')
        floats = summary(manifest, [{'status': np.array([[1, np.nan, 4.5]], np.float32)}])
        expect('  status: 0=0 1=1 2=0 3=0 4=0 other=2' in floats, 'status histogram on floats')
        expect('  depth: 0/0.892857/1.75 (1 non-finite)' in text, 'depth range')
        expect('reset_reasons=3(rejected|cut)' in text, 'reset reasons')
        np.savez(folder / 'out.npz', **npz_arrays(frames))
        with np.load(folder / 'out.npz') as npz:
            expect(len(npz.files) == 9 and 'frame1_color' in npz.files, 'npz keys')
            np.testing.assert_array_equal(npz['frame0_status'], status)

        (folder / 'frame0_status.bin').write_bytes(status.tobytes()[:-4])
        try:
            load(folder)
        except ValueError as error:
            expect('frame0_status.bin' in str(error), 'size error must name the file')
        else:
            raise AssertionError('size mismatch must raise ValueError')
        (folder / 'manifest.json').unlink()
        try:
            load(folder)
        except FileNotFoundError as error:
            expect('incomplete' in str(error) and 'manifest.json' in str(error), 'missing manifest')
        else:
            raise AssertionError('missing manifest must raise FileNotFoundError')
    print('self-test passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('folder', nargs='?', type=Path, help='capture folder (contains manifest.json)')
    parser.add_argument('--npz', type=Path, help='also save arrays as frame<i>_<name> in this file')
    parser.add_argument('--self-test', action='store_true', help='check the decoder on synthetic data')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.folder is None:
        parser.error('folder is required unless --self-test is given')
    manifest, frames = load(args.folder)
    print('\n'.join(summary(manifest, frames)))
    if args.npz:
        np.savez(args.npz, **npz_arrays(frames))


if __name__ == '__main__':
    main()
