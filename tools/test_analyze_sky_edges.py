#!/usr/bin/env python3
"""Synthetic tests for analyze_sky_edges.py.  Run: python3 test_analyze_sky_edges.py

The scene (160x96, 8 frames) is point-sampled at the jittered pixel centre (Halton(2,3) - 0.5),
so sky/non-sky flips at thin edges arise from the jitter alone: a bright textured sky, a static
thin dark diagonal wire, a thin branch and a canopy that move +2 px per frame (with matching
motion vectors), and a hill whose far and near parts make a depth edge well away from the sky.
Faults are injected per case: (b) depth shifted 1 px or colour blended at edges, (c) filtered
depth, (d) NaN colour, (e) sky motion on tree edges, (f) a foliage patch dithered per frame.
H0 cases add rr_output, the scene sampled at the unjittered output pixel centres (a converged RR):
(a) with per-frame gain noise at sky edges only, (b) stable (plus a reset frame that must be
skipped), (c) absent (older captures), (d) at a non-integer scale of the render size.
"""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import analyze_sky_edges as ase  # noqa: E402
from read_capture import decode_r11g11b10, load  # noqa: E402

W, H, N = 160, 96, 8
TREE_SPEED = 2.0            # px per frame along +x for branch and canopy
DITHER_COVERAGE = 0.5       # case (f): chance that a dithered foliage sample is covered, per frame
NEAR = 0.1                  # projection near plane (infinite reverse Z: depth = NEAR / z)
SKY, FAR_HILL, NEAR_HILL, WIRE, BRANCH, CANOPY, DITHER = range(7)
DEPTH = {SKY: 0.0, FAR_HILL: 0.002, NEAR_HILL: 0.01, WIRE: 0.003, BRANCH: 0.005, CANOPY: 0.004,
         DITHER: 0.0045}
NORMAL = {SKY: (0, 0, -1, 1), FAR_HILL: (0, 0.95, -0.312, 0.8), NEAR_HILL: (0.1, 0.9, -0.42, 0.8),
          WIRE: (0, 0.7, -0.714, 0.4), BRANCH: (0.3, 0.6, -0.742, 0.6), CANOPY: (0.3, 0.6, -0.742, 0.6),
          DITHER: (0.2, 0.7, -0.686, 0.6)}
ALBEDO = {SKY: (0.5, 0.5, 0.5, 1), FAR_HILL: (0.2, 0.25, 0.12, 1), NEAR_HILL: (0.15, 0.22, 0.1, 1),
          WIRE: (0.05, 0.05, 0.05, 1), BRANCH: (0.08, 0.15, 0.04, 1), CANOPY: (0.08, 0.15, 0.04, 1),
          DITHER: (0.1, 0.18, 0.05, 1)}


def halton(index, base):
    result, f = 0.0, 1.0
    while index:
        f /= base
        result += f * (index % base)
        index //= base
    return result


JITTER = [(halton(i + 1, 2) - 0.5, halton(i + 1, 3) - 0.5) for i in range(N)]


def segment_distance(px, py, a, b):
    ax, ay = a
    bx, by = b
    t = np.clip(((px - ax) * (bx - ax) + (py - ay) * (by - ay)) / ((bx - ax) ** 2 + (by - ay) ** 2), 0, 1)
    return np.hypot(px - (ax + t * (bx - ax)), py - (ay + t * (by - ay)))


def surface_ids(px, py, frame, dither):
    """Front-most surface at each sample point (largest reverse-Z depth wins)."""
    shift = TREE_SPEED * frame
    ids = np.full(px.shape, SKY)
    best = np.zeros(px.shape)
    skyline = 60 + 5 * np.sin(px / 23 + 1) + 2 * np.sin(px / 7)
    split = 80 + 3 * np.sin(px / 13)
    shapes = [(FAR_HILL, (py > skyline) & (py <= split)), (NEAR_HILL, py > split),
              (WIRE, segment_distance(px, py, (0, 10), (160, 40)) <= 0.6),
              (BRANCH, segment_distance(px, py, (40 + shift, 62), (70 + shift, 15)) <= 1.0),
              (CANOPY, ((px - 112 - shift) / 13) ** 2 + ((py - 34) / 9) ** 2 <= 1)]
    if dither:   # semi-transparent foliage drawn with a fresh random coverage pattern every frame
        rng = np.random.default_rng(1000 + frame)
        inside = ((px - 22) / 13) ** 2 + ((py - 40) / 8) ** 2 <= 1
        shapes.append((DITHER, inside & (rng.random(px.shape) < DITHER_COVERAGE)))
    for surface, mask in shapes:
        take = mask & (DEPTH[surface] > best)
        ids[take], best[take] = surface, DEPTH[surface]
    return ids


