import json
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

import semantic_boundary_ab as boundary


class FakeRunner:
    name = "fake-boundary"
    load_ms = 1.0
    hashes = {"fake.onnx": "0" * 64}

    def infer(self, crop, positive, negative):
        probability = np.zeros(crop.shape[:2], dtype=np.float32)
        probability[1:-1, 1:-1] = 0.9
        return probability, {"encoding_ms": 2.0, "decoding_ms": 3.0, "inference_ms": 5.0}


class FakeInput:
    def __init__(self, name):
        self.name = name


class SemanticBoundaryAbTests(unittest.TestCase):
    def test_both_models_choose_prompt_consistency_before_quality_score(self):
        masks = np.full((1, 1, 2, 3, 3), 5, dtype=np.float32)
        masks[0, 0, 1, 0, 0] = -5
        scores = np.asarray([[[.99, .75]]], dtype=np.float32)
        mask, score, consistent = boundary._prompt_consistent_mask(masks, scores, [(1, 1)], [(0, 0)], 3, 3)
        self.assertTrue(consistent)
        self.assertAlmostEqual(score, .75)
        self.assertLess(mask[0, 0], 0)

    def test_efficient_decoder_refuses_more_than_six_points(self):
        runner = boundary.EfficientSamRunner.__new__(boundary.EfficientSamRunner)
        with self.assertRaisesRegex(ValueError, "six points"):
            runner.infer(np.zeros((3, 3, 3), np.uint8), [(1, 1)] * 4, [(0, 0)] * 3)

    def test_mobile_resize_uses_actual_axes_and_normalized_zero_padding(self):
        class Encoder:
            def get_inputs(self):
                value = FakeInput("images")
                value.shape = [1, 3, 1024, 1024]
                return [value]

            def run(self, names, feed):
                self.tensor = feed["images"]
                return [np.zeros((1, 256, 64, 64), dtype=np.float32)]

        class Decoder:
            def get_inputs(self):
                return [FakeInput(name) for name in (
                    "image_embeddings", "point_coords", "point_labels", "mask_input",
                    "has_mask_input", "orig_im_size")]

            def run(self, names, feed):
                self.names, self.feed = names, feed
                masks = np.full((1, 1, 2, 3), -5, dtype=np.float32)
                masks[0, 0, 1, 1] = 5
                return [masks, np.asarray([[.9]], dtype=np.float32)]

        runner = boundary.MobileSamRunner.__new__(boundary.MobileSamRunner)
        runner.encoder, runner.decoder = Encoder(), Decoder()
        probability, metrics = runner.infer(np.full((2, 3, 3), 128, np.uint8), [(1, 1)], [(0, 0)])
        self.assertTrue(np.all(runner.encoder.tensor[:, :, 683:, :] == 0))
        self.assertTrue(np.any(runner.encoder.tensor[:, :, 682, :] != 0))
        np.testing.assert_allclose(runner.decoder.feed["point_coords"][0, 0], [1024 / 3, 683 / 2])
        self.assertEqual(runner.decoder.names, ["masks", "iou_predictions"])
        self.assertTrue(metrics["prompt_consistent"])
        self.assertGreater(probability[1, 1], .99)

    def test_mobile_has_mask_input_is_a_scalar_flag(self):
        inputs = [FakeInput(name) for name in (
            "image_embeddings", "point_coords", "point_labels", "mask_input",
            "has_mask_input", "orig_im_size")]
        feed = boundary._mobile_decoder_feed(
            inputs, np.zeros((1, 256, 64, 64), dtype=np.float32),
            np.zeros((1, 3, 2), dtype=np.float32), np.zeros((1, 3), dtype=np.float32), 80, 90)
        self.assertEqual(feed["mask_input"].shape, (1, 1, 256, 256))
        self.assertEqual(feed["has_mask_input"].shape, (1,))
        np.testing.assert_array_equal(feed["orig_im_size"], np.asarray([80, 90], dtype=np.float32))

    def test_efficient_sam_inputs_follow_official_normalized_rgb_contract(self):
        image = np.asarray([[[0, 127, 255], [255, 64, 32]]], dtype=np.uint8)
        tensor = boundary._efficient_image_tensor(image)
        self.assertEqual(tensor.shape, (1, 3, 1, 2))
        self.assertEqual(tensor.dtype, np.float32)
        self.assertAlmostEqual(float(tensor[0, 0, 0, 0]), 0.0)
        self.assertAlmostEqual(float(tensor[0, 2, 0, 0]), 1.0)
        self.assertAlmostEqual(float(tensor[0, 1, 0, 0]), 127.0 / 255.0)

        coords, labels = boundary._efficient_prompt_tensors([(3.0, 4.0)], [(7.0, 8.0)])
        np.testing.assert_array_equal(coords, np.asarray([[[[3.0, 4.0], [7.0, 8.0]]]], dtype=np.float32))
        np.testing.assert_array_equal(labels, np.asarray([[[1.0, 0.0]]], dtype=np.float32))

    def test_prompt_contract_requires_nonoverlapping_roi_and_both_prompt_kinds(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "prompts.json"
            path.write_text(json.dumps({
                "schema": boundary.PROMPT_SCHEMA,
                "regions": [{"id": "ear", "box": [0, 0, 8, 8],
                             "positive": [[2, 2]], "negative": [[6, 2]]}],
            }), encoding="utf-8")
            result = boundary.load_prompts(path, 16, 16)
            self.assertEqual(result["regions"][0]["box"], (0, 0, 8, 8))

            document = json.loads(path.read_text(encoding="utf-8"))
            document["regions"].append({"id": "other", "box": [4, 4, 12, 12],
                                        "positive": [[5, 5]], "negative": [[10, 10]]})
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "must not overlap"):
                boundary.load_prompts(path, 16, 16)

    def test_contour_metrics_report_exact_and_shifted_boundaries(self):
        reference = np.zeros((16, 16), dtype=bool)
        reference[4:12, 4:12] = True
        exact = boundary.contour_error(reference, reference)
        self.assertEqual(exact["contour_p95_px"], 0.0)
        shifted = np.zeros_like(reference)
        shifted[4:12, 5:13] = True
        metrics = boundary.contour_error(shifted, reference)
        self.assertGreater(metrics["contour_p95_px"], 0.0)
        self.assertLessEqual(metrics["contour_max_px"], 1.0)

    def test_evaluation_preserves_soft_mask_and_records_changed_pixels(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = np.full((12, 12, 3), 128, dtype=np.uint8)
            coarse = np.zeros((12, 12), dtype=np.uint8)
            Image.fromarray(coarse).save(root / "coarse.png")
            prompt_path = root / "prompts.json"
            prompt_path.write_text(json.dumps({
                "schema": boundary.PROMPT_SCHEMA,
                "regions": [{"id": "ear", "box": [2, 2, 10, 10],
                             "positive": [[4, 4]], "negative": [[8, 4]],
                             "coarse_mask": "coarse.png"}],
            }), encoding="utf-8")
            prompts = boundary.load_prompts(prompt_path, 12, 12)
            result = boundary.evaluate(FakeRunner(), image, prompt_path, prompts, root / "out", 0.5)
            self.assertEqual(result["changed_pixels"], 36)
            soft = np.load(root / "out" / "ear.soft-mask.npy", allow_pickle=False)
            self.assertEqual(soft.dtype, np.float32)
            self.assertAlmostEqual(float(soft[2, 2]), 0.9, places=6)
            self.assertTrue((root / "out" / "overlay.png").is_file())

    def test_evaluation_does_not_report_zero_changes_without_a_coarse_mask(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = np.full((12, 12, 3), 128, dtype=np.uint8)
            prompt_path = root / "prompts.json"
            prompt_path.write_text(json.dumps({
                "schema": boundary.PROMPT_SCHEMA,
                "regions": [{"id": "ear", "box": [2, 2, 10, 10],
                             "positive": [[4, 4]], "negative": [[8, 4]]}],
            }), encoding="utf-8")
            prompts = boundary.load_prompts(prompt_path, 12, 12)
            result = boundary.evaluate(FakeRunner(), image, prompt_path, prompts, root / "out", 0.5)
            self.assertIsNone(result["changed_pixels"])


if __name__ == "__main__":
    unittest.main()
