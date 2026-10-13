"""A compile database entry's compiler arguments, and the files its compile reads (-M)."""
import os
import ntpath
import posixpath
import re
import shlex
import subprocess


def compile_flags(entry):
    command = entry.get("arguments") or shlex.split(entry["command"], posix=os.name != "nt")
    directory = entry.get("directory", "")
    path = ntpath if os.name == "nt" or ntpath.splitdrive(entry["file"])[0] else posixpath
    source = path.normcase(path.normpath(path.join(directory, entry["file"].replace("\\", "/"))))
    filtered = []
    skip = False
    for arg in command:
        if os.name == "nt":
            if arg.startswith('@"'):
                arg = "@" + arg[2:-1]
            elif arg.startswith('"') and arg.endswith('"'):
                arg = arg[1:-1]
            else:
                arg = arg.replace('\\"', '"')
        if skip:
            skip = False
        elif arg in {"-o", "-MF", "-MT", "-MQ"}:
            skip = True
        elif arg not in {"-c", "-MD", "-MMD", "-MP"} and path.normcase(path.normpath(path.join(directory, arg.replace("\\", "/")))) != source:
            filtered.append(arg)
    return filtered


def parse_dependencies(output, directory=""):
    """Absolute paths a compiler's -M/-MM text lists, in order: continuations joined, each rule's target dropped, escaped spaces, '#' and '$' unescaped."""
    paths = []
    for rule in re.sub(r"\\\r?\n", " ", output).splitlines():
        for name in re.findall(r"(?:\\[ \t#]|[^\s])+", re.split(r":(?:\s|$)", rule, maxsplit=1)[-1]):
            paths.append(os.path.abspath(os.path.join(directory, re.sub(r"\\([ \t#])|\$\$", lambda m: m.group(1) or "$", name))))
    return paths


def dependencies(entry):
    """Files the entry's source depends on: its own command run with -M -MG."""
    directory = entry.get("directory", "")
    try:
        scan = subprocess.run([*compile_flags(entry), "-M", "-MG", "-MT", "deps", entry["file"]], cwd=directory or None, capture_output=True, text=True)
    except (OSError, KeyError, ValueError) as error:
        raise RuntimeError(f"dependency scan failed for {entry.get('file')}: {error}") from error
    if scan.returncode:
        raise RuntimeError(f"dependency scan failed for {entry['file']}:\n{scan.stderr.strip()}")
    return parse_dependencies(scan.stdout, directory)
