"""What the settings-file commands (scene/scene_edit.py, r3d/import_edit.py) share: their refusal, reading a file
with its own line ends, and printing what they read as JSON or writing it to a file."""

import pathlib


class Refused(Exception):
    """A read or an edit the command will not make; its message says why."""


def read_text(path):
    """The file's text with its own line ends, its failure as Refused."""
    try:
        with open(path, encoding="utf-8", newline="") as source:
            return source.read()
    except OSError as error:
        raise Refused(f"{path}: {error}") from error


def emit(text, out):
    """`text` printed, or written to the file `out` names."""
    if out:
        pathlib.Path(out).write_text(text + "\n", encoding="utf-8")
    else:
        print(text)


def add_reader(commands, name, file, help_text):
    """A command that prints what `file` holds as JSON, or writes it to -o / --out."""
    reader = commands.add_parser(name, help=help_text)
    reader.add_argument(file)
    reader.add_argument("-o", "--out", dest="out", metavar="OUT.json", help="write the JSON here instead of printing it")
