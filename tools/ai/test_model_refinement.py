#!/usr/bin/env python3
import unittest

from tools.ai.model_refinement import (
    MAX_ISSUES,
    MAX_PROMPT_SUFFIX_BYTES,
    REPORT_SCHEMA_VERSION,
    build_model_refinement_advice,
)


class ModelRefinementAdviceTests(unittest.TestCase):
    def test_structural_warnings_map_to_stable_priority_advice(self):
        advice = build_model_refinement_advice(
            {
                "status": "review",
                "warnings": [
                    "tiny_printable_color_regions",
                    "localized_overhang_regions",
                    "thin_local_wall_regions",
                ],
            },
            {},
        )

        self.assertEqual(advice["schema"], REPORT_SCHEMA_VERSION)
        self.assertTrue(advice["available"])
        self.assertEqual(
            [issue["code"] for issue in advice["issues"]],
            ["thin_local_wall_regions", "localized_overhang_regions", "tiny_printable_color_regions"],
        )
        self.assertEqual(
            [issue["category"] for issue in advice["issues"]],
            ["thickness", "overhang", "color"],
        )
        self.assertIn("打印优化要求", advice["prompt_suffix"])
        self.assertIn("3 类", advice["summary"])

    def test_related_codes_are_deduplicated_by_action(self):
        advice = build_model_refinement_advice(
            {
                "status": "review",
                "warnings": ["thin_local_wall_regions", "thin_structural_components"],
            },
            {},
        )

        self.assertEqual(len(advice["issues"]), 1)
        self.assertEqual(advice["issues"][0]["code"], "thin_structural_components")
        self.assertEqual(advice["issues"][0]["category"], "thickness")

    def test_visual_review_can_add_provider_neutral_advice(self):
        advice = build_model_refinement_advice(
            {"status": "pass", "warnings": []},
            {
                "status": "review",
                "warnings": ["visual_subject_incomplete", "visual_color_regions_unclear"],
            },
        )

        self.assertTrue(advice["available"])
        self.assertEqual(
            [issue["category"] for issue in advice["issues"]],
            ["semantics", "color"],
        )
        self.assertNotIn("Image2", advice["prompt_suffix"])
        self.assertNotIn("Tripo", advice["prompt_suffix"])

    def test_portrait_identity_and_material_mixing_become_next_run_advice(self):
        advice = build_model_refinement_advice(
            {"status": "pass", "warnings": []},
            {
                "status": "review",
                "warnings": ["visual_identity_mismatch", "visual_material_color_mixing"],
            },
        )

        self.assertEqual(
            [issue["category"] for issue in advice["issues"]],
            ["identity", "color"],
        )
        self.assertIn("脸宽", advice["prompt_suffix"])
        self.assertIn("肤色、衣物、头发和底座", advice["prompt_suffix"])

    def test_pass_unknown_and_malformed_reports_return_unavailable(self):
        for model_quality, visual_quality in (
            ({"status": "pass", "warnings": ["thin_local_wall_regions"]}, {}),
            ({"status": "review", "warnings": ["future_unknown_warning"]}, {}),
            ({"status": "review", "warnings": "thin_local_wall_regions"}, []),
            (None, None),
        ):
            with self.subTest(model_quality=model_quality, visual_quality=visual_quality):
                advice = build_model_refinement_advice(model_quality, visual_quality)
                self.assertEqual(
                    advice,
                    {
                        "schema": REPORT_SCHEMA_VERSION,
                        "available": False,
                        "summary": "",
                        "prompt_suffix": "",
                        "issues": [],
                    },
                )

    def test_reject_errors_are_actionable_and_issue_count_is_bounded(self):
        advice = build_model_refinement_advice(
            {
                "status": "reject",
                "errors": ["boundary_edges", "non_manifold_edges", "degenerate_faces"],
                "warnings": [
                    "floating_disconnected_components",
                    "thin_local_wall_regions",
                    "weak_bed_contact",
                    "localized_overhang_regions",
                    "dense_micro_triangles",
                    "tiny_printable_color_regions",
                ],
            },
            {
                "status": "review",
                "warnings": ["visual_subject_incomplete", "visual_silhouette_unclear"],
            },
        )

        self.assertEqual(len(advice["issues"]), MAX_ISSUES)
        self.assertEqual(advice["issues"][0]["category"], "topology")
        self.assertEqual(advice["issues"][1]["category"], "attachments")

    def test_all_public_text_is_bounded_utf8(self):
        advice = build_model_refinement_advice(
            {
                "status": "review",
                "warnings": [
                    "repairable_boundary_edges",
                    "floating_disconnected_components",
                    "thin_local_wall_regions",
                    "weak_bed_contact",
                    "localized_overhang_regions",
                    "tiny_printable_color_regions",
                ],
            },
            {"status": "review", "warnings": ["visual_subject_incomplete"]},
        )

        self.assertLessEqual(len(advice["prompt_suffix"].encode("utf-8")), MAX_PROMPT_SUFFIX_BYTES)
        for issue in advice["issues"]:
            self.assertTrue(issue["code"].isascii())
            self.assertLessEqual(len(issue["title"].encode("utf-8")), 120)
            self.assertLessEqual(len(issue["instruction"].encode("utf-8")), 360)


if __name__ == "__main__":
    unittest.main()
