"""A stage keyed by the units its entry functions reach (bake/code_units.py): every way code is reached
counts, an edit nothing reaches keeps the key, and what the walk cannot follow counts more."""

import pathlib
import sys
import tempfile
import textwrap
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from bake import bake, code_units  # noqa: E402


class WalkCase(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name).resolve()
        self.clear()

    def tearDown(self):
        self.clear()
        self.directory.cleanup()

    def clear(self):
        bake.module_facts.cache_clear()

    def write(self, files=None, **named):
        """Writes each module, named as imported (`pkg.sub`, `pkg.__init__`), with its text."""
        for name, text in {**(files or {}), **named}.items():
            path = self.root.joinpath(*name.split(".")).with_suffix(".py")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(textwrap.dedent(text))
        self.clear()

    def find(self, path, name, level):
        parts = name.split(".") if name else []
        for candidate in (self.root.joinpath(*parts).with_suffix(".py"), self.root.joinpath(*parts, "__init__.py")):
            if parts and candidate.is_file():
                return candidate.resolve()
        return None

    def walk(self, entry="entry:main"):
        paths = sorted(path.resolve() for path in self.root.rglob("*.py"))
        walk = code_units.Walk(paths, self.find)
        file, _, function = entry.partition(":")
        return walk, walk.units([((self.root / f"{file}.py").resolve(), function)])

    def kept(self, entry="entry:main"):
        """`module:name` of every unit the entry reaches, a method as `module:Class.name`."""
        names = set()
        for unit in self.walk(entry)[1]:
            name = unit.node.name if unit.owner else ",".join(sorted(unit.names)) or "?"
            names.add(f"{unit.path.stem}:{unit.owner + '.' if unit.owner else ''}{name}")
        return names

    def key(self, entry="entry:main"):
        units = self.walk(entry)[1]
        return bake.digest(sorted(bake.digest(tokens) for tokens in
                                  (bake.code_tokens(unit.path, *unit.lines) for unit in units) if tokens))


class ReachTests(WalkCase):
    def test_a_method_called_through_self_counts_and_its_edit_re_keys(self):
        self.write(entry="""
            class Tracer:
                def run(self):
                    return self.shade()
                def shade(self):
                    return 1
                def unused(self):
                    return 2
            def main():
                return Tracer().run()
            """)
        self.assertTrue({"entry:Tracer.run", "entry:Tracer.shade", "entry:Tracer"} <= self.kept())
        before = self.key()
        self.write(entry=(self.root / "entry.py").read_text().replace("return 1", "return 3"))
        self.assertNotEqual(self.key(), before)

    def test_an_edit_nothing_reaches_keeps_the_key(self):
        self.write(helpers="""
            def used():
                return 1
            def unused():
                return 2
            """, entry="""
            from helpers import used
            def main():
                return used()
            """)
        before = self.key()
        self.assertNotIn("helpers:unused", self.kept())
        self.write(helpers=(self.root / "helpers.py").read_text().replace("return 2", "return 5"))
        self.assertEqual(self.key(), before)

    def test_a_decorator_counts(self):
        self.write(helpers="""
            def traced(function):
                return function
            """, entry="""
            from helpers import traced
            @traced
            def main():
                return 1
            """)
        self.assertIn("helpers:traced", self.kept())

    def test_a_default_argument_counts_and_so_does_a_constant_read_only_there(self):
        self.write(helpers="""
            LIMIT = 4
            def scale(x):
                return x
            """, entry="""
            from helpers import LIMIT, scale
            def main(limit=LIMIT, op=scale):
                return op(limit)
            """)
        self.assertTrue({"helpers:LIMIT", "helpers:scale"} <= self.kept())

    def test_a_class_attribute_read_through_an_instance_counts(self):
        self.write(settings="""
            class Fit:
                STEPS = 20
                def unrelated(self):
                    return 0
            """, entry="""
            def main(fit):
                return fit.STEPS
            """)
        self.assertIn("settings:Fit", self.kept())
        self.assertNotIn("settings:Fit.unrelated", self.kept())

    def test_a_module_alias_two_hops_deep_counts(self):
        self.write({"pkg.__init__": "from pkg import sub\n", "pkg.sub": """
            def far():
                return 1
            def other():
                return 2
            """}, entry="""
            import pkg
            def main():
                return pkg.sub.far()
            """)
        self.assertIn("sub:far", self.kept())
        self.assertNotIn("sub:other", self.kept())

    def test_a_callable_passed_as_an_argument_counts(self):
        self.write(helpers="""
            def weight(x):
                return x
            def order(item):
                return item
            """, entry="""
            import functools
            from helpers import weight, order
            def main(items):
                scaled = list(map(functools.partial(weight), items))
                return sorted(scaled, key=order)
            """)
        self.assertTrue({"helpers:weight", "helpers:order"} <= self.kept())

    def test_a_name_imported_through_a_re_export_counts(self):
        self.write(inner="def deep():\n    return 1\n", outer="from inner import deep\n", entry="""
            from outer import deep
            def main():
                return deep()
            """)
        self.assertIn("inner:deep", self.kept())

    def test_a_star_import_is_followed(self):
        self.write(inner="def deep():\n    return 1\n", entry="""
            from inner import *
            def main():
                return deep()
            """)
        self.assertIn("inner:deep", self.kept())

    def test_import_time_statements_count_and_a_main_guard_does_not(self):
        self.write(helpers="""
            TABLE = {}
            def register(name):
                TABLE[name] = 1
                return name
            REGISTERED = register("a")
            for key in ("b",):
                TABLE[key] = 2
            UNREAD = 7
            def cli():
                return 0
            if __name__ == "__main__":
                cli()
            """, entry="""
            import helpers
            def main():
                return 0
            """)
        kept = self.kept()
        self.assertTrue({"helpers:REGISTERED", "helpers:register", "helpers:key", "helpers:TABLE"} <= kept)
        self.assertFalse({"helpers:UNREAD", "helpers:cli"} & kept)

    def test_a_class_brings_its_dunders_and_a_method_its_class(self):
        self.write(entry="""
            class Scene:
                SIZE = 2
                def __init__(self):
                    self.n = 1
                def __call__(self):
                    return self.n
                def other(self):
                    return 0
            def main(scene):
                return scene.other()
            """)
        self.assertTrue({"entry:Scene", "entry:Scene.__init__", "entry:Scene.__call__"} <= self.kept())


