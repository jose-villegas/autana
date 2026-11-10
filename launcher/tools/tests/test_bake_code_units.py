"""A stage keyed by the units its entry functions reach (bake/code_units.py): every way code is reached
counts, an edit nothing reaches keeps the key, and what the walk cannot follow counts more."""

import ast
import pathlib
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

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
        base = path.parents[level - 1] if level else self.root
        for candidate in (base.joinpath(*parts).with_suffix(".py") if parts else None, base.joinpath(*parts, "__init__.py")):
            if candidate is not None and candidate.is_file():
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
        return bake.digest(sorted(bake.units_digests(self.walk(entry)[1])))


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

    def test_a_decorator_or_default_from_the_same_module_counts(self):
        self.write(entry="""
            LIMIT = 4
            def traced(function):
                return function
            @traced
            def main(limit=LIMIT):
                return limit
            """)
        self.assertTrue({"entry:LIMIT", "entry:traced"} <= self.kept())

    def test_a_class_attribute_read_through_an_instance_counts_the_class_and_its_methods(self):
        self.write(settings="""
            class Fit:
                STEPS = 20
                def unrelated(self):
                    return 0
            """, entry="""
            def main(fit):
                return fit.STEPS
            """)
        self.assertTrue({"settings:Fit", "settings:Fit.unrelated"} <= self.kept())

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


class ImportTests(WalkCase):
    def test_an_import_inside_the_function_counts(self):
        self.write(helpers="def compute():\n    return 1\n", entry="""
            def main():
                from helpers import compute
                return compute()
            """)
        self.assertIn("helpers:compute", self.kept())

    def test_import_a_b_and_import_a_b_as_c_count_what_they_call(self):
        sub = "def far():\n    return 1\ndef other():\n    return 2\n"
        for package_init in ("", "from pkg import sub\n"):
            for spelling, call in (("import pkg.sub", "pkg.sub.far()"), ("import pkg.sub as s", "s.far()")):
                with self.subTest(init=bool(package_init), spelling=spelling):
                    self.write({"pkg.__init__": package_init, "pkg.sub": sub},
                               entry=f"{spelling}\ndef main():\n    return {call}\n")
                    self.assertIn("sub:far", self.kept())
                    self.assertNotIn("sub:other", self.kept())

    def test_import_a_b_counts_through_a_package_without_an_init(self):
        self.write({"ns.sub": "def far():\n    return 1\n"}, entry="import ns.sub\ndef main():\n    return ns.sub.far()\n")
        self.assertIn("sub:far", self.kept())

    def test_relative_imports_reach_the_right_file(self):
        self.write({"pkg.__init__": "", "pkg.inner.__init__": "", "pkg.helpers": "def up():\n    return 1\n",
                    "pkg.inner.helpers": "def near():\n    return 2\n", "helpers": "def up():\n    return 3\n",
                    "pkg.inner.entry": """
            from .helpers import near
            from ..helpers import up
            def main():
                return near() + up()
            """})
        units = self.walk("pkg/inner/entry:main")[1]
        kept = {(unit.path.relative_to(self.root).as_posix(), sorted(unit.names)[0]) for unit in units if unit.names}
        self.assertTrue({("pkg/inner/helpers.py", "near"), ("pkg/helpers.py", "up")} <= kept)
        self.assertNotIn(("helpers.py", "up"), kept)

    def test_every_import_of_a_name_counts(self):
        self.write(fast="def blur():\n    return 1\n", slow="def blur():\n    return 2\n", entry="""
            try:
                from fast import blur
            except ImportError:
                from slow import blur
            def main():
                return blur()
            """)
        self.assertTrue({"fast:blur", "slow:blur"} <= self.kept())

    def test_a_name_bound_to_either_of_two_modules_counts_both(self):
        self.write(cpu="def render():\n    return 1\n", gpu="def render():\n    return 2\n", entry="""
            try:
                import gpu as backend
            except ImportError:
                import cpu as backend
            def main():
                return backend.render()
            """)
        self.assertTrue({"cpu:render", "gpu:render"} <= self.kept())

    def test_a_computed_sys_modules_lookup_counts_every_unit_and_a_foreign_one_none(self):
        shading = "def shade_flat():\n    return 1\ndef other():\n    return 2\n"
        self.write(shading=shading, entry="""
            import sys
            import shading
            def main(kind):
                return sys.modules["shad" + kind].shade_flat()
            """)
        self.assertIn("shading:other", self.kept())
        self.write(shading=shading, entry="""
            import sys
            import shading
            def main():
                return sys.modules["shading"].shade_flat(), sys.modules.get("torch"), "numpy" in sys.modules
            """)
        self.assertTrue({"shading:shade_flat", "shading:other"} <= self.kept())
        self.write(shading=shading, entry="""
            import sys
            import shading
            def main():
                return sys.modules.get("torch"), "numpy" in sys.modules
            """)
        self.assertNotIn("shading:other", self.kept())

    def test_sys_modules_imported_by_name_counts_the_module_it_hands_out(self):
        self.write(shading="def shade_flat():\n    return 1\n", entry="""
            from sys import modules
            import shading
            def main():
                return modules["shading"].shade_flat()
            """)
        self.assertIn("shading:shade_flat", self.kept())

    def test_a_module_used_as_a_value_counts_all_of_it(self):
        self.write(cpu="def render():\n    return 1\n", gpu="def render():\n    return 2\n", entry="""
            import cpu, gpu
            def main(kind):
                return {"cpu": cpu, "gpu": gpu}[kind].render()
            """)
        self.assertTrue({"cpu:render", "gpu:render"} <= self.kept())

    def test_a_name_a_stage_module_does_not_define_counts_that_module_and_is_reported(self):
        self.write(lazy="""
            def __getattr__(name):
                return _impl
            def _impl():
                return 41
            """, entry="import lazy\ndef main():\n    return lazy.thing()\n")
        walk, units = self.walk()
        self.assertIn("thing", [name for _, _, name in walk.unresolved])
        self.assertIn("lazy:_impl", self.kept())


