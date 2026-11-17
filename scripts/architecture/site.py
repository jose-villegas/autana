#!/usr/bin/env python3
"""Validate a model folder and build its static site with installed LikeC4.

    python scripts/architecture/site.py <model folder> -o <site> [--base /<path>/] [--open <view id>]

The site's root lists every view; --open makes the root open that view
instead (index is the context diagram), and a link to the list still works.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

PACKAGE = pathlib.Path(__file__).parent / 'node_modules/likec4'
# Sent to the view from a root address, before the app reads the address.
OPEN_VIEW = '<script>if (!location.hash || location.hash === "#/") location.replace("#/view/{view}/");</script>'


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


def open_view(site, view):
    """Make the root of the built site open `view`."""
    if not re.fullmatch(r'\w+', view):
        raise SystemExit(f'--open takes a view id, not {view!r}')
    index = site / 'index.html'
    text = index.read_text(encoding='utf-8')
    if '<head>' not in text:
        raise SystemExit(f'{index} has no <head> to open {view} from')
    index.write_text(text.replace('<head>', '<head>' + OPEN_VIEW.format(view=view), 1), encoding='utf-8')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('model', type=pathlib.Path)
    parser.add_argument('-o', '--out', required=True, type=pathlib.Path)
    parser.add_argument('--base')
    parser.add_argument('--open', metavar='VIEW', help='view the root opens, e.g. index')
    args = parser.parse_args(argv)
    config = json.loads((args.model / 'likec4.config.json').read_text(encoding='utf-8'))
    validate(args.model, config)
    title = config['title']
    options = ['build', args.model, '-o', args.out, '--use-hash-history', '--title', title, '--project', config['name']]
    if args.base:
        options += ['--base', args.base]
    subprocess.run(command(*options), check=True)
    if args.open:
        open_view(args.out, args.open)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
