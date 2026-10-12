"""Compiler arguments shared by preprocessing and stack graph measurements."""
import os
import shlex


def compile_flags(entry):
    command = entry.get("arguments") or shlex.split(entry["command"], posix=os.name != "nt")
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
        elif arg not in {"-c", "-MD", "-MMD", "-MP"} and arg.replace("\\", "/") != entry["file"].replace("\\", "/"):
            filtered.append(arg)
    return filtered
