"""Quoted C includes resolved in compiler search order."""
from pathlib import PurePosixPath
import posixpath


def resolve_include(path, include, roots, exists):
    for directory in (str(PurePosixPath(path).parent), *roots):
        candidate = posixpath.normpath(str(PurePosixPath(directory) / include))
        if exists(candidate):
            return candidate
    return None
