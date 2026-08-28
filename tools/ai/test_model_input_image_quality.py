import tempfile
import unittest
from pathlib import Path

from PIL import Image, ImageDraw

from tools.ai.model_input_image_quality import assess_model_input_image


class ModelInputImageQualityTests(unittest.TestCase):
    def save_subject(self, root: Path, box: tuple[int, int, int, int]) -> Path:
        path = root / "subject.png"
        image = Image.new("RGBA", (512, 512), (255, 255, 255, 0))
        ImageDraw.Draw(image).rectangle(box, fill=(190, 80, 50, 255))
        image.save(path)
        return path

    def test_centered_connected_subject_is_eligible(self):
        with tempfile.TemporaryDirectory() as directory:
            result = assess_model_input_image(self.save_subject(Path(directory), (128, 96, 383, 447)))

        self.assertTrue(result["model_input_eligible"])
        self.assertGreaterEqual(result["score"], 90)
        self.assertGreater(result["metrics"]["largest_component_ratio"], 0.99)

    def test_tiny_subject_is_blocked(self):
        with tempfile.TemporaryDirectory() as directory:
            result = assess_model_input_image(self.save_subject(Path(directory), (246, 246, 265, 265)))

        self.assertFalse(result["model_input_eligible"])
        self.assertIn("subject_not_detected", result["blockers"])

    def test_subject_crossing_multiple_edges_is_cropped(self):
        with tempfile.TemporaryDirectory() as directory:
            result = assess_model_input_image(self.save_subject(Path(directory), (0, 0, 320, 511)))

        self.assertFalse(result["model_input_eligible"])
        self.assertIn("subject_cropped", result["blockers"])

    def test_disconnected_equal_subjects_are_blocked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "fragments.png"
            image = Image.new("RGBA", (512, 512), (255, 255, 255, 0))
            draw = ImageDraw.Draw(image)
            draw.rectangle((70, 140, 210, 370), fill=(190, 80, 50, 255))
            draw.rectangle((300, 140, 440, 370), fill=(60, 100, 200, 255))
            image.save(path)
            result = assess_model_input_image(path)

        self.assertFalse(result["model_input_eligible"])
        self.assertIn("fragmented_subject", result["blockers"])


if __name__ == "__main__":
    unittest.main()
