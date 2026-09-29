"""The project-local settings file, `<project>/autana.local.toml`.

Per-checkout facts only, read by every autana script that needs one; the
environment is never a second way to set any of them. Python 3.9 has no
tomllib, so this reads exactly what the one writer of the file emits - flat
`key = "..."` or `key = '...'` lines and one-line arrays of such strings - and
refuses anything else by line, as it refuses an unknown key. The syntax is
TOML's, so tomllib can replace this reader later.
"""

import os
import re
from pathlib import Path

CONFIG_NAME = "autana.local.toml"
# Private channels autana fills for its own children; never settings. The
# project a running command acts on, so a child reads the file of the checkout
# it was asked about, not its own cwd's; the board it was told to use; and
# the token that proves a flash's children hold the board's lock.
PROJECT_ENV = "_AUTANA_PROJECT"
BOARD_ENV = "_AUTANA_BOARD"
TOKEN_ENV = "_AUTANA_DEVICE_LOCK_TOKEN"

KEYS = {
    "docs_extra": (list, "extra Markdown files or folders `autana docs` searches, "
                         "relative to the project"),
    "records": (str, "where captures and index.jsonl land; default .records/device "
                     "in the checkout"),
    "lock_hook": (str, "shell command run on every device lock event; fires only for "
                       "commands run from a checkout whose file sets it"),
}


class ConfigError(ValueError):
    """A settings file that cannot be trusted; the message names file and line."""


def project_dir():
    return Path(os.environ.get(PROJECT_ENV) or Path.cwd())


def help_text():
    lines = [f"{CONFIG_NAME} in the project folder holds per-checkout settings; "
             "gitignored, optional.", ""]
    for key, (kind, meaning) in KEYS.items():
        lines.append(f"  {key:<11} {kind.__name__:<5} {meaning}")
    return "\n".join(lines)


def string(text, at, fail):
    """The string starting at text[at] and the index after it."""
    quote, out = text[at], []
    at += 1
    while at < len(text):
        char = text[at]
        at += 1
        if char == quote:
            return "".join(out), at
        if char == "\\" and quote == '"':
            escape = text[at:at + 1]
            at += 1
            if escape not in ("\\", '"'):
                fail(f"unsupported escape \\{escape} in a string; use a 'literal string' for paths")
            out.append(escape)
        else:
            out.append(char)
    fail("a string is not closed on its line")


def value(text, at, fail):
    """A string or a one-line array of strings from text[at:], and what follows it."""
    if text[at:at + 1] in ('"', "'"):
        return string(text, at, fail)
    if text[at:at + 1] != "[":
        fail("a value is a string or a one-line array of strings")
    items, at = [], at + 1
    while True:
        at += len(text[at:]) - len(text[at:].lstrip(" \t"))
        if text[at:at + 1] == "]":
            return items, at + 1
        if text[at:at + 1] not in ('"', "'"):
            fail("an array holds strings only, on one line")
        item, at = string(text, at, fail)
        items.append(item)
        at += len(text[at:]) - len(text[at:].lstrip(" \t"))
        if text[at:at + 1] == ",":
            at += 1
        elif text[at:at + 1] != "]":
            fail("expected , or ] in an array")


def parse(path, text):
    values = {}
    for number, line in enumerate(text.splitlines(), 1):
        def fail(message):
            raise ConfigError(f"{path}:{number}: {message}")

        line = line.strip()
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"([\w-]+)\s*=\s*(.+)", line)
        if not match:
            fail("expected key = value")
        key, rest = match.groups()
        if key not in KEYS:
            fail(f'unknown key "{key}" - `autana help config` lists the keys')
        if key in values:
            fail(f'"{key}" is set twice')
        parsed, end = value(rest, 0, fail)
        if rest[end:].strip() and not rest[end:].strip().startswith("#"):
            fail("one setting per line")
        if type(parsed) is not KEYS[key][0]:
            fail(f'"{key}" must be a {KEYS[key][0].__name__}')
        values[key] = parsed
    return values


def load(project=None):
    """The settings of `project` (default: the one this command acts on) as
    {key: value}; empty when it has no file. Raises ConfigError."""
    path = (Path(project) if project else project_dir()) / CONFIG_NAME
    try:
        text = path.read_text(encoding="utf-8-sig")
    except FileNotFoundError:
        return {}
    except OSError as error:
        raise ConfigError(f"{path}: cannot be read: {error}") from error
    return parse(path, text)


def path_value(value, project=None):
    """A configured path: `~` expanded, relative to the project."""
    return (Path(project) if project else project_dir()) / Path(value).expanduser()
