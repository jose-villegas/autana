"""Compiler arguments shared by preprocessing and stack graph measurements."""
import os
import ntpath
import posixpath
import shlex


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
