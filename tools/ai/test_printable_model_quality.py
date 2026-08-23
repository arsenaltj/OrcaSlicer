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


def box(offset=(0.0, 0.0, 0.0), size=(10.0, 10.0, 10.0)) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    ox, oy, oz = offset
    sx, sy, sz = size
    vertices = [
        (ox, oy, oz),
        (ox + sx, oy, oz),
        (ox + sx, oy + sy, oz),
        (ox, oy + sy, oz),
        (ox, oy, oz + sz),
        (ox + sx, oy, oz + sz),
        (ox + sx, oy + sy, oz + sz),
        (ox, oy + sy, oz + sz),
    ]
    faces = [
        (1, 3, 2), (1, 4, 3),
        (5, 6, 7), (5, 7, 8),
        (1, 2, 6), (1, 6, 5),
        (2, 3, 7), (2, 7, 6),
        (3, 4, 8), (3, 8, 7),
        (4, 1, 5), (4, 5, 8),
    ]
    return vertices, faces


def obj_text(parts) -> str:
    lines = []
    vertex_offset = 0
    for part in parts:
        vertices, faces = part[:2]
        color = part[2] if len(part) > 2 else (1.0, 0.0, 0.0)
        if color is None:
            lines.extend("v {} {} {}".format(*vertex) for vertex in vertices)
        else:
            lines.extend("v {} {} {} {} {} {}".format(*vertex, *color) for vertex in vertices)
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

    def test_equal_size_floating_component_requires_review(self):
        report = self.analyze(
            obj_text([
                tetrahedron(),
                tetrahedron((20.0, 0.0, 15.0)),
            ])
        )

        self.assertEqual(report["status"], "review")
        self.assertIn("floating_disconnected_components", report["warnings"])
        self.assertEqual(report["metrics"]["floating_component_count"], 1)
        self.assertGreater(report["metrics"]["minimum_floating_clearance_mm"], 10.0)

    def test_nearby_separate_shell_is_treated_as_supported(self):
        report = self.analyze(
            obj_text([
                tetrahedron(),
                tetrahedron((10.1, 0.0, 5.0)),
            ]),
            ModelQualityThresholds(component_contact_tolerance_mm=0.2),
        )

        self.assertEqual(report["status"], "review")
        self.assertNotIn("floating_disconnected_components", report["warnings"])
        self.assertIn("localized_overhang_regions", report["warnings"])
        self.assertEqual(report["metrics"]["floating_component_count"], 0)
        self.assertIsNone(report["metrics"]["minimum_floating_clearance_mm"])

    def test_weak_contact_requires_review_without_rejecting_topology(self):
        vertices, faces = tetrahedron(scale=(10.0, 10.0, 10.0))
        vertices[1] = (10.0, 0.0, 1.0)
        vertices[2] = (0.0, 10.0, 1.0)
        report = self.analyze(obj_text([(vertices, faces)]))
        self.assertEqual(report["status"], "review")
        self.assertIn("weak_bed_contact", report["warnings"])

    def test_widely_spaced_point_contacts_use_actual_projected_area(self):
        report = self.analyze(
            obj_text([
                tetrahedron(scale=(0.1, 0.1, 1.0)),
                tetrahedron((100.0, 0.0, 0.0), (0.1, 0.1, 1.0)),
            ])
        )

        self.assertEqual(report["metrics"]["contact_span_ratio"], 1.0)
        self.assertLess(report["metrics"]["bed_contact_area_ratio"], 0.002)
        self.assertIn("weak_bed_contact", report["warnings"])

    def test_wide_flat_base_has_full_projected_contact_area(self):
        report = self.analyze(obj_text([box()]))

        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["metrics"]["bed_contact_area_mm2"], 100.0)
        self.assertEqual(report["metrics"]["bed_contact_area_ratio"], 1.0)

    def test_localized_elevated_overhang_is_reported_before_global_ratio(self):
        report = self.analyze(
            obj_text([
                box(size=(10.0, 10.0, 20.0)),
                box((-5.0, -5.0, 20.0), (20.0, 20.0, 1.0)),
            ])
        )

        self.assertLess(report["metrics"]["downward_surface_ratio"], 0.35)
        self.assertNotIn("high_downward_surface_ratio", report["warnings"])
        self.assertIn("localized_overhang_regions", report["warnings"])
        self.assertEqual(report["metrics"]["significant_overhang_region_count"], 1)
        self.assertGreater(report["metrics"]["lowest_overhang_clearance_mm"], 19.0)

    def test_coherent_printable_color_regions_pass(self):
        red_vertices, red_faces = tetrahedron()
        blue_vertices, blue_faces = tetrahedron((20.0, 0.0, 0.0))
        report = self.analyze(
            obj_text([
                (red_vertices, red_faces, (1.0, 0.0, 0.0)),
                (blue_vertices, blue_faces, (0.0, 0.0, 1.0)),
            ])
        )

        self.assertEqual(report["status"], "pass")
        self.assertTrue(report["metrics"]["has_complete_vertex_colors"])
        self.assertEqual(report["metrics"]["printable_color_count"], 2)
        self.assertEqual(report["metrics"]["color_region_count"], 2)
        self.assertEqual(report["metrics"]["tiny_color_region_count"], 0)

    def test_color_regions_cross_exact_duplicate_vertex_seams(self):
        source = self.root / "seamed.obj"
        source.write_text(
            "\n".join(
                [
                    "v 0 0 0 1 0 0",
                    "v 1 0 0 1 0 0",
                    "v 0 1 0 1 0 0",
                    "v 1 0 0 1 0 0",
                    "v 1 1 0 1 0 0",
                    "v 0 1 0 1 0 0",
                    "f 1 2 3",
                    "f 4 5 6",
                ]
            )
            + "\n",
            encoding="ascii",
        )

        report = analyze_printable_obj(source, allow_repairable_topology=True)

        self.assertTrue(report["metrics"]["has_complete_vertex_colors"])
        self.assertEqual(report["metrics"]["printable_color_count"], 1)
        self.assertEqual(report["metrics"]["color_region_count"], 1)

    def test_tiny_printable_color_island_requires_review(self):
        red_vertices, red_faces = tetrahedron()
        blue_vertices, blue_faces = tetrahedron((5.0, 5.0, 0.0), (0.1, 0.1, 0.1))
        thresholds = ModelQualityThresholds(
            tiny_color_region_face_ratio=0.6,
            tiny_color_region_area_ratio=0.001,
        )
        report = self.analyze(
            obj_text([
                (red_vertices, red_faces, (1.0, 0.0, 0.0)),
                (blue_vertices, blue_faces, (0.0, 0.0, 1.0)),
            ]),
            thresholds,
        )

        self.assertEqual(report["status"], "review")
        self.assertIn("tiny_printable_color_regions", report["warnings"])
        self.assertEqual(report["metrics"]["tiny_color_region_count"], 1)
        self.assertEqual(report["metrics"]["printable_color_count"], 2)

    def test_uncolored_obj_keeps_structural_behavior(self):
        vertices, faces = tetrahedron()
        report = self.analyze(obj_text([(vertices, faces, None)]))

        self.assertEqual(report["status"], "pass")
        self.assertFalse(report["metrics"]["has_complete_vertex_colors"])
        self.assertEqual(report["metrics"]["printable_color_count"], 0)
        self.assertEqual(report["metrics"]["color_region_count"], 0)
        self.assertNotIn("tiny_printable_color_regions", report["warnings"])

    def test_byte_vertex_colors_are_normalized(self):
        red_vertices, red_faces = tetrahedron()
        blue_vertices, blue_faces = tetrahedron((20.0, 0.0, 0.0))
        report = self.analyze(
            obj_text([
                (red_vertices, red_faces, (255.0, 0.0, 0.0)),
                (blue_vertices, blue_faces, (0.0, 0.0, 255.0)),
            ])
        )

        self.assertEqual(report["status"], "pass")
        self.assertTrue(report["metrics"]["has_complete_vertex_colors"])
        self.assertEqual(report["metrics"]["printable_color_count"], 2)

    def test_incomplete_or_invalid_vertex_colors_disable_color_metrics(self):
        vertices, faces = tetrahedron()
        second_vertices, second_faces = tetrahedron((20.0, 0.0, 0.0))
        cases = (
            obj_text([
                (vertices, faces, (1.0, 0.0, 0.0)),
                (second_vertices, second_faces, None),
            ]),
            obj_text([(vertices, faces, (256.0, 0.0, 0.0))]),
        )
        for content in cases:
            with self.subTest(content=content):
                report = self.analyze(content)
                self.assertEqual(report["status"], "pass")
                self.assertFalse(report["metrics"]["has_complete_vertex_colors"])
                self.assertEqual(report["metrics"]["printable_color_count"], 0)
                self.assertEqual(report["metrics"]["color_region_count"], 0)

    def test_face_limit_is_a_hard_rejection(self):
        report = self.analyze(obj_text([tetrahedron()]), ModelQualityThresholds(max_faces=3))
        self.assertEqual(report["status"], "reject")
        self.assertEqual(report["errors"], ["too_many_faces"])

    def test_report_is_written_as_json(self):
        report = self.analyze(obj_text([tetrahedron()]))
        destination = write_model_quality_report(report, self.root / "model-quality.json")
        self.assertTrue(destination.is_file())
        self.assertIn('"gate_version": "structural-v4"', destination.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
