"""Formatting and asset gates exercised in isolated scratch trees."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
from gate_tree import ShellGateTestCase


class ShellGateTests(ShellGateTestCase):
    def run_shell(self, path, *args):
        return subprocess.run([self.shell, str(path), *map(str, args)],
                              capture_output=True, text=True, timeout=60)

    def format_gate(self, resolver):
        gates = self.root / 'scripts/gates'
        lib = self.root / 'scripts/lib'
        gates.mkdir(parents=True)
        lib.mkdir()
        gate = gates / 'check-format.sh'
        shutil.copyfile(ROOT / 'scripts/gates/check-format.sh', gate)
        shutil.copyfile(ROOT / 'scripts/lib/python.sh', lib / 'python.sh')
        (lib / 'pinned_tool.py').write_text(resolver)
        return gate

    def formatter(self):
        binary = self.root / 'clang-format'
        self.calls = self.root / 'formatter.calls'
        binary.write_text(
            '#!/bin/sh\n'
            'printf "%s\\n" "$@" >> "' + self.calls.as_posix() + '"\n'
            'case "$1" in\n'
            '    --dry-run) [ "$2" = --Werror ] || exit 9; shift 2;\n'
            '        [ "$(cat "$1")" = "int main(void) { return 0; }" ] ;;\n'
            '    -i) printf "int main(void) { return 0; }\\n" > "$2" ;;\n'
            '    *) exit 9 ;;\n'
            'esac\n')
        binary.chmod(0o755)
        return binary.as_posix()

    def test_crlf_resolution_broken_clean_and_format(self):
        binary = self.formatter()
        gate = self.format_gate('import sys\nsys.stdout.buffer.write(' +
            repr(('19\r\n' + binary + '\r\n').encode()) + ')\n')
        shutil.copyfile(ROOT / '.clang-format', self.root / '.clang-format')
        source = self.root / 'scratch.c'
        source.write_text('int main(){return 0;}\n')
        result = self.run_shell(gate, '--check', source)
        self.assertNotEqual(result.returncode, 0)
        result = self.run_shell(gate, source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('clang-format 19:', result.stdout)
        self.assertNotIn('\r', result.stdout)
        result = self.run_shell(gate, '--check', source)
        self.assertEqual(result.returncode, 0, result.stderr)

        self.assertEqual(self.calls.read_text().splitlines(), [
            '--dry-run', '--Werror', str(source), '-i', str(source),
            '--dry-run', '--Werror', str(source)])

    def test_failed_resolution_exits_before_formatting(self):
        binary = self.formatter()
        gate = self.format_gate('import sys\nprint("19")\nprint(' + repr(binary) + ')\nsys.exit(1)\n')
        source = self.root / 'scratch.c'
        source.write_text('int main(){return 0;}\n')
        before = source.read_bytes()
        result = self.run_shell(gate, source)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(source.read_bytes(), before)
        self.assertNotIn('Formatted', result.stdout)
        self.assertFalse(self.calls.exists())

    def asset_gate(self, conversion):
        tests = self.root / 'render/tests'
        tests.mkdir(parents=True)
        gate = tests / 'check_scene_assets.sh'
        shutil.copyfile(ROOT / 'launcher/tools/render/tests/check_scene_assets.sh', gate)
        renderer = self.root / 'renderer.sh'
        renderer.write_text('#!/bin/sh\nif [ -n "${AUTANA_ASSET_DIR:-}" ]; then\n'
            '    [ -d "$AUTANA_ASSET_DIR" ] || exit 8\n'
            '    [ -z "$(ls -A "$AUTANA_ASSET_DIR")" ] || exit 8\n'
            '    exit 1\nfi\nexit 0\n')
        renderer.chmod(0o755)
        (tests.parent / 'render_scene.sh').write_text(
            'render_scene_build() {\n'
            '    [ "$scene_assets" != main/gfx ] || return 1\n'
            f'    scene_out_dir="{self.root.as_posix()}/out"\n'
            f'    _rs_bin="{renderer.as_posix()}"\n'
            '    mkdir -p "$scene_out_dir/assets"\n'
            '    touch "$scene_out_dir/assets/fixture.apak"\n}\n'
            'to_native() { ' + conversion + '; }\n')
        return gate

    def test_asset_override_uses_an_empty_folder(self):
        result = self.run_shell(self.asset_gate('printf "%s\\n" "$1"'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('not found', result.stderr)

    def test_asset_conversion_failure_fails_the_gate(self):
        result = self.run_shell(self.asset_gate('return 7'))
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('ok scene assets: AUTANA_ASSET_DIR', result.stdout)
        self.assertNotIn('ok scene assets: a folder with no asset roots', result.stdout)
