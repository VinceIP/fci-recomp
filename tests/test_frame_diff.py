"""The frame scoring in tools/frame_diff.py (tools/regress.py's reference comparison): the mean
absolute difference, the worst 8x8 block, and a scenario's mask of a region that animates on wall
clock (the view model)."""

from PIL import Image

from frame_diff import block_mask, diff

MEAN_LIMIT = 6.0  # regress.py's MEAN_TOLERANCE
BLOCK_LIMIT = 30  # a scenario's "block"


def frame_with_block(bx, by, value=0, base=128):
    im = Image.new("L", (160, 120), base)
    for y in range(by * 8, by * 8 + 8):
        for x in range(bx * 8, bx * 8 + 8):
            im.putpixel((x, y), value)
    return im


def is_a_failure(mean, block):
    """regress.py check(): the frame differs from its reference"""
    return mean > MEAN_LIMIT or block > BLOCK_LIMIT


def test_a_block_change_fails_unmasked():
    a = Image.new("L", (160, 120), 128)
    b = frame_with_block(5, 5)  # one 8x8 block off by 128
    mean, block = diff(a, b)
    assert mean < MEAN_LIMIT  # a local change barely moves the mean, as regress.py relies on
    assert block > BLOCK_LIMIT
    assert is_a_failure(mean, block)


def test_the_same_change_inside_the_mask_passes():
    a = Image.new("L", (160, 120), 128)
    b = frame_with_block(5, 5)
    mask = block_mask([40, 40, 48, 48])  # the 8x8 block at (5, 5), in pixel coordinates
    mean, block = diff(a, b, mask)
    assert not is_a_failure(mean, block)


def test_the_mask_keeps_the_mean_over_the_whole_frame():
    a = Image.new("L", (160, 120), 128)
    b = frame_with_block(5, 5)
    (mean_open, _), (mean_masked, _) = diff(a, b), diff(a, b, block_mask([40, 40, 48, 48]))
    assert mean_masked == mean_open == 128 / (160 * 120) * 64


def test_a_change_outside_the_mask_still_fails():
    a = Image.new("L", (160, 120), 128)
    b = frame_with_block(15, 14)  # far from the mask
    mean, block = diff(a, b, block_mask([40, 40, 48, 48]))
    assert block > BLOCK_LIMIT
    assert is_a_failure(mean, block)


def test_block_mask_selects_the_intersecting_blocks():
    # [64, 64, 128, 120] (the editor-jump view model): blocks 8..15 x 8..14
    mask = block_mask([64, 64, 128, 120])
    assert mask == frozenset((bx, by) for by in range(8, 15) for bx in range(8, 16))
    assert block_mask([0, 0, 160, 120]) == frozenset((bx, by) for by in range(15) for bx in range(20))
    assert block_mask([0, 0, 1, 1]) == frozenset({(0, 0)})
