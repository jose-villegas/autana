"""Shelf-pack card masks with mip-aligned empty borders into MASK1 texture pages."""

import numpy as np

from r3d.cards import CARD_MIP_LEVELS
from r3d.texture.texture_asset import MASK1_SIZE_LOG2_MIN, SIZE_LOG2_MAX

PAGE_SIDE = 1 << SIZE_LOG2_MAX


def pack_pages(masks, levels=CARD_MIP_LEVELS):
    """(boolean pages, (page, x, y, width, height) per mask), padded by the coarsest mip's texel."""
    if not 1 <= levels <= SIZE_LOG2_MAX - MASK1_SIZE_LOG2_MIN + 1:
        raise ValueError("card mip count is invalid")
    padding = 1 << (levels - 1)
    pages, rects = [], [None] * len(masks)
    x = y = shelf = 0
    for i in sorted(range(len(masks)), key=lambda i: (-masks[i].shape[0], -masks[i].shape[1], i)):
        height, width = masks[i].shape
        padded_w = -(-(width + 2 * padding) // padding) * padding
        padded_h = -(-(height + 2 * padding) // padding) * padding
        if not width or not height or max(padded_w, padded_h) > PAGE_SIDE:
            raise ValueError("card mask does not fit a texture page with mip padding")
        if x + padded_w > PAGE_SIDE:
            x, y, shelf = 0, y + shelf, 0
        if not pages or y + padded_h > PAGE_SIDE:
            pages.append(np.zeros((PAGE_SIDE, PAGE_SIDE), dtype=bool))
            x = y = shelf = 0
        rects[i] = (len(pages) - 1, x + padding, y + padding, width, height)
        pages[-1][y + padding:y + padding + height, x + padding:x + padding + width] = masks[i]
        x += padded_w
        shelf = max(shelf, padded_h)
    return pages, rects
