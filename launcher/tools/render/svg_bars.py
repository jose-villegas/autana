"""Deterministic, accessible byte-scaled bars for generated tables.

An SVG shown through <img> follows the viewer's OS colour scheme, not the
page's theme, so text uses one grey that reads on GitHub's light and dark
backgrounds instead of a prefers-color-scheme switch. Text drawn on a bar
uses a dark colour, since the fills are the same in both themes."""
from html import escape
from pathlib import Path
from byte_sizes import size_label

_WIDTH = 1040
_MARGIN = 16
BAR_WIDTH = 640
_LEFT = 200
_TOP = 96
_ROW_HEIGHT = 44
_BAR_HEIGHT = 24
_TEXT_OFFSET = 17
_NOTE_OFFSET = 34
_END_GAP = 12
_FOOTER_HEIGHT = 40
_FONT_SIZE = 15
_TITLE_SIZE = 18
_NOTE_SIZE = 12
# Average glyph advance as a share of the font size, to decide whether a
# label fits inside its bar segment.
_GLYPH_WIDTH = 0.6
_INSIDE_PAD = 6
_CAPTION_SPACING = 24
_LEGEND_SPACING = 200
IDF_FILL = '#8c959f'
OURS_FILL = '#e36209'
SLACK_FILL = '#b1bac4'
DRAM_FILL = '#4c8bf5'
HEAP_FILL = '#2da44e'
# Contrast about 4.2:1 on GitHub's light (#ffffff) and 4.5:1 on its dark (#0d1117) background.
_TEXT_FILL = '#737c86'
_INSIDE_FILL = '#1f2328'
_AXIS_FILL = '#8c959f'
_LEGEND_SWATCH_OFFSET = 3


def text(x, y, value, anchor='start', css=None):
    attribute = f' class="{css}"' if css else ''
    return f'<text x="{x}" y="{y}" text-anchor="{anchor}"{attribute}>{escape(str(value))}</text>'


def inside_label(name, count, width):
    """The longest of `name size` and `size` that fits in `width`, or None."""
    size = size_label(count)
    for label in (f'{name} {size}', size):
        if len(label) * _FONT_SIZE * _GLYPH_WIDTH + 2 * _INSIDE_PAD <= width:
            return label
    return None


def bars(command, title, subtitle, rows, scale, captions=(), legend=(), axis=(), inside=False):
    """`rows` are (label, segments, end) or (label, segments, end, note);
    each segment is (name, bytes, fill). `inside` writes each segment's
    size on it where it fits."""
    if scale <= 0:
        raise ValueError('chart scale must be positive')
    legend_rows = -(-len(legend) // ((_WIDTH - _MARGIN) // _LEGEND_SPACING))
    height = (_TOP + len(rows) * _ROW_HEIGHT + (len(captions) + legend_rows + 1) * _CAPTION_SPACING
              + (2 * _CAPTION_SPACING if axis else 0) + _FOOTER_HEIGHT)
    lines = ['<!-- GENERATED FILE - do not edit.', escape(command, quote=False).replace('--', '&#45;&#45;'), '-->',
             f'<svg xmlns="http://www.w3.org/2000/svg" width="{_WIDTH}" viewBox="0 0 {_WIDTH} {height}" role="img">',
             f'<title>{escape(title)}</title>',
             f'<style>text{{fill:{_TEXT_FILL};font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;'
             f'font-size:{_FONT_SIZE}px}}.title{{font-size:{_TITLE_SIZE}px;font-weight:600}}'
             f'.note{{font-size:{_NOTE_SIZE}px}}.inside{{fill:{_INSIDE_FILL}}}</style>',
             text(_MARGIN, _CAPTION_SPACING + _MARGIN, title, css='title'), text(_MARGIN, 2 * _CAPTION_SPACING + _MARGIN, subtitle)]
    for index, (label, segments, end, *note) in enumerate(rows):
        y = _TOP + index * _ROW_HEIGHT
        lines.append(text(_LEFT - _END_GAP, y + _TEXT_OFFSET, label, 'end'))
        x = float(_LEFT)
        for name, count, fill in segments:
            if count < 0:
                raise ValueError(f'{label}: negative bytes for {name}')
            width = count * BAR_WIDTH / scale
            lines.append(f'<rect x="{x:.2f}" y="{y}" width="{width:.2f}" height="{_BAR_HEIGHT}" '
                         f'fill="{fill}" data-label="{escape(name, quote=True)}" data-bytes="{count}"><title>{escape(name)}: {count} B</title></rect>')
            fitted = inside_label(name, count, width) if inside else None
            if fitted:
                lines.append(text(f'{x + _INSIDE_PAD:.2f}', y + _TEXT_OFFSET, fitted, css='inside'))
            x += width
        lines.append(text(f'{x + _END_GAP:.2f}', y + _TEXT_OFFSET, end))
        if note and note[0]:
            lines.append(text(f'{x + _END_GAP:.2f}', y + _NOTE_OFFSET, note[0], css='note'))
    y = _TOP + len(rows) * _ROW_HEIGHT
    if axis:
        lines.append(f'<line x1="{_LEFT}" y1="{y}" x2="{_LEFT + BAR_WIDTH}" y2="{y}" stroke="{_AXIS_FILL}"/>')
        y += _CAPTION_SPACING
        lines.extend((text(_LEFT, y, axis[0]), text(_LEFT + BAR_WIDTH, y, axis[1], 'end')))
    y += _CAPTION_SPACING
    for index, caption in enumerate(captions):
        lines.append(text(_MARGIN, y + index * _CAPTION_SPACING, caption))
    y += len(captions) * _CAPTION_SPACING + _CAPTION_SPACING
    per_row = (_WIDTH - _MARGIN) // _LEGEND_SPACING
    for index, (label, fill) in enumerate(legend):
        x = _MARGIN + (index % per_row) * _LEGEND_SPACING
        legend_y = y + (index // per_row) * _CAPTION_SPACING
        lines.append(f'<rect x="{x}" y="{legend_y - _TEXT_OFFSET + _LEGEND_SWATCH_OFFSET}" width="{_TEXT_OFFSET}" height="{_TEXT_OFFSET}" fill="{fill}"/>')
        lines.append(text(x + _CAPTION_SPACING, legend_y, label))
    return '\n'.join([*lines, '</svg>', ''])


def write(path, svg):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(svg.encode('utf-8'))
