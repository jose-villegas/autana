"""The project-local settings file, `<project>/autana.local.toml`.

Per-checkout facts only, read by every autana script that needs one; the
environment is never a second way to set any of them. Python 3.9 has no
tomllib, so this reads the subset the keys need - strings, integers, arrays of
strings and `[table]` headers - and refuses anything else by line, as it
refuses an unknown key.
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

# key -> (type, meaning). `docs.llama.home` is `home` under `[docs.llama]`.
KEYS = {
    "docs_extra": (list, "extra Markdown files or folders `autana docs` searches, "
                         "relative to the project"),
    "records": (str, "where captures and index.jsonl land; default .records/device "
                     "in the checkout"),
    "lock_hook": (str, "shell command run on every device lock event; fires only for "
                       "commands run from a checkout whose file sets it"),
    "docs.llama.home": (str, "where the docs model and its server live; default is "
                             "per user, shared by every checkout"),
    "docs.llama.port": (int, "the docs model server's local port; 8765 by default"),
}


class ConfigError(ValueError):
    """A settings file that cannot be trusted; the message names file and line."""


def project_dir():
    return Path(os.environ.get(PROJECT_ENV) or Path.cwd())


def help_text():
    lines = [f"{CONFIG_NAME} in the project folder holds per-checkout settings; "
             "gitignored, optional.", ""]
    for key, (kind, meaning) in KEYS.items():
        lines.append(f"  {key:<16} {kind.__name__:<5} {meaning}")
    return "\n".join(lines)


class Reader:
    def __init__(self, path, text):
        self.path, self.text, self.at, self.line = path, text, 0, 1

    def fail(self, message):
        raise ConfigError(f"{self.path}:{self.line}: {message}")

    def peek(self):
        return self.text[self.at] if self.at < len(self.text) else ""

    def skip_blank(self, newlines):
        while self.peek() and (self.peek() in " \t\r" or (newlines and self.peek() == "\n")
                               or self.peek() == "#"):
            if self.peek() == "#":
                while self.peek() and self.peek() != "\n":
                    self.at += 1
                continue
            if self.peek() == "\n":
                self.line += 1
            self.at += 1

    def string(self):
        quote = self.peek()
        self.at += 1
        out = []
        while True:
            char = self.peek()
            if not char or char == "\n":
                self.fail("a string is not closed on its line")
            self.at += 1
            if char == quote:
                return "".join(out)
            if char == "\\" and quote == '"':
                escape = self.peek()
                self.at += 1
                if escape not in ('\\', '"', "n", "t"):
                    self.fail(f"unsupported escape \\{escape} in a string; "
                              "use a 'literal string' for paths")
                out.append({"n": "\n", "t": "\t"}.get(escape, escape))
            else:
                out.append(char)

    def value(self):
        char = self.peek()
        if char in ('"', "'"):
            return self.string()
        if char == "[":
            self.at += 1
            items = []
            while True:
                self.skip_blank(newlines=True)
                if self.peek() == "]":
                    self.at += 1
                    return items
                if self.peek() not in ('"', "'"):
                    self.fail("an array holds strings only")
                items.append(self.string())
                self.skip_blank(newlines=True)
                if self.peek() == ",":
                    self.at += 1
                elif self.peek() != "]":
                    self.fail("expected , or ] in an array")
        match = re.compile(r"-?\d+").match(self.text, self.at)
        if not match:
            self.fail("a value is a string, an integer or an array of strings")
        self.at = match.end()
        return int(match.group())


def parse(path, text):
    reader, values, table = Reader(path, text), {}, ""
    while True:
        reader.skip_blank(newlines=True)
        char = reader.peek()
        if not char:
            return values
        if char == "[":
            end = text.find("]", reader.at)
            name = text[reader.at + 1:end].strip() if end > 0 else ""
            if text[reader.at + 1:reader.at + 2] == "[" or not re.fullmatch(r"[\w-]+(\.[\w-]+)*", name):
                reader.fail("only plain [table] headers are supported")
            table, reader.at = name + ".", end + 1
            reader.skip_blank(newlines=False)
            if reader.peek() not in ("", "\n"):
                reader.fail("nothing follows a [table] header on its line")
            continue
        match = re.compile(r"[\w-]+").match(text, reader.at)
        if not match:
            reader.fail("expected a key")
        key, reader.at = table + match.group(), match.end()
        reader.skip_blank(newlines=False)
        if reader.peek() != "=":
            reader.fail(f'expected = after "{key}"')
        reader.at += 1
        reader.skip_blank(newlines=False)
        line = reader.line
        value = reader.value()
        if key not in KEYS:
            raise ConfigError(f'{path}:{line}: unknown key "{key}" - `autana help config` '
                              "lists the keys")
        kind = KEYS[key][0]
        if type(value) is not kind:
            raise ConfigError(f'{path}:{line}: "{key}" must be a {kind.__name__}')
        if key in values:
            raise ConfigError(f'{path}:{line}: "{key}" is set twice')
        values[key] = value
        reader.skip_blank(newlines=False)
        if reader.peek() not in ("", "\n"):
            reader.fail("one setting per line")


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
