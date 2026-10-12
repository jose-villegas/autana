"""Deterministic, accessible byte-scaled bars for generated memory tables."""
from html import escape
from pathlib import Path

WIDTH = 1240
BAR_WIDTH = 620
LEFT = 245
TOP = 90
ROW_HEIGHT = 44
BAR_HEIGHT = 24
TEXT_OFFSET = 17
END_GAP = 12
FOOTER_HEIGHT = 100
FONT_SIZE = 15
CAPTION_SPACING = 24
LEGEND_SPACING = 240
KB_BYTES = 1024
IDF_FILL = '#8c959f'
OURS_FILL = '#e36209'
SLACK_FILL = '#b1bac4'
DRAM_FILL = '#4c8bf5'
HEAP_FILL = '#2da44e'
COMPONENT_TOP_COUNT = 8


def text(x, y, value, anchor='start'):
    return f'<text x="{x}" y="{y}" text-anchor="{anchor}">{escape(str(value))}</text>'


def bars(command, title, subtitle, rows, scale, captions=(), legend=(), axis=()):
    if scale <= 0:
        raise ValueError('chart scale must be positive')
    height = TOP + len(rows) * ROW_HEIGHT + FOOTER_HEIGHT + (len(captions) + len(axis)) * CAPTION_SPACING
    lines = ['<!-- GENERATED FILE - do not edit.', escape(command, quote=False).replace('--', '&#45;&#45;'), '-->',
             f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" viewBox="0 0 {WIDTH} {height}" role="img">',
             f'<title>{escape(title)}</title>',
             '<style>text{fill:#24292f;font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;'
             f'font-size:{FONT_SIZE}px}}@media(prefers-color-scheme:dark){{text{{fill:#f0f6fc}}}}</style>',
             text(0, CAPTION_SPACING, title), text(0, CAPTION_SPACING * 2, subtitle)]
    for index, (label, segments, end) in enumerate(rows):
        y = TOP + index * ROW_HEIGHT
        lines.append(text(LEFT - END_GAP, y + TEXT_OFFSET, label, 'end'))
        x = float(LEFT)
        for name, count, fill in segments:
            if count < 0:
                raise ValueError(f'{label}: negative bytes for {name}')
            width = count * BAR_WIDTH / scale
            lines.append(f'<rect x="{x:.2f}" y="{y}" width="{width:.2f}" height="{BAR_HEIGHT}" '
                         f'fill="{fill}" data-label="{escape(name, quote=True)}" data-bytes="{count}"><title>{escape(name)}: {count} B</title></rect>')
            x += width
        lines.append(text(f'{x + END_GAP:.2f}', y + TEXT_OFFSET, end))
    y = TOP + len(rows) * ROW_HEIGHT
    if axis:
        lines.append(f'<line x1="{LEFT}" y1="{y}" x2="{LEFT + BAR_WIDTH}" y2="{y}" stroke="{IDF_FILL}"/>')
        y += CAPTION_SPACING
        lines.extend((text(LEFT, y, axis[0]), text(LEFT + BAR_WIDTH, y, axis[1], 'end')))
        y += CAPTION_SPACING
    for index, caption in enumerate(captions):
        lines.append(text(0, y + index * CAPTION_SPACING, caption))
    y += len(captions) * CAPTION_SPACING + CAPTION_SPACING
    for index, (label, fill) in enumerate(legend):
        x = (index % (WIDTH // LEGEND_SPACING)) * LEGEND_SPACING
        legend_y = y + (index // (WIDTH // LEGEND_SPACING)) * CAPTION_SPACING
        lines.append(f'<rect x="{x}" y="{legend_y - TEXT_OFFSET}" width="{TEXT_OFFSET}" height="{TEXT_OFFSET}" fill="{fill}"/>')
        lines.append(text(x + CAPTION_SPACING, legend_y, label))
    return '\n'.join([*lines, '</svg>', ''])


def write(path, svg):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(svg.encode('utf-8'))
