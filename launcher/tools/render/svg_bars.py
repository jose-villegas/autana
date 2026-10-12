"""Deterministic, accessible byte-scaled bars for generated memory tables.

An SVG shown through <img> follows the viewer's OS colour scheme, not the
page's theme, so text uses one grey that reads on GitHub's light and dark
backgrounds instead of a prefers-color-scheme switch. Text drawn on a bar
uses a dark colour, since the fills are the same in both themes."""
from html import escape
from pathlib import Path

WIDTH = 1040
MARGIN = 16
BAR_WIDTH = 640
LEFT = 200
TOP = 96
ROW_HEIGHT = 44
BAR_HEIGHT = 24
TEXT_OFFSET = 17
NOTE_OFFSET = 34
END_GAP = 12
FOOTER_HEIGHT = 40
FONT_SIZE = 15
TITLE_SIZE = 18
NOTE_SIZE = 12
# Average glyph advance as a share of the font size, to decide whether a
# label fits inside its bar segment.
GLYPH_WIDTH = 0.6
INSIDE_PAD = 6
CAPTION_SPACING = 24
LEGEND_SPACING = 200
KB_BYTES = 1024
# Contrast about 4.2:1 on GitHub's light (#ffffff) and 4.5:1 on its dark (#0d1117) background.
TEXT_FILL = '#737c86'
INSIDE_FILL = '#1f2328'
IDF_FILL = '#8c959f'
OURS_FILL = '#e36209'
SLACK_FILL = '#b1bac4'
DRAM_FILL = '#4c8bf5'
HEAP_FILL = '#2da44e'
COMPONENT_TOP_COUNT = 8


def text(x, y, value, anchor='start', css=None):
    attribute = f' class="{css}"' if css else ''
    return f'<text x="{x}" y="{y}" text-anchor="{anchor}"{attribute}>{escape(str(value))}</text>'


def inside_label(name, count, width):
    """The longest of `name KB` and `KB` that fits in `width`, or None."""
    size = f'{count / KB_BYTES:.1f} KB'
    for label in (f'{name} {size}', size):
        if len(label) * FONT_SIZE * GLYPH_WIDTH + 2 * INSIDE_PAD <= width:
            return label
    return None


def bars(command, title, subtitle, rows, scale, captions=(), legend=(), axis=(), inside=False):
    """`rows` are (label, segments, end) or (label, segments, end, note);
    each segment is (name, bytes, fill). `inside` writes each segment's
    size on it where it fits."""
    if scale <= 0:
        raise ValueError('chart scale must be positive')
    legend_rows = -(-len(legend) // ((WIDTH - MARGIN) // LEGEND_SPACING))
    height = (TOP + len(rows) * ROW_HEIGHT + (len(captions) + legend_rows + 1) * CAPTION_SPACING
              + (2 * CAPTION_SPACING if axis else 0) + FOOTER_HEIGHT)
    lines = ['<!-- GENERATED FILE - do not edit.', escape(command, quote=False).replace('--', '&#45;&#45;'), '-->',
             f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" viewBox="0 0 {WIDTH} {height}" role="img">',
             f'<title>{escape(title)}</title>',
             f'<style>text{{fill:{TEXT_FILL};font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;'
             f'font-size:{FONT_SIZE}px}}.title{{font-size:{TITLE_SIZE}px;font-weight:600}}'
             f'.note{{font-size:{NOTE_SIZE}px}}.inside{{fill:{INSIDE_FILL}}}</style>',
             text(MARGIN, CAPTION_SPACING + MARGIN, title, css='title'), text(MARGIN, 2 * CAPTION_SPACING + MARGIN, subtitle)]
    for index, (label, segments, end, *note) in enumerate(rows):
        y = TOP + index * ROW_HEIGHT
        lines.append(text(LEFT - END_GAP, y + TEXT_OFFSET, label, 'end'))
        x = float(LEFT)
        for name, count, fill in segments:
            if count < 0:
                raise ValueError(f'{label}: negative bytes for {name}')
            width = count * BAR_WIDTH / scale
            lines.append(f'<rect x="{x:.2f}" y="{y}" width="{width:.2f}" height="{BAR_HEIGHT}" '
                         f'fill="{fill}" data-label="{escape(name, quote=True)}" data-bytes="{count}"><title>{escape(name)}: {count} B</title></rect>')
            fitted = inside_label(name, count, width) if inside else None
            if fitted:
                lines.append(text(f'{x + INSIDE_PAD:.2f}', y + TEXT_OFFSET, fitted, css='inside'))
            x += width
        lines.append(text(f'{x + END_GAP:.2f}', y + TEXT_OFFSET, end))
        if note and note[0]:
            lines.append(text(f'{x + END_GAP:.2f}', y + NOTE_OFFSET, note[0], css='note'))
    y = TOP + len(rows) * ROW_HEIGHT
    if axis:
        lines.append(f'<line x1="{LEFT}" y1="{y}" x2="{LEFT + BAR_WIDTH}" y2="{y}" stroke="{IDF_FILL}"/>')
        y += CAPTION_SPACING
        lines.extend((text(LEFT, y, axis[0]), text(LEFT + BAR_WIDTH, y, axis[1], 'end')))
    y += CAPTION_SPACING
    for index, caption in enumerate(captions):
        lines.append(text(MARGIN, y + index * CAPTION_SPACING, caption))
    y += len(captions) * CAPTION_SPACING + CAPTION_SPACING
    per_row = (WIDTH - MARGIN) // LEGEND_SPACING
    for index, (label, fill) in enumerate(legend):
        x = MARGIN + (index % per_row) * LEGEND_SPACING
        legend_y = y + (index // per_row) * CAPTION_SPACING
        lines.append(f'<rect x="{x}" y="{legend_y - TEXT_OFFSET + 3}" width="{TEXT_OFFSET}" height="{TEXT_OFFSET}" fill="{fill}"/>')
        lines.append(text(x + CAPTION_SPACING, legend_y, label))
    return '\n'.join([*lines, '</svg>', ''])


def write(path, svg):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(svg.encode('utf-8'))
