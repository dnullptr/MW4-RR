"""Generate the 128x128 tileable blue-noise rank map used by the RR colour-input dropout pass.

Void-and-cluster (Ulichney 1993) on the torus, Gaussian energy with sigma 1.5, fixed seed.
Writes shaders/rr_blue_noise.inc: 8192 uint32 words, two 16-bit ranks per word (even x low).
The file is included inside an array initializer by both the HLSL shader and the C++ tests.
CLI:  python3 tools/blue_noise.py [--write] [--check] [--report]
"""
import argparse
from decimal import Decimal, ROUND_HALF_EVEN, localcontext
from pathlib import Path
import sys

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / 'shaders' / 'rr_blue_noise.inc'
SIZE = 128
N = SIZE * SIZE
SIGMA = Decimal('1.5')
SEED = 20261025
INITIAL_ONES = round(0.1 * N)
# The energy is kept in integer fixed point (units of 2**-24) so sums are exact and order
# independent, and every tie breaks to the lowest flat index the same way on every machine.
KERNEL_BITS = 24
LOW = np.iinfo(np.int64).min
HIGH = np.iinfo(np.int64).max


def kernel():
    """Toroidal Gaussian exp(-d^2 / (2 sigma^2)) centred on (0, 0), tiled 2x2 for slicing."""
    distance = np.minimum(np.arange(SIZE), SIZE - np.arange(SIZE))
    d2 = distance[:, None] ** 2 + distance[None, :] ** 2
    values = {}
    with localcontext() as context:
        context.prec = 50
        for value in np.unique(d2):
            # Decimal.exp is correctly rounded, unlike libm, so the table is platform independent.
            weight = (Decimal(-int(value)) / (2 * SIGMA * SIGMA)).exp() * (1 << KERNEL_BITS)
            values[int(value)] = int(weight.to_integral_value(rounding=ROUND_HALF_EVEN))
    table = np.vectorize(values.__getitem__, otypes=[np.int64])(d2)
    return np.tile(table, (2, 2))


class Energy:
    """Sum of the kernel over a set of pixels, updated one pixel at a time."""

    def __init__(self, tiled, pixels):
        self.tiled = tiled
        self.grid = np.zeros((SIZE, SIZE), np.int64)
        self.flat = self.grid.reshape(-1)
        for index in np.flatnonzero(pixels):
            self.add(index)

    def splat(self, index):
        y, x = divmod(int(index), SIZE)
        top, left = (SIZE - y) % SIZE, (SIZE - x) % SIZE
        return self.tiled[top:top + SIZE, left:left + SIZE]

    def add(self, index):
        self.grid += self.splat(index)

    def remove(self, index):
        self.grid -= self.splat(index)

    def tightest(self, pixels):
        """Member of the set with the highest energy."""
        return int(np.where(pixels, self.flat, LOW).argmax())

    def largest_void(self, pixels):
        """Non-member with the lowest energy."""
        return int(np.where(pixels, HIGH, self.flat).argmin())