class CountsMoreTests(WalkCase):
    def test_a_name_the_walk_cannot_place_counts_its_whole_module_and_is_reported(self):
        self.write(entry="""
            def main():
                return mystery()
            def other():
                return 1
            """)
        walk, units = self.walk()
        self.assertEqual([(path.name, name) for path, _, name in walk.unresolved], [("entry.py", "mystery")])
        self.assertIn("entry:other", self.kept())

    def test_a_computed_getattr_counts_every_method(self):
        self.write(helpers="""
            class Light:
                def shade(self):
                    return 1
            """, entry="""
            def main(thing, name):
                return getattr(thing, name)()
            """)
        self.assertIn("helpers:Light.shade", self.kept())

    def test_a_constant_getattr_counts_only_that_name(self):
        self.write(helpers="""
            class Light:
                def shade(self):
                    return 1
                def other(self):
                    return 2
            """, entry="""
            def main(thing):
                return getattr(thing, "shade")()
            """)
        self.assertIn("helpers:Light.shade", self.kept())
        self.assertNotIn("helpers:Light.other", self.kept())

    def test_eval_or_globals_count_every_unit(self):
        for call in ("eval('x')", "globals()['x']"):
            with self.subTest(call=call):
                self.write(helpers="def far():\n    return 1\n", entry=f"""
                    import helpers
                    def main():
                        return {call}
                    """)
                self.assertIn("helpers:far", self.kept())

    def test_a_method_named_eval_on_another_package_is_not_the_builtin(self):
        self.write(helpers="def far():\n    return 1\n", entry="""
            import numpy
            import helpers
            def main(x):
                return numpy.eval(x)
            """)
        self.assertNotIn("helpers:far", self.kept())

    def test_a_missing_entry_function_fails(self):
        self.write(entry="def main():\n    return 1\n")
        with self.assertRaisesRegex(LookupError, "entry.py:absent"):
            self.walk("entry:absent")


class StageTests(unittest.TestCase):
    def test_every_stage_keyed_by_functions_places_every_name_it_reaches(self):
        staged = [stage for stage, entries in bake.STAGES.items() if all(":" in entry for entry in entries)]
        self.assertIn("reference", staged)
        for stage in staged:
            with self.subTest(stage=stage):
                units, unresolved = bake.stage_units(stage)
                self.assertTrue(units)
                self.assertEqual([(bake.relative(path), line, name) for path, line, name in unresolved], [])

    def test_the_reference_key_leaves_out_code_it_does_not_reach(self):
        kept = {(unit.path.name, unit.node.name) for unit in bake.stage_units("reference")[0]
                if isinstance(unit.node, code_units.DEFS) and unit.owner is None}
        self.assertIn(("reference_set.py", "prepare_references"), kept)
        self.assertIn(("reference_render.py", "render_sets"), kept)
        self.assertNotIn(("reference_render.py", "main"), kept)
        self.assertFalse({name for path, name in kept if path == "mesh_bundle.py"})


if __name__ == "__main__":
    unittest.main()
