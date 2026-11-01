"""C and C++ comment and literal spans and normalized code for source comparisons.

The output of c_code is part of every bake key, so changing it requires a rebake.
"""
import re


# The prefixes that make a C++ string raw: R"delim( ... )delim".
RAW_PREFIXES = ("R", "u8R", "uR", "UR", "LR")
# What a raw string's delimiter may not hold, besides being at most 16 long.
RAW_DELIMITER = re.compile(r'[^\s()\\"]{0,16}\(')
PP_NUMBER = re.compile(r"\.?[0-9]")


def run_before(source, i, chars):
    """The run of letters, digits and `chars` that ends just before `i`."""
    start = i
    while start > 0 and (source[start - 1].isalnum() or source[start - 1] in chars):
        start -= 1
    return source[start:i]


def raw_string_end(source, i):
    """Where the raw string whose quote is at `i` ends, or None when the
    quote does not open one: no raw prefix, or no valid delimiter."""
    if run_before(source, i, "_") not in RAW_PREFIXES:
        return None
    opening = RAW_DELIMITER.match(source, i + 1)
    if not opening:
        return None
    closing = ")" + opening.group()[:-1] + '"'
    end = source.find(closing, opening.end())
    return len(source) if end < 0 else end + len(closing)


def digit_separator(source, i):
    """A `'` inside a number (C++14 and C23 1'000) separates digits; it opens no literal."""
    return PP_NUMBER.match(run_before(source, i, "_.'")) is not None


def tokens(source, literals=True):
    """Comment and literal spans, respecting escapes and line continuations,
    C++ raw strings and digit separators."""
    i, n = 0, len(source)
    while i < n:
        start = i
        if literals and source[i] == '"' and (end := raw_string_end(source, i)) is not None:
            i = end
            yield "literal", start, i
        elif literals and source[i] == "'" and digit_separator(source, i):
            i += 1
        elif literals and source[i] in "\"'":
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