def generate():
    """Return the rank map as int32 [y][x], a permutation of 0..N-1."""
    tiled = kernel()
    order = np.argsort(np.random.default_rng(SEED).random(N), kind='stable')
    pattern = np.zeros(N, bool)
    pattern[order[:INITIAL_ONES]] = True

    # Tighten the initial pattern: move the tightest cluster to the largest void until stable.
    energy = Energy(tiled, pattern)
    for _ in range(100 * N):
        cluster = energy.tightest(pattern)
        pattern[cluster] = False
        energy.remove(cluster)
        void = energy.largest_void(pattern)
        pattern[void] = True
        energy.add(void)
        if void == cluster:
            break
    else:
        raise RuntimeError('initial pattern did not converge')

    ranks = np.full(N, -1, np.int32)
    ones = int(pattern.sum())
    # Phase 1: rank the prototype's ones downward by removing the tightest cluster.
    shrinking, phase1 = pattern.copy(), Energy(tiled, pattern)
    for rank in range(ones - 1, -1, -1):
        cluster = phase1.tightest(shrinking)
        shrinking[cluster] = False
        phase1.remove(cluster)
        ranks[cluster] = rank
    # Phase 2: fill the largest void until half the pixels are set.
    for rank in range(ones, N // 2):
        void = energy.largest_void(pattern)
        pattern[void] = True
        energy.add(void)
        ranks[void] = rank
    # Phase 3: rank the remaining zeros by their tightest cluster, on the inverted pattern.
    zeros = ~pattern
    inverted = Energy(tiled, zeros)
    for rank in range(N // 2, N):
        cluster = inverted.tightest(zeros)
        zeros[cluster] = False
        inverted.remove(cluster)
        ranks[cluster] = rank
    return ranks.reshape(SIZE, SIZE)


def render(ranks):
    flat = ranks.reshape(-1).astype(np.uint32)
    words = flat[0::2] | (flat[1::2] << np.uint32(16))
    header = [
        '// rr_blue_noise.inc: 128x128 tileable blue-noise threshold map, void-and-cluster ranks 0..16383.',
        f'// Gaussian energy sigma {SIGMA} on the torus, seed {SEED}, {INITIAL_ONES} initial ones.',
        '// 8192 uint32 words; word (y*128+x)>>1 holds the even-x rank in the low 16 bits, odd-x in the high.',
        '// rank(x,y) = (BlueNoise[(y*128+x)>>1] >> ((x&1)*16)) & 0xffff; include inside an array initializer.',
        '// Generated by tools/blue_noise.py --write, do not edit.']
    rows = [', '.join(f'0x{int(word):08x}' for word in words[start:start + 8])
            for start in range(0, len(words), 8)]
    return '\n'.join(header) + '\n' + ',\n'.join(rows) + '\n'


def white_power(rng, count, size):
    total = np.zeros((size, size))
    for _ in range(count):
        pattern = np.zeros(size * size)
        pattern[rng.permutation(size * size)[:size * size // 2]] = 1
        total += np.abs(np.fft.fft2(pattern.reshape(size, size) - pattern.mean())) ** 2
    return total / count


def report(ranks):
    flat = np.sort(ranks.reshape(-1))
    ok = np.array_equal(flat, np.arange(N))
    print(f'permutation: {"ok" if ok else "FAILED"} ({len(np.unique(flat))} distinct ranks, '
          f'min {flat[0]}, max {flat[-1]})')
    print('threshold  differing neighbours h / v (white)   8x8 block count std (white)')
    for fraction in (0.25, 0.5, 0.75):
        mask = ranks < fraction * N
        p = mask.mean()
        horizontal = np.mean(mask != np.roll(mask, 1, axis=1))
        vertical = np.mean(mask != np.roll(mask, 1, axis=0))
        blocks = mask.reshape(SIZE // 8, 8, SIZE // 8, 8).sum(axis=(1, 3))
        print(f'  {fraction:4.0%}     {horizontal:.4f} / {vertical:.4f} ({2 * p * (1 - p):.4f})'
              f'          {blocks.std():.3f} ({np.sqrt(64 * p * (1 - p)):.3f})')
    half = (ranks < N // 2).astype(float)
    power = np.abs(np.fft.fft2(half - half.mean())) ** 2
    white = white_power(np.random.default_rng(1), 20, SIZE)
    frequency = np.fft.fftfreq(SIZE)
    radius = np.hypot(frequency[:, None], frequency[None, :])
    print('50% pattern power spectrum / mean of 20 white-noise patterns (same density):')
    for low, high in ((0.0, 0.08), (0.35, 0.5)):
        band = (radius > low) & (radius < high)
        print(f'  {low:.2f} < |f| < {high:.2f}: {power[band].mean() / white[band].mean():.4f}')
    edges = np.arange(0.0, 0.55, 0.05)
    profile = []
    for low, high in zip(edges[:-1], edges[1:]):
        band = (radius > low) & (radius <= high)
        profile.append(f'{power[band].mean() / white[band].mean():.2f}')
    print('  radial profile, 0.05-wide bins up to 0.5: ' + ' '.join(profile))
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--write', action='store_true', help=f'write {OUTPUT.relative_to(ROOT)}')
    parser.add_argument('--check', action='store_true', help='exit 1 if the written file is out of date')
    parser.add_argument('--report', action='store_true', help='print quality numbers')
    args = parser.parse_args()
    if not (args.write or args.check or args.report):
        parser.error('pass --write, --check and/or --report')
    ranks = generate()
    text = render(ranks).encode('ascii')
    status = 0
    if args.write:
        OUTPUT.write_bytes(text)
        print(f'wrote {OUTPUT.relative_to(ROOT)}')
    if args.check:
        if OUTPUT.exists() and OUTPUT.read_bytes() == text:
            print(f'{OUTPUT.relative_to(ROOT)} is up to date')
        else:
            print(f'{OUTPUT.relative_to(ROOT)} differs from the generator; run --write', file=sys.stderr)
            status = 1
    if args.report and not report(ranks):
        status = 1
    return status


if __name__ == '__main__':
    sys.exit(main())
