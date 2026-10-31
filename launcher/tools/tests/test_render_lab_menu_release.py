"""Render Lab's BOOT menu is the same in every build.

The host tests build only the development side of a build-variant gate, so
an option gated out of release passes them unseen. These read the source
instead: the menu screen and every scene's `.controls` draw no option
behind an #if, and no `.controls` is wrapped in a macro that a release
turns to NULL. A declaration a release drops fails the release build itself.
"""
import pathlib
import re
import unittest

RENDER_LAB = pathlib.Path(__file__).resolve().parents[2] / "main/apps/render_lab"
MENU_SCREEN = RENDER_LAB / "ui/render_lab_menu_screen.c"

CONDITIONAL_RE = re.compile(r"^\s*#\s*if", re.MULTILINE)
# A scene's hook, not a copy of one read through `->`.
CONTROLS_RE = re.compile(r"\.controls\s*=\s*(?![\w()]*->)([^,}\s]+)")


def function_body(source, name):
    """The text from `name(`'s definition to its closing brace in column 0."""
    match = re.search(r"^" + re.escape(name) + r"\([^;{]*\)\s*\{(.*?)^\}", source, re.MULTILINE | re.DOTALL)
    return None if match is None else match.group(1)


def scene_controls():
    """(file, hook) for every scene's `.controls`."""
    for path in sorted(RENDER_LAB.glob("*.c")) + sorted(RENDER_LAB.glob("*.h")):
        for hook in CONTROLS_RE.findall(path.read_text(encoding="utf-8")):
            yield path, hook


class MenuInReleaseTests(unittest.TestCase):
    def test_the_menu_screen_gates_no_option(self):
        self.assertIsNone(CONDITIONAL_RE.search(MENU_SCREEN.read_text(encoding="utf-8")),
                          f"{MENU_SCREEN.name} has an #if: every menu option stays in release")

    def test_every_scene_control_is_drawn_in_every_build(self):
        found = list(scene_controls())
        self.assertTrue(found, "no scene has a .controls hook")
        for path, hook in found:
            with self.subTest(file=path.name, hook=hook):
                self.assertRegex(hook, r"^[A-Za-z_]\w*$", "a .controls hook is a function, never a build-gated macro")
                body = function_body(path.read_text(encoding="utf-8"), hook)
                self.assertIsNotNone(body, f"{hook} is not defined in {path.name}")
                self.assertIsNone(CONDITIONAL_RE.search(body), f"{hook} has an #if: its options stay in release")


if __name__ == "__main__":
    unittest.main()
