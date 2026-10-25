#!/usr/bin/env python3
"""Look for sky-edge inconsistencies in an mw4-rr Ray Reconstruction input capture.

Tests these hypotheses on the captured frames (verdicts at the top of report.md):
  H0 RR's own output (rr_output, output size) flickers at sky edges, more than at hill edges
  H1 sky/non-sky class flicker          H2 inputs disagree with depth at the edge
  H3 depth bleeds across the edge       H4 motion vectors mismatch at the edge
  H5 input colour flicker               H6 bad values (NaN, Inf, negative, huge)
  H7 jitter does not match the image    H8 stochastic coverage (per-frame dither)
  H9 HDR scale of the sky against the geometry next to it
  H10 coarse shading (VRS): edges snapped to a 2 or 4 px grid, or pixel pairs shaded once
CLI:  python3 analyze_sky_edges.py CAPTURE_FOLDER --out OUT_DIR [--radius 2] [--crop x,y,w,h]
API:  analyze(manifest, frames, out_dir, radius=2, crop=None) -> metrics dict, where
      manifest, frames are what read_capture.load(folder) returns.
Every input is at render size except rr_output (output size, optional: older captures lack it).
Writes OUT_DIR/report.md, OUT_DIR/metrics.json and a few PNG maps.
"""
import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ---- region constants ----------------------------------------------------------------------
HILL_DEPTH_RATIO = 1.5      # neighbour depth ratio that makes a strong (hill) depth edge
HILL_SKY_MARGIN = 2         # hill-edge band stays at least radius + this many px from any sky
REF_HALF = 3                # local reference window for the votes: 7x7
MIN_REFS = 3                # pixels of each class needed in that window
SAME_SURFACE_RATIO = 1.25   # a non-sky pixel's own-surface neighbours: depth within this factor
DEPTH_REF_EXTRA = 3         # H3 interior-reference window half size = radius + this
CHUNK = 1 << 15             # band pixels per chunk (bounds the memory of window gathers)
LOG_EPS = 1e-6              # luminance floor before taking logs

# ---- H0: RR output flicker (mean |delta log luminance| per pixel between consecutive frames) -
H0_VISIBLE = 0.05           # per-frame mean |delta log luminance| (~5 %) that counts as visible flicker
H0_MIN_SHARE = 0.02         # visibly flickering share of the sky-edge band needed by the share test
H0_SUPPORT_RATIO, H0_AGAINST_RATIO = 2.0, 1.25   # sky-edge / hill-edge, of the p90 or of that share
H0_RATIO_FLOOR, H0_SHARE_FLOOR = 0.01, 0.005     # ratio denominators floored here (a stable control)
H0_MIN_PAIRS = 2            # consecutive frame pairs with rr_output in both and no RR reset
H0_SAME_RANGE = (0.8, 1.25)  # output / input p90 in this range reads "about as unstable as its input"
H0_RR_LIFT = 2.0            # output sky/hill p90 ratio this many times the input's points at RR itself
H0_REGIONS = {'sky_edge_band': 1, 'hill_edge_band': 2, 'interior': 3, 'sky': 4}  # label map values
H0_MAP_MAX = 0.5            # heat map full scale
H0_ROWS = 256               # rows per chunk when tonemapping output-size images
H0_CROP_TILE = 384          # crop tiles are zoomed (nearest) to about this size

# ---- H1 ------------------------------------------------------------------------------------
FLICKER_MIN_FLIPS = 2      # sky/non-sky changes over the frames that make a flicker pixel
THIN_MAX_RUN = 2            # non-sky run width (row or column) that counts as thin

# ---- H2 / H4 votes -------------------------------------------------------------------------
NORMAL_ZERO_LENGTH = 0.05   # a normal shorter than this votes sky
NORMAL_MATCH_TOL = 0.01     # max component difference to the frame's sky normal that votes sky
ALBEDO_MATCH_TOL = 1e-3     # max component difference to the frame's sky albedo that votes sky
COLOR_MIN_SEPARATION = 0.1  # |C_solid - C_sky| needed, relative to the brighter reference
MOTION_MIN_SEPARATION = 0.25  # px between the local sky and non-sky median motion
BLEND_RANGE = (0.2, 0.8)    # colour alpha in this range counts as blended
UNINFORMATIVE_FALSE_SKY = 0.5  # an absolute vote that calls this much of the interior sky is ignored
ABSOLUTE_VOTES = ('status', 'normal', 'diffuse_albedo', 'specular_albedo')
LOCAL_VOTES = ('color', 'motion')

# ---- H5 / H6 / H7 --------------------------------------------------------------------------
CONTRAST_MIN = 0.1          # log-luminance contrast below which std/contrast is not computed
HUGE_VALUE = 65000.0
NONNEGATIVE_CHANNELS = {'color': None, 'color_before_transparency': None, 'depth': None,
                        'diffuse_albedo': None, 'specular_albedo': None, 'sss_guide': None,
                        'specular_hit_distance': None, 'normal_roughness': [3]}  # None = all
HUGE_EXEMPT = {'specular_hit_distance'}   # huge values reported but not counted in the H6 verdict
OPTIONAL_INPUTS = ('sss_guide', 'color_before_transparency', 'responsivity_mask',
                   'specular_hit_distance')
JITTER_CONVENTIONS = {'shift = -jitter delta': (-1, -1), 'shift = +jitter delta': (1, 1),
                      'shift = (-x, +y) jitter delta': (-1, 1),
                      'shift = (+x, -y) jitter delta': (1, -1)}

# ---- camera stillness (H5 and H7 verdicts are skipped when the camera moved) ---------------
STILL_TRANSLATION = 0.01    # world units between consecutive frames
STILL_ROTATION_PX = 0.05    # rotation between consecutive frames, as pixels at the image centre

# ---- verdict thresholds --------------------------------------------------------------------
MIN_PIXELS = 30             # fewer measured pixels than this gives "inconclusive"
H1_SUPPORT_FRACTION, H1_AGAINST_FRACTION = 0.01, 0.001   # flicker pixels / sky-edge band
H2_SUPPORT_RATE, H2_AGAINST_RATE = 0.05, 0.02     # disagreement, sky band minus hill control
H2_SUPPORT_BLEND, H2_AGAINST_BLEND = 0.10, 0.04   # blended boundary pixels, sky minus hill
H3_LOW_RATIO, H3_MID_RATIO = 0.5, 0.9             # edge depth / interior depth
H3_SUPPORT_LOW, H3_SUPPORT_MID = 0.05, 0.25       # fraction of edge pixels below those ratios
H3_AGAINST_LOW, H3_AGAINST_MID = 0.02, 0.10
H4_SUPPORT_RATE, H4_AGAINST_RATE = 0.05, 0.02     # motion disagreement, sky minus hill control
H5_SUPPORT_EXCESS, H5_AGAINST_EXCESS = 0.10, 0.05  # p90 excess std/contrast, sky minus hill
H7_SUPPORT_RESIDUAL, H7_AGAINST_RESIDUAL = 0.15, 0.10  # px, best sign convention
H7_MIN_WEIGHT_PIXELS = 500  # static interior pixels needed for the masked shift fit
H7_MAX_CONDITION = 100.0    # gradient-matrix condition above this: shift not constrained in 2-D
LK_ITERATIONS = 30
STATIC_MOTION = 0.1         # px; pixels moving more than this are left out of the jitter fit
H8_SUPPORT_SHARE, H8_AGAINST_SHARE = 0.5, 0.25  # share of flips not growing with jitter delta
H8_TILE = 32                # px; the jitter fit is also run per tile so a dithered patch is not diluted
H8_TILE_MIN_FLIPS = 50      # static flips (summed over all frame pairs) a tile needs for its own fit
H8_SUPPORT_TILE_FRACTION, H8_AGAINST_TILE_FRACTION = 0.25, 0.10  # flips in dither-like tiles
H9_SKY_RATIO = 20.0         # sky median / sky-edge non-sky median for a "supports (lead)"
H9_BRIGHT_RANK = 0.99       # sky-edge sky pixels brighter than this share of non-sky pixels
# H10: coarse shading (VRS). A b-px block (along x, or along y) is "mixed" when it holds sky and
# non-sky. Edges a pixel shader decides (alpha-tested leaves: discard is per coarse pixel) snap to
# the coarse grid, so one grid phase has far fewer mixed blocks than the others; rasterised edges
# (terrain, wires) do not snap. Pairs of same-surface non-sky pixels with identical values show
# coarse shading of the colour or the G-buffer guides.
H10_BLOCKS = (2, 4)
H10_MIN_MIXED = 200         # mixed blocks (all phases, all frames) per axis a global ratio needs
H10_SUPPORT_RATIO, H10_AGAINST_RATIO = 0.75, 0.85  # best phase / median of the other phases
H10_TILE = 32               # px; tiles find snapped foliage among unsnapped terrain and wires
H10_TILE_MIN_MIXED = 40     # mixed 2-px blocks per axis a tile needs
H10_TILE_SNAP = 0.5         # tile ratio at or below this, in x and in y, at one phase pair
H10_SUPPORT_TILE_SHARE, H10_AGAINST_TILE_SHARE = 0.25, 0.10   # of the mixed 2-px blocks
CONTRAST_SERIES_LIMIT, CONTRAST_MAX_EXPONENT = 0.01, 18.0   # src/rr_contrast_test.h
H10_TILE_CONTROL = 3.0      # best phase pair's tile share over the best other pair (chance)
H10_EQUAL_EXCESS, H10_EQUAL_RATIO = 0.10, 2.0   # identical pairs, best minus other phase / ratio
H10_EQUAL_AGAINST = 0.02
H10_MIN_PAIRS = 500         # same-surface pairs per phase an equality rate needs
H10_CHANNELS = ('color', 'diffuse_albedo', 'normal_roughness', 'specular_albedo')

# Region flags, one uint8 map per frame.
VALID, SKY, SKY_BAND, HILL_BAND, BOUNDARY, HILL_EDGE = 1, 2, 4, 8, 16, 32


# ---- small array helpers -------------------------------------------------------------------

def dilate(mask, radius):
    """Binary dilation with a (2 radius + 1)^2 square."""
    rows = mask.copy()
    for d in range(1, radius + 1):
        rows[:, d:] |= mask[:, :-d]
        rows[:, :-d] |= mask[:, d:]
    out = rows.copy()
    for d in range(1, radius + 1):
        out[d:] |= rows[:-d]
        out[:-d] |= rows[d:]
    return out


def run_lengths(mask, axis):
    """Length of the run of True pixels along axis (1 = rows) holding each pixel; 0 where False."""
    m = mask if axis == 1 else mask.T
    starts = m.copy()
    starts[:, 1:] &= ~m[:, :-1]
    ids = np.cumsum(starts.ravel()).reshape(m.shape)   # runs never span rows: col 0 starts one
    lengths = np.bincount(ids[m], minlength=int(ids.max()) + 1)
    out = np.where(m, lengths[ids], 0)
    return out if axis == 1 else out.T


def window_index(ys, xs, half, shape):
    """Coordinates (N, K) of the (2 half + 1)^2 - 1 neighbours of each pixel, and in-bounds flags."""
    d = np.arange(-half, half + 1)
    dy, dx = np.meshgrid(d, d, indexing='ij')
    keep = (dy != 0) | (dx != 0)
    yy = ys[:, None] + dy[keep]
    xx = xs[:, None] + dx[keep]
    inside = (yy >= 0) & (yy < shape[0]) & (xx >= 0) & (xx < shape[1])
    return np.clip(yy, 0, shape[0] - 1), np.clip(xx, 0, shape[1] - 1), inside