def surface_color(ids, px, py):
    color = np.zeros(ids.shape + (3,))
    sky = np.stack([2.0 + 0.3 * np.sin(px / 9), 3.0 + 0.3 * np.cos(py / 7 + px / 17),
                    5.0 + 0.2 * np.sin(py / 5)], axis=-1)
    # Hill textures carry structure in both directions (distant trees, rocks), as real terrain
    # does; with only near-horizontal edges the horizontal image shift is barely measurable.
    texture = {FAR_HILL: ((0.25, 0.30, 0.18), 1 + 0.15 * np.sin(px / 1.9 + py / 7) +
                          0.1 * np.sin(px / 3.1 - py / 2.3)),
               NEAR_HILL: ((0.08, 0.15, 0.05), 1 + 0.15 * np.sin(px / 2.2) * np.cos(py / 3) +
                           0.1 * np.sin((px + py) / 1.7)),
               WIRE: ((0.012, 0.012, 0.012), np.ones(px.shape)),
               BRANCH: ((0.03, 0.05, 0.02), 1 + 0.15 * np.sin(px / 3 + py / 5)),
               CANOPY: ((0.03, 0.05, 0.02), 1 + 0.15 * np.sin(px / 3 + py / 5)),
               DITHER: ((0.04, 0.06, 0.025), np.ones(px.shape))}
    color[ids == SKY] = sky[ids == SKY]
    for surface, (base, scale) in texture.items():
        m = ids == surface
        color[m] = np.array(base) * scale[m][:, None]
    return color


def render_frame(frame, dither=False):
    gy, gx = np.mgrid[0:H, 0:W].astype(np.float64)
    jx, jy = JITTER[frame]
    px, py = gx + 0.5 + jx, gy + 0.5 + jy
    ids = surface_ids(px, py, frame, dither)
    lut = lambda table: np.array([table[s] for s in range(7)], np.float32)[ids]  # noqa: E731
    motion = np.zeros((H, W, 2), np.float32)
    motion[(ids == BRANCH) | (ids == CANOPY)] = (-TREE_SPEED, 0)   # points to the previous position
    hit = np.where(ids == SKY, 0, 3 + gy / 50).astype(np.float32)
    return {'color': surface_color(ids, px, py).astype(np.float32),
            'depth': lut(DEPTH), 'motion': motion,
            'normal_roughness': lut(NORMAL), 'diffuse_albedo': lut(ALBEDO),
            'specular_albedo': np.where((ids == SKY)[..., None], np.float32(0), np.float32(0.04))
            .repeat(4, axis=-1).astype(np.float32),
            'status': np.where(ids == SKY, 2, 1).astype(np.uint32),
            'specular_hit_distance': hit}


def projection():
    f = 1 / np.tan(0.6)
    return [f / (W / H), 0, 0, 0, 0, f, 0, 0, 0, 0, 0, 1, 0, 0, NEAR, 0]


def make_manifest(frames):
    identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    entries = [{'index': i, 'serial': 100 + i, 'sl_frame': 200 + i, 'viewport': 0, 'delivered': True,
                'reset': False, 'reset_reasons': 0, 'replaced_sr': True, 'jitter': list(JITTER[i]),
                'motion_vector_scale': [1, 1], 'pre_exposure': 1, 'exposure_scale': 1,
                'world_to_view': identity, 'view_to_clip': projection()} for i in range(len(frames))]
    return {'format': 'mw4-rr-capture', 'version': 1, 'frame_count': len(frames), 'render_width': W,
            'render_height': H, 'output_width': 2 * W, 'output_height': 2 * H, 'quality': 2, 'preset': 6,
            'settings': {}, 'reset_reason_bits': {'1': 'rejected', '2': 'cut', '4': 'generation', '8': 'gap'},
            'frames': entries}


