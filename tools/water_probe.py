"""Probe an RR input capture for why running water blurs under DLSS Ray Reconstruction (RR).

Reads a capture folder written by the mw4-rr addon (tools/read_capture.py) and measures, for a region
around running water and a control region, which of these holds:
  H1  depth, motion vectors and guides on the water describe what is behind it, and the water only
      appears in the colour (drawn after the colour-before-transparency snapshot);
  H2  the water surface is in depth and guides, but its flowing detail changes between frames in a
      way the motion vectors do not describe, so temporal accumulation smears it;
  H3  reflections arrive as raw noisy ray-traced lighting that RR over-smooths.
It prints numbers and clearly labelled HEURISTIC hints; it does not prove anything.

CLI:  python tools/water_probe.py CAPTURE_DIR [--region X0 Y0 X1 Y1] [--control X0 Y0 X1 Y1] [--out DIR]
      python tools/water_probe.py --self-test
Coordinates are render pixels, half-open [X0,X1) x [Y0,Y1). Output goes to --out (default
CAPTURE_DIR/water-probe): panels_frame0.png, report.json, report.txt. Without --region only the
pictures and a whole-frame summary are written. Without --control the same-size window that does not
touch the region (CONTROL_MARGIN) with the most status==1 pixels, then the fewest transparency-delta
pixels, is used; the report says which. If Pillow is missing the tiles are saved as tile_<name>.npy.

Assumptions (the report repeats the ones that were decided from data):
- Static statistics are from frame 0 over status==1 pixels (the histogram and depth range cover the
  whole window); temporal statistics average all consecutive frame pairs i-1 -> i, skipping pairs
  whose frame i has the reset flag.
- Motion: mv_px = motion * motion_vector_scale (per axis) is the offset in render pixels from a pixel
  in the current frame to where that surface was in the previous frame (DLSS convention): the
  previous frame is sampled at pixel + mv_px. If that is tiny while the camera moved, motion *
  (width, height) is tried as well, both residuals are reported, and the clearly better one is used.
- Jitter: every frame is rendered at its own sub-pixel jitter (manifest `jitter`, render pixels),
  which RR removes itself. Unless camera.motion_vectors_jittered is 1 the vectors do not contain it,
  so the previous frame is sampled at pixel + mv_px + (sx dx, sy dy), (dx, dy) = jitter[i-1] -
  jitter[i], with sx, sy in {+1, -1} (the sign convention of each axis is not known in advance).
  They are decided on the first usable pair from the guide residuals only (the colour is noisy), in
  the control window, else the frame outside the region, else the whole frame, over the pixels that
  are misaligned without compensation, and must beat no compensation clearly; the choice and the
  residuals are reported. If the jitter changes but nothing can be decided, the two animation hints
  report fires=None ("jitter not compensated").
- Resampling floor: warping with a fractional shift blurs sharp textures, which alone makes a
  static surface look like it changes. Per pair the round trip |warp(warp(frame i, +f), -f) -
  frame i| / 2, f = frac(mv + jitter offset), measures that on frame i itself (guides, colour and
  delta; reported as *_resampling_floor); the animation hints also need the region residual above
  HINT_FLOOR_RATIO x this floor. The residual can still not tell flow or waves from the per-frame
  blue-noise dither the game applies to water normals; hence the wording of those hints.
- normal_roughness: rgb = signed world-space normal, a = linear roughness. Colour is pre-exposed HDR
  (scene * pre_exposure): colour residuals use colour / pre_exposure so that exposure changes do not
  count as animation. R11G11B10 (6-bit mantissa) alone leaves about 0.1 % of the luminance as residual
  after the box filter, so the colour hint also needs 1 % of the mean luminance. Pictures use
  x / (1 + x) then gamma 1/2.2.
- Depth is hardware depth (reversed-Z when camera.depth_inverted == 1). View-space positions come
  from camera.clip_to_view (else the inverse of camera.view_to_clip / the frame's view_to_clip) for
  ndc = (2 (x+0.5)/W - 1, 1 - 2 (y+0.5)/H, depth, 1), divided by w. The 16 manifest numbers are
  row-major; whether they act on row vectors (ndc @ M, DirectXMath style) or column vectors (M @
  ndc) is decided from the structure of view_to_clip: clip w is +-z, so the last column is
  (0,0,+-1,0) for row vectors and the last row is for column vectors. If that is absent or both
  match, the data decide: the layout whose view depths have one consistent sign for the most
  pixels wins, ties by how well the vertical ray slope matches tan(camera.fov / 2) (using |z|).
  The data check also runs as a cross-check of the structure rule. The report has the evidence.
  For the self-test's matrix both layouts give positive depth (the wrong one is only off by the
  near distance), so only the structure rule or the fov check can tell them apart.
"""
import argparse
import contextlib
import io
import json
import math
import os
from pathlib import Path
import sys
import tempfile

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from read_capture import decode_r11g11b10, load  # noqa: E402

# Hint thresholds (see hints in the report) and measurement constants.
DELTA_THRESHOLD = 0.02          # relative luminance delta that counts as "changed by transparency"
HINT_DELTA_REGION = 0.5         # water drawn after the snapshot: region delta fraction above this ...
HINT_DELTA_CONTROL = 0.1        # ... and control below this
HINT_FLAT_PLANE_RMS = 0.01      # flat surface: plane-fit RMS below this fraction of median view depth ...
HINT_FLAT_ANGLE_DEG = 10.0      # ... and mean normal angle spread below this
HINT_RESIDUAL_RATIO = 3.0       # animates beyond the motion vectors: region residual above this x control
MIN_RESIDUAL = 1e-3             # ... and above this absolute floor (so 0 vs 1e-9 noise does not count)
MIN_RELATIVE_COLOUR = 0.01      # colour hint: ... and above this fraction of the region's mean luminance
HINT_FLOOR_RATIO = 2.0          # ... and above this x the region's own resampling floor (see temporal_stats)
DISOCCLUSION_TOLERANCE = 0.05   # reprojected source depth may differ from the pixel's by this fraction
BOX_SIZE = 5                    # box filter that suppresses lighting noise before comparing colour
CONTROL_MARGIN = 4              # automatic control keeps this many pixels away from the region
MV_TINY_PIXELS = 0.05           # mean |mv_px| below this while the camera moved: try other scalings
ALT_IMPROVEMENT = 0.8           # an alternative (MV scaling, jitter sign) must drop the residual below this x
MIN_HINT_PIXELS = 50            # hints need at least this many valid pixels in region and control
JITTER_MIN_PIXELS = 1e-6        # jitter differences below this need no compensation
TILE_WIDTH = 640                # panel tile width in pixels
PANEL_COLUMNS = 3
LABEL_HEIGHT = 16
NO_PILLOW_ENV = 'WATER_PROBE_NO_PILLOW'   # set to simulate a missing Pillow
LUMA = np.array([0.2126, 0.7152, 0.0722])
REQUIRED_INPUTS = ('color', 'depth', 'motion', 'normal_roughness', 'diffuse_albedo',
                   'specular_albedo', 'status')
LAYOUTS = ('row-vector', 'column-vector')
STATUS_COLORS = np.array([(0, 0, 0), (70, 70, 70), (90, 150, 230), (230, 60, 200), (240, 200, 40),
                          (255, 255, 255)], np.uint8)   # padding, ordinary, sky, special, invalid, other
RED, GREEN = (255, 0, 0), (0, 255, 0)


# --- numeric helpers ------------------------------------------------------------------------------

def luminance(rgb):
    return np.asarray(rgb, np.float64)[..., :3] @ LUMA


def relative_delta(color, before):
    """Luminance of |color - before| over luminance of color; NaN where either is not finite."""
    with np.errstate(invalid='ignore', divide='ignore'):
        rel = luminance(np.abs(color.astype(np.float64) - before)) / np.maximum(luminance(color), 1e-3)
    return np.where(np.isfinite(rel), rel, np.nan)


def box_sum(array, size):
    """Sum over size x size windows (size odd, zeros outside the image) of an H x W array."""
    r = size // 2
    for axis in (0, 1):
        pad = [(0, 0), (0, 0)]
        pad[axis] = (r + 1, r)
        cumulative = np.cumsum(np.pad(array, pad), axis=axis)
        n = array.shape[axis]
        array = np.take(cumulative, np.arange(size, size + n), axis=axis) - \
            np.take(cumulative, np.arange(n), axis=axis)
    return array


def box_filter(image, size=BOX_SIZE):
    """Mean over size x size windows of the finite pixels (NaN where a window has none)."""
    image = np.asarray(image, np.float64)
    finite = np.isfinite(image)
    total, count = box_sum(np.where(finite, image, 0.0), size), box_sum(finite.astype(np.float64), size)
    with np.errstate(invalid='ignore', divide='ignore'):
        return np.where(count > 0.5, total / count, np.nan)


