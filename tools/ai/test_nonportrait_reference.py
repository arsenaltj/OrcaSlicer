import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image, ImageDraw

from tools.ai.image_preprocessing_policy import build_image_preprocessing_policy
from tools.ai.nonportrait_reference import (
    REPORT_FILENAME, SEMANTIC_CHECKS, assess_nonportrait_reference,
    prepare_nonportrait_views, review_nonportrait_reference,
)
from tools.ai import image_preprocessing_debug as debug


class NonportraitReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.policy = build_image_preprocessing_policy("戒指", "realistic")
        self.original = self.image("original.png", hole=True)
        self.prepared = self.image("prepared.png", hole=False)

    def image(self, name, *, hole=False, opaque=False):
        image = Image.new("RGBA", (512, 512), (255, 255, 255, 255 if opaque else 0))
        draw = ImageDraw.Draw(image)
        draw.ellipse((90, 65, 420, 445), fill=(130, 60, 30, 255))
        if hole:
            draw.ellipse((175, 170, 335, 335), fill=(255, 255, 255, 255 if opaque else 0))
        path = self.root/name
        image.save(path)
        return path

    def response(self, status="pass"):
        return json.dumps({"confidence": 0.9, "checks": {
            key: {"status": status, "reason": "已比较源图与处理图"} for key in SEMANTIC_CHECKS}})

    def test_reliable_alpha_detects_closed_negative_space_without_touching_images(self):
        originals = [path.read_bytes() for path in (self.original, self.prepared)]
        report = assess_nonportrait_reference(self.original, self.prepared, self.policy)
        self.assertTrue(report["mask_comparison_available"], report)
        self.assertIn("reference_negative_space_changed", report["warnings"])
        self.assertFalse(report["physical_print_qualified"])
        self.assertEqual([path.read_bytes() for path in (self.original, self.prepared)], originals)

    def test_opaque_masks_and_expected_relief_base_do_not_claim_semantic_comparison(self):
        for style, original in (("realistic", self.image("opaque.png", opaque=True)), ("ink_relief", self.original)):
            with self.subTest(style=style):
                policy = dict(self.policy, style=style)
                report = assess_nonportrait_reference(original, self.prepared, policy)
                self.assertFalse(report["mask_comparison_available"])
                self.assertEqual(report["semantic_review"]["status"], "not_requested")
                self.assertNotIn("reference_negative_space_changed", report["warnings"])

    def test_default_is_local_and_opt_in_cache_binds_images_policy_and_reviewer(self):
        output = self.root/"review"
        report = review_nonportrait_reference(self.original, self.prepared, self.policy, output)
        self.assertEqual(report["semantic_review"]["status"], "not_requested")
        completion = mock.Mock(return_value=self.response())
        report = review_nonportrait_reference(self.original, self.prepared, self.policy, output, completion=completion, reviewer_model="v1")
        cached = review_nonportrait_reference(self.original, self.prepared, self.policy, output, completion=completion, reviewer_model="v1")
        self.assertTrue(cached["cached"])
        self.assertEqual(completion.call_count, 1)
        review_nonportrait_reference(self.original, self.prepared, self.policy, output, completion=completion, reviewer_model="v2")
        review_nonportrait_reference(self.original, self.prepared, dict(self.policy, subject_text="remove"), output, completion=completion, reviewer_model="v2")
        review_nonportrait_reference(self.original, self.prepared, dict(self.policy, user_direction="戒指，保留字标"), output, completion=completion, reviewer_model="v2")
        self.image("prepared.png", hole=True)
        review_nonportrait_reference(self.original, self.prepared, self.policy, output, completion=completion, reviewer_model="v2")
        self.assertEqual(completion.call_count, 5)
        self.assertEqual(completion.call_args.args[2], (self.original, self.prepared))

    def test_malformed_or_unavailable_review_never_becomes_a_pass_or_retry(self):
        output = self.root/"review"
        for value in ("[]", "{}", "not-json", self.response("unavailable"),
                      self.response().replace("0.9", "0.4"),
                      json.dumps({"confidence": True, "checks": {}}),
                      json.dumps({"confidence": 0.9, "checks": {key: [] for key in SEMANTIC_CHECKS}})):
            with self.subTest(value=value):
                completion = mock.Mock(return_value=value)
                report = review_nonportrait_reference(self.original, self.prepared, self.policy, output, completion=completion)
                self.assertEqual(report["semantic_review"]["status"], "unavailable")
                self.assertFalse(report["physical_print_qualified"])
                completion.assert_called_once()
        completion = mock.Mock(side_effect=RuntimeError("offline"))
        review_nonportrait_reference(self.original, self.prepared, self.policy, output, completion=completion)
        completion.assert_called_once()
        self.assertTrue((output/REPORT_FILENAME).is_file())

    def test_invalid_old_cache_does_not_prevent_one_new_requested_review(self):
        output = self.root/"review"
        output.mkdir()
        (output/REPORT_FILENAME).write_text("[]", encoding="utf-8")
        completion = mock.Mock(return_value=self.response())
        report = review_nonportrait_reference(self.original, self.prepared, self.policy, output, completion=completion)
        self.assertEqual(report["semantic_review"]["status"], "pass")
        completion.assert_called_once()

    def test_supplied_views_keep_originals_and_have_no_submission_or_consistency_claim(self):
        data = self.original.read_bytes()
        manifest = prepare_nonportrait_views({"front": self.original, "left": self.prepared}, self.root/"views")
        self.assertEqual(self.original.read_bytes(), data)
        self.assertEqual(manifest["semantic_consistency"], "unverified")
        self.assertFalse(manifest["submitted_to_3d"])
        self.assertFalse(manifest["physical_print_qualified"])
        with self.assertRaises(ValueError):
            prepare_nonportrait_views({"front": self.original}, self.root/"invalid")

    def test_debug_dry_run_and_existing_reference_never_call_remote_services(self):
        output = self.root/"debug"
        arguments = ["--input", str(self.original), "--instruction", "戒指", "--output", str(output)]
        with mock.patch.object(debug, "preprocess_image") as generate, mock.patch.object(debug, "complete_vision_once") as review:
            self.assertEqual(debug.main(arguments), 0)
            self.assertEqual(debug.main(arguments+["--prepared", str(self.prepared)]), 0)
            generate.assert_not_called()
            review.assert_not_called()
        self.assertTrue((output/"prompt.txt").is_file())
        self.assertFalse(json.loads((output/"preprocessing.json").read_text(encoding="utf-8"))["submits_3d_task"])

    def test_debug_generation_is_one_explicit_edit_and_cannot_overwrite_original(self):
        output = self.root/"debug"
        arguments = ["--input", str(self.original), "--instruction", "戒指", "--output", str(output), "--generate"]
        with mock.patch.object(debug, "preprocess_image", return_value=self.prepared) as generate, mock.patch.object(debug, "complete_vision_once") as review:
            self.assertEqual(debug.main(arguments), 0)
            generate.assert_called_once()
            review.assert_not_called()
        # Input lives at the exact requested output path: fail before billing.
        source = self.image("prepared.png")
        with mock.patch.object(debug, "preprocess_image") as generate, self.assertRaises(SystemExit):
            debug.main(["--input", str(source), "--instruction", "戒指", "--output", str(self.root), "--generate"])
        generate.assert_not_called()


if __name__ == "__main__":
    unittest.main()
