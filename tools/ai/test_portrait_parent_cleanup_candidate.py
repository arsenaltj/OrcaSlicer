import unittest

from portrait_parent_cleanup_candidate import bind_parent_ownership, build_candidate, digest_json, frozen_boundary


def plan(cells, **overrides):
    value = {
        "source_sha256": "a" * 64,
        "geometry_id": "b" * 64,
        "face_count": 10,
        "boundary_sha256": "c" * 64,
        "detail_freeze_sha256": "d" * 64,
        "partition_ref": {"sha256": "e" * 64, "path": "surface-partitions/" + "e" * 64 + ".json"},
        "shape_lock_sha256": "f" * 64,
        "shape_lock_fingerprint": "0" * 64,
        "runtime_sha256": "1" * 64,
        "policy_sha256": "2" * 64,
        "cells": cells,
    }
    value.update(overrides)
    return value


class CandidateTests(unittest.TestCase):
    def test_shared_parent_map_and_optional_six_color_role(self):
        cells = [{"id": "3" * 64, "slot_uid": "portrait-skin", "label": "skin",
                  "color_source": "PARENT_UNIFORM", "source_face_id": 1,
                  "implicit_root": True, "subject_id": "person"}]
        six = cells + [{"id": "4" * 64, "slot_uid": "portrait-mid", "label": "cloth",
                        "color_source": "PARENT_UNIFORM", "source_face_id": 2,
                        "implicit_root": False, "subject_id": "person"}]
        result = build_candidate(plan(cells), plan(six), (plan(cells), plan(cells)))
        self.assertEqual(len(result["cells"]), 2)
        self.assertEqual(result["cells"][0][1], "portrait-skin")

    def test_non_parent_role_is_rejected(self):
        cells = [{"id": "3" * 64, "slot_uid": "portrait-lips", "label": "skin",
                  "color_source": "PARENT_UNIFORM", "source_face_id": 1, "implicit_root": True}]
        with self.assertRaises(ValueError):
            build_candidate(plan(cells), plan(cells), ())

    def test_source_mapping_is_retained_and_invalid_faces_fail(self):
        row = dict(id="3" * 64, slot_uid="portrait-skin", label="skin", color_source="PARENT_UNIFORM",
                   source_face_id=3, implicit_root=True, subject_id="person")
        self.assertEqual(build_candidate(plan([row]), plan([row]))["cells"][0][3:], [3, True, "person"])
        for face in (-1, 10, True):
            with self.subTest(face=face), self.assertRaises(ValueError):
                bad = dict(row, source_face_id=face)
                build_candidate(plan([bad]), plan([bad]))
        with self.assertRaises(ValueError):
            build_candidate(plan([row, row]), plan([row, row]))

    def test_sibling_source_and_fingerprint_drift_fail(self):
        for key in ("source_sha256", "shape_lock_fingerprint", "policy_sha256"):
            with self.subTest(key=key), self.assertRaises(ValueError):
                build_candidate(plan([]), plan([]), (plan([], **{key: "9" * 64}),))

    def test_freeze_ignores_parent_changes_and_detects_eye_changes(self):
        cell = dict(id="4" * 64, label="le", parent_label="le", subject_id="person",
                    polygon=[[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]], holes=[])
        partition = dict(geometry_id="a" * 64, source_sha256="b" * 64, face_count=100,
                         faces=[dict(source_face_id=1, cells=[cell])])
        locks = dict(locks=[])
        before = frozen_boundary(partition, locks)
        partition["faces"].append(dict(source_face_id=2, cells=[dict(cell, id="5" * 64, label="skin")]))
        self.assertEqual(before, frozen_boundary(partition, locks))
        cell["polygon"][0][0] = .9
        self.assertNotEqual(before, frozen_boundary(partition, locks))

    def test_digest_sorts_keys_like_cpp_json(self):
        self.assertEqual(digest_json(dict(b=2, a=1)), digest_json(dict(a=1, b=2)))

    def test_reviewed_ownership_retains_source_labels_and_blocks_facial_details(self):
        key = "4" * 64
        cell = dict(id=key, label="face", parent_label="face", subject_id="person")
        partition = dict(faces=[dict(source_face_id=2, cells=[cell])])
        candidate = dict(cells=[[key, "portrait-skin", "skin", 2, False, "person"]])
        bind_parent_ownership(candidate, partition)
        self.assertEqual(candidate["ownership_relabels"][key],
                         dict(source_label="face", source_parent_label="face", target_label="skin"))
        self.assertEqual(cell["label"], "face")
        for changes in (dict(label="le"), dict(subject_id="another")):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                bind_parent_ownership(candidate, dict(faces=[dict(source_face_id=2, cells=[dict(cell, **changes)])]))


if __name__ == "__main__":
    unittest.main()
