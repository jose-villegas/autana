"""Cards reports from the import geometry and their generated Markdown tables."""

import csv
import unittest
import tempfile
from pathlib import Path
import sys
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from r3d.cards import cards_report, decide
from r3d.import_settings import albedo_jobs, load_import_settings
from r3d.light import drop_masked
from r3d.mesh_source import load_source
from tests.test_cards import SEEDS, SIDE, leaf_import, quads, card_settings
from tests.r3d_env import needs_llvm  # noqa: E402


def report_job(root):
    p, t = quads()
    settings_path = leaf_import(root, p, t)
    settings_path.write_text(settings_path.read_text().replace('name = "mesh"\n', ''))
    with settings_path.open('a') as stream:
        stream.write(f'simplify = {{ dense_edge = {SIDE}, seal_seams = false, colour_deviation = 1.0 }}\n'
                     f'[[variants]]\nname = "mesh"\ntriangles = {len(t)}\nceiling = {len(t)}\n')
    job = albedo_jobs(card_settings(settings_path))[0]
    job.object = SimpleNamespace(name='fitted_full', kind='mesh_renderer')
    job.bake = object()
    return job


class CardsReportTests(unittest.TestCase):
    @needs_llvm
    def test_cards_report_csv_fields_and_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp_path = Path(directory)
            job = report_job(tmp_path)
            data = tmp_path / 'docs/render/offline/data'
            data.mkdir(parents=True)
            scene = tmp_path / 'leaves.scene.toml'
            src = load_source(job.settings)
            _, _, materials, _ = drop_masked(src.p, src.uv, src.tri_v, src.tri_t, src.tri_m,
                                              src.textures, job.settings.alpha_keep)
            for seed in SEEDS:
                job.settings.seed = seed
                with patch.object(cards_report, 'REPO', tmp_path), patch.object(cards_report, 'load_scene',
                                  return_value=SimpleNamespace(renderers=[job])):
                    assert cards_report.main([str(scene), '--object', job.object.name, '--docs']) == 0
                with (data / 'leaves-fitted-full-cards.csv').open(newline='') as stream:
                    reader = csv.DictReader(stream)
                    assert reader.fieldnames == ['material', 'see_through', 'covered', 'covered_area', 'accepted', 'replaced', 'cards',
                                                 'pages', 'texture_bytes', 'triangles', 'ceiling', 'budget', 'scene_total']
                    leaf, total = list(reader)
                assert leaf['material'] == 'leaf'
                assert leaf['accepted'] == str(True)
                assert int(leaf['cards']) > 0
                assert int(leaf['replaced']) == sum(materials == src.names.index('leaf'))
                assert total['material'] == ''
                assert int(total['scene_total']) > int(leaf['cards'])
                assert job.bake is not None

    @needs_llvm
    def test_cards_report_table(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp_path = Path(directory)
            job = report_job(tmp_path)
            rows = cards_report.rows_for(job, None)
            rows[0]['material'] = 'leaf|branch'
            rows[0]['see_through'] = 'always'
            data, output = tmp_path / 'cards.csv', tmp_path / 'cards.md'
            cards_report.write(data, rows)
            assert cards_report.main([str(data), '--table', str(output)]) == 0
            text = output.read_text()
            leaf, total = rows
            assert '| Material | See-through | Covered (seen) | Covered (all area) | Carded | Triangles without cards | Card triangles | Pages | Texture bytes |' in text
            assert (f"| leaf\\|branch | always | {leaf['covered']:.1%} | {leaf['covered_area']:.1%} | yes | {leaf['replaced']} | {leaf['cards']} | {leaf['pages']} | {leaf['texture_bytes']} |") in text
            assert (f"requested {total['triangles']}, ceiling {total['ceiling']}, "
                    f"rest budget {total['budget']}, emitted by the unlit import {total['scene_total']}") in text

    @needs_llvm
    def test_cards_report_rejected_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp_path = Path(directory)
            job = report_job(tmp_path)
            with patch.object(decide, 'CARD_COVERED_MIN', float('inf')):
                leaf, total = cards_report.rows_for(job, None)
            assert not leaf['accepted']
            assert leaf['cards'] == 0
            assert leaf['replaced'] > 0
            assert 0 < total['budget'] <= job.renderer.variant.triangles

    @needs_llvm
    def test_cards_report_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp_path = Path(directory)
            job = report_job(tmp_path)
            variant = job.renderer.variant
            requested = variant.triangles
            leaf, _ = cards_report.rows_for(job, None)
            cards = leaf['cards']
            for ceiling in (None, requested, requested + cards):
                variant.ceiling = ceiling
                leaf, total = cards_report.rows_for(job, None)
                assert total['triangles'] == requested
                assert total['ceiling'] == ('' if ceiling is None else ceiling)
                limit = requested if ceiling is None else min(requested, ceiling)
                assert 0 < total['budget'] <= limit - leaf['cards']
                assert total['scene_total'] <= limit
