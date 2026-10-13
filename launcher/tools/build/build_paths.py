"""Variant build directories, external links and ESP-IDF configure identity."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
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


def remove_output(path):
    """Remove links themselves; never traverse their build targets."""
    path = Path(path)
    if os.name == 'nt' and path.is_junction():
        os.rmdir(path)
    elif path.is_symlink():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def prepare(launcher, name):
    launcher = Path(launcher).resolve()
    if not re.fullmatch(r'build(?:\.[a-z0-9_-]+)*', name):
        raise ValueError(f'invalid variant build name: {name}')
    link = launcher / name
    config = autana_config.load(launcher.parent)
    root = config.get('build_root')
    if root:
        root = autana_config.path_value(root, launcher.parent).resolve()
    elif os.name == 'nt':
        root = Path(os.environ['LOCALAPPDATA']) / 'autana/b'
    else:
        link.mkdir(parents=True, exist_ok=True)
        return link
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    pruned = 0
    for folder in root.iterdir():
        if not re.fullmatch(r'[0-9a-f]{' + str(HASH_LENGTH) + '}', folder.name):
            continue
        if folder.is_symlink() or (os.name == 'nt' and folder.is_junction()):
            continue
        owner = folder / OWNER_FILE
        if owner.is_file() and not Path(owner.read_text(encoding='utf-8').strip()).exists():
            remove_output(folder)
            pruned += 1
    if pruned:
        print(f'=== pruned {pruned} orphan build folders ===', file=sys.stderr)
    identity = hashlib.sha256(os.path.normcase(str(launcher)).encode()).hexdigest()[:HASH_LENGTH]
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
    checkout = idf_path(project).resolve()
    return {'commit': idf_commit(checkout), 'path': str(checkout)}


def stamp(build, selected):
    (Path(build) / IDF_STAMP).write_text(json.dumps(selected), encoding='utf-8')


def reconfigure(build, selected):
    build = Path(build).resolve()
    recorded = build / IDF_STAMP
    old = None
    if recorded.is_file():
        try:
            old = json.loads(recorded.read_text(encoding='utf-8'))
        except (ValueError, OSError):
            old = {'commit': None, 'path': 'invalid stamp'}
    elif (build / 'CMakeCache.txt').is_file():
        cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8', errors='replace')
        match = re.search(r'^IDF_PATH:[^=]+=(.+)$', cache, re.M)
        metadata = build / 'project_description.json'
        try:
            facts = json.loads(metadata.read_text(encoding='utf-8')) if metadata.is_file() else {}
        except ValueError:
            facts = {}
        path = match.group(1).strip() if match else facts.get('idf_path', '')
        revision = facts.get('git_revision', '')
        match = re.search(r'-g([0-9a-f]+)(?:-dirty)?$', revision)
        commit = selected['commit'] if match and selected['commit'] and selected['commit'].startswith(match[1]) else revision
        old = {'path': str(Path(path).resolve()) if path else '', 'commit': commit}
    if old is not None and old != selected:
        print(f"=== {build.name} was configured for ESP-IDF {old.get('commit')} ({old.get('path')}); "
              f"now {selected['commit']}: reconfiguring from scratch ===", file=sys.stderr)
        for path in build.iterdir():
            if path.name not in PRESERVED:
                remove_output(path)


def main():
    try:
        verb, launcher, name = sys.argv[1:4]
        build = prepare(launcher, name)
        if verb == 'prepare':
            reconfigure(build, identity(Path(launcher).parent))
            print(build.as_posix())
        elif verb == 'configured':
            return 0 if (build / IDF_STAMP).is_file() else 1
        elif verb == 'stamp':
            stamp(build, identity(Path(launcher).parent))
        else:
            raise ValueError(f'unknown build directory operation: {verb}')
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