# ---- injected faults -----------------------------------------------------------------------

def edge_mask(depth, connectivity=8):
    """Non-sky pixels next to sky (8- or 4-neighbour)."""
    sky = depth == 0
    padded = np.pad(sky, 1)
    near = np.zeros_like(sky)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if (dy or dx) and (connectivity == 8 or dy == 0 or dx == 0):
                near |= padded[1 + dy:1 + dy + H, 1 + dx:1 + dx + W]
    return ~sky & near


def neighbour(array, dy, dx):
    """array[y + dy, x + dx], edge pixels repeated (no wrap-around)."""
    pad = [(1, 1), (1, 1)] + [(0, 0)] * (array.ndim - 2)
    return np.pad(array, pad, mode='edge')[1 + dy:1 + dy + H, 1 + dx:1 + dx + W]


def shift_depth(frame, i):
    frame['depth'] = np.roll(frame['depth'], 1, axis=1)


def blend_color(frame, i):
    """Non-sky pixels 4-adjacent to sky get 50 % of the mean of their sky neighbours."""
    sky = frame['depth'] == 0
    total = np.zeros_like(frame['color'])
    count = np.zeros(sky.shape, np.float32)
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        s = neighbour(sky, dy, dx)
        total += neighbour(frame['color'], dy, dx) * s[..., None]
        count += s
    edge = edge_mask(frame['depth'], 4)
    frame['color'][edge] = 0.5 * frame['color'][edge] + 0.5 * (total[edge] / count[edge][:, None])


def filter_depth(frame, i):
    """Non-sky pixels next to sky get the 3x3 box mean of depth (sky counts as 0)."""
    d = frame['depth']
    mean = sum(neighbour(d, dy, dx) for dy in (-1, 0, 1) for dx in (-1, 0, 1)) / 9
    edge = edge_mask(d)
    d[edge] = mean[edge]


def nan_color(frame, i):
    if i == 3:
        ys, xs = np.nonzero(edge_mask(frame['depth']))
        pick = np.random.default_rng(3).choice(len(ys), 20, replace=False)
        frame['color'][ys[pick], xs[pick]] = np.nan


def sky_motion(frame, i):
    frame['motion'][edge_mask(frame['depth'])] = 0


def make_capture(fault=None, dither=False):
    frames = [render_frame(i, dither) for i in range(N)]
    if fault:
        for i, frame in enumerate(frames):
            fault(frame, i)
    return make_manifest(frames), frames


# ---- RR output (rr_output, output size) ----------------------------------------------------

def render_output(frame, shape, flicker=0.0):
    """A converged RR output: the scene sampled at the unjittered output pixel centres, RGBA. With
    flicker, output pixels on either side of a sky edge get a fresh random log-normal gain
    (sigma = flicker) every frame: the blotches."""
    oh, ow = shape
    gy, gx = np.mgrid[0:oh, 0:ow].astype(np.float64)
    px, py = (gx + 0.5) * (W / ow), (gy + 0.5) * (H / oh)
    ids = surface_ids(px, py, frame, False)
    color = surface_color(ids, px, py)
    if flicker:
        sky = ids == SKY
        edge = (sky & ase.dilate(~sky, 1)) | (~sky & ase.dilate(sky, 1))
        gain = np.exp(np.random.default_rng(500 + frame).normal(0, flicker, int(edge.sum())))
        color[edge] *= gain[:, None]
    return np.concatenate([color, np.ones(shape + (1,))], axis=-1).astype(np.float32)


def make_capture_with_output(shape=(2 * H, 2 * W), flicker=0.0):
    manifest, frames = make_capture()
    for i, frame in enumerate(frames):
        frame['rr_output'] = render_output(i, shape, flicker)
    manifest['output_width'], manifest['output_height'] = shape[1], shape[0]
    return manifest, frames


# ---- on-disk format ------------------------------------------------------------------------

