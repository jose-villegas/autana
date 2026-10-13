"""Variant build directories, external links and ESP-IDF configure identity."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'scripts/lib'))
import autana_config
from espressif import idf_commit, idf_path

HASH_LENGTH = 8
OWNER_FILE = 'owner.txt'
IDF_STAMP = '.autana-idf.json'
BUILD_LOCK = 'autana-build.lock'
PRESERVED = {'build.log', BUILD_LOCK}


def is_junction(path):
    """Recognize Windows directory junctions without requiring Python 3.12."""
    try:
        return getattr(os.lstat(path), 'st_reparse_tag', None) == stat.IO_REPARSE_TAG_MOUNT_POINT
    except (OSError, AttributeError):
        return False


def remove_output(path):
    """Remove links themselves; never traverse their build targets."""
    path = Path(path)
    if is_junction(path):
        os.rmdir(path)
    elif path.is_symlink():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def prepare(launcher, name):
    """Link a variant to its configured build root and prune abandoned owners."""
    launcher = Path(launcher).resolve()
    if not re.fullmatch(r'build(?:\.[a-z0-9_-]+)*', name):
        raise ValueError(f'invalid variant build name: {name}')
    link = launcher / name
    config = autana_config.load(launcher.parent)
    root = config.get('build_root')
    if root:
        root = autana_config.path_value(root, launcher.parent).resolve()
    elif os.name == 'nt':
        root = autana_config.user_cache_dir() / 'b'
    else:
        link.mkdir(parents=True, exist_ok=True)
        return link
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    identity = hashlib.sha256(os.path.normcase(str(launcher)).encode()).hexdigest()[:HASH_LENGTH]
    pruned = 0
    for folder in root.iterdir():
        if folder.name == identity or not re.fullmatch(r'[0-9a-f]{' + str(HASH_LENGTH) + '}', folder.name):
            continue
        if folder.is_symlink() or (is_junction(folder)):
            continue
        owner = folder / OWNER_FILE
        if owner.is_file():
            owner_path = owner.read_text(encoding='utf-8').strip()
            # An owner path from the other OS (WSL and Windows can share this
            # root) cannot be tested from here; keep its folder.
            if (os.name != 'nt' and re.match(r'^[A-Za-z]:', owner_path)) or (os.name == 'nt' and owner_path.startswith('/')):
                continue
            if Path(owner_path).exists():
                continue
            remove_output(folder)
            pruned += 1
    if pruned:
        print(f'=== pruned {pruned} orphan build folders ===', file=sys.stderr)
    folder = root / identity
    folder.mkdir(exist_ok=True)
    owner = folder / OWNER_FILE
    if owner.exists() and Path(owner.read_text(encoding='utf-8').strip()) != launcher:
        raise ValueError(f'build folder hash collision: {owner}')
    owner.write_text(str(launcher), encoding='utf-8')
    target = folder / name
    target.mkdir(exist_ok=True)
    if link.exists() or link.is_symlink():
        if link.resolve() == target.resolve():
            return target
        print(f'=== replacing build output at {link} with link to {target} ===', file=sys.stderr)
        remove_output(link)
    if os.name == 'nt':
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        link.symlink_to(target, target_is_directory=True)
    return target


def identity(project=None):
    """Checkout path and commit recorded in each configured build folder."""
    checkout = idf_path(project).resolve()
    return {'commit': idf_commit(checkout), 'path': str(checkout)}


def stamp(build, selected):
    """Record the identity after a successful configure or build."""
    (Path(build) / IDF_STAMP).write_text(json.dumps(selected), encoding='utf-8')


def reconfigure(build, selected):
    """Empty incompatible or unstamped output, preserving the log and build lock."""
    build = Path(build).resolve()
    recorded = build / IDF_STAMP
    old = None
    if recorded.is_file():
        try:
            old = json.loads(recorded.read_text(encoding='utf-8'))
        except (ValueError, OSError):
            old = {'commit': None, 'path': 'invalid stamp'}
    if (old is not None and old != selected) or (old is None and any(
            path.name not in PRESERVED for path in build.iterdir())):
        print(f"=== {build.name}: unstamped output or another ESP-IDF; reconfiguring from scratch ===", file=sys.stderr)
        for path in build.iterdir():
            if path.name not in PRESERVED:
                remove_output(path)


def main():
    try:
        verb, launcher, name = sys.argv[1:4]
        if verb == 'prepare':
            build = prepare(launcher, name)
            reconfigure(build, identity(Path(launcher).parent))
            print(build.as_posix())
        elif verb == 'reset':
            reconfigure(Path(name), identity(Path(launcher).parent))
        elif verb == 'stamp':
            stamp(Path(name), identity(Path(launcher).parent))
        else:
            raise ValueError(f'unknown build directory operation: {verb}')
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