class ImportTimeTests(WalkCase):
    def test_a_statement_that_binds_no_name_or_writes_into_an_object_counts(self):
        self.write(config="""
            import os
            class Cfg:
                samples = 1
            def patched():
                return 2
            os.environ["MODE"] = "egl"
            Cfg.samples = 64
            Cfg.render = patched
            """, entry="import config\ndef main():\n    return config.Cfg.samples\n")
        self.assertTrue({"config:?", "config:patched"} <= self.kept())

    def test_code_a_decorator_or_subclass_registers_counts(self):
        self.write(entry="""
            REG = {}
            def register(fn):
                REG[fn.__name__] = fn
                return fn
            @register
            def flat():
                return 41
            class Base:
                kinds = []
                def __init_subclass__(cls):
                    Base.kinds.append(cls)
            class Smooth(Base):
                def shade(self):
                    return 2
            def main(kind):
                return REG[kind](), Base.kinds
            """)
        self.assertTrue({"entry:flat", "entry:Smooth"} <= self.kept())

    def test_a_def_decorated_by_a_stage_objects_attribute_counts(self):
        self.write(registry="""
            class Registry:
                def __init__(self):
                    self.kinds = {}
                def register(self, fn):
                    self.kinds[fn.__name__] = fn
                    return fn
            shade = Registry()
            """, entry="""
            from registry import shade
            @shade.register
            def flat():
                return 41
            def main(kind):
                return shade.kinds[kind]()
            """)
        self.assertIn("entry:flat", self.kept())

    def test_a_class_body_statement_that_runs_at_import_counts(self):
        self.write(entry="""
            REG = {}
            class Flat:
                REG["flat"] = 41
            def main(kind):
                return REG[kind]
            """)
        self.assertIn("entry:Flat", self.kept())

    def test_a_class_body_assignment_that_calls_stage_code_counts(self):
        self.write(entry="""
            REG = {}
            def build():
                REG["flat"] = 41
                return 1
            class Flat:
                SIZE = build()
            def main(kind):
                return REG[kind]
            """)
        self.assertTrue({"entry:Flat", "entry:build"} <= self.kept())

    def test_an_import_time_call_through_either_of_two_modules_counts(self):
        self.write(gpu="def render():\n    return 1\n", cpu="""
            REG = {}
            def setup():
                REG["cpu"] = 1
                return 1
            """, entry="""
            try:
                import gpu as backend
            except ImportError:
                import cpu as backend
            READY = backend.setup()
            def main():
                return 1
            """)
        self.assertIn("cpu:setup", self.kept())

    def test_a_decorated_method_a_nested_class_statement_and_a_method_default_count(self):
        for body in ("@register\n    def flat(self):\n        return 41",
                     "class Inner:\n        REG[:] = [41]",
                     "def m(self, x=register(None)):\n        return x"):
            with self.subTest(body=body):
                self.write(entry="REG = []\ndef register(fn):\n    REG.append(fn)\n    return fn\n"
                                 f"class Shaders:\n    {body}\ndef main():\n    return REG\n")
                self.assertIn("entry:Shaders", self.kept())

    def test_a_default_argument_that_calls_stage_code_counts(self):
        self.write(entry="""
            REG = {}
            def build():
                REG["flat"] = 41
            def unused(x=build()):
                return x
            def main(kind):
                return REG[kind]
            """)
        self.assertTrue({"entry:unused", "entry:build"} <= self.kept())

    def test_moving_one_statement_past_another_re_keys(self):
        self.write(entry="SCALE = 1.0\nSCALE = 2.0\ndef main():\n    return SCALE\n")
        before = self.key()
        self.write(entry="SCALE = 2.0\nSCALE = 1.0\ndef main():\n    return SCALE\n")
        self.assertNotEqual(self.key(), before)