def make_taps(mv, offset=(0.0, 0.0)):
    """Bilinear taps that fetch, for every pixel, the previous-frame sample at pixel + mv + offset."""
    height, width = mv.shape[:2]
    gy, gx = np.indices((height, width))
    sx, sy = gx + mv[..., 0] + offset[0], gy + mv[..., 1] + offset[1]
    inside = (sx >= 0) & (sx <= width - 1) & (sy >= 0) & (sy <= height - 1)   # False for NaN
    sx, sy = np.where(inside, sx, 0.0), np.where(inside, sy, 0.0)
    x0, y0 = np.floor(sx).astype(np.intp), np.floor(sy).astype(np.intp)
    x1, y1 = np.minimum(x0 + 1, width - 1), np.minimum(y0 + 1, height - 1)
    fx, fy = (sx - x0).ravel(), (sy - y0).ravel()
    return {'shape': (height, width), 'inside': inside,
            'shift': mv + np.asarray(offset, np.float32),
            'index': [(y0 * width + x0).ravel(), (y0 * width + x1).ravel(),
                      (y1 * width + x0).ravel(), (y1 * width + x1).ravel()],
            'weight': [(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy],
            'nearest': (np.rint(sy).astype(np.intp) * width + np.rint(sx).astype(np.intp)).ravel()}


def warp(image, taps):
    """Bilinear samples of an H x W [x C] image at the taps; zero-weight texels are ignored."""
    flat = image.reshape(image.shape[0] * image.shape[1], -1)
    out = 0.0
    with np.errstate(invalid='ignore'):
        for index, weight in zip(taps['index'], taps['weight']):
            out = out + np.where(weight[:, None] > 0, weight[:, None] * flat[index], 0.0)
    return out.reshape(image.shape)


def valid_rows(array, mask):
    """array[mask] as float64 rows (N x C) without rows that hold a non-finite value."""
    rows = array[mask].astype(np.float64)
    rows = rows.reshape(len(rows), -1)
    return rows[np.isfinite(rows).all(axis=1)]


def ordinary_mask(frame):
    return (frame['status'] == 1) & np.isfinite(frame['depth'])


# --- geometry -------------------------------------------------------------------------------------

def matrix4(numbers):
    """16 row-major manifest numbers -> 4x4 float64, or None if absent or not finite (null -> NaN)."""
    if numbers is None or len(numbers) != 16:
        return None
    matrix = np.array(numbers, dtype=np.float64).reshape(4, 4)
    return matrix if np.isfinite(matrix).all() else None


def view_to_clip_matrix(meta):
    """camera.view_to_clip (the game's), else the frame's view_to_clip (as passed to RR)."""
    forward = matrix4((meta.get('camera') or {}).get('view_to_clip'))
    return forward if forward is not None else matrix4(meta.get('view_to_clip'))


def clip_to_view_matrix(meta):
    """camera.clip_to_view, else the inverse of view_to_clip_matrix."""
    matrix = matrix4((meta.get('camera') or {}).get('clip_to_view'))
    if matrix is None:
        forward = view_to_clip_matrix(meta)
        if forward is not None:
            with contextlib.suppress(np.linalg.LinAlgError):
                matrix = np.linalg.inv(forward)
    return matrix


def matrix_layout(view_to_clip):
    """Layout from the projection's structure: clip w = +-z, so the last column is (0,0,+-1,0) for
    row vectors and the last row is for column vectors. None if neither or both match."""
    if view_to_clip is None:
        return None
    tolerance = 1e-5 * np.abs(view_to_clip).max()

    def w_from_z(v):
        return abs(v[0]) < tolerance and abs(v[1]) < tolerance and abs(v[3]) < tolerance and \
            abs(abs(v[2]) - 1) < 1e-3

    row, column = w_from_z(view_to_clip[:, 3]), w_from_z(view_to_clip[3, :])
    return LAYOUTS[0] if row and not column else LAYOUTS[1] if column and not row else None


def view_positions(depth, px, py, size, c2v, layout):
    """View-space xyz (N x 3, NaN where w is 0) of pixels (px, py) with hardware depth `depth`."""
    width, height = size
    ndc = np.stack([(px + 0.5) / width * 2 - 1, 1 - (py + 0.5) / height * 2, depth,
                    np.ones(np.shape(depth))], axis=-1).astype(np.float64)
    with np.errstate(divide='ignore', invalid='ignore'):
        homogeneous = ndc @ (c2v if layout == 'row-vector' else c2v.T)
        return homogeneous[..., :3] / homogeneous[..., 3:4]


def view_depth(depth, c2v, layout):
    py, px = np.indices(depth.shape)
    return view_positions(depth, px, py, (depth.shape[1], depth.shape[0]), c2v, layout)[..., 2]


def plane_rms(points):
    """RMS orthogonal distance of Nx3 points from their total-least-squares plane (None if N < 3)."""
    if len(points) < 3:
        return None
    centred = points - points.mean(axis=0)
    return float(np.linalg.svd(centred, compute_uv=False)[-1] / math.sqrt(len(points)))


def choose_layout(c2v, depth, ordinary, fov, view_to_clip=None):
    """Row or column use of the clip_to_view numbers (see module docstring): the structure of
    view_to_clip decides if it can, else the data; the data check is always run as a cross-check."""
    height, width = depth.shape
    structure = matrix_layout(view_to_clip)
    index = np.flatnonzero(ordinary)
    if index.size == 0:
        return {'layout': structure or LAYOUTS[0], 'ambiguous': structure is None,
                'source': 'structure of view_to_clip' if structure else 'default (no status==1 pixels)',
                'structure_layout': structure, 'data_layout': None, 'data_ambiguous': True, 'scores': {}}
    index = index[::max(1, index.size // 20000)]
    py, px = np.divmod(index, width)
    ndc_y = 1 - (py + 0.5) / height * 2
    expected = math.tan(fov / 2) if fov and 0 < fov < math.pi else None
    scores = {}
    for layout in LAYOUTS:
        position = view_positions(depth.ravel()[index], px, py, (width, height), c2v, layout)
        z = position[:, 2]
        usable = np.isfinite(z) & (z != 0)
        off_axis = usable & (np.abs(ndc_y) > 0.25) & np.isfinite(position[:, 1])
        slope = (float(np.median(np.abs(position[off_axis, 1] / z[off_axis] / ndc_y[off_axis])))
                 if off_axis.any() else None)
        error = abs(math.log(slope / expected)) if expected and slope else None
        positive, negative = float((usable & (z > 0)).mean()), float((usable & (z < 0)).mean())
        scores[layout] = {'positive_depth_fraction': positive, 'negative_depth_fraction': negative,
                          'consistent_sign_fraction': max(positive, negative),
                          'vertical_ray_slope': slope, 'fov_error': error}

    def rank(layout):
        score = scores[layout]
        return (-round(score['consistent_sign_fraction'], 2),
                math.inf if score['fov_error'] is None else score['fov_error'])

    best, other = sorted(LAYOUTS, key=rank)
    a, b = scores[best], scores[other]
    tied = round(a['consistent_sign_fraction'], 2) == round(b['consistent_sign_fraction'], 2)
    data_ambiguous = tied and (a['fov_error'] is None or b['fov_error'] is None or
                               abs(a['fov_error'] - b['fov_error']) < 0.1)
    return {'layout': structure or best, 'ambiguous': structure is None and data_ambiguous,
            'source': 'structure of view_to_clip' if structure else 'data (sign consistency, fov slope)',
            'structure_layout': structure, 'data_layout': best, 'data_ambiguous': data_ambiguous,
            'scores': scores}


# --- per-frame preparation and motion convention ----------------------------------------------------

def pre_exposure(meta):
    value = meta.get('pre_exposure')
    return float(value) if isinstance(value, (int, float)) and math.isfinite(value) and value > 0 else 1.0


def jitter_delta(metas, i):
    """jitter[i-1] - jitter[i] in render pixels, or None if the vectors already include jitter
    (camera.motion_vectors_jittered) or the jitter is missing."""
    if i == 0 or (metas[i].get('camera') or {}).get('motion_vectors_jittered'):
        return None
    jitters = [m.get('jitter') or (m.get('camera') or {}).get('jitter') for m in (metas[i - 1], metas[i])]
    if any(j is None or len(j) != 2 or not all(isinstance(v, (int, float)) and math.isfinite(v) for v in j)
           for j in jitters):
        return None
    return np.array(jitters[0], np.float64) - np.array(jitters[1], np.float64)


def pair_offset(item, signs):
    """Jitter compensation (dx, dy) of the pair ending at this frame for the axis signs (sx, sy)."""
    delta = item['jitter_delta']
    return (0.0, 0.0) if delta is None else (signs[0] * delta[0], signs[1] * delta[1])


def prepare(frames, metas, notes):
    """Per-frame derived arrays (ordinary mask, depth for comparisons, filtered luminances)."""
    matrices = [clip_to_view_matrix(meta) for meta in metas]
    ordinary0 = ordinary_mask(frames[0])
    layout = None
    if all(matrix is not None for matrix in matrices):
        camera = metas[0].get('camera') or {}
        layout = choose_layout(matrices[0], frames[0]['depth'], ordinary0, camera.get('fov'),
                               view_to_clip_matrix(metas[0]))
        if layout['ambiguous']:
            notes.append('clip_to_view layout is ambiguous (view_to_clip structure inconclusive, both '
                         'layouts scored alike, fov check could not separate them); using '
                         f'{layout["layout"]}')
        elif layout['structure_layout'] and not layout['data_ambiguous'] and \
                layout['data_layout'] != layout['layout']:
            notes.append(f'view_to_clip structure says {layout["layout"]} but the data check prefers '
                         f'{layout["data_layout"]}; using {layout["layout"]}')
    else:
        notes.append('clip_to_view / view_to_clip missing or not finite: no view-space plane fit; '
                     'depth comparisons use hardware depth')
    derived = []
    for i, (frame, meta, matrix) in enumerate(zip(frames, metas, matrices)):
        depth = frame['depth'] if layout is None else view_depth(frame['depth'], matrix, layout['layout'])
        exposure = pre_exposure(meta)
        item = {'ordinary': ordinary_mask(frame), 'depth': depth.astype(np.float32), 'delta': None,
                'pre_exposure': exposure, 'jitter_delta': jitter_delta(metas, i),
                'colour': box_filter(luminance(frame['color']) / exposure).astype(np.float32)}
        if 'color_before_transparency' in frame:
            with np.errstate(invalid='ignore'):
                change = frame['color'].astype(np.float64) - frame['color_before_transparency']
            item['delta'] = box_filter(luminance(change) / exposure).astype(np.float32)
        derived.append(item)
    return derived, layout, matrices


def motion_pixels(frame, meta, scale=None):
    """motion * motion_vector_scale (or * `scale`) as float32 H x W x 2."""
    if scale is None:
        scale = meta.get('motion_vector_scale') or \
            (meta.get('camera') or {}).get('motion_vector_scale') or [1.0, 1.0]
    return frame['motion'].astype(np.float32) * np.asarray(scale, np.float32)


def camera_moved(metas):
    keys = [np.array(m.get('world_to_view') or (m.get('camera') or {}).get('position') or [], np.float64)
            for m in metas]
    return any(k.shape != keys[0].shape or not np.allclose(k, keys[0], atol=1e-6, equal_nan=True)
               for k in keys[1:])


def resampling_floor(image, shift):
    """What bilinear resampling alone does to `image` at the fractional part f of `shift`: the
    round trip |warp(warp(image, +f), -f) - image| / 2 (it errs about twice as much as one warp)."""
    fraction = (shift - np.floor(shift)).astype(np.float32)
    forward, back = make_taps(fraction), make_taps(-fraction)
    reached = forward['inside'] if image.ndim == 2 else forward['inside'][..., None]
    shifted = np.where(reached, warp(image, forward), np.nan)
    return np.abs(warp(shifted, back) - image) / 2


def pair_maps(prev, cur, taps, prev_item, cur_item, floor=False):
    """Residual maps (NaN where unusable) of frame i-1 warped into frame i, and the valid mask. With
    floor=True also '<name>_floor' maps: the resampling-only baseline of frame i at the same shift."""
    height, width = taps['shape']
    with np.errstate(invalid='ignore'):
        source_depth = prev_item['depth'].ravel()[taps['nearest']].reshape(height, width)
        agree = np.abs(source_depth - cur_item['depth']) <= DISOCCLUSION_TOLERANCE * np.abs(cur_item['depth'])
        valid = cur_item['ordinary'] & taps['inside'] & agree
        maps = {}
        for name, channels in (('normal_roughness', 4), ('diffuse_albedo', 3)):
            maps[name] = np.abs(warp(prev[name][..., :channels], taps) -
                                cur[name][..., :channels]).mean(axis=-1)
        maps['colour'] = np.abs(warp(prev_item['colour'], taps) - cur_item['colour'])
        if prev_item['delta'] is not None and cur_item['delta'] is not None:
            maps['transparency_delta'] = np.abs(warp(prev_item['delta'], taps) - cur_item['delta'])
        if floor:
            for name, channels in (('normal_roughness', 4), ('diffuse_albedo', 3)):
                maps[f'{name}_floor'] = resampling_floor(cur[name][..., :channels],
                                                         taps['shift']).mean(axis=-1)
            maps['colour_floor'] = resampling_floor(cur_item['colour'], taps['shift'])
            if 'transparency_delta' in maps:
                maps['transparency_delta_floor'] = resampling_floor(cur_item['delta'], taps['shift'])
    return {k: np.where(valid & np.isfinite(v), v, np.nan).astype(np.float32)
            for k, v in maps.items()}, valid


def first_usable_pair(metas):
    return next((i for i in range(1, len(metas)) if not metas[i].get('reset')), None)


GUIDE_KEYS = ('normal_roughness', 'diffuse_albedo')
SCORE_KEYS = GUIDE_KEYS + ('colour',)


def score_maps(maps, valid, window):
    """(mean residual of guides and colour inside the window or None, valid pixel count)."""
    x0, y0, x1, y1 = window
    values = []
    for key in SCORE_KEYS:
        part = maps[key][y0:y1, x0:x1]
        if np.isfinite(part).any():
            values.append(float(np.nanmean(part)))
    return (float(np.mean(values)) if values else None), int(valid[y0:y1, x0:x1].sum())


def combined_residual(maps, keys=SCORE_KEYS):
    """Per-pixel mean of the residual maps `keys` (NaN where all are unusable)."""
    stack = np.stack([maps[key] for key in keys])
    finite = np.isfinite(stack)
    mean = np.where(finite, stack, 0.0).sum(axis=0) / np.maximum(finite.sum(axis=0), 1)
    return np.where(finite.any(axis=0), mean, np.nan)


def pair_score(frames, derived, mv, i):
    """Whole-frame (residual, valid pixels) for pair i-1 -> i under the vectors mv."""
    maps, valid = pair_maps(frames[i - 1], frames[i], make_taps(mv[i]), derived[i - 1], derived[i])
    return score_maps(maps, valid, (0, 0, valid.shape[1], valid.shape[0]))


def choose_motion(frames, metas, derived, notes, tiny_pixels=MV_TINY_PIXELS):
    """Set derived[i]['mv'] (render pixels) and describe the convention used and why."""
    height, width = frames[0]['color'].shape[:2]
    mv = [motion_pixels(frame, meta) for frame, meta in zip(frames, metas)]
    probe = min(1, len(frames) - 1)
    rows = valid_rows(mv[probe], derived[probe]['ordinary'])
    mean = float(np.linalg.norm(rows, axis=1).mean()) if len(rows) else None
    info = {'convention': 'motion * motion_vector_scale in render pixels; the previous frame is '
                          'sampled at pixel + mv', 'mean_magnitude_px': mean,
            'camera_moved': camera_moved(metas), 'alternative_checked': False}
    pair = first_usable_pair(metas)
    if pair is not None and mean is not None and mean < tiny_pixels and info['camera_moved']:
        alternative = [motion_pixels(f, m, (width, height)) for f, m in zip(frames, metas)]
        (px_score, px_valid), (alt_score, alt_valid) = \
            pair_score(frames, derived, mv, pair), pair_score(frames, derived, alternative, pair)
        info.update(alternative_checked=True, residual_pixel_convention=px_score,
                    residual_alternative=alt_score, valid_pixels_pixel_convention=px_valid,
                    valid_pixels_alternative=alt_valid)
        lower = None not in (px_score, alt_score) and alt_score < ALT_IMPROVEMENT * px_score
        comparable = alt_valid >= 0.5 * px_valid
        if lower and comparable:
            mv = alternative
            info['convention'] = ('motion * (width, height) in render pixels; the vectors looked '
                                  'normalised (the previous frame is sampled at pixel + mv)')
            notes.append(f'mean |motion * motion_vector_scale| is {mean:.3g} px while the camera moved; '
                         f'motion * (width, height) leaves a much smaller residual '
                         f'({alt_score:.3g} vs {px_score:.3g}, {alt_valid} vs {px_valid} valid pixels) '
                         'and is used')
        else:
            why = 'it keeps fewer than half the valid pixels' if lower else \
                'its residual is not clearly lower'
            notes.append(f'mean |motion * motion_vector_scale| is {mean:.3g} px while the camera moved, '
                         f'but motion * (width, height) is rejected ({why}: residuals {alt_score} vs '
                         f'{px_score}, valid pixels {alt_valid} vs {px_valid}); keeping the pixel '
                         'convention, motion vectors may be unreliable')
    for item, vectors in zip(derived, mv):
        item['mv'] = vectors
    return info


def sign_label(signs):
    return f'{signs[0]:+d},{signs[1]:+d}'


def choose_jitter(frames, metas, derived, windows):
    """Axis signs (sx, sy) of the jitter compensation pixel + mv + (sx dx, sy dy), with (dx, dy) =
    jitter[i-1] - jitter[i]; returns ((sx, sy), info). Engines differ in the sign of each axis, so all
    of {+1, -1}^2 are tried (an axis whose jitter does not change keeps 0). Decided on the first usable
    pair from the GUIDE residuals only (the guides are noise free and rendered at the jitter; the
    colour is noisy), in the control window, else in the frame outside the region (the water must not
    decide its own compensation), else in the whole frame. Only pixels whose uncompensated residual
    exceeds MIN_RESIDUAL count (hard edges that do not move would otherwise dilute or reverse the
    decision), at least MIN_HINT_PIXELS of them; the best candidate must lower their mean residual
    below ALT_IMPROVEMENT x the uncompensated one."""
    info = {'rule': 'previous frame sampled at pixel + mv + (sx*dx, sy*dy), (dx, dy) = jitter[i-1] - '
                    'jitter[i]; (sx, sy) chosen from {+1, -1}^2 by the lowest guide residual on the '
                    'first usable pair', 'applied': False, 'sign': None, 'decided_on': None}
    pair = first_usable_pair(metas)
    delta = derived[pair]['jitter_delta'] if pair is not None else None
    if delta is None:
        info['reason'] = ('camera.motion_vectors_jittered is set, jitter is missing, or there is no usable '
                          'frame pair: no jitter compensation')
        return (0, 0), info
    if np.abs(delta).max() < JITTER_MIN_PIXELS:
        info['reason'] = 'jitter does not change between the first usable pair: nothing to compensate'
        return (0, 0), info
    info['pair'] = pair
    options = [(1, -1) if abs(d) >= JITTER_MIN_PIXELS else (0,) for d in delta]
    candidates = [(sx, sy) for sx in options[0] for sy in options[1]]
    maps = {}
    for signs in [(0, 0)] + candidates:
        taps = make_taps(derived[pair]['mv'], pair_offset(derived[pair], signs))
        maps[signs] = combined_residual(pair_maps(frames[pair - 1], frames[pair], taps,
                                                  derived[pair - 1], derived[pair])[0], GUIDE_KEYS)
    whole, region = windows['whole_frame'], windows.get('region')
    where = {'control': (windows['control'], None)} if 'control' in windows else {}
    where['outside_region' if region else 'whole_frame'] = (whole, region)
    info['residuals'] = {}
    for name, (window, exclude) in where.items():
        use = np.ones(maps[(0, 0)].shape, bool)
        if exclude is not None:
            use[exclude[1]:exclude[3], exclude[0]:exclude[2]] = False
        x0, y0, x1, y1 = window
        chosen = (use & (maps[(0, 0)] > MIN_RESIDUAL))[y0:y1, x0:x1]
        by = {}
        for signs, residual in maps.items():
            values = residual[y0:y1, x0:x1][chosen]
            values = values[np.isfinite(values)]
            by[signs] = float(values.mean()) if values.size else None
        info['residuals'][name] = {'none': by[(0, 0)], 'pixels': int(chosen.sum()),
                                   **{sign_label(signs): by[signs] for signs in candidates}}
        if chosen.sum() < MIN_HINT_PIXELS or None in by.values():
            continue
        best = min(candidates, key=lambda signs: by[signs])
        if by[best] < ALT_IMPROVEMENT * by[(0, 0)]:
            info.update(applied=True, sign=list(best), decided_on=name)
            return best, info
    info['reason'] = (f'no window had {MIN_HINT_PIXELS}+ pixels with an uncompensated guide residual '
                      f'above {MIN_RESIDUAL} that any axis sign pair lowered below {ALT_IMPROVEMENT} x: '
                      'jitter not compensated')
    return (0, 0), info


# --- window statistics ----------------------------------------------------------------------------

def clip_window(window, width, height, name, notes):
    clipped = (max(window[0], 0), max(window[1], 0), min(window[2], width), min(window[3], height))
    if clipped[0] >= clipped[2] or clipped[1] >= clipped[3]:
        raise ValueError(f'{name} {tuple(window)} is empty or outside the {width}x{height} render image '
                         '(coordinates are half-open [X0,X1) x [Y0,Y1))')
    if clipped != tuple(window):
        notes.append(f'{name} {tuple(window)} clipped to the image: {clipped}')
    return clipped


def pick_control(region, ordinary, delta):
    """Same-size window clear of the region (CONTROL_MARGIN) with the most status==1 pixels, then the
    fewest pixels whose transparency delta exceeds DELTA_THRESHOLD, counted in the window grown by
    CONTROL_MARGIN (the box filter and bilinear taps reach that far); first in raster order on ties."""
    height, width = ordinary.shape
    x0, y0, x1, y1 = region
    w, h = x1 - x0, y1 - y0
    if w > width or h > height:
        return None
    ys, xs = np.indices((height - h + 1, width - w + 1))

    def window_sums(mask, grow=0):
        total = np.zeros((height + 1, width + 1), np.int64)
        total[1:, 1:] = mask.astype(np.int64).cumsum(axis=0).cumsum(axis=1)
        top, bottom = np.clip(ys - grow, 0, height), np.clip(ys + h + grow, 0, height)
        left, right = np.clip(xs - grow, 0, width), np.clip(xs + w + grow, 0, width)
        return total[bottom, right] - total[top, right] - total[bottom, left] + total[top, left]

    count = window_sums(ordinary)
    changed = window_sums(delta > DELTA_THRESHOLD, CONTROL_MARGIN) if delta is not None \
        else np.zeros_like(count)
    clear = (xs + w <= x0 - CONTROL_MARGIN) | (xs >= x1 + CONTROL_MARGIN) | \
        (ys + h <= y0 - CONTROL_MARGIN) | (ys >= y1 + CONTROL_MARGIN)
    if not clear.any():
        return None
    candidates = np.flatnonzero(clear)
    best = candidates[np.lexsort((changed.ravel()[candidates], -count.ravel()[candidates]))[0]]
    y, x = divmod(int(best), count.shape[1])
    return {'window': (x, y, x + w, y + h), 'ordinary_pixels': int(count[y, x]),
            'changed_pixels': int(changed[y, x])}


def normal_stats(rows):
    length = np.linalg.norm(rows[:, :3], axis=1)
    unit = rows[length > 1e-6, :3] / length[length > 1e-6, None]
    if len(unit) == 0:
        return None
    mean = unit.mean(axis=0)
    resultant = float(np.linalg.norm(mean))
    if resultant < 1e-9:
        return {'mean_normal': None, 'resultant_length': resultant, 'mean_angle_deg': None}
    direction = mean / resultant
    angles = np.degrees(np.arccos(np.clip(unit @ direction, -1.0, 1.0)))
    return {'mean_normal': direction.tolist(), 'resultant_length': resultant,
            'mean_angle_deg': float(angles.mean())}


def channel_stats(rows):
    return {'mean': rows.mean(axis=0).tolist(), 'std': rows.std(axis=0).tolist()} if len(rows) else None


def plane_stats(frame, ordinary, window, matrix, layout):
    """Least-squares plane through the view-space positions of the window's status==1 pixels."""
    if layout is None:
        return None
    x0, y0, x1, y1 = window
    ys, xs = np.nonzero(ordinary[y0:y1, x0:x1])
    height, width = frame['depth'].shape
    points = view_positions(frame['depth'][y0:y1, x0:x1][ys, xs], xs + x0, ys + y0, (width, height),
                            matrix, layout['layout'])
    points = points[np.isfinite(points).all(axis=1)]
    rms = plane_rms(points)
    if rms is None:
        return None
    median = float(np.median(points[:, 2]))
    return {'points': len(points), 'median_view_depth': median, 'rms_residual': rms,
            'rms_over_median_view_depth': rms / abs(median) if median else None}


def window_stats(frame, item, delta, matrix, layout, window):
    """Static statistics of one window of frame 0 (status histogram and depth range: all pixels)."""
    x0, y0, x1, y1 = window
    sl = (slice(y0, y1), slice(x0, x1))
    ordinary = item['ordinary'][sl]
    status = frame['status'][sl]
    histogram = {str(v): int((status == v).sum()) for v in range(5)}
    histogram['other'] = int(status.size - sum(histogram.values()))
    finite_depth = frame['depth'][sl][np.isfinite(frame['depth'][sl])]
    normals = valid_rows(frame['normal_roughness'][sl], ordinary)
    motion = np.linalg.norm(valid_rows(item['mv'][sl], ordinary), axis=1)
    brightness = valid_rows(luminance(frame['color'][sl]), ordinary) / item['pre_exposure']
    stats = {'window': [x0, y0, x1, y1], 'pixels': int(status.size), 'ordinary_pixels': int(ordinary.sum()),
             'status': histogram,
             'mean_luminance': float(brightness.mean()) if len(brightness) else None,
             'depth': {'min': float(finite_depth.min()), 'median': float(np.median(finite_depth)),
                       'max': float(finite_depth.max())} if finite_depth.size else None,
             'plane_fit': plane_stats(frame, item['ordinary'], window, matrix, layout),
             'normals': normal_stats(normals),
             'roughness': {'mean': float(normals[:, 3].mean()), 'std': float(normals[:, 3].std())}
             if len(normals) else None,
             'diffuse_albedo': channel_stats(valid_rows(frame['diffuse_albedo'][sl][..., :3], ordinary)),
             'specular_albedo': channel_stats(valid_rows(frame['specular_albedo'][sl][..., :3], ordinary)),
             'motion_px': {'mean_magnitude': float(motion.mean()), 'std_magnitude': float(motion.std()),
                           'mean_vector': valid_rows(item['mv'][sl], ordinary).mean(axis=0).tolist()}
             if len(motion) else None}
    if delta is not None:
        relative = delta[sl][ordinary]
        relative = relative[np.isfinite(relative)]
        stats['transparency_delta'] = {
            'threshold': DELTA_THRESHOLD, 'fraction_above': float((relative > DELTA_THRESHOLD).mean()),
            'mean_relative': float(relative.mean())} if relative.size else None
    return stats


def temporal_stats(frames, metas, derived, windows, jitter_signs):
    """Per window: residuals after warping frame i-1 into frame i with i's motion vectors (plus the
    jitter compensation), averaged over the pairs, and the resampling-only floor of the same
    warps (what bilinear resampling at the same fractional shifts does to frame i itself). Also
    returns the maps and index of the first usable pair, for the pictures."""
    per_pair = {name: {} for name in windows}
    skipped, used, first = [], 0, None
    for i in range(1, len(frames)):
        if metas[i].get('reset'):
            skipped.append(i)
            continue
        maps, valid = pair_maps(frames[i - 1], frames[i],
                                make_taps(derived[i]['mv'], pair_offset(derived[i], jitter_signs)),
                                derived[i - 1], derived[i], floor=True)
        used += 1
        first = first or (maps, i)
        for name, (x0, y0, x1, y1) in windows.items():
            per_pair[name].setdefault('valid_pixels', []).append(float(valid[y0:y1, x0:x1].sum()))
            for key, values in maps.items():
                finite = values[y0:y1, x0:x1][np.isfinite(values[y0:y1, x0:x1])]
                if finite.size:
                    per_pair[name].setdefault(key, []).append(float(finite.mean()))
    if used == 0:
        return {name: None for name in windows}, None

    def mean(values):
        return float(np.mean(values)) if values else None

    result = {}
    for name in windows:
        means = {key: mean(values) for key, values in per_pair[name].items()}
        guides = [means[k] for k in ('normal_roughness', 'diffuse_albedo') if means.get(k) is not None]
        guide_floors = [means[f'{k}_floor'] for k in ('normal_roughness', 'diffuse_albedo')
                        if means.get(f'{k}_floor') is not None]
        result[name] = {'pairs_used': used, 'pairs_skipped_reset': skipped,
                        'valid_pixels': means['valid_pixels'],
                        'guide_residual': mean(guides),
                        'normal_roughness_residual': means.get('normal_roughness'),
                        'diffuse_albedo_residual': means.get('diffuse_albedo'),
                        'colour_residual': means.get('colour'),
                        'transparency_delta_residual': means.get('transparency_delta'),
                        'guide_resampling_floor': mean(guide_floors),
                        'colour_resampling_floor': means.get('colour_floor'),
                        'transparency_delta_resampling_floor': means.get('transparency_delta_floor')}
    return result, first


# --- hints ----------------------------------------------------------------------------------------

def dig(data, *keys):
    for key in keys:
        if not isinstance(data, dict) or data.get(key) is None:
            return None
        data = data[key]
    return data


def animates(region_value, control_value, floor=None):
    """Region residual above HINT_RESIDUAL_RATIO x control, MIN_RESIDUAL and HINT_FLOOR_RATIO x the
    region's resampling floor (when known)."""
    return region_value > HINT_RESIDUAL_RATIO * control_value and region_value > MIN_RESIDUAL and \
        (floor is None or region_value > HINT_FLOOR_RATIO * floor)


def make_hints(region, control, jitter_undecided=False):
    """HEURISTIC readings of the region (and control) statistics; fires is None if not evaluable,
    including when region or control has fewer than MIN_HINT_PIXELS valid pixels, and for the two
    animation hints when the jitter changes between frames but no compensation could be decided."""
    hints = []

    def add(hint_id, fires, statement, detail, low_sample=False):
        hints.append({'id': hint_id, 'fires': fires, 'statement': statement, 'detail': detail,
                      'low_sample': low_sample})

    def few(*counts):
        """Detail text if any of the (name, pixel count) pairs is below the floor, else None."""
        if all((n or 0) >= MIN_HINT_PIXELS for _, n in counts):
            return None
        return 'too few valid pixels (' + ', '.join(f'{name} {n or 0:.0f}' for name, n in counts) + \
            f'; needs >= {MIN_HINT_PIXELS} in each)'

    static = [('region', dig(region, 'ordinary_pixels')), ('control', dig(control, 'ordinary_pixels'))]
    moving = [('region', dig(region, 'temporal', 'valid_pixels')),
              ('control', dig(control, 'temporal', 'valid_pixels'))]

    statement = 'water drawn after the before-transparency snapshot'
    r = dig(region, 'transparency_delta', 'fraction_above')
    c = dig(control, 'transparency_delta', 'fraction_above')
    if r is None or c is None:
        add('drawn_after_snapshot', None, f'not evaluated: {statement}',
            'needs colour_before_transparency and a control window')
    elif few(*static):
        add('drawn_after_snapshot', None, f'not evaluated: {statement}', few(*static), True)
    else:
        fires = r > HINT_DELTA_REGION and c < HINT_DELTA_CONTROL
        add('drawn_after_snapshot', fires, statement if fires else f'not indicated: {statement}',
            f'region delta fraction {r:.3g} (needs > {HINT_DELTA_REGION}), control {c:.3g} '
            f'(needs < {HINT_DELTA_CONTROL})')

    rms = dig(region, 'plane_fit', 'rms_over_median_view_depth')
    angle = dig(region, 'normals', 'mean_angle_deg')
    if rms is None or angle is None:
        add('flat_surface', None, 'not evaluated: region depth/guides flat or rough',
            'needs a plane fit and normals on status==1 pixels')
    elif few(static[0]):
        add('flat_surface', None, 'not evaluated: region depth/guides flat or rough', few(static[0]), True)
    else:
        flat = rms < HINT_FLAT_PLANE_RMS and angle < HINT_FLAT_ANGLE_DEG
        add('flat_surface', flat, 'region depth/guides look like a flat surface' if flat else
            'region depth/guides look like rough geometry (riverbed?)',
            f'plane RMS {rms:.3g} of median view depth (flat needs < {HINT_FLAT_PLANE_RMS}), '
            f'normal angle spread {angle:.3g} deg (flat needs < {HINT_FLAT_ANGLE_DEG})')

    not_compensated = ('jitter not compensated: the sub-pixel jitter changes between frames and no axis '
                       'sign pair could be decided from the guides, so the residuals would mostly '
                       'measure the jitter')
    statement = ('guides change frame to frame beyond the motion vectors (flow, waves or per-frame '
                 'dither)')
    r, c = dig(region, 'temporal', 'guide_residual'), dig(control, 'temporal', 'guide_residual')
    floor = dig(region, 'temporal', 'guide_resampling_floor')
    if r is None or c is None:
        add('guides_animate', None, f'not evaluated: {statement}', 'needs a control window and 2+ frames')
    elif jitter_undecided:
        add('guides_animate', None, f'not evaluated: {statement}', not_compensated)
    elif few(*moving):
        add('guides_animate', None, f'not evaluated: {statement}', few(*moving), True)
    else:
        fires = animates(r, c, floor)
        add('guides_animate', fires, statement if fires else f'not indicated: {statement}',
            f'region guide residual {r:.3g}, control {c:.3g}, region resampling floor '
            f'{"n/a" if floor is None else format(floor, ".3g")} (needs > {HINT_RESIDUAL_RATIO} x control, '
            f'> {MIN_RESIDUAL} and > {HINT_FLOOR_RATIO} x the floor)')

    statement = ('colour or transparency layer changes frame to frame beyond the motion vectors (flow, '
                 'waves, per-frame dither or noise)')
    checks = [(label, dig(region, 'temporal', key), dig(control, 'temporal', key),
               dig(region, 'temporal', f'{key}_relative'), dig(control, 'temporal', f'{key}_relative'),
               dig(region, 'temporal', floor_key))
              for label, key, floor_key in (('colour', 'colour_residual', 'colour_resampling_floor'),
                                            ('delta', 'transparency_delta_residual',
                                             'transparency_delta_resampling_floor'))]
    checks = [check for check in checks if check[1] is not None and check[2] is not None]
    if not checks:
        add('colour_animates', None, f'not evaluated: {statement}', 'needs a control window and 2+ frames')
    elif jitter_undecided:
        add('colour_animates', None, f'not evaluated: {statement}', not_compensated)
    elif few(*moving):
        add('colour_animates', None, f'not evaluated: {statement}', few(*moving), True)
    else:
        def part(label, r, c, relative_r, relative_c, floor):
            text = f'region {label} residual {r:.3g}, control {c:.3g}'
            if floor is not None:
                text += f', region resampling floor {floor:.3g}'
            if relative_r is None or relative_c is None:
                return text
            return text + (f' (relative to mean luminance {dig(region, "mean_luminance"):.3g} / '
                           f'{dig(control, "mean_luminance"):.3g}: {relative_r:.3g} / {relative_c:.3g})')

        fires = any(animates(r, c, floor) and (rr is None or rr > MIN_RELATIVE_COLOUR)
                    for _, r, c, rr, _, floor in checks)
        add('colour_animates', fires, statement if fires else f'not indicated: {statement}',
            '; '.join(part(*check) for check in checks) + f' (needs > {HINT_RESIDUAL_RATIO} x control, > '
            f'{MIN_RESIDUAL}, > {MIN_RELATIVE_COLOUR} of the region luminance and > {HINT_FLOOR_RATIO} x '
            'the floor; luminance / pre_exposure after a 5x5 box filter)')
    return hints


# --- pictures -------------------------------------------------------------------------------------

def import_pillow():
    """(Image, ImageDraw) or None if Pillow is missing (or NO_PILLOW_ENV is set)."""
    if os.environ.get(NO_PILLOW_ENV):
        return None
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return None
    return Image, ImageDraw


def tonemap(rgb):
    x = np.maximum(np.nan_to_num(np.asarray(rgb, np.float64), nan=0.0, posinf=1e6, neginf=0.0), 0.0)
    return (x / (1 + x)) ** (1 / 2.2)


def to_u8(values):
    return (np.clip(np.nan_to_num(values, nan=0.0), 0.0, 1.0) * 255 + 0.5).astype(np.uint8)


def gray_tile(values):
    return np.repeat(to_u8(values)[..., None], 3, axis=-1)


def heat_tile(values):
    v = np.clip(np.nan_to_num(np.asarray(values, np.float64), nan=0.0, posinf=1.0, neginf=0.0), 0.0, 1.0)
    return to_u8(np.stack([3 * v, 3 * v - 1, 3 * v - 2], axis=-1))


def log_norm(values):
    """Log10 of the positive finite values scaled to 0..1 between their 1st and 99th percentile."""
    good = np.isfinite(values) & (values > 0)
    out = np.zeros(values.shape)
    if good.any():
        logs = np.log10(values[good].astype(np.float64))
        low, high = np.percentile(logs, [1, 99])
        out[good] = np.clip((logs - low) / max(high - low, 1e-9), 0.0, 1.0)
    return out


def residual_tile(values):
    """Heat map scaled to the 99th percentile (at least MIN_RESIDUAL); unusable pixels dark blue."""
    good = np.isfinite(values)
    full = max(float(np.percentile(values[good], 99)), MIN_RESIDUAL) if good.any() else MIN_RESIDUAL
    tile = heat_tile(np.where(good, values / full, 0.0))
    tile[~good] = (0, 0, 60)
    return tile, full


def build_tiles(panel):
    """[(key, label, uint8 RGB tile at render resolution)] for frame 0."""
    frame, item, delta, maps = panel['frame'], panel['item'], panel['delta'], panel['maps']
    shape = frame['color'].shape[:2]
    tiles = []

    def add(key, label, tile):
        tiles.append((key, label, tile))

    def missing(key, label):
        add(key, label, np.full(shape + (3,), 40, np.uint8))

    add('color', 'color (x/(1+x), gamma 1/2.2)', to_u8(tonemap(frame['color'])))
    if 'color_before_transparency' in frame:
        add('color_before_transparency', 'color before transparency',
            to_u8(tonemap(frame['color_before_transparency'])))
        add('transparency_delta', 'transparency delta (heat of sqrt relative luminance delta)',
            heat_tile(np.sqrt(np.clip(np.nan_to_num(delta), 0.0, 1.0))))
    else:
        missing('color_before_transparency', 'color before transparency: not sent')
        missing('transparency_delta', 'transparency delta: not available')
    add('depth', 'depth (log, hardware value)', gray_tile(log_norm(frame['depth'])))
    magnitude = np.linalg.norm(item['mv'], axis=-1)
    finite = magnitude[np.isfinite(magnitude)]
    full = max(float(np.percentile(finite, 99)), 0.5) if finite.size else 0.5
    add('motion', f'motion |mv| in pixels (full scale {full:.3g})', heat_tile(magnitude / full))
    add('normal', 'normal (n*0.5+0.5)', to_u8(frame['normal_roughness'][..., :3] * 0.5 + 0.5))
    add('roughness', 'roughness', gray_tile(frame['normal_roughness'][..., 3]))
    add('diffuse_albedo', 'diffuse albedo', to_u8(frame['diffuse_albedo'][..., :3]))
    add('specular_albedo', 'specular albedo', to_u8(frame['specular_albedo'][..., :3]))
    add('status', 'status (0 black, 1 gray, 2 sky blue, 3 magenta, 4 yellow)',
        STATUS_COLORS[np.minimum(frame['status'], 5)])
    hit = frame.get('specular_hit_distance')
    if hit is not None and hit.ndim == 2:
        add('specular_hit_distance', 'specular hit distance (log)', gray_tile(log_norm(hit)))
    else:
        missing('specular_hit_distance', 'specular hit distance: not sent')
    if maps is None:
        missing('guide_residual', 'guide temporal residual: needs 2 usable frames')
        missing('colour_residual', 'colour temporal residual: needs 2 usable frames')
    else:
        pair = f'frame {panel["pair"] - 1} vs {panel["pair"]}'
        tile, full = residual_tile((maps['normal_roughness'] + maps['diffuse_albedo']) / 2)
        add('guide_residual', f'guide temporal residual, {pair} (full scale {full:.3g})', tile)
        tile, full = residual_tile(maps['colour'])
        add('colour_residual', f'colour temporal residual, 5x5 box-filtered luminance, {pair} '
                               f'(full scale {full:.3g})', tile)
    return tiles


def draw_boxes(tile, boxes, scale, thickness):
    """Outline windows (render pixels, times scale) on an RGB tile, in place."""
    height, width = tile.shape[:2]
    for (x0, y0, x1, y1), color in boxes:
        a, b = int(round(x0 * scale)), int(round(y0 * scale))
        c, d = min(max(int(round(x1 * scale)), a + 1), width), min(max(int(round(y1 * scale)), b + 1), height)
        t = thickness
        tile[b:b + t, a:c] = color
        tile[max(d - t, b):d, a:c] = color
        tile[b:d, a:a + t] = color
        tile[b:d, max(c - t, a):c] = color
    return tile


def write_panels(tiles, boxes, out_dir):
    """panels_frame0.png with labelled tiles; without Pillow the tiles as tile_<key>.npy."""
    pillow = import_pillow()
    if pillow is None:
        for key, _, tile in tiles:
            np.save(out_dir / f'tile_{key}.npy', draw_boxes(tile.copy(), boxes, 1.0, 1))
        print('WARNING: Pillow is not installed; saved the tiles as tile_<name>.npy (render resolution, '
              'windows outlined) instead of panels_frame0.png', file=sys.stderr)
        return
    Image, ImageDraw = pillow
    scaled = []
    for _, label, tile in tiles:
        height, width = tile.shape[:2]
        resampling = getattr(Image, 'Resampling', Image)    # Pillow < 9.1 has the constants on Image
        resample = resampling.NEAREST if width <= TILE_WIDTH else resampling.BOX
        size = (TILE_WIDTH, max(1, round(height * TILE_WIDTH / width)))
        image = Image.fromarray(tile).resize(size, resample)
        scaled.append((label, draw_boxes(np.array(image), boxes, TILE_WIDTH / width, 2)))
    row_height = max(tile.shape[0] for _, tile in scaled) + LABEL_HEIGHT
    rows = -(-len(scaled) // PANEL_COLUMNS)
    canvas = Image.new('RGB', (PANEL_COLUMNS * TILE_WIDTH, rows * row_height), (24, 24, 24))
    draw = ImageDraw.Draw(canvas)
    for n, (label, tile) in enumerate(scaled):
        x, y = (n % PANEL_COLUMNS) * TILE_WIDTH, (n // PANEL_COLUMNS) * row_height
        canvas.paste(Image.fromarray(tile), (x, y + LABEL_HEIGHT))
        draw.text((x + 4, y + 3), label, fill=(255, 255, 255))
    canvas.save(out_dir / 'panels_frame0.png')


# --- report ---------------------------------------------------------------------------------------

def normals_look_encoded(normals):
    """True if no finite normal component is negative (stored as n*0.5+0.5 instead of signed?)."""
    finite = normals[np.isfinite(normals)]
    return finite.size > 0 and bool(finite.min() >= 0)


def analyse(manifest, frames, region, control, mv_tiny_pixels=MV_TINY_PIXELS):
    """Measure everything; returns (report dict, data for the pictures)."""
    if not frames:
        raise ValueError('capture has no frames')
    for i, frame in enumerate(frames):
        missing = [name for name in REQUIRED_INPUTS if name not in frame]
        if missing:
            raise ValueError(f'frame {i} lacks required input(s): {", ".join(missing)}')
    height, width = frames[0]['color'].shape[:2]
    metas = manifest['frames']
    notes = []
    derived, layout, matrices = prepare(frames, metas, notes)
    motion = choose_motion(frames, metas, derived, notes, mv_tiny_pixels)
    before = frames[0].get('color_before_transparency')
    delta = relative_delta(frames[0]['color'], before) if before is not None else None
    if delta is None:
        notes.append('color_before_transparency was not sent: no transparency-delta statistics')
    if len(frames) < 2:
        notes.append('only one frame: no temporal statistics')
    if normals_look_encoded(frames[0]['normal_roughness'][..., :3]):
        notes.append('normal_roughness rgb is never negative: the normals may be stored encoded '
                     '(n*0.5+0.5) instead of signed; normal statistics assume signed')

    windows = {'whole_frame': (0, 0, width, height)}
    control_info = None
    if region is not None:
        region = clip_window(region, width, height, 'region', notes)
        windows['region'] = region
        if control is not None:
            control = clip_window(control, width, height, 'control', notes)
            control_info = {'window': control, 'selection': 'given with --control'}
            if (region[0] < control[2] and control[0] < region[2] and
                    region[1] < control[3] and control[1] < region[3]):
                notes.append('control overlaps the region')
        else:
            picked = pick_control(region, derived[0]['ordinary'], delta)
            if picked is None:
                notes.append('no control window of the region size fits clear of the region; give --control')
            else:
                control_info = {'window': picked['window'], 'selection': (
                    f'picked automatically: same size as the region, {picked["ordinary_pixels"]} status==1 '
                    f'pixels, {picked["changed_pixels"]} pixels with transparency delta > {DELTA_THRESHOLD}')}
        if control_info:
            windows['control'] = tuple(control_info['window'])
    jitter_signs, jitter = choose_jitter(frames, metas, derived, windows)
    if jitter['applied']:
        residual = jitter['residuals'][jitter['decided_on']]
        notes.append(f'jitter compensation signs (x, y) = ({jitter_signs[0]:+d}, {jitter_signs[1]:+d}) '
                     f'decided in the {jitter["decided_on"]} window (guide residual {residual["none"]:.3g} '
                     f'without, {residual[sign_label(jitter_signs)]:.3g} with, over '
                     f'{residual["pixels"]} pixels)')
    elif 'residuals' in jitter:
        notes.append(jitter['reason'])
    jitter_undecided = 'residuals' in jitter and not jitter['applied']
    stats = {name: window_stats(frames[0], derived[0], delta, matrices[0], layout, window)
             for name, window in windows.items()}
    for name, window_stat in stats.items():
        if window_stat['ordinary_pixels'] == 0:
            notes.append(f'{name} has no status==1 pixels (see its status histogram): guide, depth-plane, '
                         'motion and delta statistics are empty')
    temporal, first = temporal_stats(frames, metas, derived, windows, jitter_signs) if len(frames) > 1 \
        else ({name: None for name in windows}, None)
    if len(frames) > 1 and first is None:
        notes.append('every frame pair was skipped (frame i has the reset flag): no temporal statistics')
    for name in windows:
        stats[name]['temporal'] = temporal[name]
        if temporal[name] is not None:
            for key in ('colour', 'transparency_delta'):
                residual, mean_luminance = temporal[name][f'{key}_residual'], stats[name]['mean_luminance']
                temporal[name][f'{key}_residual_relative'] = \
                    residual / mean_luminance if residual is not None and mean_luminance else None
    clip = {'layout': None, 'used': 'hardware depth (no usable clip_to_view / view_to_clip)'}
    if layout is not None:
        clip = {key: layout[key] for key in ('layout', 'ambiguous', 'source', 'structure_layout',
                                             'data_layout', 'scores')}
    hints = make_hints(stats.get('region'), stats.get('control'), jitter_undecided) if region else []
    notes += [f'hint {hint["id"]} not evaluated: {hint["detail"]}' for hint in hints if hint['low_sample']]
    report = {
        'capture': {'frame_count': len(frames), 'render_size': [width, height],
                    'settings': manifest.get('settings')},
        'region': list(region) if region else None, 'control': control_info,
        'conventions': {'motion': motion, 'jitter': jitter, 'clip_to_view': clip,
                        'colour_residual': f'luminance after a {BOX_SIZE}x{BOX_SIZE} box filter (raw '
                                           'lighting is noisy; the filter suppresses it)'},
        'frames': [{'index': meta.get('index', i), 'jitter': meta.get('jitter'),
                    'motion_vector_scale': meta.get('motion_vector_scale'),
                    'pre_exposure': meta.get('pre_exposure'), 'reset': meta.get('reset'),
                    'delivered': meta.get('delivered')} for i, meta in enumerate(metas)],
        'windows': stats, 'hints': hints, 'notes': notes}
    panel = {'frame': frames[0], 'item': derived[0], 'delta': delta, 'windows': windows,
             'maps': first[0] if first else None, 'pair': first[1] if first else None}
    return report, panel


def clean(value):
    """JSON-safe copy: numpy scalars/arrays to Python, tuples to lists, non-finite floats to None."""
    if isinstance(value, dict):
        return {str(k): clean(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [clean(v) for v in value]
    if isinstance(value, np.ndarray):
        return clean(value.tolist())
    if isinstance(value, (float, np.floating)):
        return float(value) if math.isfinite(value) else None
    if isinstance(value, np.integer):
        return int(value)
    if isinstance(value, np.bool_):
        return bool(value)
    return value


def fmt(value):
    if value is None:
        return 'n/a'
    if isinstance(value, float):
        return f'{value:.4g}'
    if isinstance(value, list):
        return '[' + ', '.join(fmt(v) for v in value) + ']'
    return str(value)


def dict_lines(data, indent):
    pad = ' ' * indent
    for key, value in data.items():
        if isinstance(value, dict):
            yield f'{pad}{key}:'
            yield from dict_lines(value, indent + 2)
        else:
            yield f'{pad}{key}: {fmt(value)}'


def render_text(report):
    capture = report['capture']
    lines = [f'water_probe: {capture["folder"]}: {capture["frame_count"]} frames, render '
             f'{capture["render_size"][0]}x{capture["render_size"][1]}',
             'Static statistics are from frame 0 over status==1 pixels (status 0 padding, 1 ordinary, '
             '2 sky, 3 special, 4 invalid); temporal statistics average the consecutive frame pairs.']
    if report['region'] is None:
        lines.append('No --region given: only the pictures and a whole-frame summary were made. Give '
                     '--region X0 Y0 X1 Y1 (render pixels) around running water to compare it with a '
                     'control window and get hints.')
    else:
        lines.append(f'region {report["region"]}')
    if report['control']:
        lines.append(f'control {report["control"]["window"]}: {report["control"]["selection"]}')
    if report['notes']:
        lines += ['', 'NOTES'] + [f'  {note}' for note in report['notes']]
    lines += ['', 'CONVENTIONS'] + list(dict_lines(report['conventions'], 2)) + \
        ['', 'CAPTURE SETTINGS'] + list(dict_lines(capture['settings'] or {}, 2)) + ['', 'FRAMES']
    lines += [f'  frame {f["index"]}: jitter {fmt(f["jitter"])} motion_vector_scale '
              f'{fmt(f["motion_vector_scale"])} pre_exposure {fmt(f["pre_exposure"])} reset {f["reset"]} '
              f'delivered {f["delivered"]}'
              for f in report['frames']]
    for name, stats in report['windows'].items():
        lines += ['', f'WINDOW {name} {stats["window"]}'] + \
            list(dict_lines({k: v for k, v in stats.items() if k != 'window'}, 2))
    if report['hints']:
        lines += ['', 'HINTS (HEURISTIC: rule-of-thumb readings of the numbers above, not proof)']
        lines += [f'  HEURISTIC: {h["statement"]} ({h["detail"]})' for h in report['hints']]
    return '\n'.join(lines)


def run_probe(folder, region=None, control=None, out_dir=None, echo=False,
              mv_tiny_pixels=MV_TINY_PIXELS):
    """Load the capture, measure, write pictures and report.json/report.txt; returns the report."""
    folder = Path(folder)
    manifest, frames = load(folder)
    report, panel = analyse(manifest, frames, region, control, mv_tiny_pixels)
    report['capture']['folder'] = str(folder)
    out = Path(out_dir) if out_dir else folder / 'water-probe'
    out.mkdir(parents=True, exist_ok=True)
    boxes = [(panel['windows'][name], color) for name, color in (('region', RED), ('control', GREEN))
             if name in panel['windows']]
    write_panels(build_tiles(panel), boxes, out)
    report = clean(report)
    text = render_text(report)
    (out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    (out / 'report.txt').write_text(text + '\n', encoding='utf-8')
    if echo:
        print(text)
        print(f'\nWrote report.txt, report.json and the pictures to {out}')
    return report


# --- self-test ------------------------------------------------------------------------------------

SYN_W, SYN_H = 96, 64
SYN_REGION = (48, 20, 80, 52)     # the "water" rectangle
SYN_NEAR, SYN_FOV = 0.1, 1.0
SYN_WATER_PERIOD = (32.0, 24.0)   # wavelength of the water pattern in x and y, pixels
SYN_INPUTS = ['color', 'depth', 'motion', 'normal_roughness', 'diffuse_albedo', 'specular_albedo',
              'status', 'sss_guide', 'color_before_transparency', 'responsivity_mask',
              'specular_hit_distance']
SYN_FORMATS = {26: ('R11G11B10_FLOAT', 4), 41: ('R32_FLOAT', 4), 34: ('R16G16_FLOAT', 4),
               10: ('R16G16B16A16_FLOAT', 8), 42: ('R32_UINT', 4), 54: ('R16_FLOAT', 2)}
PACKED_INF = 0x7C0 | (0x7C0 << 11) | (0x3E0 << 22)
PACKED_NAN = 0x7C1 | (0x7C1 << 11) | (0x3E1 << 22)


def encode_small_float(value, mantissa_bits):
    """Float array -> exponent|mantissa field of an unsigned 5-bit-exponent float (truncating)."""
    value = np.clip(np.asarray(value, np.float64), 0.0, 65000.0)
    exponent = np.where(value < 2.0 ** -14, 0, np.clip(np.frexp(value)[1] + 14, 1, 30))
    scale = np.ldexp(1.0, np.where(exponent == 0, -14, exponent - 15))
    mantissa = np.where(exponent == 0, value / scale, value / scale - 1.0)
    mantissa = np.minimum(np.floor(mantissa * (1 << mantissa_bits)), (1 << mantissa_bits) - 1)
    return (exponent.astype(np.uint32) << mantissa_bits) | mantissa.astype(np.uint32)


def encode_r11g11b10(rgb):
    """Float HxWx3 -> '<u4' HxW, the inverse of read_capture.decode_r11g11b10 (up to truncation)."""
    return (encode_small_float(rgb[..., 0], 6) | (encode_small_float(rgb[..., 1], 6) << 11) |
            (encode_small_float(rgb[..., 2], 5) << 22)).astype('<u4')


def synthetic_projection():
    """Row-vector (ndc = view @ M) reversed-Z, infinite-far perspective matrix and its inverse."""
    fy = 1 / math.tan(SYN_FOV / 2)
    m = np.zeros((4, 4))
    m[0, 0], m[1, 1], m[2, 3], m[3, 2] = fy / (SYN_W / SYN_H), fy, 1.0, SYN_NEAR
    return m, np.linalg.inv(m)


def scene_jitter(t, jitter, jitter_y=0.0):
    """Jitter of frame t in render pixels: alternates between 0 and `jitter` in x and between -0.125
    and -0.125 + `jitter_y` in y."""
    return jitter * (t % 2), -0.125 + jitter_y * (t % 2)


def synthetic_scene(t, drift, scroll=1.0, jitter=0.25, sign=1, jitter_y=0.0, flip_y=False,
                    period=SYN_WATER_PERIOD, noise=0.0):
    """Frame t of the synthetic scene as float arrays, rendered at the frame's jitter.

    Default: static camera, zero motion; sky rows on top; a ground plane (control); a static
    transparent patch at the left; a flat "water" rectangle whose normals and colour layer scroll
    `scroll` px per frame. With drift: a textured flat scene shifting `drift` px per frame, motion
    vectors stored normalised by the image width, as a moving camera would. Everything, the masks
    and their edges and the depth included, is evaluated at the jittered pixel centre
    pixel + sign * jitter (the tool does not know which sign the renderer uses; flip_y gives y the
    opposite sign). noise: standard deviation of colour noise (the same field in the colour and in
    the colour before transparency, new every frame, like ray-traced lighting)."""
    yy, xx = np.mgrid[0:SYN_H, 0:SYN_W].astype(np.float64)
    fy = 1 / math.tan(SYN_FOV / 2)
    jx, jy = scene_jitter(t, jitter, jitter_y)
    sx, sy = xx + sign * jx, yy + (-sign if flip_y else sign) * jy
    ndc_y = 1 - (sy + 0.5) / SYN_H * 2
    none = np.zeros(yy.shape, bool)
    x0, y0, x1, y1 = SYN_REGION
    cx, cy = sx + 0.5, sy + 0.5
    water = none if drift else (cx >= x0) & (cx < x1) & (cy >= y0) & (cy < y1)
    sky = none if drift else cy < 12
    patch = none if drift else (cx < 8) & (cy >= 12) & (cy < 44)
    strip = none if drift else (cx >= 84) & (cy >= 12)    # static textured "terrain" outside the region
    if drift:
        phase = 2 * np.pi * (sx + drift * t) / 24
        wave = np.sin(phase) * np.cos(2 * np.pi * sy / 20)
        z = np.full(yy.shape, 5.0)
    else:
        phase = 2 * np.pi * ((sx - t * scroll) / period[0] + sy / period[1])
        wave = np.where(strip, np.sin(2 * np.pi * sx / 20) * np.cos(2 * np.pi * sy / 18), np.sin(phase))
        phase = np.where(strip, 2 * np.pi * sx / 20, phase)
        z = np.where(water, 5.0, 6.0 / ((0.3 * ndc_y / fy + 1) / math.hypot(0.3, 1)))
    tilt = np.stack([0.1 * wave, 0.1 * np.cos(phase), np.ones_like(wave)], axis=-1)
    tilt /= np.linalg.norm(tilt, axis=-1, keepdims=True)
    normal = np.zeros((SYN_H, SYN_W, 4))
    normal[..., 2], normal[..., 3] = 1.0, 0.5
    textured = water | strip | bool(drift)
    normal[..., :3] = np.where(textured[..., None], tilt, normal[..., :3])
    albedo = np.full((SYN_H, SYN_W, 4), 0.5)
    specular = np.full((SYN_H, SYN_W, 4), 0.04)
    albedo[..., :3] += np.where(strip | bool(drift), 0.2 * wave, 0.0)[..., None]
    for array in (normal, albedo, specular):
        array[sky] = 0.0
    base = 0.6 + 0.2 * wave if drift else np.where(sky, 0.8, np.where(strip, 0.5 + 0.1 * wave, 0.5))
    before = base[..., None] * np.ones(3)
    if noise:
        before = before + noise * np.random.default_rng(1000 + t).standard_normal(before.shape)
    color = before + np.where(water, 0.3 + 0.2 * wave, 0.0)[..., None] + \
        np.where(patch & ~water, 0.2, 0.0)[..., None]
    motion = np.zeros((SYN_H, SYN_W, 2))
    motion[..., 0] = drift / SYN_W
    return {'color': color, 'before': before, 'depth': np.where(sky, 0.0, SYN_NEAR / z),
            'normal': normal, 'albedo': albedo, 'specular': specular, 'motion': motion,
            'status': np.where(sky, 2, 1).astype('<u4'), 'hit': np.where(water, 1.0, 3.0)}


def write_capture(folder, frames=8, before=True, drift=0, scroll=1.0, jitter=0.25, sign=1,
                  exposure=1.0, **scene_options):
    """Write a synthetic capture that follows capture-format.md (manifest.json last). Odd frames
    have pre_exposure `exposure` (colours scaled to match) and jitter `jitter`; scene_options
    (jitter_y, flip_y, period, noise) go to synthetic_scene."""
    projection, c2v = synthetic_projection()
    folder.mkdir(parents=True, exist_ok=True)
    manifest_frames = []
    for t in range(frames):
        scene = synthetic_scene(t, drift, scroll, jitter, sign, **scene_options)
        pre_exposure = exposure if t % 2 else 1.0
        color = encode_r11g11b10(scene['color'] * pre_exposure)
        prior = encode_r11g11b10(scene['before'] * pre_exposure)
        if t < 2:   # non-finite texels inside the water rectangle
            for packed in (color, prior):
                packed[30, 60], packed[30, 61] = PACKED_INF, PACKED_NAN
            scene['normal'][31, 60], scene['depth'][31, 61] = np.nan, np.inf
            scene['motion'][32, 60], scene['albedo'][32, 61] = np.inf, -np.inf
        images = {'color': (26, color), 'depth': (41, scene['depth'].astype('<f4')),
                  'motion': (34, scene['motion'].astype('<f2')),
                  'normal_roughness': (10, scene['normal'].astype('<f2')),
                  'diffuse_albedo': (10, scene['albedo'].astype('<f2')),
                  'specular_albedo': (10, scene['specular'].astype('<f2')),
                  'status': (42, scene['status']),
                  'specular_hit_distance': (54, scene['hit'].astype('<f2'))}
        if before:
            images['color_before_transparency'] = (26, prior)
        inputs = dict.fromkeys(SYN_INPUTS)
        for name, (dxgi, array) in images.items():
            (folder / f'frame{t}_{name}.bin').write_bytes(array.tobytes())
            inputs[name] = {'file': f'frame{t}_{name}.bin', 'format': SYN_FORMATS[dxgi][0],
                            'dxgi_format': dxgi, 'width': SYN_W, 'height': SYN_H,
                            'bytes_per_pixel': SYN_FORMATS[dxgi][1], 'source_offset': [0, 0],
                            'source_size': [SYN_W * 2, SYN_H * 2]}
        moved = 0.1 * t if drift else 0.0
        world_to_view = np.eye(4)
        world_to_view[3, 0] = -moved
        camera = {'position': [moved, 0, 0], 'right': [1, 0, 0], 'up': [0, 1, 0], 'forward': [0, 0, 1],
                  'near': SYN_NEAR, 'far': 1e30, 'fov': SYN_FOV, 'aspect': SYN_W / SYN_H,
                  'jitter': list(scene_jitter(t, jitter, scene_options.get('jitter_y', 0.0))),
                  'motion_vector_scale': [1, 1],
                  'view_to_clip': projection.ravel().tolist(), 'clip_to_view': c2v.ravel().tolist(),
                  'depth_inverted': 1, 'camera_motion_included': 1, 'motion_vectors_3d': 0, 'reset': 0,
                  'orthographic': 0, 'motion_vectors_dilated': 0, 'motion_vectors_jittered': 0}
        manifest_frames.append({
            'index': t, 'serial': 100 + t, 'sl_frame': 200 + t, 'viewport': 0, 'delivered': True,
            'reset': False, 'reset_reasons': 0, 'replaced_sr': True,
            'stats': {'bad_frame': 0, 'copied_frame': 1, 'isolated_guide_pixels': 0,
                      'sanitized_sss_samples': 0},
            'jitter': list(scene_jitter(t, jitter, scene_options.get('jitter_y', 0.0))),
            'motion_vector_scale': [1, 1], 'pre_exposure': pre_exposure,
            'exposure_scale': 1, 'color_subrect': [0, 0], 'depth_subrect': [0, 0],
            'motion_subrect': [0, 0], 'world_to_view': world_to_view.ravel().tolist(),
            'view_to_clip': projection.ravel().tolist(), 'camera': camera, 'cpu_time_ms': 1000.0 * t,
            'inputs': inputs})
    manifest = {
        'format': 'mw4-rr-capture', 'version': 1, 'created': '2026-10-25T10:31:02',
        'frame_count': frames, 'render_width': SYN_W, 'render_height': SYN_H,
        'output_width': SYN_W * 2, 'output_height': SYN_H * 2, 'quality': 2, 'preset': 6,
        'settings': {'sss_enabled': False, 'transparency_enabled': before, 'guide_test': 0},
        'status_values': {'0': 'padding', '1': 'ordinary', '2': 'sky', '3': 'special', '4': 'invalid'},
        'reset_reason_bits': {'1': 'rejected', '2': 'cut', '4': 'generation', '8': 'gap'},
        'formats': {name: {'dxgi_format': dxgi, 'bytes_per_pixel': bpp}
                    for dxgi, (name, bpp) in SYN_FORMATS.items()},
        'frames': manifest_frames}
    (folder / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
    return manifest


def self_test():
    def expect(condition, message):
        if not condition:
            raise AssertionError(message)

    def region_of(report, name):
        return report['windows'][name]

    def hint(report, hint_id):
        return next(h for h in report['hints'] if h['id'] == hint_id)

    # Helpers.
    rgb = np.array([[[0.5, 1.0, 2.0], [0.3, 0.0, 100.0]]])
    np.testing.assert_allclose(decode_r11g11b10(encode_r11g11b10(rgb)), rgb, rtol=0.04, atol=0)
    flat = np.ones((9, 9))
    expect(np.allclose(box_filter(flat), 1.0, atol=1e-12, rtol=0), 'box filter keeps a constant')
    flat[4, 4] = np.nan
    expect(np.allclose(box_filter(flat), 1.0, atol=1e-12, rtol=0), 'box filter skips non-finite')
    impulse = np.zeros((9, 9))
    impulse[4, 4] = 1.0
    boxed = box_filter(impulse)
    expect(abs(boxed[2, 2] - 1 / 25) < 1e-12 and abs(boxed[6, 6] - 1 / 25) < 1e-12 and
           boxed[1, 1] == 0 and boxed[7, 4] == 0, 'box filter is 5x5')
    grid = np.random.default_rng(1).uniform(-1, 1, (500, 2))
    plane = np.column_stack([grid, 0.3 * grid[:, 0] - 0.2 * grid[:, 1] + 4])
    expect(plane_rms(plane) < 1e-9, 'tilted plane fits exactly')
    plane[:, 2] += np.random.default_rng(2).normal(0, 0.01, len(plane))
    expect(0.008 < plane_rms(plane) < 0.012, 'plane fit residual tracks the noise')
    expect(plane_rms(plane[:2]) is None, 'plane fit needs three points')
    scene = synthetic_scene(0, 0)
    projection, c2v = synthetic_projection()
    depth, ordinary = scene['depth'].astype(np.float32), scene['status'] == 1
    row = choose_layout(c2v, depth, ordinary, SYN_FOV)
    expect(row['layout'] == 'row-vector' and not row['ambiguous'], 'row-vector layout from the data')
    c2v_column = np.linalg.inv(projection.T)
    expect(choose_layout(c2v_column, depth, ordinary, SYN_FOV)['layout'] == 'column-vector',
           'column-vector layout from the data')
    expect(choose_layout(c2v, depth, ordinary, None)['ambiguous'], 'no fov, no structure: ambiguous')
    structured = choose_layout(c2v, depth, ordinary, None, projection)
    expect(structured['layout'] == 'row-vector' and not structured['ambiguous'] and
           structured['data_ambiguous'], 'structure decides without fov')
    column = choose_layout(c2v_column, depth, ordinary, SYN_FOV, projection.T)
    expect(column['layout'] == 'column-vector' and column['data_layout'] == 'column-vector' and
           column['structure_layout'] == 'column-vector', 'structure and data agree on column-vector')
    near_one = projection.copy()
    near_one[3, 2] = 1.0     # last column and last row both look like w = z
    expect(matrix_layout(projection) == 'row-vector' and matrix_layout(projection.T) == 'column-vector' and
           matrix_layout(near_one) is None and matrix_layout(np.eye(4)) is None and
           matrix_layout(None) is None, 'matrix structure')
    right_handed = projection.copy()
    right_handed[2, 3] = -1.0     # forward is -z: view depth is negative
    handed = choose_layout(np.linalg.inv(right_handed), depth, ordinary, SYN_FOV, right_handed)
    expect(handed['layout'] == 'row-vector' and handed['data_layout'] == 'row-vector' and
           not handed['data_ambiguous'] and handed['scores']['row-vector']['negative_depth_fraction'] == 1.0,
           f'right-handed view space: {handed}')
    expect(normals_look_encoded(np.array([[0.2, 0.5, np.nan], [1.0, 0.0, 0.3]])) and
           not normals_look_encoded(np.array([[-0.1, 0.5, 1.0]])) and
           not normals_look_encoded(np.full((2, 3), np.nan)), 'encoded-normals note ignores NaN')
    expect(pick_control((0, 0, SYN_W, SYN_H), ordinary, None) is None, 'no room for a control')
    changed = np.zeros((SYN_H, SYN_W))
    changed[20, 30] = 1.0     # one changed pixel: windows within CONTROL_MARGIN of it lose to a clear one
    fallback = pick_control((50, 20, 70, 40), np.ones((SYN_H, SYN_W), bool), changed)
    expect(fallback['changed_pixels'] == 0 and
           not (fallback['window'][0] - CONTROL_MARGIN <= 30 < fallback['window'][2] + CONTROL_MARGIN and
                fallback['window'][1] - CONTROL_MARGIN <= 20 < fallback['window'][3] + CONTROL_MARGIN),
           f'control keeps CONTROL_MARGIN away from changed pixels: {fallback}')
    stripes = np.tile(np.array([0.0, 1.0]), (8, 4))     # columns alternate 0, 1
    def floor_of(image, shift):
        return resampling_floor(image, np.full((8, 8, 2), shift, np.float32))[2:6, 2:6]

    expect(np.allclose(floor_of(stripes, [0.5, 0.0]), 0.25) and
           np.allclose(floor_of(stripes, [3.0, 2.0]), 0.0) and
           np.allclose(floor_of(np.ones((8, 8)), [0.3, 0.7]), 0.0, atol=1e-6),
           'resampling floor: a half-pixel shift of 0/1 stripes errs by 0.25 after the round trip, '
           'whole-pixel shifts and flat images by 0')
    expect(animates(2.0, 0.0, None) and animates(2.0, 0.0, 0.9) and not animates(2.0, 0.0, 1.1) and
           not animates(2.0, 1.0, None), 'animates: control ratio and floor ratio')
    rough = make_hints({'ordinary_pixels': 1000, 'plane_fit': {'rms_over_median_view_depth': 0.05},
                        'normals': {'mean_angle_deg': 3.0}}, None)
    flat_hint = next(h for h in rough if h['id'] == 'flat_surface')
    expect(flat_hint['fires'] is False and 'rough geometry (riverbed?)' in flat_hint['statement'],
           'rough geometry hint')
    expect(all(h['fires'] is None for h in rough if h['id'] != 'flat_surface'), 'no control, no hints')

    def moving_stats(guide_floor, colour_floor):
        region = {'ordinary_pixels': 1000, 'mean_luminance': 0.8,
                  'temporal': {'valid_pixels': 900, 'guide_residual': 0.01,
                               'guide_resampling_floor': guide_floor, 'colour_residual': 0.02,
                               'colour_residual_relative': 0.025, 'colour_resampling_floor': colour_floor}}
        quiet = {'ordinary_pixels': 1000, 'mean_luminance': 0.5,
                 'temporal': {'valid_pixels': 900, 'guide_residual': 0.0, 'colour_residual': 0.0,
                              'colour_residual_relative': 0.0}}
        return region, quiet

    def fires(hints, hint_id):
        return next(h for h in hints if h['id'] == hint_id)['fires']

    above = make_hints(*moving_stats(0.004, 0.005))     # residuals 2.5x and 4x their floors
    below = make_hints(*moving_stats(0.006, 0.015))     # 1.7x and 1.3x
    expect(fires(above, 'guides_animate') is True and fires(above, 'colour_animates') is True and
           fires(below, 'guides_animate') is False and fires(below, 'colour_animates') is False,
           'animation hints need HINT_FLOOR_RATIO x the resampling floor')
    unsure = make_hints(*moving_stats(0.004, 0.005), jitter_undecided=True)
    expect(all(fires(unsure, name) is None for name in ('guides_animate', 'colour_animates')) and
           all('jitter not compensated' in h['detail'] for h in unsure if h['id'] in
               ('guides_animate', 'colour_animates')), 'undecided jitter: animation hints not evaluated')

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        write_capture(tmp / 'water')
        out = tmp / 'out'

        # Region plus automatic control, end to end.
        report = run_probe(tmp / 'water', region=SYN_REGION, out_dir=out)
        region, control = region_of(report, 'region'), region_of(report, 'control')
        expect(report['control']['window'] == [12, 12, 44, 44], f'automatic control {report["control"]}')
        expect(control['ordinary_pixels'] == 32 * 32, 'control is all status==1')
        expect(region['transparency_delta']['fraction_above'] > 0.5, 'region delta fraction')
        expect(control['transparency_delta']['fraction_above'] < 0.1, 'control delta fraction')
        rt, ct = region['temporal'], control['temporal']
        expect(rt['pairs_used'] == 7 and ct['pairs_used'] == 7, 'seven frame pairs')
        expect(rt['guide_residual'] > 3 * ct['guide_residual'] and rt['guide_residual'] > MIN_RESIDUAL,
               f'guide residual {rt["guide_residual"]} vs {ct["guide_residual"]}')
        # (edges are rendered at the jitter too, and the control keeps clear of the patch edge)
        expect(ct['guide_residual'] < 1e-6 and ct['colour_residual'] < 1e-6, 'control is static')
        expect(all(rt[f'{key}_residual'] > HINT_FLOOR_RATIO * rt[f'{key}_resampling_floor'] > 0
                   for key in ('guide', 'colour', 'transparency_delta')),
               f'moving water is well above its resampling floor: {rt}')
        expect(rt['colour_residual'] > MIN_RESIDUAL and rt['transparency_delta_residual'] > MIN_RESIDUAL,
               'region colour and delta animate')
        expect(region['plane_fit']['rms_over_median_view_depth'] < 1e-4, 'water is a flat plane')
        expect(control['plane_fit']['rms_over_median_view_depth'] < 1e-4, 'ground is a plane')
        expect(abs(region['plane_fit']['median_view_depth'] - 5.0) < 1e-3, 'view depth is 5')
        expect(0.5 < region['normals']['mean_angle_deg'] < 10 and control['normals']['mean_angle_deg'] < 1e-3,
               'normal spread')
        expect(region['status']['1'] == 32 * 32 and region_of(report, 'whole_frame')['status']['2'] > 0,
               'status histogram')
        for hint_id in ('drawn_after_snapshot', 'flat_surface', 'guides_animate', 'colour_animates'):
            expect(hint(report, hint_id)['fires'] is True, f'hint {hint_id} must fire')
        clip = report['conventions']['clip_to_view']
        expect(clip['layout'] == 'row-vector' and clip['source'] == 'structure of view_to_clip' and
               clip['data_layout'] == 'row-vector', 'clip layout')
        expect(report['conventions']['motion']['alternative_checked'] is False, 'motion convention')
        info = report['conventions']['jitter']
        expect(info['applied'] and info['sign'] == [-1, 0] and info['decided_on'] == 'outside_region',
               f'jitter sign is found from the textured terrain: {info}')
        expect(region['mean_luminance'] > control['mean_luminance'] and
               'relative to mean luminance' in hint(report, 'colour_animates')['detail'], 'luminance detail')
        expect(len(report['frames']) == 8 and report['frames'][1]['jitter'] == [0.25, -0.125] and
               report['frames'][1]['pre_exposure'] == 1, 'frames')
        text = (out / 'report.txt').read_text(encoding='utf-8')
        expect(text.count('HEURISTIC') >= 4 and 'frame 0' in text, 'report.txt hints')
        raw = (out / 'report.json').read_text(encoding='utf-8')
        expect('NaN' not in raw and 'Infinity' not in raw, 'report.json is strict JSON')
        expect(json.loads(raw)['hints'] == report['hints'], 'report.json matches')
        if import_pillow() is not None:
            from PIL import Image
            panel = np.asarray(Image.open(out / 'panels_frame0.png'))
            expect(panel.shape[1] == PANEL_COLUMNS * TILE_WIDTH, 'panel width')
            expect((panel == RED).all(-1).any() and (panel == GREEN).all(-1).any(), 'region boxes drawn')
        else:
            expect(len(list(out.glob('tile_*.npy'))) == 13, 'tiles saved as .npy')

        # Explicit control, window clipped at the image border.
        given = run_probe(tmp / 'water', region=(80, 40, 120, 90), control=(8, 12, 40, 44), out_dir=out)
        expect(given['region'] == [80, 40, 96, 64] and given['control']['window'] == [8, 12, 40, 44],
               'explicit control and clipped region')
        expect(any('clipped' in note for note in given['notes']), 'clipping is reported')

        # No region: whole-frame summary and pictures only.
        whole = run_probe(tmp / 'water', out_dir=out)
        expect(list(whole['windows']) == ['whole_frame'] and whole['hints'] == [], 'whole-frame mode')
        expect('--region' in (out / 'report.txt').read_text(encoding='utf-8'), 'says a region is needed')
        expect(region_of(whole, 'whole_frame')['plane_fit']['rms_over_median_view_depth'] > 0.01,
               'two planes are not one plane')

        # Without Pillow: tiles as .npy, a warning, no failure.
        os.environ[NO_PILLOW_ENV] = '1'
        try:
            warning = io.StringIO()
            with contextlib.redirect_stderr(warning):
                run_probe(tmp / 'water', region=SYN_REGION, out_dir=tmp / 'nopil')
        finally:
            del os.environ[NO_PILLOW_ENV]
        expect('Pillow' in warning.getvalue(), 'missing Pillow is warned about')
        expect(len(list((tmp / 'nopil').glob('tile_*.npy'))) == 13 and
               not (tmp / 'nopil' / 'panels_frame0.png').exists(), 'npy tiles instead of png')

        # Whole image as region: no room for a control, no comparison hints.
        full = run_probe(tmp / 'water', region=(0, 0, SYN_W, SYN_H), out_dir=out)
        expect(full['control'] is None and hint(full, 'guides_animate')['fires'] is None, 'no control')

        # A frame with the reset flag leaves its pair out.
        def copy_capture(source, name, edit):
            """Copy a capture folder with its manifest edited by edit(manifest)."""
            manifest = json.loads((source / 'manifest.json').read_text(encoding='utf-8'))
            edit(manifest)
            (tmp / name).mkdir()
            for path in source.glob('frame*.bin'):
                (tmp / name / path.name).write_bytes(path.read_bytes())
            (tmp / name / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
            return tmp / name

        reset = copy_capture(tmp / 'water', 'reset', lambda m: m['frames'][3].update(reset=True))
        resets = region_of(run_probe(reset, region=SYN_REGION, out_dir=out), 'region')['temporal']
        expect(resets['pairs_used'] == 6 and resets['pairs_skipped_reset'] == [3], 'reset pair is skipped')

        # No colour_before_transparency, then a single frame.
        write_capture(tmp / 'nobefore', before=False)
        nob = run_probe(tmp / 'nobefore', region=SYN_REGION, out_dir=out)
        expect('transparency_delta' not in region_of(nob, 'region'), 'no delta statistics')
        expect(hint(nob, 'drawn_after_snapshot')['fires'] is None and
               hint(nob, 'guides_animate')['fires'] is True, 'hints without before')
        write_capture(tmp / 'single', frames=1)
        single = run_probe(tmp / 'single', region=SYN_REGION, out_dir=out)
        expect(region_of(single, 'region')['temporal'] is None, 'one frame has no temporal statistics')
        expect(hint(single, 'guides_animate')['fires'] is None, 'one frame, no animation hint')
        expect(hint(single, 'drawn_after_snapshot')['fires'] is True, 'one frame, static hints remain')

        # Too few valid pixels for a hint.
        tiny = run_probe(tmp / 'water', region=(50, 22, 55, 27), out_dir=out)
        expect(all(h['fires'] is None for h in tiny['hints']) and
               sum('too few valid pixels' in note for note in tiny['notes']) == 4, 'hint sample floor')

        # Sub-pixel jitter alone (static water, alternating 0.5 px, either sign convention) is
        # compensated and does not read as animation; without compensation it would.
        for sign in (1, -1):
            write_capture(tmp / f'jitter{sign}', scroll=0, jitter=0.5, sign=sign)
            args = dict(region=(52, 24, 76, 48), control=(12, 14, 36, 38), out_dir=out)
            still = run_probe(tmp / f'jitter{sign}', **args)
            info = still['conventions']['jitter']
            residual = info['residuals']['outside_region']
            expect(info['applied'] and info['sign'] == [-sign, 0] and info['decided_on'] == 'outside_region',
                   f'jitter sign for renderer sign {sign}: {info}')
            right, wrong = f'{-sign:+d},+0', f'{sign:+d},+0'
            expect(residual[wrong] > residual['none'] > residual[right], f'jitter residuals {residual}')
            temporal = region_of(still, 'region')['temporal']
            expect(all(hint(still, name)['fires'] is False for name in ('guides_animate', 'colour_animates')),
                   f'static water with jitter must not animate: {temporal}')
        flagged = copy_capture(tmp / 'jitter1', 'flagged', lambda m: [f['camera'].update(
            motion_vectors_jittered=1) for f in m['frames']])
        raw_jitter = run_probe(flagged, **args)
        expect(not raw_jitter['conventions']['jitter']['applied'] and
               'residuals' not in raw_jitter['conventions']['jitter'] and
               all(hint(raw_jitter, name)['fires'] is True for name in ('guides_animate', 'colour_animates')),
               'jitter in the pixels but not compensated would read as animation')

        # Noisy colour (sigma 0.15 on a 0.5 base, new every frame) and a y jitter that changes with the
        # opposite renderer sign to x: the sign pair is decided from the guides, static water does not
        # animate, scrolling water still does. The region is a sub-rectangle of the water, so a 4 px
        # rim of scrolling water is part of "outside the region" and the decision has to survive it.
        args = dict(region=(52, 24, 76, 48), control=(12, 20, 36, 44), out_dir=out)
        for label, options, signs in (('noisy colour', dict(noise=0.15), [-1, 0]),
                                      ('flipped y jitter', dict(jitter_y=0.4, flip_y=True), [-1, 1]),
                                      ('noisy and flipped', dict(noise=0.15, jitter_y=0.4, flip_y=True),
                                       [-1, 1])):
            runs = {}
            for motion, scroll in (('static', 0), ('scrolling', 1.0)):
                folder = tmp / f'{label}-{motion}'.replace(' ', '_')
                write_capture(folder, scroll=scroll, jitter=0.5, **options)
                runs[motion] = run_probe(folder, **args)
                info = runs[motion]['conventions']['jitter']
                expect(info['applied'] and info['sign'] == signs and info['decided_on'] == 'outside_region',
                       f'{label}, {motion}: jitter signs {info}')
            temporal = region_of(runs['static'], 'region')['temporal']
            expect(all(hint(runs['static'], name)['fires'] is False
                       for name in ('guides_animate', 'colour_animates')),
                   f'{label}: static water must not animate: {temporal}')
            expect(all(hint(runs['scrolling'], name)['fires'] is True
                       for name in ('guides_animate', 'colour_animates')),
                   f'{label}: scrolling water must animate: '
                   f'{region_of(runs["scrolling"], "region")["temporal"]}')

        # Jitter that changes while the only texture is inside the region (so nothing outside can
        # decide the sign): the two animation hints are not evaluated, and say why.
        lost = run_probe(tmp / 'jitter1', region=(48, 12, 96, 64), control=(12, 14, 36, 38), out_dir=out)
        info = lost['conventions']['jitter']
        expect(not info['applied'] and 'residuals' in info and 'jitter not compensated' in info['reason'] and
               any('jitter not compensated' in note for note in lost['notes']), f'undecided jitter: {info}')
        for name in ('guides_animate', 'colour_animates'):
            undecided = hint(lost, name)
            expect(undecided['fires'] is None and 'jitter not compensated' in undecided['detail'] and
                   undecided['statement'].startswith('not evaluated'), f'{name} needs a decided jitter')

        # Sharp texture (8x12 px wavelength): bilinear resampling alone leaves a residual above
        # MIN_RESIDUAL. The static pattern must not fire guides_animate (without the floor rule it
        # would), the scrolling one must, and the round-trip floor tracks the static residual.
        sharp = {}
        for motion, scroll in (('static', 0), ('scrolling', 1.0)):
            write_capture(tmp / f'sharp-{motion}', scroll=scroll, jitter=0.5, period=(8.0, 12.0))
            sharp[motion] = run_probe(tmp / f'sharp-{motion}', region=SYN_REGION, out_dir=out)
        still = region_of(sharp['static'], 'region')['temporal']
        quiet = region_of(sharp['static'], 'control')['temporal']
        expect(sharp['static']['conventions']['jitter']['applied'] and
               animates(still['guide_residual'], quiet['guide_residual']) and
               hint(sharp['static'], 'guides_animate')['fires'] is False and
               0.7 < still['guide_residual'] / still['guide_resampling_floor'] < 1.5,
               f'sharp static pattern is resampling, not animation: {still} control {quiet}')
        moving = region_of(sharp['scrolling'], 'region')['temporal']
        expect(hint(sharp['scrolling'], 'guides_animate')['fires'] is True and
               hint(sharp['scrolling'], 'colour_animates')['fires'] is True and
               moving['guide_residual'] > 5 * moving['guide_resampling_floor'],
               f'sharp scrolling pattern animates: {moving}')

        # Exposure changing between frames must not read as animation.
        write_capture(tmp / 'exposure', scroll=0, jitter=0, exposure=2.0)
        exposed = run_probe(tmp / 'exposure', region=SYN_REGION, out_dir=out)
        expect([f['pre_exposure'] for f in exposed['frames'][:3]] == [1, 2, 1] and
               region_of(exposed, 'region')['temporal']['colour_residual'] < MIN_RESIDUAL and
               hint(exposed, 'colour_animates')['fires'] is False, 'colour residual divides by pre_exposure')

        # Motion vectors stored normalised while the camera moves: the other scaling must win.
        write_capture(tmp / 'drift', drift=2)
        drift = run_probe(tmp / 'drift', region=(20, 10, 60, 50), out_dir=out)
        motion = drift['conventions']['motion']
        expect(motion['alternative_checked'] and 'width, height' in motion['convention'],
               f'normalised motion vectors must be detected: {motion}')
        expect(motion['residual_alternative'] < 0.5 * motion['residual_pixel_convention'], 'residuals')
        expect(motion['valid_pixels_alternative'] >= 0.5 * motion['valid_pixels_pixel_convention'],
               'the alternative keeps its valid pixels')
        expect(region_of(drift, 'region')['temporal']['guide_residual'] < 5e-3, 'drift reprojects cleanly')

        # An alternative scaling that keeps fewer than half the valid pixels is rejected even if its
        # residual is lower (a 50 px drift leaves 46 of 96 columns).
        write_capture(tmp / 'bigdrift', drift=50)
        big = run_probe(tmp / 'bigdrift', region=(20, 10, 60, 50), out_dir=out, mv_tiny_pixels=10.0)
        motion = big['conventions']['motion']
        expect(motion['alternative_checked'] and 'width, height' not in motion['convention'] and
               motion['residual_alternative'] < motion['residual_pixel_convention'] and
               motion['valid_pixels_alternative'] < 0.5 * motion['valid_pixels_pixel_convention'] and
               any('fewer than half the valid pixels' in note for note in big['notes']),
               f'alternative with too few valid pixels is rejected: {motion}')
    print('self-test passed')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('folder', nargs='?', type=Path, help='capture folder (contains manifest.json)')
    parser.add_argument('--region', nargs=4, type=int, metavar=('X0', 'Y0', 'X1', 'Y1'),
                        help='water window in render pixels, half-open')
    parser.add_argument('--control', nargs=4, type=int, metavar=('X0', 'Y0', 'X1', 'Y1'),
                        help='control window; default: picked automatically')
    parser.add_argument('--out', type=Path, help='output folder (default: FOLDER/water-probe)')
    parser.add_argument('--self-test', action='store_true', help='check the tool on a synthetic capture')
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.folder is None:
        parser.error('folder is required unless --self-test is given')
    if args.control and not args.region:
        parser.error('--control needs --region')
    try:
        run_probe(args.folder, args.region, args.control, args.out, echo=True)
    except (ValueError, FileNotFoundError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
