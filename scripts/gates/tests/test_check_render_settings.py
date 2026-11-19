"""Regression fixtures for the render settings ownership gate."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import check_render_settings  # noqa: E402

SETTINGS = """typedef struct {
    int ceiling; /* basic [0, 16]: largest side */
} render_small_fill_settings_t;
typedef struct {
    bool cull; /* basic: skips what is outside */
    render_small_fill_settings_t small_fill; /* small triangles */
} render_settings_t;
"""


class RenderSettingsGateTest(unittest.TestCase):
    def problems(self, lens="vec2f_t shift;", camera="", context="", other="", toml="[small_fill]\nceiling = 4\n"):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            files = {check_render_settings.SETTINGS_HEADER: SETTINGS,
                     check_render_settings.LENS_HEADER: "typedef struct {\n    " + lens + "\n} lens_t;\n",
                     check_render_settings.CAMERA_HEADER: camera,
                     check_render_settings.CONTEXT_HEADER: context,
                     check_render_settings.SCENE_HEADER: "bool scene_set_paused(scene_t* scene, bool on);\n",
                     "main/render/span/span.h": other,
                     "app.render.toml": toml}
            for path, text in files.items():
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                (root / path).write_text(text, encoding="utf-8")
            return check_render_settings.problems(root, [root / "app.render.toml"])

    def test_one_owner_each_passes(self):
        self.assertEqual(self.problems(camera="bool camera_set_shift(entity_t camera, vec2f_t shift);\n"), [])

    def test_a_lens_field_named_like_a_render_setting_fails(self):
        self.assertEqual(self.problems(lens="small_fill_t small_fill;"),
                         ["small_fill.ceiling has two owners: render_settings_t and lens_t.small_fill"])

    def test_a_setter_for_a_render_setting_fails(self):
        found = self.problems(context="void render_context_set_cull(render_context_t* context, bool on);\n")
        self.assertEqual(found, ["cull has two owners: render_settings_t and render_context_set_cull()"])

    def test_any_setter_under_render_or_scene_named_for_a_nested_setting_fails(self):
        found = self.problems(other="void span_set_small_fill_ceiling(span_t* span, int ceiling);\n")
        self.assertEqual(found, ["small_fill.ceiling has two owners: render_settings_t and span_set_small_fill_ceiling()"])

    def test_a_setter_named_only_for_a_group_sets_something_else(self):
        self.assertEqual(self.problems(other="void span_set_small_fill(span_t* span, const fill_t* fill);\n"), [])

    def test_an_undeclared_key_in_a_settings_file_fails(self):
        found = self.problems(toml="[small_fill]\nfar_z = 800.0\n")
        self.assertEqual(len(found), 1)
        self.assertIn("small_fill.far_z is not a render setting", found[0])

    def test_a_value_outside_its_interval_fails(self):
        found = self.problems(toml="[small_fill]\nceiling = 17\n")
        self.assertEqual(len(found), 1)
        self.assertIn("small_fill.ceiling = 17 is outside [0, 16]", found[0])


if __name__ == "__main__":
    unittest.main()
