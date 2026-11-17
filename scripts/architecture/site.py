#!/usr/bin/env python3
"""Validate a model folder and build its static site with installed LikeC4."""
import argparse
import json
import pathlib
import subprocess
import sys

PACKAGE = pathlib.Path(__file__).parent / 'node_modules/likec4'


def command(*args):
    manifest = PACKAGE / 'package.json'
    if not manifest.is_file():
        raise SystemExit(f'LikeC4 is not installed; run npm ci --prefix {PACKAGE.parent}')
    package = json.loads(manifest.read_text(encoding='utf-8'))
    binary = package['bin']
    if isinstance(binary, dict):
        binary = binary['likec4']
    return ['node', str(PACKAGE / binary), *map(str, args)]


def validate(model, config):
    subprocess.run(command('validate', model, '--project', config['name']), check=True)

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('model', type=pathlib.Path)
    parser.add_argument('-o', '--out', required=True, type=pathlib.Path)
    parser.add_argument('--base')
    args = parser.parse_args(argv)
    config = json.loads((args.model / 'likec4.config.json').read_text(encoding='utf-8'))
    validate(args.model, config)
    title = config['title']
    options = ['build', args.model, '-o', args.out, '--use-hash-history', '--title', title, '--project', config['name']]
    if args.base:
        options += ['--base', args.base]
    subprocess.run(command(*options), check=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
