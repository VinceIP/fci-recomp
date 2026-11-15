"""Frame scoring for tools/regress.py: 160x120 greyscale frames, the mean absolute difference and
the worst 8x8 block (a missing piece of a model is local: it barely moves the mean)."""

from PIL import Image


def lum_stats(path):
    im = Image.open(path).convert("L").resize((160, 120))
    return im, sum(im.tobytes()) / (160 * 120)


def block_mask(rect):
    """the 8x8 blocks ((bx, by) of the 20x15 grid) that intersect rect [x0, y0, x1, y1] in pixel
    coordinates: a region a scenario animates on wall clock (the view model), excluded from the
    worst-block score; the mean still covers it"""
    x0, y0, x1, y1 = rect
    return frozenset(
        (bx, by)
        for by in range(15)
        for bx in range(20)
        if bx * 8 + 8 > x0 and by * 8 + 8 > y0 and bx * 8 < x1 and by * 8 < y1
    )


def diff(a, b, mask=frozenset()):
    """mean absolute difference over the frame, and the worst 8x8 block outside mask"""
    pa, pb = a.tobytes(), b.tobytes()
    mean = sum(abs(x - y) for x, y in zip(pa, pb)) / (160 * 120)
    worst = 0.0
    for by in range(15):
        for bx in range(20):
            if (bx, by) in mask:
                continue
            s = 0
            for y in range(by * 8, by * 8 + 8):
                o = y * 160 + bx * 8
                s += sum(abs(pa[o + i] - pb[o + i]) for i in range(8))
            worst = max(worst, s / 64)
    return mean, worst
