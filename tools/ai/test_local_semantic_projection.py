"""Offline synthetic evidence tests; no inference, provider, files, or GUI."""
import itertools
import unittest
from dataclasses import replace
from unittest.mock import patch

import numpy as np

import local_semantic_projection as p


def observation(view, faces, labels=2, confidence=1., detection=1., repeat=2):
    ids = np.repeat(np.asarray(faces, dtype=np.int32), repeat).reshape(1, -1)
    label = np.full(ids.shape, labels, dtype=np.uint8) if np.isscalar(labels) else np.repeat(labels, repeat).astype(np.uint8).reshape(ids.shape)
    score = np.full(ids.shape, confidence, dtype=np.float64)
    return p.Observation(view, ids, label, score, detection)


def assignments(result):
    return {s[0]: (r["subject_id"], r["label"], s[1:]) for r in result["regions"] for s in r["samples"]}


def shared_rasters(items):
    """Place synthetic detections in their common full-view face-ID raster."""
    result = []
    for item in items:
        peers = [o for o in items if o.view_id == item.view_id]
        raster = np.concatenate([o.face_ids.ravel() for o in peers]).reshape(1, -1)
        labels = np.zeros(raster.shape, dtype=np.uint8)
        scores = np.ones(raster.shape, dtype=np.float64)
        for face, label, score in zip(item.face_ids.ravel(), item.labels.ravel(), item.confidence.ravel()):
            labels[raster == face], scores[raster == face] = label, score
        result.append(replace(item, face_ids=raster, labels=labels, confidence=scores))
    return result


