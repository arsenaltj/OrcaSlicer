#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path
import sys

TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))
from printable_model_quality import ModelQualityThresholds, analyze_printable_obj, write_model_quality_report


def tetrahedron(offset=(0.0, 0.0, 0.0), scale=(10.0, 10.0, 10.0)) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    ox, oy, oz = offset
    sx, sy, sz = scale
    vertices = [(ox, oy, oz), (ox + sx, oy, oz), (ox, oy + sy, oz), (ox, oy, oz + sz)]
    faces = [(1, 3, 2), (1, 2, 4), (1, 4, 3), (2, 3, 4)]
    return vertices, faces


def obj_text(parts) -> str:
    lines = []
    vertex_offset = 0
    for vertices, faces in parts:
        lines.extend("v {} {} {} 1 0 0".format(*vertex) for vertex in vertices)
        lines.extend("f {} {} {}".format(*(index + vertex_offset for index in face)) for face in faces)
        vertex_offset += len(vertices)
    return "\n".join(lines) + "\n"


class PrintableModelQualityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def analyze(self, content: str, thresholds=None):
        source = self.root / "model.obj"
        source.write_text(content, encoding="ascii")
        return analyze_printable_obj(source, thresholds)

    def test_closed_grounded_tetrahedron_passes(self):
        report = self.analyze(obj_text([tetrahedron()]))
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["metrics"]["component_count"], 1)
        self.assertEqual(report["metrics"]["boundary_edges"], 0)
        self.assertFalse(report["metrics"]["repairable_topology"])

    def test_open_mesh_is_rejected(self):
        vertices, faces = tetrahedron()
        report = self.analyze(obj_text([(vertices, faces[:-1])]))
        self.assertEqual(report["status"], "reject")
        self.assertIn("boundary_edges", report["errors"])

    def test_small_open_mesh_can_be_deferred_to_a_declared_repair_consumer(self):
        vertices, faces = tetrahedron()
        source = self.root / "repairable.obj"
        source.write_text(obj_text([(vertices, faces[:-1])]), encoding="ascii")

        report = analyze_printable_obj(source, allow_repairable_topology=True)

        self.assertEqual(report["status"], "review")
        self.assertEqual(report["errors"], [])
        self.assertIn("repairable_boundary_edges", report["warnings"])
        self.assertTrue(report["metrics"]["repairable_topology"])

    def test_same_direction_shared_edge_is_rejected(self):
        vertices, faces = tetrahedron()
        faces[1] = tuple(reversed(faces[1]))
        report = self.analyze(obj_text([(vertices, faces)]))
        self.assertEqual(report["status"], "reject")
        self.assertIn("inconsistent_winding_edges", report["errors"])

    def test_degenerate_triangle_is_rejected(self):
        vertices, faces = tetrahedron()
        faces.append((1, 1, 2))
        report = self.analyze(obj_text([(vertices, faces)]))
        self.assertEqual(report["status"], "reject")
        self.assertIn("degenerate_faces", report["errors"])

    def test_tiny_detached_component_requires_review(self):
        thresholds = ModelQualityThresholds(tiny_component_face_ratio=0.6)
        report = self.analyze(
            obj_text([tetrahedron(), tetrahedron((5, 5, 10), (0.01, 0.01, 0.01))]),
            thresholds,
        )
        self.assertEqual(report["status"], "review")
        self.assertIn("tiny_detached_components", report["warnings"])
        self.assertEqual(report["metrics"]["component_count"], 2)

    def test_weak_contact_requires_review_without_rejecting_topology(self):
        vertices, faces = tetrahedron(scale=(10.0, 10.0, 10.0))
        vertices[1] = (10.0, 0.0, 1.0)
        vertices[2] = (0.0, 10.0, 1.0)
        report = self.analyze(obj_text([(vertices, faces)]))
        self.assertEqual(report["status"], "review")
        self.assertIn("weak_bed_contact", report["warnings"])

    def test_face_limit_is_a_hard_rejection(self):
        report = self.analyze(obj_text([tetrahedron()]), ModelQualityThresholds(max_faces=3))
        self.assertEqual(report["status"], "reject")
        self.assertEqual(report["errors"], ["too_many_faces"])

    def test_report_is_written_as_json(self):
        report = self.analyze(obj_text([tetrahedron()]))
        destination = write_model_quality_report(report, self.root / "model-quality.json")
        self.assertTrue(destination.is_file())
        self.assertIn('"gate_version": "structural-v1"', destination.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
