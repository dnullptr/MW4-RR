"""Read raw frame captures written by the mw4-rr addon (Pause/Break hotkey).

A capture folder holds manifest.json (written last) and frame<i>_<name>.bin raw texel files.
From another script:  sys.path.insert(0, 'tools'); from read_capture import load
    manifest, frames = load(folder)   # frames[i] = {input_name: numpy array}, null inputs omitted
Arrays are float32 (H, W[, C]) for float and R8_UNORM formats, uint32 (H, W) for R32_UINT, else
uint8 (H, W, bpp).
CLI: python tools/read_capture.py FOLDER [--npz OUT.npz]   |   python tools/read_capture.py --self-test
Test passes that change RR's inputs (color_scale, depth_floor, and linear_depth: weapon depth's
linear view depth): undo them with tools/analyze_sky_edges.py undo_tests(manifest, frames).
"""
import argparse
import json
from pathlib import Path
import tempfile

import numpy as np

# dxgi_format -> (little-endian dtype, channels); 26 (R11G11B10_FLOAT) is unpacked from uint32,
# 61 (R8_UNORM) is scaled to 0-1.
FORMATS = {2: ('<f4', 4), 10: ('<f2', 4), 16: ('<f4', 2), 26: ('<u4', 1), 34: ('<f2', 2),
           41: ('<f4', 1), 42: ('<u4', 1), 54: ('<f2', 1), 61: ('<u1', 1)}
# settings.lighting_input -> label, as in src/rr_lighting_source.h (9-15 need the SSS pre-denoise
# build).
LIGHTING = ['Off (raw lighting)', 'Lobe 1 from native temporal', 'Lobe 2 from native temporal',
            'Lobe 2 fill gaps', 'Both lobes fill gaps', 'Raw, cap bright samples (8x)',
            'Raw, cap bright samples (4x)', 'Raw, AO fix A (fill gaps)', 'Raw, AO fix B (native)',
            'SSS pixels: lobe 2 native temporal', 'SSS pixels: lobe 2 fill gaps',
            'SSS pixels: both lobes native temporal', 'SSS pixels: both lobes fill gaps',
            'SSS pixels: lobe 2 fill gaps, history + spatial (test)',
            'SSS pixels: lobe 2 spatial fill only (test)',
            'SSS pixels: lobe 2 fill gaps, settled + spatial (test)']
MIXED_LIGHTING = 0xFFFFFFFF  # A frame's lighting_input when it changed within the frame.
# Per-frame lighting_masked: whether an SSS-masked lighting input had its SSS guide.
MASKED = {True: 'SSS guide applied', False: 'SSS guide missing, raw lighting',
          None: 'not SSS-masked or not known'}
# The same, on the lighting input line, when every frame has one value.
MASKED_ALL = {True: 'SSS guide applied', False: 'SSS guide missing on every frame: raw lighting'}
# Per-frame test settings and their off values; the summary shows only the ones that are on.
FRAME_TESTS = {'color_scale': 1, 'guide_test': 0, 'depth_floor': 0, 'responsivity_source': 'none',
               'hit_distance_test': 0, 'water_masks': False, 'water_guide': 0}


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
    if fmt == 61:
        return raw[..., 0].astype(np.float32) / 255
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
    try:
        manifest = json.loads(path.read_text(encoding='utf-8'))
    except ValueError as error:  # JSONDecodeError, UnicodeDecodeError: cut off while written
        raise ValueError(f'{path}: unreadable ({error}); the capture is incomplete or was cut '
                         'off while being written (manifest.json is written last)') from None
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


def lighting_label(lobe):
    if lobe == MIXED_LIGHTING:
        return 'mixed (changed within the frame)'
    return LIGHTING[lobe] if 0 <= lobe < len(LIGHTING) else 'unknown'


def input_states(frame):
    """Where each input's copy state came from, leaving out the addon's own inputs, e.g.
    'color layouts (0x40), depth contract, motion proven'; '' for manifests without the keys."""
    parts = []
    for name, info in (frame.get('inputs') or {}).items():
        state = info.get('state') if isinstance(info, dict) else None
        if state is None or state == 'addon':
            continue
        before = info.get('state_before')
        tracked = state == 'layouts' and type(before) is int
        parts.append(f'{name} {state}' + (f' ({before:#x})' if tracked else ''))
    return ', '.join(parts)


