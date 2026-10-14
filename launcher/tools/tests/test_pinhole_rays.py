"""Pinhole classification and fitted coverage on tiny meshes."""
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from tests.r3d_env import needs_mitsuba


@needs_mitsuba
class PinholeRays(unittest.TestCase):
    def setUp(self):
        import numpy as np
        from render import pinhole_rays
        from r3d.lit_mesh import write_lit_mesh
        self.np, self.tool, self.write_mesh = np, pinhole_rays, write_lit_mesh
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.pose = [0, 0, 0, 0, 0, -1]
        camera = SimpleNamespace(background=0, path=SimpleNamespace(animation='camera.anim.toml', node='camera'),
                                 half_fov_short_tan=1.0, near_z=0.1)
        fit = SimpleNamespace(size=(5, 3), coverage_every_ms=100)
        job = SimpleNamespace(object=SimpleNamespace(name='wall'), renderer=SimpleNamespace(fit=fit))
        self.scene = SimpleNamespace(camera=SimpleNamespace(component=camera), renderers=[job])

    def mesh(self, name, positions, double=None):
        np = self.np
        positions = np.asarray(positions, float)
        tris = np.arange(len(positions)).reshape(-1, 3)
        double = np.zeros(len(tris), bool) if double is None else np.asarray(double)
        self.write_mesh(self.root, name, positions, np.full(positions.shape, 255), tris, double)
        return self.root / (name + '.mesh')

    def test_rays_reports_gap_and_raster_miss_at_covered_pixel(self):
        np = self.np
        mesh = self.mesh('gap', [[-3, -3, -2], [-0.3, -3, -2], [-0.3, 3, -2],
                                 [0.3, -3, -2], [3, -3, -2], [0.3, 3, -2]])
        frame = np.full((5, 5, 3), 255, np.uint8)
        frame[2, 2] = 0
        frame[3, 1] = 0
        frame = np.repeat(np.repeat(frame, 2, axis=0), 2, axis=1)
        poses = 'size 5 5\nlens 1 0.1\npose 0 0 0 0 0 1\npose 0 0 0 0 0 -1\n'
        output = io.StringIO()
        with mock.patch.object(self.tool, 'load_scene', return_value=self.scene), \
             mock.patch.object(self.tool, 'read_video', return_value=(30, iter([frame]))), \
             mock.patch.object(self.tool.track_host, 'poses', return_value=poses) as sample, \
             contextlib.redirect_stdout(output):
            self.assertEqual(self.tool.main(['rays', 'scene.scene.toml', '--mesh', str(mesh),
                                             '--video', 'frames.avi', '--dt', '33']), 0)
        self.assertIn('hole pixels 2; hits 1; pass through 1', output.getvalue())
        self.assertEqual(sample.call_args.kwargs['until_ms'], 66)

    def test_shown_excludes_hidden_triangle(self):
        mesh = self.mesh('stack', [[-4, -4, -2], [4, -4, -2], [0, 4, -2],
                                   [-4, -4, -3], [4, -4, -3], [0, 4, -3]])
        self.assertEqual(self.tool.count_shown(mesh, (5, 5, 1, 0.1, [self.pose]), (5, 5), variant='scalar_rgb'), (1, 2))

    def test_back_faces_pass_through_and_near_plane_clips(self):
        mesh = self.mesh('sides', [[-4, -4, -0.05], [4, -4, -0.05], [0, 4, -0.05],
                                   [0, 4, -1], [4, -4, -1], [-4, -4, -1],
                                   [-4, -4, -2], [4, -4, -2], [0, 4, -2]])
        query, normals, double = self.tool.mesh_query(mesh, 'scalar_rgb')
        origin, direction = self.tool.pose_rays(1, 1, 1, 0.1, self.pose)
        tri, depth = self.tool.front_hits(query, normals, double, origin, direction)
        self.assertGreaterEqual(tri[0], 0)
        self.assertAlmostEqual(depth[0], 1.9, places=5)

    def test_shown_uses_fit_spacing_and_square_coverage_view(self):
        output = io.StringIO()
        with mock.patch.object(self.tool, 'sample_camera_rows', return_value=(5, 3, 1, 0.1, [self.pose])) as sample, \
             mock.patch.object(self.tool, 'count_shown', return_value=(1, 2)) as count, \
             mock.patch.object(self.tool, 'load_scene', return_value=self.scene), \
             contextlib.redirect_stdout(output):
            self.assertEqual(self.tool.main(['shown', 'scene.scene.toml', '--object', 'wall', 'start.mesh']), 0)
        self.assertEqual(sample.call_args.args[2:5], (100, 5, 3))
        self.assertEqual(count.call_args.args[1][:4], (5, 5, 5 / 3, 0.1))
        self.assertIn('triangles 2; shown 1', output.getvalue())

    def test_llvm_shown_excludes_hidden_triangle(self):
        import drjit as dr
        if not dr.has_backend(dr.JitBackend.LLVM):
            self.skipTest('the LLVM variant needs libLLVM')
        mesh = self.mesh('stack', [[-4, -4, -2], [4, -4, -2], [0, 4, -2],
                                   [-4, -4, -3], [4, -4, -3], [0, 4, -3]])
        self.assertEqual(self.tool.count_shown(mesh, (5, 5, 1, 0.1, [self.pose]), (5, 5)), (1, 2))

    def test_against_depth_categories(self):
        np = self.np
        self.assertEqual(self.tool.classify_against([11.5, 12, np.inf, 5], [10, 10, 10, np.inf]),
                         {'front': 1, 'behind': 1, 'miss': 1, 'unknown': 1})

    def test_double_sided_back_face_is_drawn(self):
        mesh = self.mesh('double', [[0, 4, -1], [4, -4, -1], [-4, -4, -1]], double=[True])
        query, normals, double = self.tool.mesh_query(mesh, 'scalar_rgb')
        origin, direction = self.tool.pose_rays(1, 1, 1, 0.1, self.pose)
        tri, depth = self.tool.front_hits(query, normals, double, origin, direction)
        self.assertGreaterEqual(tri[0], 0)
        self.assertAlmostEqual(depth[0], 0.9, places=5)


if __name__ == '__main__':
    unittest.main()