class ProjectionTests(unittest.TestCase):
    def test_schema_matches_worker_without_loading_models(self):
        import local_semantic_worker as worker
        self.assertEqual(tuple(worker.LABEL_NAMES), p.LABEL_NAMES)
        # Clothing is accepted only from the optional body supplement; enabling
        # it in FaRL projection would change the established face baseline.
        self.assertEqual(tuple(worker.SUPPORTED_LABELS), p.SUPPORTED_LABELS + ('cloth',))

    def test_empty_no_face_and_low_detection_are_unknown(self):
        for items in ([], [observation("a", [-1], labels=0)], [observation("a", [0], detection=.89)]):
            result = p.project(items, 8)
            self.assertEqual(result["subjects"], [])
            self.assertEqual(result["regions"], [])
            self.assertEqual(result["statistics"]["unknown_faces"], 8)

    def test_only_visible_faces_are_projected(self):
        result = p.project([observation("a", [-1, 2], labels=8)], 8)
        self.assertEqual(set(assignments(result)), {2})
        self.assertEqual(result["statistics"]["unseen_faces"], 7)
        self.assertEqual(assignments(result)[2][1], "re")

    def test_multi_person_and_view_order_are_exactly_stable(self):
        items = shared_rasters([observation("a", [0, 1, 2, 3]), observation("b", [1, 2, 3, 4]),
                                observation("a", [10, 11, 12]), observation("b", [10, 11, 12, 13])])
        expected = p.project(items, 20)
        self.assertEqual(len(expected["subjects"]), 2)
        for order in itertools.permutations(items):
            self.assertEqual(p.project(order, 20), expected)

    def test_surface_based_subject_id_ignores_camera_name_and_label(self):
        a = p.project([observation("front", [7, 8, 9])], 20)
        b = p.project([observation("back", [9, 7, 8], labels=14)], 20)
        self.assertEqual(a["subjects"], b["subjects"])
        self.assertTrue(a["subjects"][0].startswith("surface-"))

    def test_insufficient_overlap_keeps_independent_subjects_and_shared_faces_unknown(self):
        result = p.project([observation("a", [0, 1, 2]), observation("b", [2, 3, 4])], 8)
        self.assertEqual(len(result["subjects"]), 2)
        self.assertEqual(set(assignments(result)), {0, 1, 3, 4})
        self.assertEqual(result["statistics"]["cross_subject_faces"], 1)

    def test_overlap_fraction_is_required_in_addition_to_three_faces(self):
        result = p.project([observation("a", list(range(20))),
                            observation("b", [0, 1, 2] + list(range(20, 37)))], 40)
        self.assertEqual(len(result["subjects"]), 2)
        self.assertEqual(result["statistics"]["cross_subject_faces"], 3)

    def test_same_view_bridge_marks_whole_component_unknown(self):
        items = shared_rasters([observation("a", [0, 1, 2]), observation("a", [3, 4, 5]),
                                observation("b", [0, 1, 2, 3, 4, 5])])
        result = p.project(items, 10)
        self.assertEqual(result["regions"], [])
        self.assertEqual(result["statistics"]["ambiguous_components"], 1)
        self.assertEqual(result["statistics"]["ambiguous_faces"], 6)

    def test_same_view_detections_cannot_claim_shared_face_by_score(self):
        result = p.project(shared_rasters([observation("a", [0, 1], detection=.91),
                                           observation("a", [1, 2], detection=1.)]), 4)
        self.assertEqual(set(assignments(result)), {0, 2})

    def test_resolution_does_not_dominate_conflicting_view(self):
        a = observation("a", [0, 1, 2], labels=8, repeat=200)
        b = observation("b", [0, 1, 2], labels=9, repeat=2)
        result = p.project([a, b], 4)
        self.assertEqual(result["regions"], [])
        self.assertEqual(result["statistics"]["below_threshold_faces"], 3)

    def test_background_unsupported_and_low_confidence_not_removed_from_denominator(self):
        for other in (0, 3, 15, 16, 17, 18, 9):
            item = observation("a", [0] * 10, labels=[8] * 8 + [other] * 2, repeat=1)
            item.confidence[0, -2:] = .01
            result = p.project([item], 1)
            self.assertEqual(result["regions"], [], other)

    def test_low_confidence_and_single_pixel_are_unknown(self):
        self.assertFalse(p.project([observation("a", [0], confidence=.899)], 2)["regions"])
        self.assertFalse(p.project([observation("a", [0], repeat=1)], 2)["regions"])
        result = p.project([observation("a", [0, 1, 2], repeat=1),
                            observation("b", [0, 1, 2], repeat=1)], 3)
        self.assertEqual(result["statistics"]["known_faces"], 3)
        for entry in assignments(result).values():
            self.assertEqual(entry[2][-2:], [2, 2])

    def test_confidence_is_not_pixel_count_weighted_between_views(self):
        a = observation("a", [0, 1, 2], confidence=.91, repeat=100)
        b = observation("b", [0, 1, 2], confidence=1., repeat=2)
        for entry in assignments(p.project([a, b], 4)).values():
            self.assertAlmostEqual(entry[2][0], .955)
            self.assertAlmostEqual(entry[2][1], 1.)

    def test_exact_confidence_threshold_is_not_lost_by_accumulation(self):
        item = observation("a", [0], confidence=.9, repeat=1000)
        result = p.project([item], 1)
        self.assertEqual(result["statistics"]["known_faces"], 1)
        self.assertEqual(assignments(result)[0][2][0], .9)

    def test_unaligned_same_view_rasters_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "same face raster"):
            p.project([observation("a", [0]), observation("a", [1])], 2)

    def test_correlated_crops_cannot_outvote_a_conflicting_angle(self):
        # Seven agreeing IDs would dominate 7:1 without families. They all use
        # one physical viewing direction and must contribute only one vote.
        items = [replace(observation(f"crop-{i}", [0, 1, 2], labels=8), view_family="front")
                 for i in range(7)]
        items.append(replace(observation("other-angle", [0, 1, 2], labels=9), view_family="side"))
        self.assertEqual(p.project(items, 3)["regions"], [])
        self.assertEqual(p.project(list(reversed(items)), 3), p.project(items, 3))
        independent = [replace(o, view_family=None) for o in items]
        self.assertEqual(p.project(independent, 3)["statistics"]["known_faces"], 3)

    def test_same_family_multiresolution_has_one_view_support(self):
        items = [replace(observation("parent", [0, 1, 2], repeat=2), view_family="front"),
                 replace(observation("crop", [0, 1, 2], repeat=100), view_family="front")]
        result = p.project(items, 3)
        self.assertEqual(result["statistics"]["view_families"], 1)
        for entry in assignments(result).values():
            self.assertEqual(entry[2][-2:], [102, 1])
        other = replace(observation("side", [0, 1, 2]), view_family="side")
        for entry in assignments(p.project(items + [other], 3)).values():
            self.assertEqual(entry[2][-2:], [104, 2])

    def test_family_divisor_is_per_face_not_per_component(self):
        parent = replace(observation("parent", [0, 1, 2, 3], confidence=.9), view_family="front")
        crop = replace(observation("crop", [0, 1, 2]), view_family="front")
        side = replace(observation("side", [0, 1, 2, 3]), view_family="side")
        result = assignments(p.project([parent, crop, side], 4))
        self.assertEqual(result[3][2][-2:], [4, 2])
        self.assertAlmostEqual(result[3][2][0], .95)
        self.assertAlmostEqual(result[3][2][1], 1.)

    def test_family_does_not_change_surface_id_or_default_behavior(self):
        items = [observation("a", [0, 1, 2]), observation("b", [0, 1, 2])]
        original = p.project(items, 3)
        explicit_default = p.project([replace(o, view_family=o.view_id) for o in items], 3)
        self.assertEqual(original, explicit_default)
        grouped = p.project([replace(o, view_family="same") for o in items], 3)
        self.assertEqual(grouped["subjects"], original["subjects"])

    def test_family_does_not_relax_same_view_detection_ambiguity(self):
        items = shared_rasters([observation("a", [0, 1, 2]), observation("a", [3, 4, 5]),
                                observation("b", [0, 1, 2, 3, 4, 5])])
        result = p.project([replace(o, view_family="same") for o in items], 6)
        self.assertEqual(result["regions"], [])
        self.assertEqual(result["statistics"]["ambiguous_components"], 1)

    def test_family_identifier_and_per_view_consistency_are_strict(self):
        original = observation("a", [0])
        for family in ("", "../bad", "x" * 97, 4, True):
            with self.subTest(family=family), self.assertRaises(ValueError):
                p.project([replace(original, view_family=family)], 1)
        with self.assertRaisesRegex(ValueError, 'different families'):
            p.project([original, replace(original, view_family="other")], 1)

    def test_unsupported_classes_never_become_regions(self):
        for label in (0, 3, 15, 16, 17, 18):
            self.assertEqual(p.project([observation("a", [0], labels=label)], 1)["regions"], [])

    def test_wire_samples_are_canonical_and_no_user_authority_fields_exist(self):
        item = observation("a", [3, 1, 2], labels=[8, 9, 8])
        originals = [a.copy() for a in (item.face_ids, item.labels, item.confidence)]
        result = p.project([item], 5)
        self.assertEqual(set(result), {"subjects", "regions", "statistics"})
        for region in result["regions"]:
            self.assertEqual(set(region), {"subject_id", "label", "samples"})
            self.assertEqual([s[0] for s in region["samples"]], sorted(s[0] for s in region["samples"]))
            self.assertTrue(all(len(s) == 5 for s in region["samples"]))
        for actual, original in zip((item.face_ids, item.labels, item.confidence), originals):
            np.testing.assert_array_equal(actual, original)

    def test_invalid_values_shapes_and_types_are_rejected(self):
        base = observation("a", [0])
        bad = [replace(base, view_id="../path"), replace(base, detection_score=True),
               replace(base, detection_score=float("nan")), replace(base, detection_score=1.1),
               replace(base, face_ids=np.array([[-2]], dtype=np.int32)),
               replace(base, face_ids=np.array([[9, 9]], dtype=np.uint64)),
               replace(base, face_ids=base.face_ids.astype(float)),
               replace(base, labels=base.labels.astype(np.int32)),
               replace(base, labels=np.full(base.labels.shape, 19, np.uint8)),
               replace(base, labels=base.labels[:, :1]),
               replace(base, confidence=np.full(base.confidence.shape, np.nan)),
               replace(base, confidence=np.full(base.confidence.shape, -1.)),
               replace(base, confidence=np.full(base.confidence.shape, 1.01)),
               replace(base, face_ids=np.zeros((1, 1025), dtype=np.int32))]
        for item in bad:
            with self.subTest(item=item), self.assertRaises(ValueError):
                p.project([item], 2)
        for count in (0, -1, True, 2_000_001, 1.):
            with self.assertRaises(ValueError):
                p.project([], count)

    def test_resource_limits_reject_instead_of_truncating(self):
        for items in ([observation(str(i), [0]) for i in range(17)],
                      [observation("a", [0]) for _ in range(9)],
                      shared_rasters([observation(str(i // 8), [i]) for i in range(33)])):
            with self.assertRaises(ValueError):
                p.project(items, 40)
        with patch.object(p, "MAX_PIXELS", 3), self.assertRaises(ValueError):
            p.project([observation("a", [0, 1])], 2)


if __name__ == "__main__":
    unittest.main()
