import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
import beauty_color_quality as quality
from printable_image_pipeline import _srgb_to_lab


class ColorQualityTests(unittest.TestCase):
    def test_color_conversion_matches_existing_reference(self):
        rgb = np.array([[0, 0, 0], [1, 1, 1], [1, 0, 0], [.04, .2, .6], [.8, .3, .4]])
        expected = np.array([_srgb_to_lab(tuple(c*255)) for c in rgb])
        np.testing.assert_allclose(quality._lab(rgb), expected, atol=1e-10)
        self.assertAlmostEqual(quality._ciede2000((50, 2.6772, -79.7751), (50, 0, -82.7485)), 2.0425, places=4)

    def test_area_weighting_and_palette_bound_are_distinct(self):
        source = np.array([[0., 0., 0.], [1., 1., 1.]])
        before = source.copy()
        report = quality.diagnose(source, np.ones((2, 3)), [9., 1.], [10, 20], source)
        distance = quality._ciede2000(*quality._lab(source))
        self.assertAlmostEqual(report['automatic_vs_source']['area_weighted_mean'], .9*distance)
        self.assertEqual(report['independent_face_nearest_palette_bound']['area_weighted_mean'], 0)
        self.assertAlmostEqual(report['extra_vs_independent_face_bound']['area_weighted_mean'], .9*distance)
        self.assertEqual(report['regions'][0]['piece_id'], 10)
        self.assertIsNone(report['regions'][0]['semantic_label'])
        np.testing.assert_array_equal(source, before)
        limited = quality.diagnose(source, np.ones((2, 3)), [9., 1.], [10, 20], [[1., 1., 1.]])
        self.assertEqual(limited['extra_vs_independent_face_bound']['area_weighted_mean'], 0)
        self.assertGreater(limited['independent_face_nearest_palette_bound']['area_weighted_mean'], 0)

    def test_invalid_colors_and_areas_cannot_be_quality_passes(self):
        for area, color in [([0], [[0, 0, 0]]), ([-1], [[0, 0, 0]]),
                            ([float('nan')], [[0, 0, 0]]), ([1], [[2, 0, 0]]),
                            ([1], [[.2, .2, .2]])]:
            with self.subTest(area=area, color=color), self.assertRaises(ValueError):
                quality.diagnose([[0, 0, 0]], color, area, [0], [[0, 0, 0]])

    def test_stationary_faces_are_protected_even_when_the_area_mean_prefers_another_color(self):
        # Two regions exchange a face. Region 1 has a large incoming blue face;
        # it must not turn its stationary black face blue to improve the mean.
        distance = np.array([[0., 10.], [0., 10.], [10., 0.], [10., 0.]])
        old = np.array([1, 1, 2, 2]); new = np.array([1, 2, 1, 2])
        slots, report = quality.constrain_region_palette(distance, [1, 1, 10, 1], old, new,
                                                         [0, 0, 1, 1], [1, 1, 1, 1])
        np.testing.assert_array_equal(slots, [0, 1, 0, 1])
        self.assertEqual(report['stationary_max_error_increase'], 0)
        self.assertEqual(report['all_faces_infeasible_regions'], [1, 2])
        self.assertEqual(report['status'], 'offline_candidate_not_accepted')
        np.testing.assert_array_equal(new, [1, 2, 1, 2])

    def test_constraint_can_improve_a_region_and_handles_anchors_missing_from_new_regions(self):
        distances = [[8., 2.], [9., 1.], [1., 9.]]
        slots, report = quality.constrain_region_palette(distances, [1, 3, 1], [1, 1, 2],
                                                         [1, 1, 3], [0, 0, 1], [0, 0, 1])
        np.testing.assert_array_equal(slots, [1, 1, 0])
        self.assertLess(report['stationary_max_error_increase'], 0)
        self.assertEqual(report['all_faces_infeasible_regions'], [])
        # Zero-area faces do not block an otherwise feasible physical choice.
        slots, _ = quality.constrain_region_palette([[0., 20.], [10., 0.]], [0, 1],
                                                    [1, 1], [1, 1], [0, 0], [0, 0])
        np.testing.assert_array_equal(slots, [1, 1])

    def test_constraint_rejects_inconsistent_regions_and_invalid_distances(self):
        with self.assertRaises(ValueError):
            quality.constrain_region_palette([[0., 1.], [1., 0.]], [1, 1], [1, 1], [1, 1], [0, 1], [0, 0])
        for distance, areas in [([[float('nan'), 1.]], [1]), ([[0., 1.]], [0]), ([[-1., 1.]], [1])]:
            with self.subTest(distance=distance), self.assertRaises(ValueError):
                quality.constrain_region_palette(distance, areas, [1], [1], [0], [0])

    def fixture(self, root):
        values = {'vertices.f32': [[0, 0, 0], [1, 0, 0], [0, 0, 1]],
                  'triangles.i32': [[0, 1, 2]], 'source.f32': [[0, 0, 0]],
                  'automatic.f32': [[0, 0, 0]], 'areas.f64': [[.5]], 'pieces.u32': [[1]]}
        meta = {'schema': 1, 'files': {}, 'source_sha256': 'a'*64,
                'loaded_mesh_colors_sha256': 'b'*64, 'output_sha256': 'c'*64,
                'palette': [{'slot': 0, 'color': '#000000', 'material': 'PLA', 'compatible': True}],
                'automatic_method': 'test fixture'}
        for name, (dtype, columns) in quality.LAYOUT.items():
            array = np.asarray(values[name], dtype=dtype)
            array.tofile(root/name)
            meta['files'][name] = {'shape': list(array.shape), 'dtype': dtype,
                                   'bytes': array.nbytes, 'sha256': quality.digest(root/name)}
        path = root/'manifest.json'
        path.write_text(json.dumps(meta), encoding='utf-8')
        return path

    def test_export_hashes_missing_groups_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manifest = self.fixture(root)
            report = quality.run(manifest, root/'review')
            self.assertEqual(report['status'], 'diagnostic_only')
            self.assertIsNone(report['groups']['target'])
            self.assertIsNone(report['groups']['manual_correction'])
            self.assertTrue(all(value is None for value in report['semantic_regions'].values()))
            self.assertTrue((root/'review/comparison.png').exists())
            with self.assertRaises(FileExistsError): quality.run(manifest, root/'review')
            (root/'areas.f64').write_bytes(b'corrupt')
            with self.assertRaises(ValueError): quality.load_export(manifest)


if __name__ == '__main__':
    unittest.main()