def summary(manifest, frames):
    m = manifest
    lines = [f'{m["frame_count"]} frames, render {m["render_width"]}x{m["render_height"]}, '
             f'output {m["output_width"]}x{m["output_height"]}, quality {m["quality"]}, '
             f'preset {m["preset"]}, create_flags {m.get("create_flags", "?")}, '
             f'settings {json.dumps(m["settings"], separators=(",", ":"))}']
    lobe = m['settings'].get('lighting_input')
    if type(lobe) is int:
        guides = {json.dumps(f.get('lighting_masked')) for f in m['frames']}  # True is not 1.
        guide = m['frames'][0].get('lighting_masked') if len(guides) == 1 else None
        note = '' if guide is None else (f' ({MASKED_ALL[guide]})' if type(guide) is bool else
                                         f' (lighting_masked={json.dumps(guide)}: ?)')
        lines.append(f'lighting input {lobe}: {lighting_label(lobe)}{note}')
    setups = {(*(f.get(k) for k in ('lighting_input', 'lighting_masked', *FRAME_TESTS)),
               bool(f.get('linear_depth'))) for f in m['frames']}
    masked = any(f.get('lighting_masked') is not None for f in m['frames'])
    if len(setups) > 1:
        lines.append('warning: the frames differ in lighting input or test settings')
    states = [input_states(f) for f in m['frames']]
    if states and states[0]:
        lines.append(f'input states: {states[0]}')
    if len(set(states)) > 1:
        lines.append('warning: the frames differ in input states')
    for frame, arrays in zip(m['frames'], frames):
        names = [n for bit, n in m['reset_reason_bits'].items() if frame['reset_reasons'] & int(bit)]
        reasons = f'({"|".join(names)})' if names else ''
        tests = ''.join(f' {k}={frame[k]}' for k, off in FRAME_TESTS.items()
                        if k in frame and frame[k] != off)
        if frame.get('linear_depth'):   # Weapon depth's mapping: depth is linear view depth.
            tests += ' linear_depth=on'
        lobe = frame.get('lighting_input')
        lighting = '' if lobe is None else (
            f' lighting_input={lighting_label(lobe) if lobe == MIXED_LIGHTING else lobe}')
        if masked:
            guide = frame.get('lighting_masked')
            known = guide is None or type(guide) is bool
            lighting += f' lighting_masked={json.dumps(guide)} ({MASKED[guide] if known else "?"})'
        lines.append(f'frame {frame["index"]} serial={frame["serial"]} sl_frame={frame["sl_frame"]} '
                     f'delivered={frame["delivered"]} reset={frame["reset"]} '
                     f'reset_reasons={frame["reset_reasons"]}{reasons} jitter={frame["jitter"]}'
                     f'{lighting}{tests}')
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
    water = np.array([0, 255, 0, 51, 255, 255, 0, 102], np.uint8).reshape(height, width)
    discard = np.where(water == 255, 10000, 0).astype('<f2')
    output = (np.arange(128, dtype='<f2') / 64).reshape(4, 8, 4)  # output size, not render size
    names = ['color', 'depth', 'motion', 'normal_roughness', 'diffuse_albedo', 'specular_albedo',
             'status', 'sss_guide', 'color_before_transparency', 'responsivity_mask',
             'specular_hit_distance', 'disocclusion_mask', 'animated_texture_mask', 'rr_output']
    images = [('color', color, 26), ('depth', depth, 41), ('motion', motion, 34),
              ('normal_roughness', normal, 10), ('specular_albedo', pair, 16),
              ('status', status, 42), ('sss_guide', sss, 54), ('specular_hit_distance', odd, 99),
              ('disocclusion_mask', discard, 54), ('animated_texture_mask', water, 61),
              ('rr_output', output, 10)]

    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        frames_json = []
        for i, used in enumerate([images, images[:1]]):
            inputs = dict.fromkeys(names)
            for name, array, fmt in used:
                h, w = array.shape[:2]
                (folder / f'frame{i}_{name}.bin').write_bytes(array.tobytes())
                inputs[name] = {'file': f'frame{i}_{name}.bin', 'dxgi_format': fmt, 'width': w,
                                'height': h, 'bytes_per_pixel': array.nbytes // (w * h)}
            frames_json.append({'index': i, 'serial': 10 + i, 'sl_frame': 20 + i, 'delivered': True,
                                'reset': bool(i), 'reset_reasons': 3 * i, 'jitter': [0.25, -0.125],
                                'lighting_input': 12, 'color_scale': [1, 4][i], 'guide_test': 0,
                                'responsivity_source': ['none', 'sky_edge'][i], 'inputs': inputs,
                                'linear_depth': [None, {'scale': 0.1, 'offset': 0, 'sky': 1e5,
                                                        'slice': 0.984375, 'knee': 2 ** -11}][i]})
        manifest = {'format': 'mw4-rr-capture', 'version': 1, 'frame_count': 2,
                    'render_width': width, 'render_height': height, 'output_width': 8,
                    'output_height': 4, 'quality': 2, 'preset': 6, 'settings': {'sss_enabled': True,
                                                                          'lighting_input': 12},
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
        got = frames[0]['animated_texture_mask']
        expect(got.dtype == np.float32 and got.shape == (height, width), 'unorm8 dtype/shape')
        np.testing.assert_array_equal(got, water.astype(np.float32) / 255)
        expect(got.max() == 1 and got[1, 3] == np.float32(0.4), 'unorm8 scales to 0-1')
        np.testing.assert_array_equal(frames[0]['disocclusion_mask'], discard.astype(np.float32))
        got = frames[0]['rr_output']
        expect(got.dtype == np.float32 and got.shape == (4, 8, 4), 'rr_output keeps its own size')
        np.testing.assert_array_equal(got, output.astype(np.float32))

        text = '\n'.join(summary(manifest, frames))
        expect('  status: 0=1 1=2 2=2 3=1 4=1 other=1' in text, 'status histogram with garbage texel')
        floats = summary(manifest, [{'status': np.array([[1, np.nan, 4.5]], np.float32)}])
        expect('  status: 0=0 1=1 2=0 3=0 4=0 other=2' in floats, 'status histogram on floats')
        expect('  depth: 0/0.892857/1.75 (1 non-finite)' in text, 'depth range')
        expect('reset_reasons=3(rejected|cut)' in text, 'reset reasons')
        expect('jitter=[0.25, -0.125] lighting_input=12\n' in text, 'frame 0 lighting, tests off')
        expect('lighting_input=12 color_scale=4 responsivity_source=sky_edge linear_depth=on\n' in text,
               'frame 1 tests that are on')
        expect('warning: the frames differ in lighting input or test settings' in text,
               'mixed test settings warned')
        same = {**manifest, 'frames': [manifest['frames'][0]] * 2}
        expect(not any(line.startswith('warning') for line in summary(same, frames)),
               'uniform test settings not warned')
        linear = {**manifest, 'frames': [manifest['frames'][0],
                                         {**manifest['frames'][0], 'linear_depth': {'scale': 1}}]}
        expect('warning: the frames differ in lighting input or test settings' in
               summary(linear, frames), 'weapon depth in some frames warned')
        expect('lighting input 12: SSS pixels: both lobes fill gaps' in text, 'lighting label')
        expect('  rr_output: 0/0.992188/1.98438' in text, 'rr_output range')
        # Whether an SSS-masked input had its guide: older manifests have no key, and print nothing.
        expect('lighting_masked' not in text and 'SSS guide' not in text, 'no lighting_masked key')
        base = json.loads(json.dumps(manifest))  # Frames 0 and 1, no test on.
        for f in base['frames']:
            f.update(color_scale=1, responsivity_source='none', linear_depth=None)
        expect(not any(line.startswith('warning') for line in summary(base, frames)), 'base')
        masked = json.loads(json.dumps(base))
        for f, v in zip(masked['frames'], [True, False]):
            f['lighting_masked'] = v
        (folder / 'manifest.json').write_text(json.dumps(masked), encoding='utf-8')
        expect(load(folder)[0] == masked, 'a manifest with lighting_masked loads')
        lines = summary(masked, frames)
        expect(any(line.startswith('frame 0 ') and
                   line.endswith('lighting_input=12 lighting_masked=true (SSS guide applied)')
                   for line in lines), f'frame 0 guide applied: {lines}')
        expect(any(line.startswith('frame 1 ') and line.endswith(
                   'lighting_input=12 lighting_masked=false (SSS guide missing, raw lighting)')
                   for line in lines), f'frame 1 guide missing: {lines}')
        expect('warning: the frames differ in lighting input or test settings' in lines,
               'frames that differ only in lighting_masked are warned')
        masked['frames'][1]['lighting_masked'] = None  # Shown once any frame has a value.
        lines = summary(masked, frames)
        expect(any(line.startswith('frame 1 ') and
                   line.endswith('lighting_masked=null (not SSS-masked or not known)')
                   for line in lines), f'frame 1 not known: {lines}')
        masked['frames'][1]['lighting_masked'] = 2  # Not a value the addon writes.
        lines = summary(masked, frames)
        expect(any(line.startswith('frame 1 ') and line.endswith('lighting_masked=2 (?)')
                   for line in lines), f'frame 1 unknown value: {lines}')
        # The lighting input line says it when every frame shares one value.
        header = 'lighting input 12: SSS pixels: both lobes fill gaps'
        for values, note in (([True, True], ' (SSS guide applied)'),
                             ([False, False], ' (SSS guide missing on every frame: raw lighting)'),
                             ([2, 2], ' (lighting_masked=2: ?)'), ([True, False], ''),
                             ([True, None], ''), ([True, 1], ''), ([None, None], '')):
            for f, v in zip(masked['frames'], values):
                f['lighting_masked'] = v
            lines = summary(masked, frames)
            expect(header + note in lines, f'lighting input line for {values}: {lines[:3]}')
        lines = summary(masked, frames)
        expect(not any('lighting_masked' in line or line.startswith('warning') for line in lines),
               f'all null: nothing printed, nothing warned: {lines}')
        # A frame whose lighting changed within it, and a header (frame 0) that was one.
        mixed = json.loads(json.dumps(base))
        mixed['frames'][1]['lighting_input'] = 0xFFFFFFFF
        lines = summary(mixed, frames)
        expect(any(line.startswith('frame 1 ') and
                   line.endswith('lighting_input=mixed (changed within the frame)')
                   for line in lines), f'mixed frame labelled: {lines}')
        mixed['settings'] = {**mixed['settings'], 'lighting_input': 0xFFFFFFFF}
        expect('lighting input 4294967295: mixed (changed within the frame)' in
               summary(mixed, frames), 'mixed header label')
        mixed['settings']['lighting_input'] = 16
        expect('lighting input 16: unknown' in summary(mixed, frames), 'unknown lighting label')
        # Input states: a manifest without them (older builds) prints nothing new.
        expect('input states' not in text and 'differ in input states' not in text,
               'no input states line without the keys')
        stated = json.loads(json.dumps(manifest))
        for name, (state, before) in {'color': ('layouts', 0x40), 'depth': ('contract', 0x40),
                                      'motion': ('proven', 0x40), 'status': ('addon', 0x40),
                                      'rr_output': ('addon', 8)}.items():
            stated['frames'][0]['inputs'][name].update(state=state, state_before=before)
        stated['frames'][1]['inputs']['color'].update(state='layouts', state_before=0x80)
        (folder / 'manifest.json').write_text(json.dumps(stated), encoding='utf-8')
        expect(load(folder)[0] == stated, 'a manifest with input states loads')
        lines = summary(stated, frames)
        expect('input states: color layouts (0x40), depth contract, motion proven' in lines,
               f'input states of frame 0, addon ones left out: {lines[:4]}')
        # Frame 1 a copy of frame 0 but for one input's state_before, or its state.
        for name, key, value in (('color', 'state_before', 0x80), ('motion', 'state', 'contract')):
            differ = json.loads(json.dumps(stated))
            differ['frames'][1] = json.loads(json.dumps(stated['frames'][0]))
            differ['frames'][1]['inputs'][name][key] = value
            lines = summary(differ, frames)
            expect('warning: the frames differ in input states' in lines and
                   'warning: the frames differ in lighting input or test settings' not in lines,
                   f'frames that differ only in {name} {key} are warned: {lines[:5]}')
        same = {**stated, 'frames': [stated['frames'][0]] * 2}
        lines = summary(same, frames)
        expect('input states: color layouts (0x40), depth contract, motion proven' in lines and
               not any(line.startswith('warning') for line in lines), 'uniform states not warned')
        (folder / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
        np.savez(folder / 'out.npz', **npz_arrays(frames))
        with np.load(folder / 'out.npz') as npz:
            expect(len(npz.files) == 12 and 'frame1_color' in npz.files, 'npz keys')
            expect(npz['frame0_rr_output'].shape == (4, 8, 4), 'npz rr_output shape')
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
        # A cut-off manifest (empty, truncated, or broken mid-character) is incomplete too.
        text = json.dumps(manifest).encode('utf-8')
        for broken in (b'', text[:len(text) // 2], text[:-1], b'{"format": "\xff'):
            (folder / 'manifest.json').write_bytes(broken)
            try:
                load(folder)
            except ValueError as error:
                expect(not isinstance(error, (json.JSONDecodeError, UnicodeDecodeError)) and
                       'incomplete' in str(error) and 'manifest.json' in str(error),
                       f'unreadable manifest must say incomplete, got {error!r}')
            else:
                raise AssertionError('an unreadable manifest must raise ValueError')
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
