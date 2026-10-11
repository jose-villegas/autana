"""C comment and literal spans and normalized code for source comparisons.

The output of c_code is part of every bake key, so changing it requires a rebake.
"""


def tokens(source, literals=True):
    """Comment and literal spans, respecting escapes and line continuations.

    A C23 digit separator (1'000) opens a char literal, which can hide a
    comment inside a later string on that line.
    """
    i, n = 0, len(source)
    while i < n:
        start = i
        if literals and source[i] in "\"'":
            quote = source[i]
            i += 1
            while i < n and source[i] not in (quote, "\n"):
                i += 2 if source[i] == "\\" and i + 1 < n else 1
            if i < n and source[i] == quote:
                i += 1
            yield "literal", start, i
        elif source.startswith("/*", i):
            end = source.find("*/", i + 2)
            i = n if end < 0 else end + 2
            yield "block", start, i
        elif source.startswith("//", i):
            end = source.find("\n", i)
            while end >= 0 and source[end - 1] == "\\":
                end = source.find("\n", end + 1)
            i = n if end < 0 else end
            yield "line", start, i
        else:
            i += 1


def c_code(text) -> str:
    """Code with comments removed and outside-literal line spacing normalized."""
    content = []
    space = False
    line_start = True
    at = 0
    for kind, start, end in [*tokens(text), ("end", len(text), len(text))]:
        for byte in text[at:start]:
            if byte in " \t":
                space = not line_start
            elif byte == "\n":
                if not line_start:
                    content.append(byte)
                space = False
                line_start = True
            else:
                if space:
                    content.append(" ")
                content.append(byte)
                space = False
                line_start = False
        if kind == "literal":
            if space:
                content.append(" ")
            content.append(text[start:end])
            space = False
            line_start = False
        elif kind == "block":
            space = not line_start
        at = end
    return "".join(content)
