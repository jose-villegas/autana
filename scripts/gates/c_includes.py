"""Quoted C includes resolved in compiler search order."""
from pathlib import PurePosixPath
import posixpath


def normalize_include_path(path):
    return posixpath.normpath(path)


def resolve_include(path, include, roots, exists):
    for directory in (str(PurePosixPath(path).parent), *roots):
        candidate = normalize_include_path(str(PurePosixPath(directory) / include))
        if exists(candidate):
            return candidate
    return None