def small_float_bits(values, mantissa_bits):
    """Unsigned float bits with a 5-bit exponent (bias 15), via float16 (same exponent), with
    round to nearest; NaN stays NaN, negatives become 0, overflow becomes Inf."""
    values = np.asarray(values, np.float32)
    nan = np.isnan(values)
    with np.errstate(over='ignore', invalid='ignore'):
        half = np.maximum(np.where(nan, 0, values), 0).astype(np.float16)
    bits = half.view(np.uint16).astype(np.uint32) & 0x7FFF
    drop = 10 - mantissa_bits
    bits = np.minimum((bits + (1 << (drop - 1))) >> drop, 31 << mantissa_bits)
    return np.where(nan, (31 << mantissa_bits) | 1, bits).astype(np.uint32)


def encode_r11g11b10(rgb):
    return (small_float_bits(rgb[..., 0], 6) | (small_float_bits(rgb[..., 1], 6) << 11) |
            (small_float_bits(rgb[..., 2], 5) << 22)).astype('<u4')


# name -> (DXGI format, numpy dtype for the file, bytes per pixel)
DISK_FORMATS = {'color': (26, None, 4), 'depth': (41, '<f4', 4), 'motion': (34, '<f2', 4),
                'normal_roughness': (10, '<f2', 8), 'diffuse_albedo': (10, '<f2', 8),
                'specular_albedo': (10, '<f2', 8), 'status': (42, '<u4', 4),
                'specular_hit_distance': (54, '<f2', 2), 'rr_output': (10, '<f2', 8)}
INPUT_NAMES = ['color', 'depth', 'motion', 'normal_roughness', 'diffuse_albedo', 'specular_albedo',
               'status', 'sss_guide', 'color_before_transparency', 'responsivity_mask',
               'specular_hit_distance', 'rr_output']


