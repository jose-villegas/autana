"""Pose-level linear stage fitting and held-out scoring."""
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import stress_predict as predict


def pose(x, y=0, row="scene", t=0):
    return {"row": row, "t_ms": t, "counts": {"x": x, "y": y},
            "stages": {stage: 2 + 3 * x + 5 * y for stage in predict.STAGES}}


def every(*names):
    return {stage: names for stage in predict.STAGES}


class StressPredictTest(unittest.TestCase):
    def test_exact_model_recovers_intercept_and_coefficients_and_passes(self):
        training = [pose(x, y) for x, y in ((0, 0), (1, 0), (0, 1), (2, 3))]
        model = predict.fit(training, every("x", "y"))
        np.testing.assert_allclose(model["weights"]["r3d.draw"], [2, 3, 5])
        rows = predict.score(model, [pose(3, 4)])
        self.assertTrue(all(row["pass"] for row in rows))
        self.assertEqual(rows[-1]["stage"], "frame")

    def test_missing_term_fails_and_names_it(self):
        model = predict.fit([pose(x) for x in range(4)], every("x", "y"))
        rows = predict.score(model, [pose(1, 8)])
        self.assertFalse(all(row["pass"] for row in rows))
        self.assertIn("y", " ".join(model["notes"]))
        self.assertIn("y", " ".join(rows[0]["unsupported"]))

    def test_weights_stay_non_negative_and_stage_uses_only_its_inputs(self):
        training = [pose(x) for x in range(4)]
        for item in training:
            item["stages"]["r3d.cull"] = 4 - item["counts"]["x"]
        model = predict.fit(training, {**every("x"), "r3d.upscale": ()})
        self.assertTrue(all(np.all(w >= 0) for w in model["weights"].values()))
        np.testing.assert_allclose(model["weights"]["r3d.upscale"], [2 + 3 * 1.5])
        self.assertEqual(model["inputs"]["r3d.upscale"], ())

    def test_p95_uses_pose_errors_instead_of_error_of_means(self):
        model = predict.fit([pose(x) for x in range(4)], every("x"))
        scenes = [pose(1, t=t) for t in range(20)]
        for item in scenes[:2]:
            item["stages"]["r3d.draw"] *= 2
        rows = predict.score(model, scenes)
        self.assertFalse(next(r for r in rows if r["stage"] == "r3d.draw")["pass"])

    def test_delta_uses_a_counts_and_noise_floor(self):
        a = [pose(x) for x in range(4)]
        b = [pose(x) for x in range(4)]
        for item in b:
            item["stages"] = {stage: value + item["counts"]["x"] for stage, value in item["stages"].items()}
        scene_a, scene_b = [pose(2)], [pose(99)]
        scene_b[0]["stages"] = {stage: 10 for stage in predict.STAGES}
        rows = predict.score(predict.fit(a, every("x")), scene_a, predict.fit(b, every("x")), scene_b,
                             {stage: 0.01 for stage in (*predict.STAGES, "frame")})
        self.assertTrue(all(row["pass"] for row in rows))
        self.assertAlmostEqual(rows[0]["predicted"], 2)

    def test_layout_pair_without_shift_is_not_seen(self):
        a = [pose(1, row="stress", t=t) for t in range(3)]
        rows = predict.layout_rows(a, a, {stage: 0.01 for stage in (*predict.STAGES, "frame")})
        self.assertTrue(all(row["seen"] == "not seen" for row in rows))

    def test_pair_keeps_shared_poses_and_rejects_none_shared(self):
        left, right = predict.paired([pose(1), pose(2, t=1)], [pose(3), pose(4, t=2)])
        self.assertEqual([(p["t_ms"], p["counts"]["x"]) for p in left + right], [(0, 1), (0, 3)])
        with self.assertRaisesRegex(ValueError, "share no pose"):
            predict.paired([pose(1)], [pose(1, t=1)])

    def test_cli_scores_each_scene_stage_frame_delta_and_layout(self):
        from build.c_header import read_define
        header = predict.ROOT / "launcher/test/frame_profile.h"
        replay = read_define(header, "FRAME_PROFILE_REPLAY")
        with tempfile.TemporaryDirectory() as folder:
            def capture(name, points, change=0, quarter="portrait"):
                lines = []
                for t, (x, y) in enumerate(points):
                    counts = dict.fromkeys(predict.TERMS, 0)
                    counts.update(clusters_kept=x, vertices_transformed=y, pixels_shaded=y)
                    fields = " ".join(f"{key}={value}" for key, value in counts.items())
                    beta = {"clusters_kept": 3 + change, "vertices_transformed": 5, "pixels_shaded": 4}
                    stages = "  ".join(
                        f"{stage} {2 + sum(beta.get(n, 0) * counts[n] for n in names):.6f}/0.00"
                        for stage, names in predict.STAGE_INPUTS.items())
                    lines.append(replay % (name, t, fields, stages + " | total 1.00") + f" quarter={quarter}")
                path = pathlib.Path(folder) / f"{name}-{change}-{quarter}.log"
                path.write_text("\n".join(lines))
                return str(path)
            training = ((0, 0), (1, 0), (0, 1), (2, 3))
            a = capture("stress", training)
            b = capture("stress", training, 1)
            scenes_a = capture("scene", ((3, 4), (2, 1)))
            scenes_b = capture("scene", ((3, 4), (2, 1)), 1)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = predict.main(["--a", a, "--b", b, "--scenes-a", scenes_a, "--scenes-b", scenes_b,
                                       "--noise-a", a, "--noise-b", a, "--layout-a", a, "--layout-b", a])
            self.assertEqual(result, 0)
            text = output.getvalue()
            self.assertIn("Absolute frame rule: PASS", text)
            self.assertIn("Option-flip delta frame rule: PASS", text)
            self.assertIn("not seen", text)
            self.assertIn("Same-build p95 absolute noise floor", text)
            self.assertTrue(text.rstrip().endswith("every stage and every scene passes."))
            landscape = capture("water", ((3, 4), (2, 1)), quarter="landscape")
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ValueError, "no frame_replay lines in portrait"):
                    predict.main(["--a", a, "--scenes", landscape])
                self.assertEqual(predict.main(["--a", a, "--scenes", landscape, "--scene-quarter", "landscape"]), 0)

    def test_loader_rejects_missing_counts(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / "capture.log"
            from build.c_header import read_define
            header = predict.ROOT / "launcher/test/frame_profile.h"
            line = read_define(header, "FRAME_PROFILE_REPLAY") % (
                "stress", 0, "x=1", "r3d.cull 1.00/1.00 | total 1.00")
            path.write_text(line + " quarter=portrait\n")
            with self.assertRaisesRegex(ValueError, "missing counts.*y"):
                predict.load([path], "portrait", names=("x", "y"))


if __name__ == "__main__":
    unittest.main()