class ClassTests(WalkCase):
    def test_inherited_and_super_methods_count(self):
        self.write(base="""
            class Shader:
                def __init__(self):
                    self.n = 1
                def light(self):
                    return 1
            """, entry="""
            from base import Shader
            class Flat(Shader):
                def light(self):
                    return super().light() + self.tone()
                def tone(self):
                    return 2
            def main():
                return Flat().light()
            """)
        self.assertTrue({"base:Shader", "base:Shader.__init__", "base:Shader.light", "entry:Flat.tone"} <= self.kept())

    def test_properties_and_methods_named_in_the_class_body_count(self):
        self.write(entry="""
            class Box:
                def _get(self):
                    return 1
                area = property(_get)
                @property
                def size(self):
                    return 2
                @staticmethod
                def unit():
                    return 3
            def main(box):
                return box.area, box.size, Box.unit()
            """)
        self.assertTrue({"entry:Box._get", "entry:Box.size", "entry:Box.unit"} <= self.kept())

    def test_methods_another_packages_base_calls_count(self):
        self.write(entry="""
            import ast
            class Visitor(ast.NodeVisitor):
                def visit_Call(self, node):
                    return 41
            def main(tree):
                return Visitor().visit(tree)
            """)
        self.assertIn("entry:Visitor.visit_Call", self.kept())

    def test_a_method_only_another_package_calls_counts(self):
        self.write(entry="""
            import io
            class Source(io.RawIOBase):
                pass
            class Plain:
                def read(self, size=-1):
                    return b""
            def main():
                return io.BufferedReader(Plain())
            """)
        self.assertIn("entry:Plain.read", self.kept())

    def test_a_global_read_only_in_a_nested_scope_counts(self):
        self.write(entry="""
            LIMIT = 3
            SCALE = 2
            def main(items):
                def inner(x):
                    return x * SCALE
                return [inner(x) for x in items if x < LIMIT]
            """)
        self.assertTrue({"entry:LIMIT", "entry:SCALE"} <= self.kept())


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
            def other():
                return 2
            """, entry="""
            def main(thing):
                return getattr(thing, "shade")()
            """)
        self.assertIn("helpers:Light.shade", self.kept())
        self.assertNotIn("helpers:other", self.kept())

    def test_a_getattr_on_a_module_counts_the_module_or_its_named_function(self):
        self.write(shading="def shade_flat():\n    return 1\ndef other():\n    return 2\n", entry="""
            import shading
            def main(kind):
                return getattr(shading, "shade_" + kind)(), getattr(shading, "shade_flat")
            """)
        self.assertTrue({"shading:shade_flat", "shading:other"} <= self.kept())

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

    def test_the_reference_producers_are_entries_and_unpacking_only_consumes_bytes(self):
        tree = ast.parse((TOOLS / "bake" / "produce.py").read_text(encoding="utf-8"))
        entries = set(bake.STAGES["reference"])
        for node in ast.walk(tree):
            if isinstance(node, ast.ImportFrom) and node.module in ("r3d.reference_set", "r3d.reference_render"):
                for alias in node.names:
                    entry = f"{node.module.replace('.', '/')}.py:{alias.name}"
                    if alias.name == "unpack_references":
                        self.assertNotIn(entry, entries)
                    else:
                        self.assertIn(entry, entries)

    def test_an_edit_to_a_reached_helper_re_keys_the_reference_stage(self):
        units = bake.stage_units("reference")[0]
        helper = next(unit for unit in units if unit.path.name == "mesh_source.py" and unit.names == {"camera_path_poses"})
        before = bake.digest(sorted(bake.units_digests(units)))
        original = bake.line_tokens

        def edited(path):
            return tuple((line, kind, text + "_edited") if path == helper.path and line == helper.node.lineno
                         and kind == "NAME" else (line, kind, text) for line, kind, text in original(path))

        with mock.patch.object(bake, "line_tokens", edited):
            self.assertNotEqual(bake.digest(sorted(bake.units_digests(units))), before)

    def test_the_reference_key_leaves_out_code_it_does_not_reach(self):
        kept = {(unit.path.name, unit.node.name) for unit in bake.stage_units("reference")[0]
                if isinstance(unit.node, code_units.DEFS) and unit.owner is None}
        self.assertIn(("reference_set.py", "prepare_references"), kept)
        self.assertIn(("reference_render.py", "render_sets"), kept)
        self.assertNotIn(("reference_render.py", "main"), kept)
        self.assertFalse({name for path, name in kept if path == "mesh_bundle.py"})


if __name__ == "__main__":
    unittest.main()