def write_capture(folder, manifest, frames):
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(json.dumps(manifest))
    for i, (entry, frame) in enumerate(zip(manifest['frames'], frames)):
        inputs = dict.fromkeys(INPUT_NAMES)
        for name, array in frame.items():
            fmt, dtype, bpp = DISK_FORMATS[name]
            data = encode_r11g11b10(array) if name == 'color' else array.astype(dtype)
            path = folder / f'frame{i}_{name}.bin'
            path.write_bytes(data.tobytes())
            h, w = array.shape[:2]   # rr_output is at output size
            inputs[name] = {'file': path.name, 'format': name, 'dxgi_format': fmt, 'width': w, 'height': h,
                            'bytes_per_pixel': bpp, 'source_offset': [0, 0], 'source_size': [w, h]}
        entry['inputs'] = inputs
    (folder / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')


# ---- tests ---------------------------------------------------------------------------------

def run(fault=None, dither=False):
    manifest, frames = make_capture(fault, dither)
    with tempfile.TemporaryDirectory() as tmp:
        return ase.analyze(manifest, frames, tmp)


def verdict(metrics, key):
    return metrics['verdicts'][key]['verdict']


class HelperTests(unittest.TestCase):
    def test_dilate_and_runs(self):
        mask = np.zeros((7, 9), bool)
        mask[3, 4] = True
        self.assertEqual(int(ase.dilate(mask, 2).sum()), 25)
        row = np.array([[1, 1, 0, 1, 1, 1, 0, 1]], bool)
        np.testing.assert_array_equal(ase.run_lengths(row, 1), [[2, 2, 0, 3, 3, 3, 0, 1]])
        np.testing.assert_array_equal(ase.run_lengths(row.T, 0), np.array([[2, 2, 0, 3, 3, 3, 0, 1]]).T)

    def test_masked_median(self):
        values = np.array([[[1.0], [5.0], [3.0], [100.0]], [[2.0], [4.0], [np.nan], [8.0]]])
        mask = np.array([[True, True, True, False], [True, True, True, False]])
        np.testing.assert_allclose(ase.masked_median(values, mask)[:, 0], [3.0, 3.0])
        self.assertTrue(np.isnan(ase.masked_median(values, np.zeros_like(mask))).all())

    def test_phase_shift_sign_and_subpixel(self):
        rng = np.random.default_rng(0)
        spectrum = np.fft.fft2(rng.standard_normal((64, 96)))
        fy, fx = np.meshgrid(np.fft.fftfreq(64), np.fft.fftfreq(96), indexing='ij')
        a = np.fft.ifft2(spectrum * np.exp(-(fx ** 2 + fy ** 2) * 40)).real
        d = (2.3, -0.7)   # b(x) = a(x - d)
        b = np.fft.ifft2(np.fft.fft2(a) * np.exp(-2j * np.pi * (fx * d[0] + fy * d[1]))).real
        (dx, dy), peak = ase.phase_shift(np.fft.fft2(a), np.fft.fft2(b))
        self.assertAlmostEqual(dx, 2.3, delta=0.01)
        self.assertAlmostEqual(dy, -0.7, delta=0.01)
        self.assertGreater(peak, 0.9)

    def test_masked_shift(self):
        gy, gx = np.mgrid[0:80, 0:120].astype(np.float64)
        scene = lambda x, y: np.sin(x / 3.1 + y / 7) + 0.5 * np.cos(y / 2.3 - x / 5)  # noqa: E731
        a, b = scene(gx, gy), scene(gx - 0.37, gy + 0.61)     # b(x) = a(x - d), d = (0.37, -0.61)
        weight = np.ones(a.shape, bool)
        weight[:4], weight[-4:], weight[:, :4], weight[:, -4:] = False, False, False, False
        d, converged, condition = ase.masked_shift(a, b, weight)
        self.assertTrue(converged)
        np.testing.assert_allclose(d, (0.37, -0.61), atol=0.005)

    def test_r11g11b10_encoder(self):
        values = np.array([[1.0, 0.5, 2.0], [0.0, 3.14159, 1e-5], [65024.0, 100.0, 0.25],
                           [np.nan, np.inf, -1.0]], np.float32)
        decoded = decode_r11g11b10(encode_r11g11b10(values))
        np.testing.assert_allclose(decoded[:3], values[:3], rtol=2 ** -6, atol=2 ** -20)
        self.assertTrue(np.isnan(decoded[3, 0]) and np.isinf(decoded[3, 1]) and decoded[3, 2] == 0)
        self.assertEqual(decoded[0].tolist(), [1.0, 0.5, 2.0])


class SyntheticCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clean = run()

    def test_weapon_depth_capture_refused(self):
        manifest, frames = make_capture(None, False)
        manifest['settings']['weapon_depth'] = True
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(ValueError):
            ase.analyze(manifest, frames, tmp)

    def test_tree_tests_undone(self):
        # Color brightness test (on all frames or every other one) and the sky depth floor on all,
        # written the way the GPU writes them: the color copy is RGBA32F with alpha 1, only RGB is
        # scaled, and only depth == 0 is lifted to the floor. The analysis undoes both, so every
        # verdict matches the clean capture.
        for s, step in ((0.0625, 1), (0.25, 1), (4.0, 1), (0.0625, 2)):
            with self.subTest(scale=s, step=step):
                manifest, frames = make_capture_with_output()
                for i, (meta, frame) in enumerate(zip(manifest['frames'], frames)):
                    if i % step == step - 1:
                        meta['color_scale'] = s
                        rgb = frame['color'] * np.float32(s)
                        frame['color'] = np.concatenate([rgb, np.ones(rgb.shape[:2] + (1,), np.float32)], -1)
                        out = frame['rr_output'].copy()
                        out[..., :3] *= np.float32(s)
                        frame['rr_output'] = out
                    meta['depth_floor'] = 1e-7
                    meta['responsivity_source'] = 'sky_edge'
                    frame['depth'] = np.where(frame['depth'] == 0, np.float32(1e-7), frame['depth'])
                reference, original = make_capture_with_output()
                with tempfile.TemporaryDirectory() as tmp:
                    expected = ase.analyze(reference, original, tmp)
                    m = ase.analyze(manifest, frames, tmp)
                    report = (Path(tmp) / 'report.md').read_text()
                self.assertEqual({k: verdict(m, k) for k in m['verdicts']},
                                 {k: verdict(expected, k) for k in expected['verdicts']})
                scaled = list(range(step - 1, len(frames), step))
                self.assertTrue(any(f'frames {scaled}:' in n for n in m['capture']['tests']))
                self.assertIn('depth == depth_floor put back to sky', report)
                self.assertIn('responsivity mask from: sky_edge', report)
                self.assertEqual(expected['capture']['tests'], [])
                # The caller's arrays are left alone.
                self.assertTrue((frames[0]['depth'] > 0).all())
                self.assertEqual(frames[1]['color'].shape[-1], 4)

    def test_tree_tests_undo_exact(self):
        # The floor replaced only depth == 0, so a game's negative or tiny depth stays as RR got
        # it; the scaled color copy loses its constant alpha and rr_output keeps its own.
        depth = np.array([[1e-7, -0.5, 5e-8, 0.3]], np.float32)
        color = np.concatenate([np.full((1, 4, 3), 0.5, np.float32), np.ones((1, 4, 1), np.float32)], -1)
        output = np.full((1, 4, 4), 0.25, np.float32)
        manifest = {'frames': [{'color_scale': 0.25, 'depth_floor': 1e-7, 'responsivity_source': 'none'}]}
        (frame,), notes = ase.undo_tests(manifest, [{'depth': depth, 'color': color, 'rr_output': output}])
        np.testing.assert_array_equal(frame['depth'], np.array([[0, -0.5, 5e-8, 0.3]], np.float32))
        np.testing.assert_array_equal(frame['color'], np.full((1, 4, 3), 2.0, np.float32))
        np.testing.assert_array_equal(frame['rr_output'][..., :3], np.full((1, 4, 3), 1.0, np.float32))
        np.testing.assert_array_equal(frame['rr_output'][..., 3], np.full((1, 4), 0.25, np.float32))
        self.assertEqual(depth[0, 0], np.float32(1e-7))   # The caller's arrays are left alone.
        self.assertEqual(len(notes), 2)

    def test_no_test_notes_without_tests(self):
        # A new-format capture with no tree-noise test on (mask from SSS or none) gets no note.
        for source in ('none', 'sss'):
            with self.subTest(source=source):
                manifest, frames = make_capture_with_output()
                for meta in manifest['frames']:
                    meta.update(color_scale=1.0, depth_floor=0.0, guide_test=0, responsivity_source=source)
                out, notes = ase.undo_tests(manifest, frames)
                self.assertEqual(notes, [])
                self.assertTrue(all(a is b for a, b in zip(out, frames)))
        manifest['frames'][3]['responsivity_source'] = 'water'
        self.assertEqual(ase.undo_tests(manifest, frames)[1], ['responsivity mask from: sss, water'])

    def test_guide_test_noted(self):
        # A guide test can't be undone, so the report says which frames had one, and warns when
        # the mode changed within the capture.
        manifest, frames = make_capture_with_output()
        for i, meta in enumerate(manifest['frames']):
            meta.update(color_scale=1.0, depth_floor=0.0, responsivity_source='none',
                        guide_test=9 if i >= 6 else 7)
        notes = ase.undo_tests(manifest, frames)[1]
        self.assertEqual(notes, [
            "guide test 7 (sky normals face the camera) on in frames [0, 1, 2, 3, 4, 5]: the guides "
            "are the test's, not the game's",
            "guide test 9 (sky roughness 0) on in frames [6, 7]: the guides are the test's, not the "
            "game's",
            'the guide test changed within the capture, so its frames mix guide values'])
        for meta in manifest['frames']:
            meta['guide_test'] = 7
        self.assertEqual(len(ase.undo_tests(manifest, frames)[1]), 1)
        manifest['frames'][0]['guide_test'] = 0   # Off then on: mixed too.
        self.assertIn('the guide test changed within the capture, so its frames mix guide values',
                      ase.undo_tests(manifest, frames)[1])

    def test_a_consistent_inputs(self):
        m = self.clean
        self.assertEqual(verdict(m, 'H1'), 'supports')
        self.assertGreater(m['H1']['flicker_pixels'], 50)
        self.assertEqual(verdict(m, 'H2'), 'against')
        for name in ('status', 'normal', 'diffuse_albedo', 'specular_albedo', 'color'):
            self.assertLess(m['H2']['bands']['sky'][name]['rate'], 0.01, name)
        self.assertEqual(verdict(m, 'H3'), 'against')
        self.assertEqual(verdict(m, 'H4'), 'against')
        self.assertEqual(verdict(m, 'H6'), 'against')
        self.assertTrue(m['camera']['still'])
        self.assertEqual(verdict(m, 'H7'), 'against')
        self.assertEqual(m['H7']['best_convention'], 'shift = -jitter delta')
        self.assertLess(m['H7']['best_residual'], 0.1)
        self.assertNotEqual(verdict(m, 'H8'), 'supports')
        self.assertEqual(verdict(m, 'H9'), 'supports')   # bright synthetic sky: numbers must print
        self.assertIsNotNone(m['H9']['sky_over_edge_non_sky'])

    def test_b1_depth_shifted_one_pixel(self):
        m = run(shift_depth)
        self.assertEqual(verdict(m, 'H2'), 'supports')
        self.assertEqual((m['H2']['best_shift']['status']['dx'], m['H2']['best_shift']['status']['dy']), (1, 0))

    def test_b2_color_blended_at_edges(self):
        m = run(blend_color)
        self.assertEqual(verdict(m, 'H2'), 'supports')
        self.assertGreater(m['H2']['blended']['sky_boundary']['fraction'] - m['H2']['blended']['hill_edge']['fraction'], ase.H2_SUPPORT_BLEND)

    def test_c_filtered_depth(self):
        self.assertEqual(verdict(run(filter_depth), 'H3'), 'supports')

    def test_d_nan_in_color(self):
        m = run(nan_color)
        self.assertEqual(verdict(m, 'H6'), 'supports')
        self.assertEqual(m['H6']['counts']['color']['nan']['sky_edge_band'], 20)

    def test_e_sky_motion_on_tree_edges(self):
        m = run(sky_motion)
        self.assertEqual(verdict(m, 'H4'), 'supports')
        self.assertNotEqual(verdict(m, 'H8'), 'supports')   # its static mask relies on motion

    def test_f_dithered_foliage(self):
        m = run(dither=True)
        self.assertEqual(verdict(m, 'H8'), 'supports')
        for key in ('H2', 'H3', 'H4', 'H6', 'H7'):
            self.assertNotEqual(verdict(m, key), 'supports', key)
        self.assertGreater(m['H8']['jitter_fit']['sky']['share'], self.clean['H8']['jitter_fit']['sky']['share'])

    def test_h0_a_output_flickers_at_sky_edges(self):
        manifest, frames = make_capture_with_output(flicker=0.4)
        with tempfile.TemporaryDirectory() as tmp:
            m = ase.analyze(manifest, frames, tmp)
            for name in ('h0_output_flicker.png', 'h0_crop.png'):
                self.assertIn(name, m['images'])
                self.assertTrue((Path(tmp) / name).is_file(), name)
        self.assertEqual(verdict(m, 'H0'), 'supports')
        h = m['H0']
        self.assertEqual(len(h['pairs']), N - 1)
        self.assertGreater(h['regions']['sky_edge_band']['output']['p90'], ase.H0_VISIBLE)
        self.assertLess(h['regions']['hill_edge_band']['output']['p90'], 0.01)
        self.assertGreaterEqual(h['lift'], ase.H0_RR_LIFT)
        self.assertIn('points at RR itself', m['verdicts']['H0']['text'])
        for key in ('H2', 'H3', 'H4', 'H6', 'H7'):   # rr_output must not disturb the input checks
            self.assertEqual(verdict(m, key), verdict(self.clean, key), key)
        manifest['frames'][4]['world_to_view'] = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5, 0, 0, 1]
        with tempfile.TemporaryDirectory() as tmp:
            moved = ase.analyze(manifest, frames, tmp)
        self.assertEqual(verdict(moved, 'H0'), 'inconclusive')
        self.assertTrue(moved['verdicts']['H0']['text'].startswith('skipped, camera moved'))

    def test_h0_b_output_stable(self):
        manifest, frames = make_capture_with_output()
        manifest['frames'][0]['reset'] = True   # RR reset: frame 0 is noisy at the edges but must be skipped
        frames[0]['rr_output'] = render_output(0, (2 * H, 2 * W), flicker=0.8)
        with tempfile.TemporaryDirectory() as tmp:
            m = ase.analyze(manifest, frames, tmp)
        self.assertEqual(verdict(m, 'H0'), 'against')
        self.assertEqual(m['H0']['frames_skipped_reset'], [0])
        self.assertEqual(m['H0']['pairs'], [[i, i + 1] for i in range(1, N - 1)])
        self.assertEqual(m['H0']['regions']['sky_edge_band']['output']['p90'], 0)
        self.assertIn('less unstable than its input', m['verdicts']['H0']['text'])

    def test_h0_c_no_rr_output(self):
        m = self.clean
        self.assertFalse(m['H0']['available'])
        self.assertEqual(verdict(m, 'H0'), 'inconclusive')
        self.assertIn('no rr_output', m['verdicts']['H0']['text'])
        self.assertFalse(any(name.startswith('h0_') for name in m['images']))
        report = ase.report_text(m)
        self.assertIn("## H0: is the noise visible in RR's output", report)
        self.assertIn('Not measured: the capture has no rr_output', report)

    def test_h0_d_non_integer_scale(self):
        np.testing.assert_array_equal(ase.output_index((4, 4), (6, 5))[0], [0, 1, 1, 2, 3, 3])
        np.testing.assert_array_equal(ase.output_index((4, 4), (6, 5))[1], [0, 1, 2, 2, 3])
        self.assertEqual(ase.crop_to_output((10, 20, 30, 40), (H, W), (139, 251)), (15, 28, 48, 59))
        shape = (139, 251)   # x 1.569, y 1.448 of render
        manifest, frames = make_capture_with_output(shape, flicker=0.4)
        frames[3]['rr_output'][0, 0, 1] = np.nan
        with tempfile.TemporaryDirectory() as tmp:
            m = ase.analyze(manifest, frames, tmp, crop=(10, 5, 60, 40))
            with Image.open(Path(tmp) / 'h0_output_flicker.png') as image:
                self.assertEqual(image.width, 3 * shape[1])   # upscaled x3 to >= MIN_IMAGE_WIDTH
        self.assertEqual(verdict(m, 'H0'), 'supports')
        h = m['H0']
        self.assertEqual((h['output_height'], h['output_width']), shape)
        self.assertAlmostEqual(h['scale'][0], shape[1] / W)
        self.assertEqual(h['crop']['window_output_px'], list(ase.crop_to_output((10, 5, 60, 40), (H, W), shape)))
        self.assertGreater(h['crop']['regions']['sky_edge_band']['output']['pixels'], ase.MIN_PIXELS)
        self.assertEqual(h['output_bad_values']['nan']['total'], 1)
        self.assertEqual(verdict(m, 'H6'), 'against')   # bad output values are reported in H0, not H6

    def test_round_trip_through_files_and_cli(self):
        manifest, frames = make_capture_with_output(flicker=0.4)
        with tempfile.TemporaryDirectory() as tmp:
            folder, out = Path(tmp) / 'capture', Path(tmp) / 'out'
            write_capture(folder, manifest, frames)
            loaded_manifest, loaded = load(folder)
            self.assertEqual(len(loaded), N)
            self.assertEqual(sorted(loaded[0]), sorted(frames[0]))
            np.testing.assert_allclose(loaded[2]['color'], frames[2]['color'], rtol=2 ** -5)
            np.testing.assert_array_equal(loaded[2]['depth'], frames[2]['depth'])
            np.testing.assert_array_equal(loaded[2]['status'], frames[2]['status'])
            self.assertEqual(loaded[2]['rr_output'].shape, (2 * H, 2 * W, 4))
            np.testing.assert_allclose(loaded[2]['rr_output'], frames[2]['rr_output'], rtol=2 ** -10)
            result = subprocess.run([sys.executable, str(HERE / 'analyze_sky_edges.py'), str(folder),
                                     '--out', str(out)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = (out / 'report.md').read_text()
            for key in ase.HYPOTHESES:
                self.assertIn(f'**{key} ', report)
            metrics = json.loads((out / 'metrics.json').read_text())
            for key in ('H1', 'H2', 'H3', 'H6', 'H7'):
                self.assertEqual(metrics['verdicts'][key]['verdict'], self.clean['verdicts'][key]['verdict'], key)
            self.assertEqual(metrics['verdicts']['H0']['verdict'], 'supports')
            self.assertIn('H0 RR output noise at sky edges: supports', result.stdout)
            for name in metrics['images']:
                self.assertTrue((out / name).is_file(), name)


if __name__ == '__main__':
    unittest.main(verbosity=2)
