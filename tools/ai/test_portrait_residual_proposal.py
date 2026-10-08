import copy
import json
import tempfile
import unittest
from pathlib import Path

from portrait_residual_proposal import (
    POLICY_VERSION,
    ProposalError,
    SCHEMA,
    apply_proposal,
    build_proposal,
    cache_key,
    validate_proposal,
    write_cache,
)


def identity():
    return {
        "source_sha256": "a" * 64,
        "geometry_id": "b" * 64,
        "face_count": 100,
        "evidence_sha256": "c" * 64,
        "shape_lock_sha256": "d" * 64,
        "runtime_sha256": "e" * 64,
        "policy_sha256": "f" * 64,
    }


def unit(unit_id, parent, faces, kind, **extra):
    value = {"unit_id": unit_id, "subject_id": "person-a", "parent_region": parent,
             "face_ids": faces, "proposal": kind, "confidence": 0.94,
             "views": [{"id": "front", "pixels": 100}, {"id": "three-quarter", "pixels": 80}]}
    value.update(extra)
    return value


def request(*units, budget=15):
    value = {"schema": "orca.portrait-residual-request/v1", **identity(),
             "remaining_triangle_budget": budget, "frozen_faces": [1, 2], "units": list(units)}
    return value


class PortraitResidualProposalTests(unittest.TestCase):
    def test_all_local_proposal_types_and_identity(self):
        result = build_proposal(
            request(unit("skin-1", "face", [10], "skin"),
                    unit("hair-1", "hair", [11], "hair"),
                    unit("cloth-1", "cloth", [12], "cloth"),
                    unit("keep-1", "face", [13], "preserve"),
                    unit("cut-1", "face", [14], "subdivide", additional_triangles=3)),
        )
        self.assertEqual(result["schema"], SCHEMA)
        self.assertEqual(result["policy_version"], POLICY_VERSION)
        self.assertEqual([item["proposal"] for item in result["proposals"]],
                         ["skin", "hair", "cloth", "preserve", "subdivide"])
        self.assertEqual(result["remaining_triangle_budget"], 12)
        validate_proposal(result, identity())

    def test_frozen_and_cross_boundary_units_are_rejected(self):
        value = request(unit("frozen", "face", [1, 3], "skin"),
                        unit("eye", "le", [4], "skin", eye_side="left"),
                        unit("mixed", "face", [5], "skin", mixed=True),
                        unit("cross", "face", [6], "skin", cross_subject=True))
        value["frozen_faces"].append(4)
        result = build_proposal(value)
        self.assertEqual(result["status"], "PROTECTED_R9")
        self.assertEqual([item["reasons"][0] for item in result["rejected"]],
                         ["FROZEN_SHAPE_LOCK", "FROZEN_SHAPE_LOCK", "MIXED_FACE_UNCONFIRMED", "CROSS_SUBJECT"])
        self.assertEqual(len(result["proposals"]), 0)
        self.assertEqual(result["statistics"]["rejected"], 4)

    def test_mixed_face_accepts_only_explicit_leaf_keys(self):
        result = build_proposal(
            request(unit("mixed", "face", [10], "skin", mixed=True,
                         leaf_keys=[[10, 1, 0], [10, 1, 1]])),
        )
        self.assertEqual(result["proposals"][0]["leaf_keys"], [[10, 1, 0], [10, 1, 1]])
        with self.assertRaises(ProposalError):
            build_proposal(request(unit("bad", "face", [10], "skin", mixed=True,
                                    leaf_keys=[[11, 1, 0]])))

    def test_single_view_is_preserved_and_reported(self):
        result = build_proposal(request(unit("one", "face", [10], "skin",
                                           views=[{"id": "front", "pixels": 2}])))
        self.assertEqual(result["proposals"][0]["proposal"], "preserve")
        self.assertIn("INSUFFICIENT_VIEW_SUPPORT", result["proposals"][0]["reasons"])

    def test_subdivide_budget_is_fail_closed(self):
        result = build_proposal(request(unit("cut", "face", [10], "subdivide", additional_triangles=16), budget=15))
        self.assertEqual(result["proposals"][0]["status"], "UNAPPLIED_BUDGET")
        self.assertIn("SUBDIVIDE_BUDGET_UNAVAILABLE", result["proposals"][0]["reasons"])
        applied = apply_proposal(result, identity())
        self.assertEqual(applied["applied"], [])

    def test_identity_drift_is_rejected(self):
        result = build_proposal(request(unit("skin", "face", [10], "skin")))
        changed = identity()
        changed["source_sha256"] = "0" * 64
        with self.assertRaises(ProposalError):
            validate_proposal(result, changed)
        with self.assertRaises(ProposalError):
            apply_proposal(result, changed)

    def test_apply_only_returns_non_frozen_local_actions(self):
        result = build_proposal(request(unit("skin", "face", [10], "skin"),
                                        unit("preserve", "face", [20], "preserve")))
        applied = apply_proposal(result, identity())
        self.assertEqual(applied["status"], "APPLIED")
        self.assertEqual([item["unit_id"] for item in applied["applied"]], ["skin"])
        self.assertFalse(applied["material_tree_changed"])
        self.assertFalse(applied["provider_called"])

    def test_cancelled_and_unavailable_paths_keep_r9(self):
        result = build_proposal(request(unit("skin", "face", [10], "skin")), cancelled=True)
        self.assertEqual(result["status"], "CANCELLED")
        applied = apply_proposal(result | {"status": "PROTECTED_R9", "proposals": [], "rejected": []}, identity(), cancelled=True)
        self.assertEqual(applied, {"status": "CANCELLED", "applied": [], "preserved": "R9"})

    def test_content_addressed_cache_does_not_replace_payload(self):
        result = build_proposal(request(unit("skin", "face", [10], "skin")))
        with tempfile.TemporaryDirectory() as directory:
            path = write_cache(Path(directory), result)
            self.assertTrue(path.is_file())
            self.assertIn("portrait_residuals", path.parts)
            self.assertEqual(write_cache(Path(directory), copy.deepcopy(result)), path)
            self.assertEqual(path.read_bytes(), json.dumps(result, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode())
            self.assertTrue(cache_key(result))

    def test_online_fields_are_rejected(self):
        value = request(unit("skin", "face", [10], "skin"))
        value["endpoint"] = "https://example.invalid"
        with self.assertRaises(ProposalError):
            build_proposal(value)

    def test_verified_counts_use_real_support_and_low_confidence_is_preserved(self):
        result = build_proposal(request(unit("safe", "face", [10], "skin",
            evidence_source="verified-local-semantics", view_support=3, pixel_support=72),
            unit("uncertain", "face", [11], "skin", confidence=.5)))
        self.assertEqual(result["proposals"][0]["view_support"], 3)
        self.assertEqual(result["proposals"][1]["proposal"], "preserve")
        self.assertEqual(result["statistics"]["unresolved_source_faces"], 99)

    def test_zero_pixel_views_cannot_authorize_paint(self):
        result = build_proposal(request(unit("empty", "face", [10], "skin",
            views=[{"id":"front", "pixels":0}, {"id":"side", "pixels":0}])))
        self.assertEqual(result["proposals"][0]["proposal"], "preserve")

    def test_parent_mismatch_and_hard_risk_are_rejected(self):
        result = build_proposal(request(unit("wrong", "hair", [10], "skin"),
            unit("hard", "face", [11], "skin", risk_reasons=["CROSS_SUBJECT"])))
        self.assertEqual([r["reasons"][0] for r in result["rejected"]], ["CROSS_PARENT", "CROSS_SUBJECT"])

    def test_overlap_and_ancestor_leaves_are_rejected(self):
        result = build_proposal(request(unit("safe", "face", [10], "skin")))
        result["proposals"][0]["rejected_faces"] = [10]
        with self.assertRaises(ProposalError): validate_proposal(result)
        with self.assertRaises(ProposalError):
            build_proposal(request(unit("ancestor", "face", [10], "skin", leaf_keys=[[10,0,0],[10,1,0]])))

    def test_cache_separates_palette_and_edit_state(self):
        a = build_proposal(request(unit("safe", "face", [10], "skin", target_rgb=[.8,.6,.5])))
        b = build_proposal(request(unit("safe", "face", [10], "skin", target_rgb=[.9,.7,.6])))
        self.assertNotEqual(cache_key(a), cache_key(b))


if __name__ == "__main__":
    unittest.main()