def masked_median(values, mask):
    """Componentwise median of values (N, K[, C]) over K where mask (N, K); NaN rows when empty."""
    flat = values.ndim == 2
    if flat:
        values = values[..., None]
    mask = mask & np.isfinite(values).all(axis=-1)
    v = np.ascontiguousarray(np.where(mask[..., None], values, np.inf).transpose(0, 2, 1))
    v.sort(axis=-1)
    count = mask.sum(axis=1)
    lo = np.maximum(count - 1, 0) // 2
    hi = np.minimum(count // 2, v.shape[-1] - 1)

    def pick(index):
        index = np.broadcast_to(index[:, None, None], v.shape[:2] + (1,))
        return np.take_along_axis(v, index, axis=-1)[..., 0]

    median = 0.5 * (pick(lo) + pick(hi))
    median[count == 0] = np.nan
    return median[..., 0] if flat else median


def luminance(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def log_luminance(rgb):
    with np.errstate(invalid='ignore', divide='ignore'):
        return np.log(np.maximum(luminance(rgb), LOG_EPS))


def finite_stats(values, percentiles=(50, 90)):
    values = values[np.isfinite(values)]
    if values.size == 0:
        return [None] * len(percentiles)
    return [float(v) for v in np.percentile(values, percentiles)]


def rate(hits, total):
    return hits / total if total else None


# ---- regions -------------------------------------------------------------------------------

def depth_discontinuity(depth, solid):
    """Non-sky pixels with a non-sky 4-neighbour whose depth differs by more than HILL_DEPTH_RATIO."""
    d = np.where(solid & np.isfinite(depth) & (depth > 0), depth, np.nan)
    edge = np.zeros(depth.shape, bool)
    with np.errstate(invalid='ignore', divide='ignore'):
        across = np.maximum(d[:, :-1] / d[:, 1:], d[:, 1:] / d[:, :-1]) > HILL_DEPTH_RATIO
        down = np.maximum(d[:-1] / d[1:], d[1:] / d[:-1]) > HILL_DEPTH_RATIO
    edge[:, :-1] |= across
    edge[:, 1:] |= across
    edge[:-1] |= down
    edge[1:] |= down
    return edge


def compute_regions(depth, status, radius):
    """uint8 flag map: VALID, SKY (depth == 0), SKY_BAND (within radius of the sky/non-sky
    boundary), HILL_BAND (within radius of a strong non-sky depth edge, radius + HILL_SKY_MARGIN
    away from sky), BOUNDARY (8-neighbour of the other sky class), HILL_EDGE (the depth edge)."""
    valid = np.ones(depth.shape, bool) if status is None else status != 0
    sky = valid & (depth == 0)
    solid = valid & ~sky
    sky_band = valid & dilate(sky, radius) & dilate(solid, radius)
    boundary = (sky & dilate(solid, 1)) | (solid & dilate(sky, 1))
    hill_edge = depth_discontinuity(depth, solid)
    hill_band = dilate(hill_edge, radius) & solid & ~dilate(sky, radius + HILL_SKY_MARGIN)
    flags = (valid * VALID | sky * SKY | sky_band * SKY_BAND | hill_band * HILL_BAND |
             boundary * BOUNDARY | (hill_edge & hill_band) * HILL_EDGE)
    return flags.astype(np.uint8)


def has(regions, flag):
    return (regions & flag) > 0


def interior_mask(reg):
    return has(reg, VALID) & ~has(reg, SKY) & ~has(reg, SKY_BAND) & ~has(reg, HILL_BAND)


def region_summary(regions):
    """Pixel counts per frame and over all frames, plus the union masks used by H1 and H5."""
    per_frame = []
    sky_band = np.zeros(regions[0].shape, bool)
    hill_band = np.zeros_like(sky_band)
    interior_all = np.ones_like(sky_band)
    sky_all = np.ones_like(sky_band)
    for reg in regions:
        interior = interior_mask(reg)
        per_frame.append({'sky': int(has(reg, SKY).sum()),
                          'sky_edge_band': int(has(reg, SKY_BAND).sum()),
                          'sky_edge_band_non_sky': int((has(reg, SKY_BAND) & ~has(reg, SKY)).sum()),
                          'hill_edge_band': int(has(reg, HILL_BAND).sum()),
                          'interior': int(interior.sum()),
                          'invalid_padding': int((~has(reg, VALID)).sum())})
        sky_band |= has(reg, SKY_BAND)
        hill_band |= has(reg, HILL_BAND)
        interior_all &= interior
        sky_all &= has(reg, SKY)
    unions = {'sky_band': sky_band, 'hill_band': hill_band & ~sky_band,
              'interior': interior_all, 'sky': sky_all & ~sky_band}
    keys = per_frame[0].keys()
    return ({'per_frame': per_frame,
             'mean': {k: float(np.mean([f[k] for f in per_frame])) for k in keys},
             'union': {'sky_edge_band': int(unions['sky_band'].sum()),
                       'hill_edge_band': int(unions['hill_band'].sum()),
                       'interior_all_frames': int(interior_all.sum()),
                       'sky_all_frames_outside_band': int(unions['sky'].sum())}},
            unions)


def sky_status_agreement(frames, regions):
    """depth == 0 versus status == 2, per frame, in total and inside the sky-edge band."""
    rows = []
    for frame, reg in zip(frames, regions):
        if 'status' not in frame:
            continue
        status = frame['status']
        depth_sky = has(reg, SKY)
        status_sky = has(reg, VALID) & (status == 2)
        band = has(reg, SKY_BAND)
        a = depth_sky & ~status_sky
        b = status_sky & ~depth_sky
        rows.append({'depth_sky_status_not': int(a.sum()), 'status_sky_depth_not': int(b.sum()),
                     'in_band_depth_sky_status_not': int((a & band).sum()),
                     'in_band_status_sky_depth_not': int((b & band).sum()),
                     'band_status_histogram': [int(((status == v) & band).sum()) for v in range(5)],
                     'status_out_of_range': int((status > 4).sum())})
    return rows


# ---- H2 / H4 / H9: per-pixel "sky votes" from each input -----------------------------------

def sky_reference(values, mask):
    """Componentwise median of values over mask (finite rows only), or None."""
    picked = values[mask]
    picked = picked[np.isfinite(picked).all(axis=-1)]
    if len(picked) == 0:
        return None
    step = max(1, len(picked) // 200000)
    return np.median(picked[::step], axis=0)


def absolute_sky_votes(frame, sky_reference_mask):
    """Full-frame boolean "looks like sky" maps from inputs that hold a fixed sky value."""
    votes, refs = {}, {}
    if 'status' in frame:
        votes['status'] = frame['status'] == 2
    if 'normal_roughness' in frame:
        normal = frame['normal_roughness'][..., :3]
        ref = sky_reference(normal, sky_reference_mask)
        if ref is not None:
            refs['normal'] = ref
            with np.errstate(invalid='ignore'):
                votes['normal'] = ((np.linalg.norm(normal, axis=-1) < NORMAL_ZERO_LENGTH) |
                                   (np.abs(normal - ref).max(axis=-1) < NORMAL_MATCH_TOL))
    for name in ('diffuse_albedo', 'specular_albedo'):
        if name in frame:
            albedo = frame[name][..., :3]
            ref = sky_reference(albedo, sky_reference_mask)
            if ref is not None:
                refs[name] = ref
                with np.errstate(invalid='ignore'):
                    votes[name] = np.abs(albedo - ref).max(axis=-1) < ALBEDO_MATCH_TOL
    return votes, {k: [float(x) for x in v] for k, v in refs.items()}


def coverage_alpha(color, ref0, ref1):
    """Coverage of class 1 in color: projection of color - ref0 onto ref1 - ref0, clipped 0..1."""
    axis = ref1 - ref0
    length2 = (axis * axis).sum(axis=-1)
    scale = np.maximum(np.linalg.norm(ref0, axis=-1), np.linalg.norm(ref1, axis=-1))
    with np.errstate(invalid='ignore', divide='ignore'):
        ok = (np.isfinite(length2) & (length2 > 0) &
              (np.sqrt(length2) > COLOR_MIN_SEPARATION * scale))
        alpha = ((color - ref0) * axis).sum(axis=-1) / np.where(ok, length2, 1)
    ok &= np.isfinite(alpha)
    return np.clip(alpha, 0, 1), ok


def local_measurements(color, motion, depth, ys, xs, kind):
    """Window-based measurements at band pixels (ys, xs), see _local_chunk."""
    parts = [_local_chunk(color, motion, depth, ys[s:s + CHUNK], xs[s:s + CHUNK], kind)
             for s in range(0, len(ys), CHUNK)]
    if not parts:
        parts = [_local_chunk(color, motion, depth, ys, xs, kind)]
    return {key: np.concatenate([p[key] for p in parts]) for key in parts[0]}


def _local_chunk(color, motion, depth, y, x, kind):
    """kind 'sky': class 0 = sky (depth == 0), class 1 = non-sky; a non-sky pixel's class-1
    references come from its own surface (depth within SAME_SURFACE_RATIO), so a static wire next
    to a swaying branch is compared with the wire. kind 'hill': class 0 = far, class 1 = near side
    of the split at the geometric mean of the window's min and max depth (defined only where
    max/min > HILL_DEPTH_RATIO)."""
    yy, xx, inside = window_index(y, x, REF_HALF, depth.shape)
    nd = depth[yy, xx]
    own = depth[y, x]
    usable = inside & np.isfinite(nd)
    if kind == 'sky':
        class0 = usable & (nd == 0)
        any_solid = usable & (nd != 0)
        own0 = own == 0
        same = (nd >= own[:, None] / SAME_SURFACE_RATIO) & (nd <= own[:, None] * SAME_SURFACE_RATIO)
        class1 = any_solid & (own0[:, None] | same)
        class_ok = np.ones(len(y), bool)
    else:
        usable &= nd > 0
        with np.errstate(invalid='ignore'):
            lo = np.minimum(np.where(usable, nd, np.inf).min(axis=1), own)
            hi = np.maximum(np.where(usable, nd, 0).max(axis=1), own)
            class_ok = (lo > 0) & (hi > HILL_DEPTH_RATIO * lo)
            split = np.sqrt(lo * hi)
        class0 = usable & (nd < split[:, None])
        class1 = usable & (nd >= split[:, None])
        any_solid = usable
        own0 = own < split
    refs_ok = class_ok & (class0.sum(axis=1) >= MIN_REFS) & (class1.sum(axis=1) >= MIN_REFS)

    neighbours = color[yy, xx]
    ref0 = masked_median(neighbours, class0)
    ref1 = masked_median(neighbours, class1)
    alpha, alpha_ok = coverage_alpha(color[y, x], ref0, ref1)
    out = {'own0': own0, 'class_ok': class_ok, 'alpha': alpha, 'alpha_ok': alpha_ok & refs_ok,
           'contrast': np.abs(log_luminance(ref0) - log_luminance(ref1)),
           'luminance': luminance(color[y, x]),
           'solid_luminance': masked_median(luminance(neighbours), any_solid)}  # for H9
    if motion is not None:
        neighbours = motion[yy, xx]
        m0, m1 = masked_median(neighbours, class0), masked_median(neighbours, class1)
        m = motion[y, x]
        with np.errstate(invalid='ignore'):
            separation = np.linalg.norm(m0 - m1, axis=-1)
            out['motion_vote0'] = np.linalg.norm(m - m0, axis=-1) < np.linalg.norm(m - m1, axis=-1)
            out['motion_ok'] = (refs_ok & (separation > MOTION_MIN_SEPARATION) &
                                np.isfinite(m).all(axis=-1))
    return out


def new_counts():
    return {'defined': 0, 'disagree': 0, 'class0_vote1': 0, 'class1_vote0': 0, 'at_status4': 0}


def vote_analysis(frames, regions):
    """Per-input sky votes compared with depth in the sky-edge band, and the hill-edge control.

    Sky band: truth = depth == 0. Inputs with a fixed sky value (status, normal, albedos) vote by
    matching it; colour and motion vote by being closer to the local sky or non-sky reference.
    Hill band: absolute votes give the false-sky rate (depth says non-sky); colour and motion
    vote far vs near against the local depth split, the same test at an ordinary depth edge."""
    shape = regions[0].shape
    bands = {'sky': {}, 'hill': {}}
    false_sky = {}
    shift_counts, maps = {}, {}
    hist_edges = np.linspace(0, 1, 11)
    alpha_hist = {k: np.zeros(10, np.int64) for k in ('sky_band_sky', 'sky_band_non_sky', 'hill_band')}
    blend = {'sky_boundary': [0, 0], 'hill_edge': [0, 0]}
    status4 = []
    sky_refs = []
    hdr = {'ratio': [], 'over10': 0, 'over100': 0, 'defined': 0}
    acc = {'contrast_sum': np.zeros(shape, np.float32), 'contrast_n': np.zeros(shape, np.uint8),
           'class0_n': np.zeros(shape, np.uint8), 'class_n': np.zeros(shape, np.uint8)}

    for frame, reg in zip(frames, regions):
        sky = has(reg, SKY)
        interior = interior_mask(reg)
        absolute, refs = absolute_sky_votes(frame, sky & ~has(reg, SKY_BAND))
        sky_refs.append(refs)
        for name, vote in absolute.items():
            hits = false_sky.setdefault(name, [0, 0])
            hits[0] += int((vote & interior).sum())
            hits[1] += int(interior.sum())
        is_status4 = frame['status'] == 4 if 'status' in frame else np.zeros(shape, bool)
        status4.append({'total': int(is_status4.sum()),
                        'sky_edge_band': int((is_status4 & has(reg, SKY_BAND)).sum()),
                        'hill_edge_band': int((is_status4 & has(reg, HILL_BAND)).sum()),
                        'elsewhere': int((is_status4 & ~has(reg, SKY_BAND) &
                                          ~has(reg, HILL_BAND)).sum())})
        for band, flag in (('sky', SKY_BAND), ('hill', HILL_BAND)):
            ys, xs = np.nonzero(has(reg, flag))
            local = local_measurements(frame['color'], frame.get('motion'), frame['depth'],
                                       ys, xs, band)
            own0, class_ok = local['own0'], local['class_ok']
            votes = {}
            for name, vote in absolute.items():
                truth = own0 if band == 'sky' else np.zeros(len(ys), bool)
                votes[name] = (vote[ys, xs], np.ones(len(ys), bool), truth)
            votes['color'] = (local['alpha'] < 0.5, local['alpha_ok'] & class_ok, own0)
            if 'motion_ok' in local:
                votes['motion'] = (local['motion_vote0'], local['motion_ok'] & class_ok, own0)
            at4 = is_status4[ys, xs]
            for name, (vote0, defined, truth0) in votes.items():
                c = bands[band].setdefault(name, new_counts())
                wrong = defined & (vote0 != truth0)
                c['defined'] += int(defined.sum())
                c['disagree'] += int(wrong.sum())
                c['class0_vote1'] += int((wrong & truth0).sum())
                c['class1_vote0'] += int((wrong & ~truth0).sum())
                c['at_status4'] += int((wrong & at4).sum())
                m = maps.setdefault(name, np.zeros(shape, np.uint8))
                m[ys[wrong], xs[wrong]] += 1
                if band == 'sky':   # does the input agree better with depth moved by one pixel?
                    counts = shift_counts.setdefault(name, np.zeros((3, 3), np.int64))
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            moved = sky[np.clip(ys + dy, 0, shape[0] - 1),
                                        np.clip(xs + dx, 0, shape[1] - 1)]
                            counts[dy + 1, dx + 1] += int((defined & (vote0 != moved)).sum())

            ok = local['alpha_ok'] & class_ok
            alpha = local['alpha']
            if band == 'sky':
                alpha_hist['sky_band_sky'] += np.histogram(alpha[ok & own0], hist_edges)[0]
                alpha_hist['sky_band_non_sky'] += np.histogram(alpha[ok & ~own0], hist_edges)[0]
                edge = ok & has(reg, BOUNDARY)[ys, xs]
                key = 'sky_boundary'
                lum, solid_lum = local['luminance'], local['solid_luminance']
                with np.errstate(invalid='ignore', divide='ignore'):
                    ratio = lum / solid_lum
                good = np.isfinite(ratio) & (solid_lum > 0)
                hdr['defined'] += int(good.sum())
                hdr['over10'] += int((ratio[good] > 10).sum())
                hdr['over100'] += int((ratio[good] > 100).sum())
            else:
                alpha_hist['hill_band'] += np.histogram(alpha[ok], hist_edges)[0]
                edge = ok & has(reg, HILL_EDGE)[ys, xs]
                key = 'hill_edge'
            blended = (alpha >= BLEND_RANGE[0]) & (alpha <= BLEND_RANGE[1])
            blend[key][0] += int((edge & blended).sum())
            blend[key][1] += int(edge.sum())
            acc['contrast_sum'][ys[ok], xs[ok]] += local['contrast'][ok]
            acc['contrast_n'][ys[ok], xs[ok]] += 1
            acc['class0_n'][ys[class_ok], xs[class_ok]] += own0[class_ok].astype(np.uint8)
            acc['class_n'][ys[class_ok], xs[class_ok]] += 1

    for band in bands.values():
        for c in band.values():
            c['rate'] = rate(c['disagree'], c['defined'])
    best_shift = {}
    for name, counts in shift_counts.items():
        defined = bands['sky'][name]['defined']
        dy, dx = np.unravel_index(np.argmin(counts), counts.shape)
        best_shift[name] = {'dx': int(dx) - 1, 'dy': int(dy) - 1,
                            'rate': rate(int(counts[dy, dx]), defined),
                            'rate_unshifted': rate(int(counts[1, 1]), defined)}
    metrics = {
        'bands': bands,
        'interior_false_sky': {k: rate(*v) for k, v in false_sky.items()},
        'best_shift': best_shift,
        'alpha_histogram': {'bin_edges': hist_edges.tolist(),
                            **{k: v.tolist() for k, v in alpha_hist.items()}},
        'blended': {k: {'blended': v[0], 'defined': v[1], 'fraction': rate(*v)}
                    for k, v in blend.items()},
        'sky_references': sky_refs,
        'status4': status4,
    }
    hdr_metrics = {'edge_pixels_with_local_reference': hdr['defined'],
                   'fraction_over_10x_local_non_sky': rate(hdr['over10'], hdr['defined']),
                   'fraction_over_100x_local_non_sky': rate(hdr['over100'], hdr['defined'])}
    return metrics, maps, acc, hdr_metrics


# ---- H0: flicker in RR's own output (rr_output, output size) -------------------------------
# Render-size regions and masks are mapped to output size nearest-neighbour; the input colour
# metric is measured at render size and read at each output pixel's matching render pixel.

def output_index(render_shape, output_shape):
    """Row and column index of the render pixel nearest each output pixel centre, per axis (the
    scale need not be an integer)."""
    (h, w), (oh, ow) = render_shape, output_shape
    return ((2 * np.arange(oh, dtype=np.intp) + 1) * h // (2 * oh),   # floor((o + 0.5) h / oh), exact
            (2 * np.arange(ow, dtype=np.intp) + 1) * w // (2 * ow))


def to_output(array, index):
    return array[np.ix_(*index)]


def crop_to_output(rect, render_shape, output_shape):
    """Render-pixel window x, y, w, h -> the output pixels it covers."""
    x, y, w, h = rect
    sy, sx = output_shape[0] / render_shape[0], output_shape[1] / render_shape[1]
    x0, y0 = min(int(x * sx), output_shape[1] - 1), min(int(y * sy), output_shape[0] - 1)
    x1 = min(output_shape[1], max(x0 + 1, math.ceil((x + w) * sx)))
    y1 = min(output_shape[0], max(y0 + 1, math.ceil((y + h) * sy)))
    return x0, y0, x1 - x0, y1 - y0


def region_labels(unions):
    """H0_REGIONS label map (0 = none) from the disjoint union masks of region_summary."""
    labels = np.zeros(unions['sky_band'].shape, np.uint8)
    for name, key in (('sky_edge_band', 'sky_band'), ('hill_edge_band', 'hill_band'),
                      ('interior', 'interior'), ('sky', 'sky')):
        labels[unions[key]] = H0_REGIONS[name]
    return labels


def mean_abs_delta(log_lum, pairs, valid, static, index=None):
    """Per-pixel mean |delta log luminance| over the frame pairs: over the pairs where the pixel is
    static in both frames, and over all pairs (moving geometry included; reported only). log_lum(i)
    gives frame i; valid and static are render-size masks, mapped through index for an output-size
    image. Each pair's median delta over static pixels (a global exposure change) is removed first."""
    sums = counts = None
    offsets, previous = [], (None, None)
    for i, k in pairs:
        a = previous[1] if previous[0] == i else log_lum(i)
        b = log_lum(k)
        previous = (k, b)
        d = b - a
        v, s = valid[i] & valid[k], static[i] & static[k]
        if index is not None:
            v, s = to_output(v, index), to_output(s, index)
        v &= np.isfinite(d)
        s &= v
        picked = d[s]
        offset = float(np.median(picked[::max(1, picked.size // 200000)])) if picked.size else 0.0
        d -= offset
        np.abs(d, out=d)
        if sums is None:
            sums = [np.zeros(d.shape, np.float32) for _ in range(2)]
            counts = [np.zeros(d.shape, np.uint8) for _ in range(2)]
        for total, count, mask in zip(sums, counts, (s, v)):
            total += np.where(mask, d, 0)
            count += mask
        offsets.append(offset)
    with np.errstate(invalid='ignore', divide='ignore'):
        static_mean, all_mean = (total / count for total, count in zip(sums, counts))
    return static_mean, all_mean, offsets


def flicker_stats(values):
    v = values[np.isfinite(values)]
    median, p90 = finite_stats(v)
    return {'pixels': int(v.size), 'median': median, 'p90': p90,
            'visible_fraction': rate(int((v >= H0_VISIBLE).sum()), int(v.size))}


def floored_ratio(a, b, floor):
    return None if a is None or b is None else a / max(b, floor)


def output_bad_values(frames, used, band):
    """NaN / Inf / negative rr_output pixel-frames (RGB), in total and in the mapped sky-edge band."""
    counts = {k: {'total': 0, 'sky_edge_band': 0} for k in ('nan', 'inf', 'negative')}
    for i in used:
        rgb = frames[i]['rr_output'][..., :3]
        with np.errstate(invalid='ignore'):
            kinds = {'nan': np.isnan(rgb).any(axis=-1), 'inf': np.isinf(rgb).any(axis=-1),
                     'negative': (rgb < 0).any(axis=-1)}
        for kind, mask in kinds.items():
            counts[kind]['total'] += int(mask.sum())
            counts[kind]['sky_edge_band'] += int((mask & band).sum())
    return counts


def output_flicker_analysis(manifest, frames, regions, unions, rect):
    """H0: temporal instability of rr_output per output pixel, per region, against the same metric
    on the colour input at the matching render pixels. Frames RR reset are skipped. Returns
    (metrics, maps for the images or None)."""
    with_output = [i for i, f in enumerate(frames) if 'rr_output' in f]
    if not with_output:
        return {'available': False, 'note': 'the capture has no rr_output (older capture format)'}, None
    render_shape = frames[0]['depth'].shape
    out_shape = frames[with_output[0]]['rr_output'].shape[:2]
    entries = manifest.get('frames', [])
    reset = [i for i, e in enumerate(entries[:len(frames)]) if e.get('reset')]
    used = [i for i in with_output if frames[i]['rr_output'].shape[:2] == out_shape]
    usable = [i in used and i not in reset for i in range(len(frames))]
    pairs = [(i, i + 1) for i in range(len(frames) - 1) if usable[i] and usable[i + 1]]
    index = output_index(render_shape, out_shape)
    labels = to_output(region_labels(unions), index)
    m = {'available': True, 'output_width': int(out_shape[1]), 'output_height': int(out_shape[0]),
         'scale': [out_shape[1] / render_shape[1], out_shape[0] / render_shape[0]],
         'frames_with_output': with_output, 'frames_skipped_reset': reset,
         'frames_other_size': [i for i in with_output if i not in used],
         'pairs': [list(p) for p in pairs],
         'output_bad_values': output_bad_values(frames, used, labels == H0_REGIONS['sky_edge_band'])}
    if not pairs:
        return m, None
    valid = [has(reg, VALID) for reg in regions]
    static = [static_pixels(f, reg) for f, reg in zip(frames, regions)]
    out_static, out_all, m['global_offset_output'] = mean_abs_delta(
        lambda i: log_luminance(frames[i]['rr_output']), pairs, valid, static, index)
    in_static, _, m['global_offset_input'] = mean_abs_delta(
        lambda i: log_luminance(frames[i]['color']), pairs, valid, static)
    in_static = to_output(in_static, index)
    moving = np.isnan(out_static) & np.isfinite(out_all)

    m['regions'] = {}
    for name, label in H0_REGIONS.items():
        mask = labels == label
        m['regions'][name] = {'output': flicker_stats(out_static[mask]), 'input': flicker_stats(in_static[mask]),
                              'output_all_pixels': flicker_stats(out_all[mask]),
                              'moving_excluded': int((moving & mask).sum())}
    x, y, w, h = crop_to_output(rect, render_shape, out_shape)
    window = (slice(y, y + h), slice(x, x + w))
    m['crop'] = {'window_output_px': [x, y, w, h], 'regions': {
        name: {side: flicker_stats(values[window][labels[window] == label])
               for side, values in (('output', out_static), ('input', in_static))}
        for name, label in H0_REGIONS.items()}}
    r = m['regions']
    m['sky_over_hill'] = {side: {
        'p90': floored_ratio(r['sky_edge_band'][side]['p90'], r['hill_edge_band'][side]['p90'], H0_RATIO_FLOOR),
        'visible_fraction': floored_ratio(r['sky_edge_band'][side]['visible_fraction'],
                                          r['hill_edge_band'][side]['visible_fraction'], H0_SHARE_FLOOR)}
        for side in ('output', 'input')}
    out_ratio, in_ratio = m['sky_over_hill']['output']['p90'], m['sky_over_hill']['input']['p90']
    m['lift'] = out_ratio / in_ratio if out_ratio is not None and in_ratio else None
    m['output_over_input_p90'] = {name: floored_ratio(row['output']['p90'], row['input']['p90'], H0_RATIO_FLOOR)
                                  for name, row in r.items()}
    first = pairs[0][0]   # does RR's output share the input's scale (pre-exposed HDR)?
    diff = log_luminance(frames[first]['rr_output']) - to_output(log_luminance(frames[first]['color']), index)
    diff = diff[(labels == H0_REGIONS['interior']) & np.isfinite(diff)]
    m['output_over_input_luminance_interior'] = float(np.exp(np.median(diff))) if diff.size else None
    return m, {'output': out_static, 'input': in_static, 'moving': moving, 'labels': labels,
               'frame': first, 'window': window}


# ---- H1: sky/non-sky class flips -----------------------------------------------------------

def flicker_analysis(regions, unions):
    shape, n = regions[0].shape, len(regions)
    flips = np.zeros(shape, np.uint8)
    thin_any = np.zeros(shape, bool)
    previous = None
    for reg in regions:
        sky, valid = has(reg, SKY), has(reg, VALID)
        solid = valid & ~sky
        if previous is not None:
            flips += (previous[0] != sky) & previous[1] & valid
        thin_any |= solid & ((run_lengths(solid, 1) <= THIN_MAX_RUN) |
                             (run_lengths(solid, 0) <= THIN_MAX_RUN))
        previous = (sky, valid)
    flicker = flips >= FLICKER_MIN_FLIPS
    band = unions['sky_band']
    count = int(flicker.sum())
    metrics = {'flip_histogram_in_sky_band': np.bincount(flips[band], minlength=n).tolist(),
               'pixels_with_any_flip': int((flips > 0).sum()),
               'flicker_pixels': count,
               'flicker_pixels_in_sky_band': int((flicker & band).sum()),
               'flicker_fraction_of_sky_band': rate(count, int(band.sum())),
               'thin_flicker_pixels': int((flicker & thin_any).sum()),
               'thin_fraction': rate(int((flicker & thin_any).sum()), count)}
    return metrics, flips, flicker


# ---- H8: stochastic coverage (per-frame dither) versus jitter ------------------------------


def box_sum(mask, half):
    """Sum over a (2 half + 1)^2 window, zero outside the image."""
    k = 2 * half + 1
    c = np.pad(np.pad(mask.astype(np.int32), half).cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    return c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]


def any_4_neighbour(mask):
    out = np.zeros_like(mask)
    out[1:] |= mask[:-1]
    out[:-1] |= mask[1:]
    out[:, 1:] |= mask[:, :-1]
    out[:, :-1] |= mask[:, 1:]
    return out


def neighbour_extremes(values, include_centre=False):
    """Min and max over the 8 neighbours (NaN and out-of-bounds ignored; inf/-inf when none)."""
    h, w = values.shape
    padded = np.pad(values, 1, constant_values=np.nan)
    lo = np.full(values.shape, np.inf, values.dtype)
    hi = np.full(values.shape, -np.inf, values.dtype)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dy == dx == 0 and not include_centre:
                continue
            s = padded[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
            lo, hi = np.fmin(lo, s), np.fmax(hi, s)
    return lo, hi


def isolated_pixels(depth, reg):
    """(sky-class isolated, near/far isolated): pixels whose depth class differs from all 8
    neighbours. Near/far isolated: non-sky pixel whose non-sky neighbours are all more than
    HILL_DEPTH_RATIO nearer, or all that much farther."""
    valid, sky = has(reg, VALID), has(reg, SKY)
    solid = valid & ~sky
    n_valid = box_sum(valid, 1) - valid
    n_sky = box_sum(sky, 1) - sky
    sky_class = (n_valid > 0) & ((sky & (n_sky == 0)) | (solid & (n_sky == n_valid)))
    d = np.where(solid, depth, np.nan).astype(np.float32)
    lo, hi = neighbour_extremes(d)
    with np.errstate(invalid='ignore'):
        depth_class = solid & (n_sky == 0) & ((lo > HILL_DEPTH_RATIO * d) | (hi * HILL_DEPTH_RATIO < d))
    return sky_class, depth_class


def region_split(mask, reg):
    sky_band, hill_band = has(reg, SKY_BAND), has(reg, HILL_BAND)
    return {'sky_edge_band': int((mask & sky_band).sum()),
            'hill_edge_band': int((mask & hill_band).sum()),
            'interior': int((mask & interior_mask(reg)).sum()),
            'other': int((mask & ~sky_band & ~hill_band & ~interior_mask(reg)).sum())}


def flip_coherence(before, after, valid):
    """Among pixels whose class changed: how many have a 4- or 8-neighbour that changed the same
    way, and the 8-neighbour share expected by chance at the local (9x9 ring) flip density."""
    out = {'flips': 0, 'coherent4': 0, 'coherent8': 0, 'chance8': 0.0}
    ring_valid = np.maximum(box_sum(valid, 4) - box_sum(valid, 1), 1)
    for flipped in (valid & ~before & after, valid & before & ~after):
        if not flipped.any():
            continue
        near8 = (box_sum(flipped, 1) - flipped) > 0
        density = (box_sum(flipped, 4) - box_sum(flipped, 1)) / ring_valid
        out['flips'] += int(flipped.sum())
        out['coherent4'] += int((flipped & any_4_neighbour(flipped)).sum())
        out['coherent8'] += int((flipped & near8).sum())
        out['chance8'] += float((1 - (1 - density[flipped]) ** 8).sum())
    return out


def coherence_summary(rows):
    total = {k: sum(r[k] for r in rows) for k in ('flips', 'coherent4', 'coherent8', 'chance8',
                                                   'isolated')}
    flips = total['flips']
    c8, chance = rate(total['coherent8'], flips), rate(total['chance8'], flips)
    lift = None
    if c8 is not None and chance is not None and chance < 1:
        lift = (c8 - chance) / (1 - chance)
    return {'pairs': rows, 'flips': flips, 'coherent4_fraction': rate(total['coherent4'], flips),
            'coherent8_fraction': c8, 'chance8_fraction': chance, 'coherence_lift': lift,
            'isolated_fraction': rate(total['isolated'], flips)}


def jitter_share(classes, usable, region, jitter):
    """Fit flips(i, k) = a + bx |djx| + by |djy| over all frame pairs (flip fraction of usable
    pixels in region). Jitter-driven flips vanish as the jitter difference goes to 0; per-frame
    dither does not, so a / mean(flips) is the share of flips that jitter does not explain."""
    rows = []
    for i in range(len(classes)):
        for k in range(i + 1, len(classes)):
            ok = usable[i] & usable[k] & region
            n = int(ok.sum())
            if n:
                f = int(((classes[i] != classes[k]) & ok).sum())
                dj = np.abs(np.subtract(jitter[k], jitter[i]))
                rows.append((f, n, float(dj[0]), float(dj[1])))
    flips = sum(r[0] for r in rows)
    if len(rows) < 4 or flips < MIN_PIXELS:
        return {'pairs': len(rows), 'flips': flips, 'share': None}
    y = np.array([r[0] / r[1] for r in rows])
    a = np.array([[1.0, r[2], r[3]] for r in rows])
    coef = np.linalg.lstsq(a, y, rcond=None)[0]
    return {'pairs': len(rows), 'flips': flips, 'intercept': float(coef[0]),
            'slope_x': float(coef[1]), 'slope_y': float(coef[2]),
            'mean_flip_fraction': float(y.mean()), 'share': float(coef[0] / y.mean())}


def tile_jitter_share(classes, usable, region, jitter):
    """jitter_share per H8_TILE x H8_TILE tile. Returns {tiles with enough static flips,
    dither-like tiles (share >= H8_SUPPORT_SHARE), fraction of all static flips inside those,
    the strongest dither-like tiles}."""
    h, w = region.shape
    ty, tx = -(-h // H8_TILE), -(-w // H8_TILE)

    def per_tile(mask):
        padded = np.zeros((ty * H8_TILE, tx * H8_TILE), np.int64)
        padded[:h, :w] = mask
        return padded.reshape(ty, H8_TILE, tx, H8_TILE).sum(axis=(1, 3))

    flips, counts, design = [], [], []
    for i in range(len(classes)):
        for k in range(i + 1, len(classes)):
            ok = usable[i] & usable[k] & region
            flips.append(per_tile((classes[i] != classes[k]) & ok))
            counts.append(per_tile(ok))
            dj = np.abs(np.subtract(jitter[k], jitter[i]))
            design.append([1.0, float(dj[0]), float(dj[1])])
    if len(design) < 4:
        return None
    f, n = np.array(flips, np.float64), np.array(counts, np.float64)
    y = f / np.maximum(n, 1)
    coef = np.linalg.lstsq(np.array(design), y.reshape(len(design), -1), rcond=None)[0]
    mean = y.mean(axis=0).ravel()
    total = f.sum(axis=0).ravel()
    fitted = (total >= H8_TILE_MIN_FLIPS) & (n.min(axis=0).ravel() > 0) & (mean > 0)
    share = np.full(total.shape, np.nan)
    share[fitted] = coef[0][fitted] / mean[fitted]
    dither = fitted & (share >= H8_SUPPORT_SHARE)
    order = np.argsort(-np.where(dither, total, -1))[:5]
    return {'tile': H8_TILE, 'tiles_fitted': int(fitted.sum()), 'dither_like_tiles': int(dither.sum()),
            'flips_in_dither_like_tiles': rate(float(total[dither].sum()), float(total.sum())),
            'top_dither_like_tiles': [{'x': int(t % tx) * H8_TILE, 'y': int(t // tx) * H8_TILE,
                                       'share': float(share[t]), 'flips': int(total[t])}
                                      for t in order if dither[t]]}


def hill_split(frames, regions, hill_union):
    """Fixed near/far split per pixel: geometric mean of the 3x3 min and max non-sky depth over
    all frames, where they differ by more than HILL_DEPTH_RATIO; NaN elsewhere."""
    lo = np.full(hill_union.shape, np.inf, np.float32)
    hi = np.zeros(hill_union.shape, np.float32)
    for frame, reg in zip(frames, regions):
        depth = frame['depth']
        good = has(reg, VALID) & ~has(reg, SKY) & np.isfinite(depth) & (depth > 0)
        l, h = neighbour_extremes(np.where(good, depth, np.nan).astype(np.float32), True)
        lo, hi = np.fmin(lo, l), np.fmax(hi, h)
    defined = hill_union & np.isfinite(lo) & (lo > 0) & (hi > HILL_DEPTH_RATIO * lo)
    with np.errstate(invalid='ignore'):
        return np.where(defined, np.sqrt(lo * hi), np.nan)


def static_pixels(frame, reg):
    """Valid and not within 2 px of a pixel whose motion exceeds STATIC_MOTION (or is not finite)."""
    ok = has(reg, VALID)
    if 'motion' in frame:
        with np.errstate(invalid='ignore'):
            ok = ok & ~dilate(~(np.linalg.norm(frame['motion'], axis=-1) <= STATIC_MOTION), 2)
    return ok


def stochastic_coverage_analysis(manifest, frames, regions, unions):
    n = len(frames)
    jitter = [f.get('jitter') for f in manifest.get('frames', [])]
    static = [static_pixels(frame, reg) for frame, reg in zip(frames, regions)]
    split = hill_split(frames, regions, unions['hill_band'])
    split_ok = np.isfinite(split)
    skies, fars, solids = [], [], []
    isolated_rows, sky_rows, hill_rows = [], [], []
    previous = None
    for i, (frame, reg) in enumerate(zip(frames, regions)):
        sky, valid = has(reg, SKY), has(reg, VALID)
        solid = valid & ~sky
        with np.errstate(invalid='ignore'):
            far = solid & (frame['depth'] < split)
        iso_sky, iso_depth = isolated_pixels(frame['depth'], reg)
        row = {'frame': i, 'sky_class': region_split(iso_sky, reg),
               'depth_class': region_split(iso_depth, reg)}
        if previous is not None:
            p_sky, p_far, p_valid, p_solid, p_iso_sky, p_iso_depth = previous
            row['new_vs_previous'] = int(((iso_sky & ~p_iso_sky) | (iso_depth & ~p_iso_depth)).sum())
            c = flip_coherence(p_sky, sky, p_valid & valid)
            c['isolated'] = int(((p_sky != sky) & p_valid & valid & (iso_sky | p_iso_sky)).sum())
            sky_rows.append({'pair': [i - 1, i], **c})
            usable = p_solid & solid & split_ok
            c = flip_coherence(p_far, far, usable)
            c['isolated'] = int(((p_far != far) & usable & (iso_depth | p_iso_depth)).sum())
            hill_rows.append({'pair': [i - 1, i], **c})
        isolated_rows.append(row)
        previous = (sky, far, valid, solid, iso_sky, iso_depth)
        skies.append(sky)
        fars.append(far)
        solids.append(solid & split_ok)
    metrics = {'isolated': isolated_rows,
               'sky_coherence': coherence_summary(sky_rows),
               'hill_coherence': coherence_summary(hill_rows)}
    if len(jitter) == n and all(j is not None for j in jitter):
        hill_usable = [s & st for s, st in zip(solids, static)]
        metrics['jitter_fit'] = {
            'sky': jitter_share(skies, static, unions['sky_band'], jitter),
            'hill': jitter_share(fars, hill_usable, unions['hill_band'], jitter)}
        metrics['jitter_fit_tiles'] = {
            'sky': tile_jitter_share(skies, static, unions['sky_band'], jitter),
            'hill': tile_jitter_share(fars, hill_usable, unions['hill_band'], jitter)}
    return metrics


# ---- H3: depth bleed at sky edges ----------------------------------------------------------

def depth_bleed_analysis(frames, regions, radius):
    """Edge pixels = non-sky pixels with a sky 8-neighbour. Spatial: depth / median depth of
    interior non-sky pixels (outside the sky-edge band) in a (2 (radius + DEPTH_REF_EXTRA) + 1)^2
    window. Temporal (reported only): min / max non-sky depth of the pixel over the frames."""
    half = radius + DEPTH_REF_EXTRA
    ratios, no_reference = [], 0
    shape = regions[0].shape
    dmin = np.full(shape, np.inf, np.float32)
    dmax = np.zeros(shape, np.float32)
    solid_frames = np.zeros(shape, np.uint8)
    edge_any = np.zeros(shape, bool)
    for frame, reg in zip(frames, regions):
        depth = frame['depth']
        good = has(reg, VALID) & ~has(reg, SKY) & np.isfinite(depth) & (depth > 0)
        edge = good & has(reg, BOUNDARY)
        reference = good & ~has(reg, SKY_BAND)
        ys, xs = np.nonzero(edge)
        for s in range(0, len(ys), CHUNK):
            y, x = ys[s:s + CHUNK], xs[s:s + CHUNK]
            yy, xx, inside = window_index(y, x, half, shape)
            use = inside & reference[yy, xx]
            ref = masked_median(depth[yy, xx], use)
            ok = use.sum(axis=1) >= MIN_REFS
            ratios.append(depth[y, x][ok] / ref[ok])
            no_reference += int((~ok).sum())
        dmin = np.where(good, np.minimum(dmin, depth), dmin)
        dmax = np.where(good, np.maximum(dmax, depth), dmax)
        solid_frames += good
        edge_any |= edge
    ratios = np.concatenate(ratios) if ratios else np.zeros(0, np.float32)
    temporal = (dmin / np.maximum(dmax, 1e-30))[edge_any & (solid_frames >= 2)]
    return {'spatial': fraction_summary(ratios), 'edge_pixels_without_interior_reference': no_reference,
            'temporal_min_over_max': fraction_summary(temporal)}


def fraction_summary(ratios):
    bins = [0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, np.inf]
    n = int(ratios.size)
    return {'pixels': n,
            'below_low': rate(int((ratios < H3_LOW_RATIO).sum()), n),
            'below_mid': rate(int((ratios < H3_MID_RATIO).sum()), n),
            'percentiles_5_50_95': finite_stats(ratios, (5, 50, 95)),
            'histogram_edges': [b if np.isfinite(b) else 'inf' for b in bins],
            'histogram': np.histogram(ratios, bins)[0].tolist() if n else [0] * (len(bins) - 1)}


# ---- H4: motion magnitudes per region ------------------------------------------------------

def motion_magnitudes(frames, regions):
    names = ('sky_interior', 'sky_band_sky', 'sky_band_non_sky', 'hill_band', 'interior')
    per_frame = {k: [] for k in names}
    for frame, reg in zip(frames, regions):
        if 'motion' not in frame:
            continue
        magnitude = np.linalg.norm(frame['motion'], axis=-1)
        sky, band = has(reg, SKY), has(reg, SKY_BAND)
        masks = {'sky_interior': sky & ~band, 'sky_band_sky': sky & band,
                 'sky_band_non_sky': band & ~sky, 'hill_band': has(reg, HILL_BAND),
                 'interior': interior_mask(reg)}
        for k, m in masks.items():
            value = finite_stats(magnitude[m], (50,))[0]
            if value is not None:
                per_frame[k].append(value)
    return {k: (float(np.median(v)) if v else None) for k, v in per_frame.items()}


# ---- H5: temporal log-luminance flicker ----------------------------------------------------

def temporal_analysis(frames, unions, acc):
    """Per-pixel std of log luminance over the frames, raw, divided by the local two-class
    contrast, and as 'excess': the part above contrast * sqrt(p (1 - p)), which is what a pixel
    that just switches between the two class colours (p = share of frames in class 0) would show."""
    shape = unions['sky_band'].shape
    s1 = np.zeros(shape, np.float64)
    s2 = np.zeros(shape, np.float64)
    count = np.zeros(shape, np.uint8)
    for frame in frames:
        lum = log_luminance(frame['color'])
        ok = np.isfinite(lum)
        lum = np.where(ok, lum, 0)
        s1 += lum
        s2 += lum.astype(np.float64) ** 2
        count += ok
    with np.errstate(invalid='ignore', divide='ignore'):
        mean = s1 / count
        std = np.sqrt(np.maximum(s2 / count - mean ** 2, 0)).astype(np.float32)
        std[count < 2] = np.nan
        contrast = acc['contrast_sum'] / acc['contrast_n']
        p = acc['class0_n'] / acc['class_n']
        explained = contrast * np.sqrt(p * (1 - p))
        normalised = std / contrast
        excess = np.maximum(std - explained, 0) / contrast
    rows = {}
    for name, key in (('sky_edge_band', 'sky_band'), ('hill_edge_band', 'hill_band'),
                      ('interior', 'interior')):
        mask = unions[key] & np.isfinite(std)
        row = {'pixels': int(mask.sum())}
        row['std_median'], row['std_p90'] = finite_stats(std[mask])
        if key != 'interior':
            with_contrast = mask & (contrast > CONTRAST_MIN)
            row['pixels_with_contrast'] = int(with_contrast.sum())
            row['contrast_median'] = finite_stats(contrast[with_contrast], (50,))[0]
            row['std_over_contrast_median'], row['std_over_contrast_p90'] = \
                finite_stats(normalised[with_contrast])
            row['excess_median'], row['excess_p90'] = finite_stats(excess[with_contrast])
        rows[name] = row
    return rows, std


# ---- H6: bad values ------------------------------------------------------------------------

def bad_value_analysis(frames, regions):
    counts = {}
    for frame, reg in zip(frames, regions):
        band = has(reg, SKY_BAND)
        for name, array in frame.items():
            if array.dtype.kind != 'f' or array.shape[:2] != band.shape:   # rr_output: see H0
                continue
            a = array if array.ndim == 3 else array[..., None]
            kinds = {'nan': np.isnan(a).any(axis=-1), 'inf': np.isinf(a).any(axis=-1)}
            with np.errstate(invalid='ignore'):
                if name in NONNEGATIVE_CHANNELS:
                    channels = NONNEGATIVE_CHANNELS[name]
                    sub = a if channels is None else a[..., [c for c in channels if c < a.shape[-1]]]
                    kinds['negative'] = (sub < 0).any(axis=-1)
                kinds['huge'] = (np.isfinite(a) & (np.abs(a) > HUGE_VALUE)).any(axis=-1)
            entry = counts.setdefault(name, {})
            for kind, mask in kinds.items():
                c = entry.setdefault(kind, {'total': 0, 'sky_edge_band': 0})
                c['total'] += int(mask.sum())
                c['sky_edge_band'] += int((mask & band).sum())
    in_band = sum(c['sky_edge_band'] for name, entry in counts.items() for kind, c in entry.items()
                  if not (kind == 'huge' and name in HUGE_EXEMPT))
    return {'counts': counts, 'bad_in_sky_band_for_verdict': in_band,
            'luminance': luminance_percentiles(frames, regions)}


def luminance_percentiles(frames, regions):
    rows = {'sky': [], 'non_sky': []}
    for frame, reg in zip(frames, regions):
        lum = luminance(frame['color'])
        sky = has(reg, SKY)
        for key, mask in (('sky', sky), ('non_sky', has(reg, VALID) & ~sky)):
            v = lum[mask]
            v = v[np.isfinite(v)]
            if v.size:
                rows[key].append(list(np.percentile(v, (50, 99, 99.9))) + [float(v.max())])
    out = {}
    for key, values in rows.items():
        if values:
            v = np.array(values)
            out[key] = {'p50': float(v[:, 0].mean()), 'p99': float(v[:, 1].mean()),
                        'p99_9': float(v[:, 2].mean()), 'max': float(v[:, 3].max())}
    return out


# ---- H9: HDR scale of the sky against the foliage next to it -------------------------------


def hdr_scale_analysis(manifest, frames, regions, local):
    groups = {'sky': [], 'sky_edge_non_sky': [], 'interior_non_sky': []}
    ranks = []
    for frame, reg in zip(frames, regions):
        lum = luminance(frame['color'])
        sky, band = has(reg, SKY), has(reg, SKY_BAND)
        solid = has(reg, VALID) & ~sky
        masks = {'sky': sky, 'sky_edge_non_sky': band & solid, 'interior_non_sky': interior_mask(reg)}
        for key, mask in masks.items():
            v = lum[mask]
            v = v[np.isfinite(v)]
            if v.size:
                groups[key].append(np.percentile(v, (50, 99)))
        edge_sky = lum[sky & band]
        solid_values = np.sort(lum[solid][np.isfinite(lum[solid])])
        edge_sky = edge_sky[np.isfinite(edge_sky)]
        if edge_sky.size and solid_values.size:
            ranks.append(np.searchsorted(solid_values, np.median(edge_sky)) / solid_values.size)
    out = {key: ({'p50': float(np.mean([v[0] for v in vals])), 'p99': float(np.mean([v[1] for v in vals]))}
                 if vals else None) for key, vals in groups.items()}
    sky, edge = out['sky'], out['sky_edge_non_sky']
    out['sky_over_edge_non_sky'] = (sky['p50'] / edge['p50']
                                    if sky and edge and edge['p50'] > 0 else None)
    out['edge_sky_brightness_rank_among_non_sky'] = float(np.mean(ranks)) if ranks else None
    out.update(local)
    out['pre_exposure'] = [f.get('pre_exposure') for f in manifest.get('frames', [])]
    out['exposure_scale'] = [f.get('exposure_scale') for f in manifest.get('frames', [])]
    return out


# ---- H10: coarse shading (VRS) -------------------------------------------------------------

def _blocks(array, axis, size, phase):
    """array (H, W, ...) cut into size-px blocks along axis starting at phase: (H', k, size, ...),
    plus each block's first pixel coordinate along that axis (k,)."""
    a = np.swapaxes(array, 0, 1) if axis == 0 else array
    k = (a.shape[1] - phase) // size
    cut = a[:, phase:phase + size * k]
    return cut.reshape((a.shape[0], k, size) + a.shape[2:]), phase + size * np.arange(k)


def mixed_blocks(frames, regions, axis, size, tile=H10_TILE):
    """Mixed (sky and non-sky) blocks per phase, summed over frames, and per tile
    (tiles_y, tiles_x, phase) for that axis."""
    h, w = frames[0]['depth'].shape
    counts = np.zeros(size, np.int64)
    per_tile = np.zeros(((h + tile - 1) // tile, (w + tile - 1) // tile, size), np.int64)
    for reg in regions:
        for p in range(size):
            sky, start = _blocks(has(reg, SKY), axis, size, p)
            valid, _ = _blocks(has(reg, VALID), axis, size, p)
            mixed = valid.all(-1) & sky.any(-1) & ~sky.all(-1)      # (rows, k)
            counts[p] += int(mixed.sum())
            rows, cols = np.nonzero(mixed)
            along, across = start[cols] // tile, rows // tile
            ty, tx = (across, along) if axis == 1 else (along, across)
            np.add.at(per_tile, (ty, tx, p), 1)
    return counts, per_tile


def phase_ratio(counts):
    """(best phase, best / median of the other phases); ratio None without counts."""
    best = int(np.argmin(counts))
    others = np.delete(counts, best)
    median = float(np.median(others))
    return best, (float(counts[best]) / median if median > 0 else None)


def equal_pairs(frames, regions, axis):
    """Identical same-surface non-sky pixel pairs (2-px blocks along axis) per channel, per
    region (sky-edge band, interior) and phase: {channel: {region: [[equal, pairs] per phase]}}."""
    out = {}
    for name in H10_CHANNELS:
        if not all(name in f for f in frames):
            continue
        rows = {'sky_edge_band': [[0, 0], [0, 0]], 'interior': [[0, 0], [0, 0]]}
        for frame, reg in zip(frames, regions):
            values = frame[name]
            if values.ndim == 2:
                values = values[..., None]
            solid = has(reg, VALID) & ~has(reg, SKY)
            depth = np.where(solid, frame['depth'], np.nan).astype(np.float64)
            band, interior = has(reg, SKY_BAND) & solid, interior_mask(reg)
            for p in range(2):
                v, _ = _blocks(values, axis, 2, p)
                d, _ = _blocks(depth, axis, 2, p)
                with np.errstate(invalid='ignore', divide='ignore'):
                    same = np.maximum(d[..., 0] / d[..., 1], d[..., 1] / d[..., 0]) <= SAME_SURFACE_RATIO
                equal = same & np.all(v[:, :, 0] == v[:, :, 1], axis=-1)
                for key, mask in (('sky_edge_band', band), ('interior', interior)):
                    m, _ = _blocks(mask, axis, 2, p)
                    pair = same & m.any(-1)
                    rows[key][p][0] += int((equal & pair).sum())
                    rows[key][p][1] += int(pair.sum())
        out[name] = rows
    return out


def equality_excess(phases):
    """[[equal, pairs] per phase] -> (rates, excess = best - other, ratio) or Nones when a phase
    has fewer than H10_MIN_PAIRS pairs."""
    if min(n for _, n in phases) < H10_MIN_PAIRS:
        return None, None, None
    rates = [e / n for e, n in phases]
    hi, lo = max(rates), min(rates)
    return rates, hi - lo, hi / max(lo, 1e-3)


def coarse_shading_analysis(frames, regions):
    """H10: does the sky/non-sky boundary snap to a coarse (2 or 4 px) grid, and are same-surface
    pixel pairs identical on one grid phase only?"""
    axes = {}
    for axis, label in ((1, 'x'), (0, 'y')):
        per = {}
        for size in H10_BLOCKS:
            counts, per_tile = mixed_blocks(frames, regions, axis, size)
            best, ratio = phase_ratio(counts)
            per[size] = {'counts': counts.tolist(), 'best_phase': best, 'ratio': ratio,
                         'tiles': per_tile if size == 2 else None}
        axes[label] = per
    # Tiles snapped in x and in y, per 2-px phase pair. A VRS grid is one screen grid, so its
    # tiles all land in one pair; static rasterised edges that snap by chance spread over all
    # four, which the other pairs measure (the control). The pair is chosen from the tiles, not
    # the frame-wide counts, which one long straight edge can tip.
    tx, ty = axes['x'][2].pop('tiles'), axes['y'][2].pop('tiles')
    axes['x'][4].pop('tiles'), axes['y'][4].pop('tiles')
    enough = (tx.sum(-1) >= H10_TILE_MIN_MIXED) & (ty.sum(-1) >= H10_TILE_MIN_MIXED)
    mixed = tx.sum(-1) + ty.sum(-1)
    total = int(mixed.sum())
    pairs = {}
    for px in (0, 1):
        for py in (0, 1):
            with np.errstate(invalid='ignore', divide='ignore'):
                rx = tx[..., px] / tx[..., 1 - px]
                ry = ty[..., py] / ty[..., 1 - py]
            snapped = enough & (rx <= H10_TILE_SNAP) & (ry <= H10_TILE_SNAP)
            pairs[(px, py)] = (snapped, (float(mixed[snapped].sum()) / total) if total else 0.0)
    best = max(pairs, key=lambda k: pairs[k][1])
    snapped, share = pairs[best]
    control = max(v[1] for k, v in pairs.items() if k != best)
    equal = {label: equal_pairs(frames, regions, axis) for axis, label in ((1, 'x'), (0, 'y'))}
    return {'axes': axes, 'tiles_fitted': int(enough.sum()), 'tiles_snapped': int(snapped.sum()),
            'tile_phase': list(best), 'snapped_tile_share': share if total else None,
            'control_tile_share': control if total else None,
            'phase_pair_shares': {f'{k[0]}{k[1]}': v[1] for k, v in pairs.items()},
            'snapped_tiles': [[int(y) * H10_TILE, int(x) * H10_TILE] for y, x in zip(*np.nonzero(snapped))][:20],
            'mixed_2px_blocks': total, 'equal_pairs': equal}


# ---- optional inputs: value ranges ---------------------------------------------------------

def optional_input_ranges(frames, regions):
    out = {}
    for name in OPTIONAL_INPUTS:
        per = {'sky': [], 'sky_edge_band': [], 'interior': []}
        for frame, reg in zip(frames, regions):
            if name not in frame or frame[name].dtype.kind != 'f':
                continue
            v = frame[name]
            if v.ndim == 3:
                v = luminance(v) if v.shape[-1] >= 3 else v[..., 0]
            masks = {'sky': has(reg, SKY) & ~has(reg, SKY_BAND), 'sky_edge_band': has(reg, SKY_BAND),
                     'interior': interior_mask(reg)}
            for key, mask in masks.items():
                values = v[mask]
                values = values[np.isfinite(values)]
                if values.size:
                    per[key].append((float(values.min()), float(np.median(values)), float(values.max())))
        if any(per.values()):
            out[name] = {key: ({'min': min(r[0] for r in rows), 'median': float(np.median([r[1] for r in rows])),
                                'max': max(r[2] for r in rows)} if rows else None)
                         for key, rows in per.items()}
    return out


# ---- camera stillness ----------------------------------------------------------------------

def view_rotation_translation(values):
    """Rotation (3x3) and translation of a world_to_view matrix in row-vector layout
    (p_view = p_world M). A column-vector matrix (translation in the last column) is transposed."""
    m = np.array(values, np.float64).reshape(4, 4)
    if np.abs(m[3, :3]).max() < 1e-12 and np.abs(m[:3, 3]).max() > 1e-12:
        m = m.T
    return m[:3, :3], m[3, :3]


def camera_motion(manifest, width):
    frames = manifest.get('frames', [])
    mats = [f.get('world_to_view') for f in frames]
    if len(mats) < 2 or any(m is None or len(m) != 16 or None in m for m in mats):
        return {'known': False, 'still': False, 'note': 'world_to_view missing'}
    focal = width / 2
    clip = frames[0].get('view_to_clip')
    if clip and len(clip) == 16 and clip[0]:
        focal = abs(clip[0]) * width / 2
    rows = []
    for a, b in zip(mats, mats[1:]):
        ra, ta = view_rotation_translation(a)
        rb, tb = view_rotation_translation(b)
        position_a, position_b = -ta @ np.linalg.inv(ra), -tb @ np.linalg.inv(rb)
        relative = ra.T @ rb
        angle = math.acos(max(-1.0, min(1.0, (np.trace(relative) - 1) / 2)))
        rows.append({'translation': float(np.linalg.norm(position_b - position_a)),
                     'rotation_deg': math.degrees(angle), 'rotation_px': angle * focal})
    max_t = max(r['translation'] for r in rows)
    max_px = max(r['rotation_px'] for r in rows)
    return {'known': True, 'pairs': rows, 'max_translation': max_t,
            'max_rotation_deg': max(r['rotation_deg'] for r in rows), 'max_rotation_px': max_px,
            'still': max_t <= STILL_TRANSLATION and max_px <= STILL_ROTATION_PX}


# ---- H7: measured sub-pixel image shift versus recorded jitter -----------------------------
# Two measurements per consecutive frame pair, both of log luminance:
#  * phase correlation of the whole (Hann-windowed) frame, with DFT peak refinement; reported,
#    but thin aliased edges (a wire's staircase crawls along it when the wire moves a little
#    across) and swaying foliage can pull it;
#  * the shift used for the verdict: a masked least-squares (Lucas-Kanade) fit over static
#    interior pixels only (no depth-edge band, no motion), with a local cubic shift. A mask can
#    not be put into the images for the phase correlation: the masked area then is a static
#    pattern in both frames and pins the peak at zero.

def correlation_at(cross, ys, xs):
    """Inverse DFT of a cross-power spectrum at fractional shifts ys (ny), xs (nx)."""
    h, w = cross.shape
    ey = np.exp(2j * np.pi * np.outer(ys, np.fft.fftfreq(h))).astype(np.complex64)
    ex = np.exp(2j * np.pi * np.outer(np.fft.fftfreq(w), xs)).astype(np.complex64)
    return (ey @ cross @ ex).real / (h * w)


def phase_shift(spectrum_a, spectrum_b):
    """Displacement (dx, dy) of image b relative to a (b(x) ~ a(x - d)) and the peak height
    (1 = perfect shift): integer peak of the phase correlation, refined on finer DFT grids."""
    cross = spectrum_b * np.conj(spectrum_a)
    cross /= np.maximum(np.abs(cross), 1e-20)
    h, w = cross.shape
    # Whitening weights every frequency equally; taper towards Nyquist, where noise and
    # point-sampling aliasing do not follow the shift. A symmetric taper does not move the peak.
    taper = np.outer(0.5 + 0.5 * np.cos(2 * np.pi * np.fft.fftfreq(h)),
                     0.5 + 0.5 * np.cos(2 * np.pi * np.fft.fftfreq(w))).astype(np.float32)
    cross *= taper / taper.mean()
    corr = np.fft.ifft2(cross).real
    iy, ix = np.unravel_index(np.argmax(corr), corr.shape)
    cy = float(iy - h if iy > h // 2 else iy)
    cx = float(ix - w if ix > w // 2 else ix)
    peak = float(corr[iy, ix])
    for span in (1.0, 0.1, 0.01):
        offsets = np.linspace(-span, span, 21)
        surface = correlation_at(cross, cy + offsets, cx + offsets)
        j, k = np.unravel_index(np.argmax(surface), surface.shape)
        cy, cx, peak = cy + offsets[j], cx + offsets[k], float(surface[j, k])
    return (cx, cy), peak


def log_luminance_spectrum(color, window):
    lum = log_luminance(color).astype(np.float32)
    ok = np.isfinite(lum)
    lum = np.where(ok, lum, np.median(lum[ok]) if ok.any() else 0)
    lum -= lum.mean()
    return np.fft.fft2(lum * window)


def shift_axis(a, d, axis):
    """a(x - d) along one axis by Catmull-Rom interpolation, clamped at the borders."""
    n = a.shape[axis]
    base = int(np.floor(-d))
    f = -d - base
    weights = ((-f ** 3 + 2 * f ** 2 - f) / 2, (3 * f ** 3 - 5 * f ** 2 + 2) / 2,
               (-3 * f ** 3 + 4 * f ** 2 + f) / 2, (f ** 3 - f ** 2) / 2)
    index = np.arange(n) + base
    return sum(wk * np.take(a, np.clip(index + k, 0, n - 1), axis=axis)
               for k, wk in zip((-1, 0, 1, 2), weights))


def masked_shift(a, b, weight):
    """Least-squares d with b(x) ~ a(x - d) over pixels where weight is set (Gauss-Newton from 0).
    Returns d, whether it converged, and the condition number of the gradient matrix (large =
    texture only constrains one direction)."""
    a, b = np.nan_to_num(a.astype(np.float64)), np.nan_to_num(b.astype(np.float64))
    w = weight.astype(np.float64)
    d = np.zeros(2)
    for _ in range(LK_ITERATIONS):
        s = shift_axis(shift_axis(a, d[0], 1), d[1], 0)
        gy, gx = np.gradient(s)
        r = b - s
        m = np.array([[(w * gx * gx).sum(), (w * gx * gy).sum()],
                      [(w * gx * gy).sum(), (w * gy * gy).sum()]])
        if not np.isfinite(m).all() or np.linalg.det(m) <= 0:
            return d, False, None
        step = np.linalg.solve(m, -np.array([(w * gx * r).sum(), (w * gy * r).sum()]))
        d += np.clip(step, -0.5, 0.5)
        if np.abs(step).max() < 1e-4:
            return d, True, float(np.linalg.cond(m))
    return d, False, float(np.linalg.cond(m))


def static_interior(frame_a, frame_b, reg_a, reg_b):
    """Pixels away from every depth-edge band and border, static (|motion| <= STATIC_MOTION) and
    finite in both frames; the local cubic shift reaches 2 px, hence the dilation by 3."""
    bad = np.zeros(reg_a.shape, bool)
    for frame, reg in ((frame_a, reg_a), (frame_b, reg_b)):
        bad |= has(reg, SKY_BAND) | has(reg, HILL_BAND) | ~has(reg, VALID)
        bad |= ~np.isfinite(log_luminance(frame['color']))
        if 'motion' in frame:
            with np.errstate(invalid='ignore'):
                bad |= ~(np.linalg.norm(frame['motion'], axis=-1) <= STATIC_MOTION)
    good = ~dilate(bad, 3)
    good[:3], good[-3:], good[:, :3], good[:, -3:] = False, False, False, False
    return good


def jitter_analysis(manifest, frames, regions):
    jitter = [f.get('jitter') for f in manifest.get('frames', [])]
    if len(jitter) != len(frames) or any(j is None for j in jitter):
        return {'available': False}
    h, w = frames[0]['color'].shape[:2]
    window = np.outer(np.hanning(h), np.hanning(w)).astype(np.float32)
    previous = log_luminance_spectrum(frames[0]['color'], window)
    pairs = []
    for i in range(1, len(frames)):
        current = log_luminance_spectrum(frames[i]['color'], window)
        phase, peak = phase_shift(previous, current)
        previous = current
        weight = static_interior(frames[i - 1], frames[i], regions[i - 1], regions[i])
        la, lb = log_luminance(frames[i - 1]['color']), log_luminance(frames[i]['color'])
        refined, converged, condition = (masked_shift(la, lb, weight)
                                         if weight.sum() >= H7_MIN_WEIGHT_PIXELS else (None, False, None))
        dj = np.subtract(jitter[i], jitter[i - 1])
        trusted = converged and condition is not None and condition <= H7_MAX_CONDITION
        pairs.append({'pair': [i - 1, i], 'phase_correlation': list(phase), 'peak': peak,
                      'measured': None if refined is None else [float(x) for x in refined],
                      'static_interior_pixels': int(weight.sum()), 'condition': condition,
                      'trusted': bool(trusted), 'jitter_delta': [float(dj[0]), float(dj[1])]})
    good = [p for p in pairs if p['trusted']]
    out = {'available': True, 'pairs': pairs, 'trusted_pairs': len(good)}
    for p in pairs:
        if p['measured'] is not None:
            p['residual'] = {name: float(np.hypot(*(np.array(p['measured']) - np.array(sign) *
                                                    np.array(p['jitter_delta']))))
                             for name, sign in JITTER_CONVENTIONS.items()}
    if not good:
        return out
    d = np.array([p['measured'] for p in good])
    j = np.array([p['jitter_delta'] for p in good])
    residuals = {name: float(np.hypot(*(d - np.array(sign) * j).T).mean())
                 for name, sign in JITTER_CONVENTIONS.items()}
    best = min(residuals, key=residuals.get)
    with np.errstate(invalid='ignore', divide='ignore'):
        scale = (d * j).sum(axis=0) / (j * j).sum(axis=0)
    phase = np.array([p['phase_correlation'] for p in good])
    out.update({'mean_residual': residuals, 'best_convention': best, 'best_residual': residuals[best],
                'fitted_scale': [float(s) if np.isfinite(s) else None for s in scale],
                'phase_correlation_residual_best_convention':
                    float(np.hypot(*(phase - np.array(JITTER_CONVENTIONS[best]) * j).T).mean())})
    return out


# ---- verdicts ------------------------------------------------------------------------------

def pct(value):
    return 'n/a' if value is None else f'{100 * value:.1f}%'


def num(value, digits=3):
    return 'n/a' if value is None else f'{value:.{digits}g}'


def grade(value, support_at, against_below):
    if value is None or not np.isfinite(value):
        return 'inconclusive'
    if value >= support_at:
        return 'supports'
    return 'against' if value < against_below else 'inconclusive'


def control_rate(bands, name):
    """Hill-band rate for this vote when it has enough pixels, else 0 (no control)."""
    hill = bands['hill'].get(name)
    return hill['rate'] if hill and hill['defined'] >= MIN_PIXELS else 0.0


def verdict_h0(m):
    h = m['H0']
    if not h['available']:
        return 'inconclusive', h['note'] + '; H0 needs a capture from a build that saves rr_output.'
    skipped = f", RR reset in frame(s) {h['frames_skipped_reset']} skipped" if h['frames_skipped_reset'] else ''
    if len(h['pairs']) < H0_MIN_PAIRS:
        return 'inconclusive', (f"only {len(h['pairs'])} consecutive frame pairs with rr_output in both and no RR "
                                f"reset (need {H0_MIN_PAIRS}){skipped}.")
    r = h['regions']
    sky, hill, interior = r['sky_edge_band'], r['hill_edge_band'], r['interior']
    so, si = h['sky_over_hill']['output'], h['sky_over_hill']['input']
    sky_p90, sky_share = sky['output']['p90'], sky['output']['visible_fraction']
    enough = min(sky['output']['pixels'], hill['output']['pixels']) >= MIN_PIXELS
    if enough and ((sky_p90 >= H0_VISIBLE and so['p90'] >= H0_SUPPORT_RATIO) or
                   (sky_share >= H0_MIN_SHARE and so['visible_fraction'] >= H0_SUPPORT_RATIO)):
        v = 'supports'
    elif enough and (sky_p90 < H0_VISIBLE or so['p90'] < H0_AGAINST_RATIO) and \
            (sky_share < H0_MIN_SHARE or so['visible_fraction'] < H0_AGAINST_RATIO):
        v = 'against'
    else:
        v = 'inconclusive'
    vs_input = h['output_over_input_p90']['sky_edge_band']
    compare = ('not comparable with' if vs_input is None else 'less unstable than' if vs_input < H0_SAME_RANGE[0] else
               'about as unstable as' if vs_input <= H0_SAME_RANGE[1] else 'more unstable than')
    lift = h['lift']
    if v != 'supports':
        blame = ''
    elif lift is not None and lift >= H0_RR_LIFT:
        blame = (f' The output is far more sky-edge specific than its input (>= {H0_RR_LIFT:g}x), which points '
                 'at RR itself.')
    else:
        blame = ' The input is about as sky-edge specific, so the output may just follow its input.'
    moved = sky['moving_excluded']
    text = (f"RR output, mean |delta log luminance| per frame, p90: sky edge {num(sky_p90)} vs hill edge "
            f"{num(hill['output']['p90'])} (ratio {num(so['p90'])}), interior {num(interior['output']['p90'])}; "
            f"share >= {H0_VISIBLE:g}: {pct(sky_share)} vs {pct(hill['output']['visible_fraction'])} (ratio "
            f"{num(so['visible_fraction'])}), interior {pct(interior['output']['visible_fraction'])}. Colour input at "
            f"the matching render pixels, p90: {num(sky['input']['p90'])} vs {num(hill['input']['p90'])} (ratio "
            f"{num(si['p90'])}); at sky edges the output is {compare} its input (output / input p90 {num(vs_input)}), "
            f"and its sky/hill ratio is {num(lift)}x the input's.{blame} Static pixels only: {len(h['pairs'])} frame "
            f"pairs{skipped}; {moved} sky-edge output pixels "
            f"({pct(rate(moved, moved + sky['output']['pixels']))}) left out as moving. Threshold: supports if "
            f"sky-edge p90 >= {H0_VISIBLE:g} and sky/hill p90 >= {H0_SUPPORT_RATIO:g}, or share >= {pct(H0_MIN_SHARE)} "
            f"and sky/hill share >= {H0_SUPPORT_RATIO:g}; against if each test fails by level or by ratio < "
            f"{H0_AGAINST_RATIO:g} (denominators floored at {H0_RATIO_FLOOR:g} / {pct(H0_SHARE_FLOOR)}).")
    if not m['camera']['still']:
        return 'inconclusive', 'skipped, camera moved: ' + camera_note(m) + '. ' + text
    if not enough:
        return 'inconclusive', f'too few static sky-edge or hill-edge output pixels (< {MIN_PIXELS}). ' + text
    return v, text


def verdict_h1(m):
    h = m['H1']
    v = grade(h['flicker_fraction_of_sky_band'], H1_SUPPORT_FRACTION, H1_AGAINST_FRACTION)
    return v, (f"{h['flicker_pixels']} flicker pixels (>= {FLICKER_MIN_FLIPS} sky/non-sky flips) = "
               f"{pct(h['flicker_fraction_of_sky_band'])} of the {m['regions']['union']['sky_edge_band']}-px "
               f"sky-edge band union, {pct(h['thin_fraction'])} of them thin (run <= {THIN_MAX_RUN} px). "
               f"Threshold: supports >= {pct(H1_SUPPORT_FRACTION)}, against < {pct(H1_AGAINST_FRACTION)}. "
               "Flips alone are expected with jitter (SR sees them too); they matter only together with H2-H4.")


def verdict_h2(m):
    h = m['H2']
    bands, false_sky = h['bands'], h['interior_false_sky']
    rows, ignored = [], []
    for name in ABSOLUTE_VOTES + ('color',):
        sky = bands['sky'].get(name)
        if not sky or sky['defined'] < MIN_PIXELS:
            continue
        if name in ABSOLUTE_VOTES and (false_sky.get(name) or 0) > UNINFORMATIVE_FALSE_SKY:
            ignored.append(name)
            continue
        rows.append((sky['rate'] - control_rate(bands, name), name, sky['rate'], control_rate(bands, name)))
    blend_sky, blend_hill = h['blended']['sky_boundary'], h['blended']['hill_edge']
    blend_excess = None
    if blend_sky['defined'] >= MIN_PIXELS:
        blend_excess = blend_sky['fraction'] - (blend_hill['fraction'] if blend_hill['defined'] >= MIN_PIXELS else 0)
    if not rows and blend_excess is None:
        return 'inconclusive', 'too few sky-edge pixels with usable votes.'
    worst = max(rows) if rows else (None, 'none', None, None)
    if (worst[0] is not None and worst[0] >= H2_SUPPORT_RATE) or \
            (blend_excess is not None and blend_excess >= H2_SUPPORT_BLEND):
        v = 'supports'
    elif (worst[0] is None or worst[0] < H2_AGAINST_RATE) and \
            (blend_excess is None or blend_excess < H2_AGAINST_BLEND):
        v = 'against'
    else:
        v = 'inconclusive'
    s4 = h['status4']
    s4_total, s4_band = sum(r['total'] for r in s4), sum(r['sky_edge_band'] for r in s4)
    text = (f"worst input {worst[1]}: {pct(worst[2])} disagreement with depth in the sky-edge band vs "
            f"{pct(worst[3])} hill-band control; blended colour alpha ({BLEND_RANGE[0]}..{BLEND_RANGE[1]}) at "
            f"sky boundary pixels {pct(blend_sky['fraction'])} vs {pct(blend_hill['fraction'])} at hill edges; "
            f"status 4 (invalid, zero guides) pixels: {s4_total} total, {s4_band} in the sky-edge band. "
            f"Threshold: supports if sky minus control >= {pct(H2_SUPPORT_RATE)} (votes) or >= "
            f"{pct(H2_SUPPORT_BLEND)} (blend); against if < {pct(H2_AGAINST_RATE)} and < {pct(H2_AGAINST_BLEND)}.")
    if ignored:
        text += f" Ignored as uninformative (call > {pct(UNINFORMATIVE_FALSE_SKY)} of the interior sky): {', '.join(ignored)}."
    return v, text


def verdict_h3(m):
    s = m['H3']['spatial']
    t = m['H3']['temporal_min_over_max']
    tail = (f" Temporal min/max depth of edge pixels (not in the verdict): {pct(t['below_low'])} < {H3_LOW_RATIO}, "
            f"{pct(t['below_mid'])} < {H3_MID_RATIO}.")
    if s['pixels'] < MIN_PIXELS:
        return 'inconclusive', (f"only {s['pixels']} sky-edge pixels have interior non-sky pixels nearby "
                                f"({m['H3']['edge_pixels_without_interior_reference']} without; thin geometry)." + tail)
    if s['below_mid'] >= H3_SUPPORT_MID or s['below_low'] >= H3_SUPPORT_LOW:
        v = 'supports'
    elif s['below_mid'] < H3_AGAINST_MID and s['below_low'] < H3_AGAINST_LOW:
        v = 'against'
    else:
        v = 'inconclusive'
    return v, (f"edge depth / nearby interior depth: {pct(s['below_low'])} below {H3_LOW_RATIO}, "
               f"{pct(s['below_mid'])} below {H3_MID_RATIO} (median {num(s['percentiles_5_50_95'][1])}, "
               f"{s['pixels']} px). Threshold: supports if >= {pct(H3_SUPPORT_LOW)} below {H3_LOW_RATIO} or >= "
               f"{pct(H3_SUPPORT_MID)} below {H3_MID_RATIO}; against if < {pct(H3_AGAINST_LOW)} and < "
               f"{pct(H3_AGAINST_MID)}." + tail)


def verdict_h4(m):
    bands = m['H2']['bands']
    sky = bands['sky'].get('motion')
    mags = m['H4']['motion_magnitude']
    tail = (f" Median |motion| px: sky {num(mags['sky_interior'])}, sky-edge non-sky "
            f"{num(mags['sky_band_non_sky'])}, interior {num(mags['interior'])}.")
    if not sky or sky['defined'] < MIN_PIXELS:
        return 'inconclusive', (f"only {sky['defined'] if sky else 0} sky-edge pixels where local sky and non-sky "
                                f"motion differ by > {MOTION_MIN_SEPARATION} px; nothing to compare." + tail)
    control = control_rate(bands, 'motion')
    v = grade(sky['rate'] - control, H4_SUPPORT_RATE, H4_AGAINST_RATE)
    return v, (f"motion vote disagrees with depth at {pct(sky['rate'])} of {sky['defined']} sky-edge pixels "
               f"({sky['class1_vote0']} non-sky pixels carry sky-like motion) vs {pct(control)} hill control. "
               f"Threshold: supports if minus control >= {pct(H4_SUPPORT_RATE)}, against < {pct(H4_AGAINST_RATE)}." + tail)


def camera_note(m):
    c = m['camera']
    if not c['known']:
        return 'camera motion unknown (world_to_view missing)'
    return (f"max translation {num(c['max_translation'])} units, rotation {num(c['max_rotation_deg'])} deg "
            f"= {num(c['max_rotation_px'])} px (still if <= {STILL_TRANSLATION} and <= {STILL_ROTATION_PX} px)")


def verdict_h5(m):
    sky, hill = m['H5']['sky_edge_band'], m['H5']['hill_edge_band']
    text = (f"excess log-luminance std beyond class switching, / contrast, p90: sky edge "
            f"{num(sky.get('excess_p90'))} vs hill edge {num(hill.get('excess_p90'))} (raw std p90 "
            f"{num(sky['std_p90'])} vs {num(hill['std_p90'])}; std/contrast p90 "
            f"{num(sky.get('std_over_contrast_p90'))} vs {num(hill.get('std_over_contrast_p90'))}). "
            f"Threshold: supports if the difference >= {H5_SUPPORT_EXCESS}, against < {H5_AGAINST_EXCESS}.")
    if not m['camera']['still']:
        return 'inconclusive', 'skipped, camera moved: ' + camera_note(m) + '. ' + text
    if min(sky.get('pixels_with_contrast', 0), hill.get('pixels_with_contrast', 0)) < MIN_PIXELS:
        return 'inconclusive', 'too few hill-edge or sky-edge pixels with contrast. ' + text
    if sky['excess_p90'] is None or hill['excess_p90'] is None:
        return 'inconclusive', text
    return grade(sky['excess_p90'] - hill['excess_p90'], H5_SUPPORT_EXCESS, H5_AGAINST_EXCESS), text


def verdict_h6(m):
    h = m['H6']
    fired = sorted({f'{name} {kind}' for name, entry in h['counts'].items() for kind, c in entry.items()
                    if c['sky_edge_band'] and not (kind == 'huge' and name in HUGE_EXEMPT)})
    total = sum(c['total'] for entry in h['counts'].values() for c in entry.values())
    v = 'supports' if h['bad_in_sky_band_for_verdict'] > 0 else 'against'
    lum = h['luminance']
    return v, (f"{h['bad_in_sky_band_for_verdict']} bad values in the sky-edge band"
               f"{' (' + ', '.join(fired) + ')' if fired else ''}, {total} in all inputs over the whole frames "
               f"(pixel-frames). Sky luminance p50/p99.9/max {num(lum.get('sky', {}).get('p50'))}/"
               f"{num(lum.get('sky', {}).get('p99_9'))}/{num(lum.get('sky', {}).get('max'))}. "
               "Threshold: supports if any NaN/Inf/negative/>65000 value is in the band.")


def verdict_h7(m):
    h = m['H7']
    if not h.get('available'):
        return 'inconclusive', 'no jitter in the manifest.'
    if h['trusted_pairs'] < 2:
        return 'inconclusive', (f"only {h['trusted_pairs']} frame pairs with a usable static-interior shift fit "
                                f"(>= {H7_MIN_WEIGHT_PIXELS} px, converged, condition <= {H7_MAX_CONDITION:g}).")
    others = ', '.join(f'{name} {num(r, 2)} px' for name, r in h['mean_residual'].items()
                       if name != h['best_convention'])
    text = (f"best fit '{h['best_convention']}' with mean residual {num(h['best_residual'], 2)} px over "
            f"{h['trusted_pairs']} pairs (others: {others}); fitted scale x {num(h['fitted_scale'][0])}, "
            f"y {num(h['fitted_scale'][1])}; shift measured by a masked least-squares fit on static interior "
            f"pixels (whole-frame phase correlation: residual {num(h['phase_correlation_residual_best_convention'], 2)} px). "
            f"Threshold: supports (mismatch) if >= {H7_SUPPORT_RESIDUAL} px, "
            f"against if < {H7_AGAINST_RESIDUAL} px.")
    if not m['camera']['still']:
        return 'inconclusive', 'skipped, camera moved: ' + camera_note(m) + '. ' + text
    return grade(h['best_residual'], H7_SUPPORT_RESIDUAL, H7_AGAINST_RESIDUAL), text


def verdict_h8(m):
    h = m['H8']
    sky_c, hill_c = h['sky_coherence'], h['hill_coherence']
    fit = h.get('jitter_fit', {})
    tiles = (h.get('jitter_fit_tiles') or {}).get('sky') or {}
    share, in_tiles = fit.get('sky', {}).get('share'), tiles.get('flips_in_dither_like_tiles')
    new = sum(r.get('new_vs_previous', 0) for r in h['isolated'])
    text = (f"share of static sky/non-sky flips that does not shrink with the jitter difference "
            f"{num(share, 2)} (hill near/far {num(fit.get('hill', {}).get('share'), 2)}); "
            f"{tiles.get('dither_like_tiles', 0)} of {tiles.get('tiles_fitted', 0)} {H8_TILE}-px tiles look "
            f"dithered, holding {pct(in_tiles)} of the static flips. Coherence (reported, not decisive: dense "
            f"dither has same-direction neighbours by chance and few isolated pixels): same-direction 4-neighbour "
            f"{pct(sky_c['coherent4_fraction'])}, 8-neighbour {pct(sky_c['coherent8_fraction'])}, "
            f"{pct(sky_c['isolated_fraction'])} of flips isolated, {new} isolated pixels new vs the previous frame. "
            f"Threshold: supports if share >= {H8_SUPPORT_SHARE} or dither-like tiles hold >= "
            f"{pct(H8_SUPPORT_TILE_FRACTION)} of flips; against if share < {H8_AGAINST_SHARE} and < "
            f"{pct(H8_AGAINST_TILE_FRACTION)}.")
    if not m['camera']['still']:
        return 'inconclusive', 'skipped, camera moved (jitter and motion flips can not be separated): ' + text
    if share is None:
        return 'inconclusive', 'too few static flips for the jitter fit. ' + text
    if verdict_h4(m)[0] == 'supports':   # the fit trusts motion vectors to leave out moving geometry
        return 'inconclusive', 'H4 fired, so moving geometry may pass as static here: ' + text
    in_tiles = in_tiles or 0.0
    if share >= H8_SUPPORT_SHARE or in_tiles >= H8_SUPPORT_TILE_FRACTION:
        return 'supports', text
    if share < H8_AGAINST_SHARE and in_tiles < H8_AGAINST_TILE_FRACTION:
        return 'against', text
    return 'inconclusive', text


def verdict_h9(m):
    h = m['H9']
    ratio, rank = h['sky_over_edge_non_sky'], h['edge_sky_brightness_rank_among_non_sky']
    pre = [p for p in h['pre_exposure'] if p is not None]
    text = (f"sky median luminance {num((h['sky'] or {}).get('p50'))}, sky-edge non-sky "
            f"{num((h['sky_edge_non_sky'] or {}).get('p50'))}, interior non-sky {num((h['interior_non_sky'] or {}).get('p50'))} "
            f"(ratio sky / edge non-sky {num(ratio)}); sky-edge sky pixels brighter than {pct(rank)} of non-sky pixels; "
            f"{pct(h['fraction_over_10x_local_non_sky'])} / {pct(h['fraction_over_100x_local_non_sky'])} of sky-edge band "
            f"pixels > 10x / 100x the local non-sky median; pre_exposure {num(min(pre)) if pre else 'n/a'}.."
            f"{num(max(pre)) if pre else 'n/a'}. Threshold: supports (a lead to test in game) if ratio >= "
            f"{H9_SKY_RATIO:g} and rank >= {pct(H9_BRIGHT_RANK)}; otherwise inconclusive.")
    v = 'supports' if ratio is not None and rank is not None and ratio >= H9_SKY_RATIO and rank >= H9_BRIGHT_RANK \
        else 'inconclusive'
    return v, text


def h10_equality(h):
    """Per channel and region: (rates x, rates y, min excess over x and y, min ratio)."""
    out = {}
    for name in H10_CHANNELS:
        for region in ('sky_edge_band', 'interior'):
            if name not in h['equal_pairs']['x']:
                continue
            ex = equality_excess(h['equal_pairs']['x'][name][region])
            ey = equality_excess(h['equal_pairs']['y'][name][region])
            if ex[0] is None or ey[0] is None:
                continue
            out[(name, region)] = (ex[0], ey[0], min(ex[1], ey[1]), min(ex[2], ey[2]), max(ex[1], ey[1]))
    return out


def verdict_h10(m):
    h = m['H10']
    ratios = {}
    for label in ('x', 'y'):
        for size in H10_BLOCKS:
            a = h['axes'][label][size]
            if sum(a['counts']) >= H10_MIN_MIXED and a['ratio'] is not None:
                ratios[f'{size} px along {label}'] = (a['ratio'], a['best_phase'])
    eq = h10_equality(h)
    share, control = h['snapped_tile_share'], h.get('control_tile_share') or 0.0
    coarse = [f"{n} ({r.replace('_', ' ')}) {pct(rx[0])}/{pct(rx[1])} along x, {pct(ry[0])}/{pct(ry[1])} along y"
              for (n, r), (rx, ry, lo, ratio, hi) in eq.items() if lo >= H10_EQUAL_EXCESS and ratio >= H10_EQUAL_RATIO]
    text = (f"best-phase / other-phase mixed sky-edge blocks: "
            f"{', '.join(f'{k} {num(r, 2)} (phase {p})' for k, (r, p) in ratios.items()) or 'too few edges'}; "
            f"{h['tiles_snapped']} of {h['tiles_fitted']} {H10_TILE}-px tiles snapped to the 2-px grid in x and y "
            f"at phase {tuple(h.get('tile_phase') or ())}, holding {pct(share)} of the mixed blocks (best other "
            f"phase pair, the chance level: {pct(control)}); identical same-surface pairs on one grid phase only: "
            f"{'; '.join(coarse) or 'none'}. Only edges a pixel shader decides (alpha-tested leaves) snap to the "
            f"VRS grid; static rasterised edges snap by chance and spread over all four phase pairs, so a low "
            f"excess can still be real in a scene with few trees. Threshold: supports if a ratio <= "
            f"{H10_SUPPORT_RATIO} with a tile excess over chance >= {pct(H10_AGAINST_TILE_SHARE)}, or an excess >= "
            f"{pct(H10_SUPPORT_TILE_SHARE)}, both with the best pair >= {H10_TILE_CONTROL:g}x the chance level, "
            f"or a channel identical on one phase (excess >= {pct(H10_EQUAL_EXCESS)} and x{H10_EQUAL_RATIO:g} in "
            f"x and y); against if every ratio >= {H10_AGAINST_RATIO}, tile excess < {pct(H10_AGAINST_TILE_SHARE)} "
            f"and no equality excess >= {pct(H10_EQUAL_AGAINST)}.")
    if not ratios and not eq:
        return 'inconclusive', 'too few sky edges and pixel pairs. ' + text
    low = min((r for r, _ in ratios.values()), default=None)
    share = share or 0.0
    excess = share - control
    above_chance = share >= H10_TILE_CONTROL * control
    if coarse or (above_chance and (excess >= H10_SUPPORT_TILE_SHARE or
                                    (low is not None and low <= H10_SUPPORT_RATIO and
                                     excess >= H10_AGAINST_TILE_SHARE))):
        return 'supports', text
    if (low is None or low >= H10_AGAINST_RATIO) and excess < H10_AGAINST_TILE_SHARE and \
            all(hi < H10_EQUAL_AGAINST for (_, _, _, _, hi) in eq.values()):
        return 'against', text
    return 'inconclusive', text


HYPOTHESES = {'H0': ('RR output noise at sky edges', verdict_h0),
              'H1': ('class flicker', verdict_h1), 'H2': ('inconsistent inputs at the edge', verdict_h2),
              'H3': ('depth bleed', verdict_h3), 'H4': ('motion mismatch', verdict_h4),
              'H5': ('input colour flicker', verdict_h5), 'H6': ('bad values', verdict_h6),
              'H7': ('jitter mismatch', verdict_h7), 'H8': ('stochastic coverage', verdict_h8),
              'H9': ('HDR scale', verdict_h9), 'H10': ('coarse shading (VRS)', verdict_h10)}


def verdicts(metrics):
    out = {}
    for key, (title, function) in HYPOTHESES.items():
        verdict, text = function(metrics)
        out[key] = {'title': title, 'verdict': verdict, 'text': text}
    return out


# ---- images --------------------------------------------------------------------------------
STATUS_COLORS = {0: (255, 0, 255), 1: (110, 110, 110), 2: (90, 160, 255), 3: (255, 170, 0),
                 4: (255, 40, 40)}
FLIP_COLORS = np.array([(0, 0, 0), (40, 90, 255), (0, 200, 255), (0, 230, 120), (230, 230, 0),
                        (255, 140, 0), (255, 40, 40), (255, 0, 200), (255, 255, 255)], np.uint8)
RAMP = [(0.0, (60, 20, 120)), (0.33, (220, 40, 60)), (0.66, (255, 170, 20)), (1.0, (255, 255, 210))]
MIN_IMAGE_WIDTH = 640       # smaller maps are upscaled (nearest) to at least this width
MAX_TILE_WIDTH = 960        # disagreement tiles are max-pooled down to at most this width
CROP_SIZE, CROP_ZOOM = 96, 4


def font(size=14):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def auto_exposure(color):
    lum = luminance(color)
    lum = lum[np.isfinite(lum) & (lum > 0)]
    return 0.18 / float(np.exp(np.log(lum).mean())) if lum.size else 1.0


def tonemap(color, exposure):
    c = np.nan_to_num(color, nan=0.0, posinf=1e4, neginf=0.0) * exposure
    c = np.clip(c / (1 + c), 0, 1) ** (1 / 2.2)
    return (c * 255 + 0.5).astype(np.uint8)


def ramp(t):
    """Map t (0..1) to an RGB uint8 heat colour."""
    t = np.clip(np.nan_to_num(t), 0, 1)
    out = np.zeros(t.shape + (3,), np.float32)
    for (t0, c0), (t1, c1) in zip(RAMP, RAMP[1:]):
        f = np.clip((t - t0) / (t1 - t0), 0, 1)[..., None]
        inside = ((t >= t0) & (t <= t1))[..., None]
        out = np.where(inside, np.array(c0) * (1 - f) + np.array(c1) * f, out)
    return out.astype(np.uint8)


def dim_background(color, exposure, factor=0.3):
    gray = tonemap(color, exposure).mean(axis=-1) * factor
    return np.repeat(gray[..., None], 3, axis=-1).astype(np.uint8)


def upscale(rgb, minimum_width=MIN_IMAGE_WIDTH):
    factor = max(1, math.ceil(minimum_width / rgb.shape[1]))
    return np.repeat(np.repeat(rgb, factor, axis=0), factor, axis=1) if factor > 1 else rgb


def pool_max(a, factor):
    if factor <= 1:
        return a
    h, w = a.shape[0] // factor * factor, a.shape[1] // factor * factor
    a = a[:h, :w]
    return a.reshape(h // factor, factor, w // factor, factor, *a.shape[2:]).max(axis=(1, 3))


def save_with_legend(path, rgb, title, entries):
    """Save rgb with a caption strip: title, then coloured squares with labels (wrapping)."""
    image = Image.fromarray(upscale(rgb))
    f = font()
    probe = ImageDraw.Draw(image)
    lines, line, x = [], [], 8
    for color, label in entries:
        width = 22 + int(probe.textlength(label, font=f)) + 14
        if x + width > image.width and line:
            lines.append(line)
            line, x = [], 8
        line.append((x, color, label))
        x += width
    if line:
        lines.append(line)
    strip = Image.new('RGB', (image.width, 24 + 20 * len(lines)), (0, 0, 0))
    draw = ImageDraw.Draw(strip)
    draw.text((8, 4), title, fill=(255, 255, 255), font=f)
    for row, items in enumerate(lines):
        y = 24 + 20 * row
        for x, color, label in items:
            draw.rectangle((x, y + 2, x + 14, y + 16), fill=tuple(int(c) for c in color))
            draw.text((x + 20, y), label, fill=(255, 255, 255), font=f)
    out = Image.new('RGB', (image.width, image.height + strip.height))
    out.paste(image, (0, 0))
    out.paste(strip, (0, image.height))
    out.save(path)


def status_rgb(status):
    out = np.full(status.shape + (3,), 255, np.uint8)
    for value, color in STATUS_COLORS.items():
        out[status == value] = color
    return out


def depth_class_rgb(depth, sky):
    """Sky in light blue; non-sky grey by log depth (nearer = brighter) within this image."""
    out = np.zeros(depth.shape + (3,), np.uint8)
    out[sky] = (110, 170, 255)
    solid = ~sky & np.isfinite(depth) & (depth > 0)
    if solid.any():
        log_d = np.log(depth[solid])
        lo, hi = log_d.min(), log_d.max()
        g = 80 + 175 * ((log_d - lo) / (hi - lo) if hi > lo else 0.5)
        out[solid] = np.repeat(g[:, None], 3, axis=1).astype(np.uint8)
    out[~sky & ~solid] = (255, 0, 255)
    return out


def alpha_rgb(alpha):
    out = np.full(alpha.shape + (3,), (70, 0, 0), np.uint8)
    ok = np.isfinite(alpha)
    out[ok] = np.repeat((alpha[ok] * 255 + 0.5).astype(np.uint8)[:, None], 3, axis=1)
    return out


def choose_crop(crop, flicker, sky_band, shape):
    h, w = shape
    if crop:
        x, y, cw, ch = crop
        x, y = max(0, min(x, w - 1)), max(0, min(y, h - 1))
        return x, y, max(1, min(cw, w - x)), max(1, min(ch, h - y))
    mask = flicker if flicker.any() else sky_band
    ch, cw = min(CROP_SIZE, h), min(CROP_SIZE, w)
    c = np.pad(mask.astype(np.int64).cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    sums = c[ch:, cw:] - c[:-ch, cw:] - c[ch:, :-cw] + c[:-ch, :-cw]
    y, x = np.unravel_index(np.argmax(sums), sums.shape)
    return int(x), int(y), cw, ch


def crop_alpha(frame, rect):
    """Colour coverage alpha (sky vs non-sky references) for every pixel of the crop; NaN where
    the 7x7 window lacks one of the classes."""
    x, y, w, h = rect
    ys, xs = np.mgrid[y:y + h, x:x + w]
    local = local_measurements(frame['color'], None, frame['depth'], ys.ravel(), xs.ravel(), 'sky')
    alpha = np.where(local['alpha_ok'], local['alpha'], np.nan)
    return alpha.reshape(h, w)


def write_crop_strip(path, frames, regions, rect, exposure):
    x, y, w, h = rect
    rows = ['colour', 'depth class', 'status', 'colour alpha']
    tile_w, tile_h, gap, left, top = w * CROP_ZOOM, h * CROP_ZOOM, 4, 110, 22
    image = Image.new('RGB', (left + len(frames) * (tile_w + gap), top + len(rows) * (tile_h + gap)),
                      (0, 0, 0))
    draw, f = ImageDraw.Draw(image), font()
    window = (slice(y, y + h), slice(x, x + w))
    for i, (frame, reg) in enumerate(zip(frames, regions)):
        tiles = [tonemap(frame['color'][window], exposure),
                 depth_class_rgb(frame['depth'][window], has(reg, SKY)[window]),
                 status_rgb(frame['status'][window]) if 'status' in frame else
                 np.zeros((h, w, 3), np.uint8),
                 alpha_rgb(crop_alpha(frame, rect))]
        px = left + i * (tile_w + gap)
        draw.text((px + 4, 4), f'frame {i}', fill=(255, 255, 255), font=f)
        for r, tile in enumerate(tiles):
            big = np.repeat(np.repeat(tile, CROP_ZOOM, axis=0), CROP_ZOOM, axis=1)
            image.paste(Image.fromarray(big), (px, top + r * (tile_h + gap)))
    for r, label in enumerate(rows):
        draw.text((6, top + r * (tile_h + gap) + 4), label, fill=(255, 255, 255), font=f)
    draw.text((6, 4), f'x={x} y={y} {w}x{h}', fill=(200, 200, 200), font=f)
    image.save(path)


def write_images(out_dir, frames, regions, flips, vote_maps, std_map, rect):
    exposure = auto_exposure(frames[0]['color'])
    n = len(frames)
    background = dim_background(frames[0]['color'], exposure)
    written = []

    def done(name):
        written.append(name)
        return out_dir / name

    Image.fromarray(upscale(tonemap(frames[0]['color'], exposure))).save(done('color_frame0.png'))
    if 'status' in frames[0]:
        save_with_legend(done('status_frame0.png'), status_rgb(frames[0]['status']), 'status, frame 0',
                         [(c, f'{v} {name}') for (v, c), name in
                          zip(STATUS_COLORS.items(), ('padding', 'ordinary', 'sky', 'special', 'invalid'))]
                         + [((255, 255, 255), 'other')])
    rgb = np.where((flips > 0)[..., None], FLIP_COLORS[np.minimum(flips, len(FLIP_COLORS) - 1)], background)
    save_with_legend(done('flip_count.png'), rgb, f'sky/non-sky flips over {n} frames',
                     [(FLIP_COLORS[k], str(k)) for k in range(1, min(n, len(FLIP_COLORS)))])
    scale = 1.0
    rgb = np.where(np.isfinite(std_map)[..., None] & (std_map > 0.01)[..., None],
                   ramp(std_map / scale), background)
    save_with_legend(done('temporal_std.png'), rgb, 'temporal std of log luminance',
                     [(ramp(np.array(t)), f'{t * scale:.2f}') for t in (0.05, 0.25, 0.5, 0.75, 1.0)])

    factor = max(1, math.ceil(flips.shape[1] / MAX_TILE_WIDTH))
    names = [k for k in ABSOLUTE_VOTES + LOCAL_VOTES if k in vote_maps]
    if names:
        small_bg = background[::factor, ::factor][:flips.shape[0] // factor, :flips.shape[1] // factor]
        tiles = []
        for name in names:
            counts = pool_max(vote_maps[name], factor)
            tile = np.where((counts > 0)[..., None], ramp(counts / n), small_bg[:counts.shape[0], :counts.shape[1]])
            tile = upscale(tile, 320)
            label = Image.fromarray(tile)
            ImageDraw.Draw(label).text((6, 4), f'{name}: {int(vote_maps[name].sum())} px-frames',
                                       fill=(255, 255, 255), font=font())
            tiles.append(np.asarray(label))
        columns = min(3, len(tiles))
        rows_n = math.ceil(len(tiles) / columns)
        th, tw = tiles[0].shape[:2]
        grid = np.zeros((rows_n * (th + 4), columns * (tw + 4), 3), np.uint8)
        for k, tile in enumerate(tiles):
            r, c = divmod(k, columns)
            grid[r * (th + 4):r * (th + 4) + th, c * (tw + 4):c * (tw + 4) + tw] = tile
        save_with_legend(done('disagreement.png'), grid,
                         'input sky vote disagrees with depth (count over frames; hill band: far/near or false sky)',
                         [(ramp(np.array(k / n)), str(k)) for k in range(1, n + 1)])
    write_crop_strip(done('crop_strip.png'), frames, regions, rect, exposure)
    return written


H0_MOVING_COLOR = (0, 60, 110)
H0_REGION_COLORS = {0: (0, 0, 0), 1: (255, 170, 0), 2: (0, 200, 120), 3: (110, 110, 110), 4: (90, 160, 255)}


def flicker_rgb(values, background, moving):
    """Heat colours where values > 0.01 (full scale H0_MAP_MAX), moving pixels marked, else background;
    in row chunks (ramp's temporaries are large at output size)."""
    rgb = background.copy()
    for s in range(0, rgb.shape[0], H0_ROWS):
        v, out = values[s:s + H0_ROWS], rgb[s:s + H0_ROWS]
        with np.errstate(invalid='ignore'):
            heat = np.isfinite(v) & (v > 0.01)
        out[heat] = ramp(v[heat] / H0_MAP_MAX)
        out[moving[s:s + H0_ROWS]] = H0_MOVING_COLOR
    return rgb


def write_h0_images(out_dir, frames, maps, n_pairs):
    """h0_output_flicker.png (whole output frame) and h0_crop.png (the crop window at output size)."""
    exposure = auto_exposure(frames[0]['color'])   # rr_output shares the input's (pre-exposed) scale
    color = frames[maps['frame']]['rr_output']
    background = np.concatenate([dim_background(color[s:s + H0_ROWS, :, :3], exposure)
                                 for s in range(0, color.shape[0], H0_ROWS)])
    legend = [(ramp(np.array(t / H0_MAP_MAX)), f'{t:g}') for t in (0.05, 0.1, 0.25, 0.5)]
    save_with_legend(out_dir / 'h0_output_flicker.png', flicker_rgb(maps['output'], background, maps['moving']),
                     f'RR output: mean |delta log luminance| per frame, static pixels, {n_pairs} frame pairs',
                     legend + [(H0_MOVING_COLOR, 'moving (left out)')])
    window = maps['window']
    labels = maps['labels'][window]
    tiles = [tonemap(color[window][..., :3], exposure),
             flicker_rgb(maps['output'][window], background[window], maps['moving'][window]),
             flicker_rgb(maps['input'][window], background[window], maps['moving'][window]),
             np.array([H0_REGION_COLORS[k] for k in range(5)], np.uint8)[labels]]
    zoom = max(1, H0_CROP_TILE // max(labels.shape))
    tiles = [np.repeat(np.repeat(t, zoom, axis=0), zoom, axis=1) for t in tiles]
    h, w = tiles[0].shape[:2]
    strip = np.zeros((h, len(tiles) * (w + 4) - 4, 3), np.uint8)
    for k, (tile, label) in enumerate(zip(tiles, ('RR output', 'output flicker', 'input flicker', 'regions'))):
        image = Image.fromarray(tile)
        ImageDraw.Draw(image).text((4, 2), label, fill=(255, 255, 255), font=font())
        strip[:, k * (w + 4):k * (w + 4) + w] = np.asarray(image)
    x, y = window[1].start, window[0].start
    save_with_legend(out_dir / 'h0_crop.png', strip, f'crop at output size: x={x} y={y} {labels.shape[1]}x{labels.shape[0]}',
                     legend + [(H0_REGION_COLORS[v], name.replace('_', ' ')) for name, v in H0_REGIONS.items()])
    return ['h0_output_flicker.png', 'h0_crop.png']


# ---- report --------------------------------------------------------------------------------

def table(headers, rows):
    lines = ['| ' + ' | '.join(headers) + ' |', '|' + '---|' * len(headers)]
    lines += ['| ' + ' | '.join(str(c) for c in row) + ' |' for row in rows]
    return '\n'.join(lines)


def h0_report(h):
    out = ["## H0: is the noise visible in RR's output, and is it specific to sky edges?", '']
    if not h['available']:
        return out + [f"Not measured: {h['note']}.", '']
    out += [f"rr_output {h['output_width']}x{h['output_height']} = render x {h['scale'][0]:.4g}, y {h['scale'][1]:.4g} "
            f"(regions and masks mapped nearest-neighbour). Frames with rr_output: {h['frames_with_output']}; RR reset "
            f"(skipped): {h['frames_skipped_reset'] or 'none'}; other size (skipped): {h['frames_other_size'] or 'none'}; "
            f"pairs used: {h['pairs']}.", '']
    if 'regions' in h:
        out += [f"Metric: per-pixel mean |delta log luminance| between consecutive frames, over the pairs in which "
                f"the pixel is static in both frames (no motion > {STATIC_MOTION} px within 2 render px); each pair's "
                f"median delta (a global exposure change) is removed first. Share = pixels with a mean >= {H0_VISIBLE:g}. "
                "Input = the colour input at the matching render pixel, same pairs and masks.", '']
        rows = [[name, row['output']['pixels'], num(row['output']['median']), num(row['output']['p90']),
                 pct(row['output']['visible_fraction']), num(row['input']['median']), num(row['input']['p90']),
                 pct(row['input']['visible_fraction']), num(h['output_over_input_p90'][name]),
                 num(row['output_all_pixels']['p90']), row['moving_excluded']] for name, row in h['regions'].items()]
        out += [table(['region (output px)', 'pixels', 'output median', 'output p90', 'output share',
                       'input median', 'input p90', 'input share', 'output / input p90', 'output p90 incl. moving',
                       'moving, left out'], rows), '']
        so, si = h['sky_over_hill']['output'], h['sky_over_hill']['input']
        out += [f"Sky edge / hill edge: output p90 {num(so['p90'])}, share {num(so['visible_fraction'])}; input p90 "
                f"{num(si['p90'])}, share {num(si['visible_fraction'])}; output ratio / input ratio {num(h['lift'])}.", '',
                f"Global offset removed per pair (log luminance): output {[round(x, 4) for x in h['global_offset_output']]}, "
                f"input {[round(x, 4) for x in h['global_offset_input']]}. Median output / input luminance in the "
                f"interior (frame {h['pairs'][0][0]}): {num(h['output_over_input_luminance_interior'])}.", '']
        cr = h['crop']
        x, y, w, hh = cr['window_output_px']
        out += [f"Crop window at output size (x={x} y={y} {w}x{hh}):", '', table(
            ['region', 'output px', 'output median', 'output p90', 'output share', 'input p90', 'input share'],
            [[name, row['output']['pixels'], num(row['output']['median']), num(row['output']['p90']),
              pct(row['output']['visible_fraction']), num(row['input']['p90']), pct(row['input']['visible_fraction'])]
             for name, row in cr['regions'].items()]), '']
    bad = h['output_bad_values']
    out += ['rr_output bad values (pixel-frames, total / in sky-edge band): ' +
            ', '.join(f"{k} {c['total']} / {c['sky_edge_band']}" for k, c in bad.items()) + '.', '']
    return out


def report_text(m):
    c, v = m['capture'], m['verdicts']
    output = f", output {c['output_width']}x{c['output_height']}" if c.get('output_width') else ''
    out = ['# Sky-edge analysis of RR inputs', '',
           f"Capture: {c['frames']} frames, render {c['width']}x{c['height']}{output}, preset {c['preset']}, "
           f"quality {c['quality']}, band radius {c['radius']} px. Camera "
           f"{'still' if m['camera']['still'] else 'moving or unknown'}: {camera_note(m)}.", '']
    if c.get('tests'):
        out += ['Tests in the capture: ' + '; '.join(c['tests']) + '.', '']
    out += ['## Summary', '']
    out += [f"- **{k} {h['title']}: {h['verdict']}.** {h['text']}" for k, h in v.items()]
    out += ['', '"supports" means the capture shows the problem the hypothesis describes.', '']

    r = m['regions']
    out += ['## Regions', '', table(
        ['region', 'mean px per frame', 'union / all frames'],
        [['sky (depth == 0)', f"{r['mean']['sky']:.0f}", f"{r['union']['sky_all_frames_outside_band']} (sky in all frames, outside band)"],
         ['sky-edge band', f"{r['mean']['sky_edge_band']:.0f}", r['union']['sky_edge_band']],
         ['  of which non-sky', f"{r['mean']['sky_edge_band_non_sky']:.0f}", ''],
         ['hill-edge band', f"{r['mean']['hill_edge_band']:.0f}", r['union']['hill_edge_band']],
         ['interior non-sky', f"{r['mean']['interior']:.0f}", f"{r['union']['interior_all_frames']} (interior in all frames)"],
         ['padding (status 0)', f"{r['mean']['invalid_padding']:.0f}", '']]), '']
    rows = [[i, s['depth_sky_status_not'], s['status_sky_depth_not'], s['in_band_depth_sky_status_not'],
             s['in_band_status_sky_depth_not'], ' '.join(map(str, s['band_status_histogram'])),
             s['status_out_of_range']] for i, s in enumerate(m['sky_vs_status'])]
    if rows:
        out += ['Depth sky (== 0) versus status sky (== 2):', '', table(
            ['frame', 'depth sky, status not', 'status sky, depth not', 'in band: depth sky, status not',
             'in band: status sky, depth not', 'band status 0..4', 'status > 4'], rows), '']

    out += h0_report(m['H0'])
    h1 = m['H1']
    out += ['## H1 class flicker', '',
            f"Flip-count histogram over the sky-edge band union (0..{c['frames'] - 1} flips): "
            f"{h1['flip_histogram_in_sky_band']}. Pixels with any flip: {h1['pixels_with_any_flip']}; "
            f"flicker (>= {FLICKER_MIN_FLIPS}): {h1['flicker_pixels']}, thin: {h1['thin_flicker_pixels']}.", '']

    h2 = m['H2']
    out += ['## H2 inconsistent inputs (and H4 motion)', '',
            'Sky band: truth = depth == 0; "missed sky" = depth sky but input says not, "false sky" = the '
            'reverse. Hill band: status/normal/albedo columns give the false-sky rate (depth is non-sky '
            'there); colour/motion give the far/near disagreement with the local depth split. Best shift: '
            'the input at (x, y) agrees best with depth at (x + dx, y + dy).', '']
    rows = []
    for name in ABSOLUTE_VOTES + LOCAL_VOTES:
        s = h2['bands']['sky'].get(name)
        if not s:
            continue
        hb = h2['bands']['hill'].get(name, new_counts() | {'rate': None})
        shift = h2['best_shift'].get(name, {})
        rows.append([name, s['defined'], pct(s['rate']), s['class0_vote1'], s['class1_vote0'], s['at_status4'],
                     f"({shift.get('dx')}, {shift.get('dy')}) {pct(shift.get('rate'))}",
                     hb['defined'], pct(hb['rate']), pct(h2['interior_false_sky'].get(name))])
    out += [table(['input', 'sky band px-frames', 'disagree', 'missed sky', 'false sky', 'at status 4',
                   'best shift (rate)', 'hill px-frames', 'hill rate', 'interior false sky'], rows), '']
    refs = h2['sky_references'][0] if h2['sky_references'] else {}
    out += ['Frame 0 sky reference values (median over sky pixels outside the band): ' +
            (', '.join(f"{k} {[round(x, 4) for x in val]}" for k, val in refs.items()) or 'none') + '.', '']
    hist = h2['alpha_histogram']
    out += ['Colour coverage alpha (0 = sky colour, 1 = non-sky colour), counts per 0.1 bin:', '', table(
        ['pixels'] + [f"{a:.1f}" for a in hist['bin_edges'][:-1]],
        [[k] + hist[k] for k in ('sky_band_sky', 'sky_band_non_sky', 'hill_band')]), '']
    b = h2['blended']
    out += [f"Blended ({BLEND_RANGE[0]}..{BLEND_RANGE[1]}) at sky boundary pixels: {b['sky_boundary']['blended']} of "
            f"{b['sky_boundary']['defined']} ({pct(b['sky_boundary']['fraction'])}); at hill-edge pixels: "
            f"{b['hill_edge']['blended']} of {b['hill_edge']['defined']} ({pct(b['hill_edge']['fraction'])}).", '']
    out += ['Status 4 (invalid) pixels, which keep zero guides (their zero normals vote "sky"):', '', table(
        ['frame', 'total', 'sky-edge band', 'hill-edge band', 'elsewhere'],
        [[i, s['total'], s['sky_edge_band'], s['hill_edge_band'], s['elsewhere']]
         for i, s in enumerate(h2['status4'])]), '']
    mags = m['H4']['motion_magnitude']
    out += ['Median |motion| (px, median over frames): ' +
            ', '.join(f'{k} {num(val)}' for k, val in mags.items()) +
            f". motion_vector_scale per frame: {c['motion_vector_scale']} (motion used as stored, render px).", '']

    h3 = m['H3']
    for key, title in (('spatial', 'edge depth / median nearby interior depth'),
                       ('temporal_min_over_max', 'min / max non-sky depth over frames (edge pixels)')):
        s = h3[key]
        out += [f"## H3 {title}" if key == 'spatial' else f"H3 {title}:", '',
                f"{s['pixels']} px; 5/50/95%: {[num(x) for x in s['percentiles_5_50_95']]}; histogram "
                f"(edges {s['histogram_edges']}): {s['histogram']}.", '']
    out += [f"Edge pixels without interior reference (thin geometry): {h3['edge_pixels_without_interior_reference']}.", '']

    h5 = m['H5']
    keys = ['pixels', 'std_median', 'std_p90', 'contrast_median', 'std_over_contrast_median',
            'std_over_contrast_p90', 'excess_median', 'excess_p90']
    out += ['## H5 temporal log-luminance std', '', table(
        ['region'] + keys, [[name] + [num(row.get(k)) if isinstance(row.get(k), float) else row.get(k, '')
                                       for k in keys] for name, row in h5.items()]), '']

    h6 = m['H6']
    rows = [[name] + [f"{e[k]['total']} / {e[k]['sky_edge_band']}" if k in e else '-'
                      for k in ('nan', 'inf', 'negative', 'huge')] for name, e in h6['counts'].items()]
    out += ['## H6 bad values (pixel-frames: total / in sky-edge band)', '',
            table(['input', 'NaN', 'Inf', 'negative', f'> {HUGE_VALUE:g}'], rows), '',
            table(['luminance', 'p50', 'p99', 'p99.9', 'max'],
                  [[k] + [num(val[q]) for q in ('p50', 'p99', 'p99_9', 'max')] for k, val in h6['luminance'].items()]), '']

    h7 = m['H7']
    if h7.get('available'):
        def pair_xy(v):
            return '-' if v is None else f'({v[0]:.3f}, {v[1]:.3f})'

        rows = [[f"{p['pair'][0]}-{p['pair'][1]}", pair_xy(p['measured']), p['static_interior_pixels'],
                 num(p['condition']), 'yes' if p['trusted'] else 'no', pair_xy(p['jitter_delta']),
                 pair_xy(p['phase_correlation']), f"{p['peak']:.3f}"] +
                [f"{p['residual'][name]:.3f}" if 'residual' in p else '-' for name in JITTER_CONVENTIONS]
                for p in h7['pairs']]
        out += ['## H7 measured shift (frame i+1 content relative to frame i) vs jitter delta', '',
                'measured = masked least-squares fit on static interior pixels (used for the verdict); '
                'phase corr = whole-frame phase correlation (thin aliased edges and moving foliage can pull it). '
                'Residual columns: |measured - convention|, px.', '',
                table(['pair', 'measured (dx, dy)', 'fit px', 'condition', 'trusted', 'jitter delta',
                       'phase corr (dx, dy)', 'peak'] + list(JITTER_CONVENTIONS), rows), '']

    h8 = m['H8']
    out += ['## H8 stochastic coverage', '', 'Isolated pixels (class differs from all 8 neighbours):', '', table(
        ['frame', 'sky/non-sky: sky band', 'hill band', 'interior', 'other', 'near/far: sky band', 'hill band',
         'interior', 'other', 'new vs previous'],
        [[r['frame']] + list(r['sky_class'].values()) + list(r['depth_class'].values()) +
         [r.get('new_vs_previous', '')] for r in h8['isolated']]), '']
    for key in ('sky_coherence', 'hill_coherence'):
        s = h8[key]
        out += [f"{key.replace('_', ' ')}: {s['flips']} flips over consecutive pairs; same-direction 4-neighbour "
                f"{pct(s['coherent4_fraction'])}, 8-neighbour {pct(s['coherent8_fraction'])}, chance "
                f"{pct(s['chance8_fraction'])}, lift {num(s['coherence_lift'], 2)}, isolated {pct(s['isolated_fraction'])}.", '']
    for key, fit in h8.get('jitter_fit', {}).items():
        out += [f"Jitter fit ({key}): flips(i,k) = a + bx|djx| + by|djy| over {fit['pairs']} pairs, {fit['flips']} "
                f"static flips: a = {num(fit.get('intercept'))}, bx = {num(fit.get('slope_x'))}, by = "
                f"{num(fit.get('slope_y'))}, mean {num(fit.get('mean_flip_fraction'))}; share a / mean = {num(fit['share'], 2)}.", '']

    h9 = m['H9']
    out += ['## H9 HDR scale', '', table(['pixels', 'p50', 'p99'],
                                         [[k, num((h9[k] or {}).get('p50')), num((h9[k] or {}).get('p99'))]
                                          for k in ('sky', 'sky_edge_non_sky', 'interior_non_sky')]), '',
            f"pre_exposure per frame: {h9['pre_exposure']}; exposure_scale: {h9['exposure_scale']}.", '']

    h10 = m['H10']
    rows = [[f"{size} px along {label}", ' / '.join(map(str, a['counts'])), a['best_phase'], num(a['ratio'], 2)]
            for label in ('x', 'y') for size, a in h10['axes'][label].items()]
    out += ['## H10 coarse shading (VRS)', '', 'Mixed (sky and non-sky) blocks per grid phase, all frames:', '',
            table(['blocks', 'mixed per phase', 'best phase', 'best / median other'], rows), '']
    rows = []
    for (name, region), (rx, ry, lo, ratio, hi) in h10_equality(h10).items():
        rows.append([name, region.replace('_', ' '), ' / '.join(pct(r) for r in rx), ' / '.join(pct(r) for r in ry)])
    if rows:
        out += ['Identical same-surface non-sky pixel pairs per 2-px grid phase:', '',
                table(['channel', 'region', 'along x (phase 0 / 1)', 'along y (phase 0 / 1)'], rows), '']
    if h10['snapped_tiles']:
        out += ['Snapped tiles (top-left y, x): ' + ', '.join(f'({y}, {x})' for y, x in h10['snapped_tiles']) + '.', '']

    if m['optional_inputs']:
        rows = [[name, region] + ([num(val[k]) for k in ('min', 'median', 'max')] if val else ['-'] * 3)
                for name, regions in m['optional_inputs'].items() for region, val in regions.items()]
        out += ['## Optional inputs', '', table(['input', 'region', 'min', 'median', 'max'], rows), '']

    out += ['## Images', ''] + [f'- `{name}`' for name in m['images']] + \
           [f"- crop strip window: x={m['crop'][0]} y={m['crop'][1]} {m['crop'][2]}x{m['crop'][3]}", '']
    return '\n'.join(out)


def jsonable(value):
    if isinstance(value, dict):
        return {str(k): jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [jsonable(v) for v in value]
    if isinstance(value, np.ndarray):
        return jsonable(value.tolist())
    if isinstance(value, (np.integer, np.bool_)):
        return value.item()
    if isinstance(value, (float, np.floating)):
        return float(value) if np.isfinite(value) else None
    return value


# ---- entry points --------------------------------------------------------------------------

# src/rr_guide_test.h labels; a guide test changes the guides RR got (only mode 8's depth floor can be
# undone).
GUIDE_TESTS = {1: 'flip back-facing normals', 2: 'all normals face the camera',
               3: 'flat diffuse albedo', 4: 'no specular albedo', 5: 'flat roughness',
               6: 'all guides flat', 7: 'sky normals face the camera',
               8: 'sky normals + non-zero sky depth', 9: 'sky roughness 0'}


def expand_contrast(rgb, knee):
    """Undo the contrast test's compression (src/rr_contrast_test.h ExpandFactor) on an RGB
    float32 array, in place."""
    t = rgb.max(axis=-1)
    with np.errstate(invalid='ignore', over='ignore', divide='ignore'):
        y = np.minimum(t / np.float32(knee), np.float32(CONTRAST_MAX_EXPONENT))
        factor = np.where(y < CONTRAST_SERIES_LIMIT, 1 + y * (0.5 + y / 6), np.expm1(y) / y)
    rgb *= np.where(t > 0, factor, 1).astype(np.float32)[..., None]


def hardware_depth(depth, mapping, sky_depth=0.0):
    """Weapon depth's linear view depth back to the game's hardware depth, in float32 as
    src/rr_weapon_depth.h Hardware: mapping is a frame's linear_depth (world depth = scale /
    (depth + offset), the weapon slice depth > slice in front of the near plane), sky_depth the
    hardware depth sky stands for (the frame's depth_floor). World depth that was farther than sky
    comes back as sky; depth linear depth never holds (NaN, infinite, zero or below) is returned
    as it is, so it still counts as bad."""
    f = np.float32
    z = np.asarray(depth, np.float32)
    scale, offset, sky = f(mapping['scale']), f(mapping['offset']), f(mapping['sky'])
    cut, knee = f(mapping['slice']), f(mapping['knee'])
    with np.errstate(divide='ignore', invalid='ignore', over='ignore'):
        world = np.minimum(scale / z - offset, cut)   # Rounding never makes world the weapon.
        t = knee * (scale / (f(1) + offset) / z - f(1))
        weapon = cut + (f(1) - cut) * np.clip(t, f(0), f(1))
        out = np.where(z < scale / (cut + offset), weapon, world)   # In front of all world depth.
        out = np.where(z < sky, out, f(sky_depth))
        return np.where((z > 0) & ~np.isinf(z), out, z).astype(np.float32)


def undo_tests(manifest, frames):
    """Frames captured with a tree-noise test or weapon depth on, brought back to the game's
    values: color and rr_output RGB divided by the frame's color_scale (the color copy's constant
    alpha dropped; contrast_knee expanded first), linear depth (a frame's linear_depth mapping) back to hardware depth, and sky
    depth lifted to exactly depth_floor put back to 0. Returns (frames, notes); unchanged frames
    are the same objects."""
    if (manifest.get('settings') or {}).get('weapon_depth') and \
            any('linear_depth' not in f for f in manifest.get('frames') or [{}]):
        raise ValueError('depth is linear view depth (weapon depth on) and this build did not record '
                         'its mapping, so it cannot be read as hardware depth: take the capture '
                         'with weapon depth off, or with a newer build')
    entries = manifest.get('frames') or []
    out, scaled, floored, linear, sources, guides = [], [], [], [], set(), {}
    squeezed = []
    for i, frame in enumerate(frames):
        meta = entries[i] if i < len(entries) else {}
        scale = meta.get('color_scale') or 1.0
        knee = meta.get('contrast_knee') or 0.0
        floor = meta.get('depth_floor') or 0.0
        mapping = meta.get('linear_depth')
        sources.add(meta.get('responsivity_source') or 'unknown')
        guides.setdefault(meta.get('guide_test'), []).append(i)
        if scale != 1.0 or knee > 0 or floor > 0 or mapping:
            frame = dict(frame)
        if mapping:
            linear.append(i)
            frame['depth'] = hardware_depth(frame['depth'], mapping, floor)
        if knee > 0:
            squeezed.append(i)
        if scale != 1.0:
            scaled.append(i)
        if scale != 1.0 or knee > 0:
            for name in ('color', 'rr_output'):
                if name in frame:
                    a = frame[name].astype(np.float32, copy=True)
                    if knee > 0:
                        expand_contrast(a[..., :3], knee)
                    a[..., :3] /= np.float32(scale)
                    # The scaled copy is RGBA32F with alpha 1; the game's color has no alpha.
                    frame[name] = a[..., :3] if name == 'color' else a
        if floor > 0:
            floored.append(i)
            depth = frame['depth'].astype(np.float32, copy=True)
            depth[depth == np.float32(floor)] = 0   # The floor replaced only depth == 0.
            frame['depth'] = depth
        out.append(frame)
    notes = []
    if scaled:
        notes.append(f'color brightness test on in frames {scaled}: color and rr_output divided by '
                     'their color_scale (the SSS and transparency guides were withheld)')
    if squeezed:
        notes.append(f'color contrast test on in frames {squeezed}: color and rr_output expanded by '
                     'their contrast_knee (the SSS and transparency guides were withheld)')
    if linear:
        notes.append(f'weapon depth on in frames {linear}: linear view depth converted back to hardware '
                     'depth')
    if floored:
        notes.append(f'sky depth floor on in frames {floored}: depth == depth_floor put back to sky (0)')
    shown = sources - {'unknown'}
    if shown & {'sky_edge', 'water'}:   # Only a test mask is worth a note.
        notes.append('responsivity mask from: ' + ', '.join(sorted(shown)))
    for mode, at in sorted((k, v) for k, v in guides.items() if k):
        notes.append(f"guide test {mode} ({GUIDE_TESTS.get(mode, 'unknown')}) on in frames {at}: "
                     "the guides are the test's, not the game's")
    if len(guides.keys() - {None}) > 1:
        notes.append('the guide test changed within the capture, so its frames mix guide values')
    return out, notes


def capture_info(manifest, frames, radius):
    entries = manifest.get('frames', [])
    h, w = frames[0]['depth'].shape
    output = next((f['rr_output'].shape[:2] for f in frames if 'rr_output' in f),
                  (manifest.get('output_height'), manifest.get('output_width')))
    return {'frames': len(frames), 'width': w, 'height': h, 'radius': radius,
            'output_width': output[1], 'output_height': output[0],
            'preset': manifest.get('preset'), 'quality': manifest.get('quality'),
            'settings': manifest.get('settings'), 'inputs': sorted(frames[0]),
            'serials': [f.get('serial') for f in entries], 'resets': [f.get('reset') for f in entries],
            'jitter': [f.get('jitter') for f in entries],
            'motion_vector_scale': [f.get('motion_vector_scale') for f in entries]}


def analyze(manifest, frames, out_dir, radius=2, crop=None):
    """Run every measurement on in-memory frames, write report.md, metrics.json and PNGs to
    out_dir, and return the metrics dict (verdicts under metrics['verdicts'])."""
    if not frames or any('color' not in f or 'depth' not in f for f in frames):
        raise ValueError('every frame needs color and depth')
    if radius < 1:
        raise ValueError('radius must be >= 1')
    frames, test_notes = undo_tests(manifest, frames)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    shape = frames[0]['depth'].shape
    regions = [compute_regions(f['depth'], f.get('status'), radius) for f in frames]
    region_metrics, unions = region_summary(regions)
    m = {'capture': dict(capture_info(manifest, frames, radius), tests=test_notes),
         'camera': camera_motion(manifest, shape[1]),
         'regions': region_metrics,
         'sky_vs_status': sky_status_agreement(frames, regions)}
    m['H1'], flips, flicker = flicker_analysis(regions, unions)
    m['crop'] = list(choose_crop(crop, flicker, unions['sky_band'], shape))
    m['H0'], h0_maps = output_flicker_analysis(manifest, frames, regions, unions, m['crop'])
    m['H2'], vote_maps, acc, hdr_local = vote_analysis(frames, regions)
    m['H3'] = depth_bleed_analysis(frames, regions, radius)
    m['H4'] = {'motion_vote_sky_band': m['H2']['bands']['sky'].get('motion'),
               'motion_vote_hill_band': m['H2']['bands']['hill'].get('motion'),
               'motion_magnitude': motion_magnitudes(frames, regions)}
    m['H5'], std_map = temporal_analysis(frames, unions, acc)
    m['H6'] = bad_value_analysis(frames, regions)
    m['H7'] = jitter_analysis(manifest, frames, regions)
    m['H8'] = stochastic_coverage_analysis(manifest, frames, regions, unions)
    m['H9'] = hdr_scale_analysis(manifest, frames, regions, hdr_local)
    m['H10'] = coarse_shading_analysis(frames, regions)
    m['optional_inputs'] = optional_input_ranges(frames, regions)
    m['verdicts'] = verdicts(m)
    m['images'] = write_images(out_dir, frames, regions, flips, vote_maps, std_map, m['crop'])
    if h0_maps is not None:
        m['images'] += write_h0_images(out_dir, frames, h0_maps, len(m['H0']['pairs']))
    m = jsonable(m)
    (out_dir / 'metrics.json').write_text(json.dumps(m, indent=1), encoding='utf-8')
    (out_dir / 'report.md').write_text(report_text(m), encoding='utf-8')
    return m


def parse_crop(text):
    parts = [int(p) for p in text.split(',')]
    if len(parts) != 4 or parts[2] <= 0 or parts[3] <= 0:
        raise argparse.ArgumentTypeError('crop must be x,y,w,h with w, h > 0')
    return tuple(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('folder', type=Path, help='capture folder (contains manifest.json)')
    parser.add_argument('--out', type=Path, required=True, help='output folder for report and images')
    parser.add_argument('--radius', type=int, default=2, help='sky-edge band radius in px (default 2)')
    parser.add_argument('--crop', type=parse_crop, help='crop strip window x,y,w,h (default: auto)')
    args = parser.parse_args()
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from read_capture import load
    manifest, frames = load(args.folder)
    metrics = analyze(manifest, frames, args.out, radius=args.radius, crop=args.crop)
    for key, h in metrics['verdicts'].items():
        print(f"{key} {h['title']}: {h['verdict']}")
    print(f"report: {args.out / 'report.md'}")


if __name__ == '__main__':
    main()
