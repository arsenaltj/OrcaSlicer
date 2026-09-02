from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from PIL import Image

from tools.ai.portrait_multiview_cleanup import (
    project_front_portrait_materials,
    project_geometry_aligned_portrait_materials,
    quantize_geometry_aligned_material_reference,
)


ROLES = {
    "primary": "#F4F4F0",
    "structure": "#1F1B1C",
    "light": "#F2C9AE",
    "accent": "#4E6F5B",
}


def _rgb(value: str) -> str:
    return " ".join(f"{int(value[index:index + 2], 16) / 255.0:.6f}" for index in (1, 3, 5))


class PortraitMultiviewCleanupTests(unittest.TestCase):
    def test_semantic_reference_uses_the_exact_geometry_mask(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = Image.new("RGB", (160, 160), (250, 250, 250))
            source.paste((75, 105, 84), (20, 20, 140, 140))
            source_path = root / "semantic.png"
            source.save(source_path)
            mask = Image.new("L", (80, 80), 0)
            mask.paste(255, (20, 10, 60, 70))
            mask_path = root / "mask.png"
            mask.save(mask_path)

            report = quantize_geometry_aligned_material_reference(
                source_path, mask_path, root / "prepared", ROLES
            )

            with Image.open(root / "prepared" / "aligned_reference.png") as aligned:
                self.assertEqual(aligned.getchannel("A").getbbox(), (40, 20, 120, 140))
            with Image.open(root / "prepared" / "clean_preview.png") as clean:
                self.assertEqual(clean.getpixel((80, 80)), tuple(
                    int(ROLES["accent"][index:index + 2], 16) for index in (1, 3, 5)
                ))
            self.assertEqual(report["subject_pixels"], 80 * 120)

    def test_back_semantic_reference_preserves_the_explicit_collar_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            primary = tuple(int(ROLES["primary"][index:index + 2], 16) for index in (1, 3, 5))
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            source = Image.new("RGB", (200, 200), primary)
            source.paste(structure, (60, 20, 140, 75))
            for y in range(75, 91):
                inset = (y - 75) * 2
                source.paste(structure, (60 + inset, y, 140 - inset, y + 1))
            source_path = root / "back-semantic.png"
            source.save(source_path)
            mask = Image.new("L", (200, 200), 0)
            mask.paste(255, (20, 10, 180, 190))
            mask_path = root / "mask.png"
            mask.save(mask_path)

            report = quantize_geometry_aligned_material_reference(
                source_path,
                mask_path,
                root / "prepared",
                ROLES,
                view_name="back",
            )

            with Image.open(root / "prepared" / "clean_preview.png") as clean:
                self.assertEqual(clean.getpixel((100, 100)), primary)
                self.assertEqual(clean.getpixel((45, 100)), primary)
                self.assertEqual(clean.getpixel((100, 60)), structure)
            self.assertFalse(report["nape_repair"]["activated"])
            self.assertEqual(
                report["nape_repair"]["reason"],
                "semantic_internal_boundary_preserved",
            )

    def test_back_nape_skin_survives_the_rear_garment_guard(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vertices = [
                # Rear nape and rear jacket occupy the generic torso cleanup
                # band; only the exact back view is allowed to distinguish
                # them.
                *(f"v -1 {y} {z} {_rgb(ROLES['primary'])}" for y, z in (
                    (-0.6, 5.0), (0.6, 5.0), (0.0, 6.0),
                    (-2.0, 3.0), (2.0, 3.0), (0.0, 4.0),
                )),
                # Matching front layers make the front/back z-buffer decision
                # explicit instead of relying on an uncovered surface.
                *(f"v 1 {y} {z} {_rgb(ROLES['primary'])}" for y, z in (
                    (-0.6, 5.0), (0.6, 5.0), (0.0, 6.0),
                    (-2.0, 3.0), (2.0, 3.0), (0.0, 4.0),
                )),
                *(f"v 0 {y} {z} {_rgb(ROLES['structure'])}" for y, z in (
                    (-2.0, 0.0), (2.0, 0.0), (0.0, 10.0),
                )),
            ]
            obj = root / "back-nape.obj"
            obj.write_text(
                "\n".join(vertices + [
                    "f 1 3 2", "f 4 6 5", "f 7 8 9", "f 10 11 12", "f 13 14 15",
                ]) + "\n",
                encoding="ascii",
            )
            primary = tuple(int(ROLES["primary"][index:index + 2], 16) for index in (1, 3, 5))
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            views = {}
            for name in ("front", "back"):
                view = root / name
                view.mkdir()
                reference = Image.new("RGB", (160, 160), primary)
                if name == "back":
                    reference.paste(skin, (52, 55, 108, 88))
                reference.save(view / "clean_preview.png")
                Image.new("L", (160, 160), 255).save(view / "mask_subject.png")
                views[name] = view

            report = project_geometry_aligned_portrait_materials(
                obj, views, root / "back-nape-report.json", ROLES, margin_ratio=0.1
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[:3], [skin] * 3)
            self.assertEqual(output[3:6], [primary] * 3)
            self.assertGreaterEqual(report["authoritative_back_skin_vertices"], 3)

    def test_side_supported_upper_neck_skin_survives_the_rear_jacket_guard(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vertices = [
                *(f"v -1 {y} {z} {_rgb(ROLES['primary'])}" for y, z in (
                    (-0.6, 5.0), (0.6, 5.0), (0.0, 6.0),
                )),
                *(f"v 1 {y} {z} {_rgb(ROLES['primary'])}" for y, z in (
                    (-0.6, 5.0), (0.6, 5.0), (0.0, 6.0),
                )),
                *(f"v 0 {y} {z} {_rgb(ROLES['structure'])}" for y, z in (
                    (-2.0, 0.0), (2.0, 0.0), (0.0, 10.0),
                )),
            ]
            obj = root / "side-neck.obj"
            obj.write_text(
                "\n".join(vertices + ["f 1 3 2", "f 4 5 6", "f 7 8 9"]) + "\n",
                encoding="ascii",
            )
            primary = tuple(int(ROLES["primary"][index:index + 2], 16) for index in (1, 3, 5))
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            views = {}
            for name, color in (("right", skin), ("back", primary), ("left", skin)):
                view = root / name
                view.mkdir()
                Image.new("RGB", (160, 160), color).save(view / "clean_preview.png")
                Image.new("L", (160, 160), 255).save(view / "mask_subject.png")
                views[name] = view

            report = project_geometry_aligned_portrait_materials(
                obj, views, root / "side-neck-report.json", ROLES, margin_ratio=0.1
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[:3], [skin] * 3)
            self.assertGreaterEqual(report["protected_upper_neck_skin_vertices"], 3)

    def test_geometry_projection_removes_accent_from_the_base(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vertices = [
                f"v 1 {y} {z} {_rgb(ROLES['accent'])}"
                for z in (2.0, 3.5, 5.0)
                for y in (-1.0, 0.0, 1.0)
            ] + [
                f"v 1 {y} {z} {_rgb(ROLES['accent'])}"
                for y, z in ((-1.0, 0.0), (1.0, 0.0), (0.0, 0.2))
            ] + [
                f"v 1 {y} {z} {_rgb(ROLES['structure'])}"
                for y, z in ((-1.0, 10.0), (1.0, 10.0), (0.0, 9.0))
            ]
            faces = []
            for row in range(2):
                for column in range(2):
                    first = row * 3 + column
                    faces.extend(((first, first + 3, first + 1), (first + 1, first + 3, first + 4)))
            faces.extend(((9, 10, 11), (12, 13, 14)))
            obj = root / "portrait.obj"
            obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )
            views = {}
            accent = tuple(int(ROLES["accent"][index:index + 2], 16) for index in (1, 3, 5))
            for name in ("front", "back"):
                view = root / name
                view.mkdir()
                Image.new("RGB", (160, 160), accent).save(view / "clean_preview.png")
                Image.new("L", (160, 160), 255).save(view / "mask_subject.png")
                views[name] = view

            report = project_geometry_aligned_portrait_materials(
                obj, views, root / "report.json", ROLES, margin_ratio=0.1
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            self.assertEqual(output[9:12], [structure] * 3)
            self.assertGreaterEqual(report["removed_accent_components"], 1)

    def test_geometry_aligned_views_recolor_only_the_visible_surface(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vertices = [
                *(f"v 1 {y} {z} {_rgb(ROLES['structure'])}" for y, z in ((0, 0), (1, 0), (1, 1), (0, 1))),
                *(f"v -1 {y} {z} {_rgb(ROLES['structure'])}" for y, z in ((0, 0), (1, 0), (1, 1), (0, 1))),
            ]
            obj = root / "layered.obj"
            obj.write_text(
                "\n".join(vertices + ["f 1 2 3", "f 1 3 4", "f 5 7 6", "f 5 8 7"]) + "\n",
                encoding="ascii",
            )
            views = {}
            for name, role in (("front", "light"), ("back", "primary")):
                view = root / name
                view.mkdir()
                color = tuple(int(ROLES[role][index:index + 2], 16) for index in (1, 3, 5))
                Image.new("RGB", (160, 160), color).save(view / "clean_preview.png")
                Image.new("L", (160, 160), 255).save(view / "mask_subject.png")
                views[name] = view

            report = project_geometry_aligned_portrait_materials(
                obj, views, root / "geometry-report.json", ROLES, margin_ratio=0.1
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            primary = tuple(int(ROLES["primary"][index:index + 2], 16) for index in (1, 3, 5))
            self.assertEqual(output[:4], [skin] * 4)
            self.assertEqual(output[4:], [primary] * 4)
            self.assertEqual(report["recolored_vertices"], 8)
            self.assertEqual(report["vertices_with_evidence"], 8)

    def test_geometry_projection_keeps_supported_hands_and_clears_unsupported_skin(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vertices = [
                # A real hand below the head: both exact views label it skin.
                *(f"v 1 {y} {z} {_rgb(ROLES['light'])}" for y, z in (
                    (-1.8, 4.5), (-1.2, 4.5), (-1.5, 5.2),
                )),
                # A peach sleeve seam at the same height has no view evidence.
                *(f"v 1 {y} {z} {_rgb(ROLES['light'])}" for y, z in (
                    (1.2, 4.5), (1.8, 4.5), (1.5, 5.2),
                )),
                # A peach pedestal patch must always return to structure.
                *(f"v 1 {y} {z} {_rgb(ROLES['light'])}" for y, z in (
                    (-1.0, 0.0), (1.0, 0.0), (0.0, 0.2),
                )),
                # Establish the full portrait height without adding skin.
                *(f"v 0 {y} {z} {_rgb(ROLES['structure'])}" for y, z in (
                    (-1.0, 10.0), (1.0, 10.0), (0.0, 9.5),
                )),
            ]
            obj = root / "hand-evidence.obj"
            obj.write_text(
                "\n".join(vertices + ["f 1 2 3", "f 4 5 6", "f 7 8 9", "f 10 11 12"]) + "\n",
                encoding="ascii",
            )

            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            primary = tuple(int(ROLES["primary"][index:index + 2], 16) for index in (1, 3, 5))
            views = {}
            # Front maps the left hand near x=61; back mirrors it near x=99.
            for name, hand_x in (("front", 61), ("back", 99)):
                view = root / name
                view.mkdir()
                reference = Image.new("RGB", (160, 160), primary)
                reference.paste(skin, (hand_x - 10, 68, hand_x + 11, 94))
                reference.save(view / "clean_preview.png")
                subject = Image.new("L", (160, 160), 0)
                subject.paste(255, (hand_x - 10, 68, hand_x + 11, 94))
                subject.save(view / "mask_subject.png")
                views[name] = view

            report = project_geometry_aligned_portrait_materials(
                obj, views, root / "hand-evidence-report.json", ROLES, margin_ratio=0.1
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))

            self.assertEqual(output[:3], [skin] * 3)
            self.assertEqual(output[3:6], [primary] * 3)
            self.assertEqual(output[6:9], [structure] * 3)
            self.assertGreaterEqual(report["strong_skin_evidence_vertices"], 3)
            self.assertGreaterEqual(report["skin_guard_recolored_vertices"], 6)

    def test_geometry_projection_uses_visible_hair_ring_for_unseen_crown(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vertices = [
                # Unseen top-facing crown pixels inherited warm texture skin.
                f"v 1 -0.8 9.7 {_rgb(ROLES['light'])}",
                f"v 1 0.8 9.7 {_rgb(ROLES['primary'])}",
                f"v 1 0.0 10.0 {_rgb(ROLES['accent'])}",
                # The visible ring immediately below is correctly labelled hair.
                *(f"v 1 {y} {z} {_rgb(ROLES['structure'])}" for y, z in (
                    (-1.0, 8.7), (1.0, 8.7), (0.0, 9.1),
                )),
                # A central face patch with no exact pixel evidence still stays skin.
                *(f"v 1 {y} {z} {_rgb(ROLES['light'])}" for y, z in (
                    (-0.5, 7.4), (0.5, 7.4), (0.0, 8.0),
                )),
                *(f"v 0 {y} {z} {_rgb(ROLES['structure'])}" for y, z in (
                    (-2.0, 0.0), (2.0, 0.0), (0.0, 0.2),
                )),
            ]
            obj = root / "crown.obj"
            obj.write_text(
                "\n".join(vertices + ["f 1 2 3", "f 4 5 6", "f 7 8 9", "f 10 11 12"]) + "\n",
                encoding="ascii",
            )
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            views = {}
            for name in ("front", "back"):
                view = root / name
                view.mkdir()
                Image.new("RGB", (160, 160), structure).save(view / "clean_preview.png")
                mask = Image.new("L", (160, 160), 255)
                # Suppress the crown and face samples, but leave the hair ring visible.
                mask.paste(0, (0, 0, 160, 27))
                mask.paste(0, (0, 38, 160, 68))
                mask.save(view / "mask_subject.png")
                views[name] = view

            report = project_geometry_aligned_portrait_materials(
                obj, views, root / "crown-report.json", ROLES, margin_ratio=0.1
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            self.assertEqual(output[:3], [structure] * 3)
            self.assertEqual(output[6:9], [skin] * 3)
            self.assertTrue(report["dark_hair_crown"])
            self.assertEqual(report["crown_hair_guard_recolored_vertices"], 3)

    def test_geometry_projection_trims_connected_accent_tail_from_wrist(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            accent_vertices = [
                f"v 1 {y} {z} {_rgb(ROLES['accent'])}"
                for z in (3.0, 4.5, 6.0)
                for y in (-0.5, 0.0, 0.5)
            ]
            accent_vertices.extend(
                f"v 1 {y} {z} {_rgb(ROLES['accent'])}"
                for y, z in ((2.2, 4.2), (2.8, 4.2), (2.5, 5.0))
            )
            bounds = [
                f"v 0 {y} {z} {_rgb(ROLES['structure'])}"
                for y, z in ((-3.0, 0.0), (3.0, 0.0), (0.0, 10.0))
            ]
            faces = []
            for row in range(2):
                for column in range(2):
                    first = row * 3 + column
                    faces.extend(((first, first + 3, first + 1), (first + 1, first + 3, first + 4)))
            faces.extend(((7, 9, 10), (7, 10, 11), (12, 13, 14)))
            obj = root / "accent-tail.obj"
            obj.write_text(
                "\n".join(accent_vertices + bounds + [
                    f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces
                ]) + "\n",
                encoding="ascii",
            )
            views = {}
            for name in ("front", "back"):
                view = root / name
                view.mkdir()
                Image.new("RGB", (160, 160), (244, 244, 240)).save(view / "clean_preview.png")
                Image.new("L", (160, 160), 0).save(view / "mask_subject.png")
                views[name] = view

            report = project_geometry_aligned_portrait_materials(
                obj, views, root / "accent-tail-report.json", ROLES, margin_ratio=0.1
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]
            accent = tuple(int(ROLES["accent"][index:index + 2], 16) for index in (1, 3, 5))
            primary = tuple(int(ROLES["primary"][index:index + 2], 16) for index in (1, 3, 5))
            self.assertEqual(output[:9], [accent] * 9)
            self.assertEqual(output[9:12], [primary] * 3)
            self.assertEqual(report["accent_spatial_guard_recolored_vertices"], 3)

    def test_front_accent_evidence_wins_conflicting_side_and_back_votes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vertices = [
                f"v 1 {y} {z} {_rgb(ROLES['primary'])}"
                for z in (0.85, 0.95, 1.05)
                for y in (-0.5, 0.0, 0.5)
            ] + [
                f"v 0 {y} {z} {_rgb(ROLES['structure'])}"
                for y, z in ((-3.0, 0.0), (3.0, 0.0), (0.0, 10.0))
            ]
            faces = []
            for row in range(2):
                for column in range(2):
                    first = row * 3 + column
                    faces.extend(((first, first + 3, first + 1), (first + 1, first + 3, first + 4)))
            faces.append((9, 10, 11))
            obj = root / "front-accent.obj"
            obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )
            accent = tuple(int(ROLES["accent"][index:index + 2], 16) for index in (1, 3, 5))
            primary = tuple(int(ROLES["primary"][index:index + 2], 16) for index in (1, 3, 5))
            views = {}
            for name, color in (("front", accent), ("right", primary), ("back", primary)):
                view = root / name
                view.mkdir()
                Image.new("RGB", (160, 160), color).save(view / "clean_preview.png")
                Image.new("L", (160, 160), 255).save(view / "mask_subject.png")
                views[name] = view

            report = project_geometry_aligned_portrait_materials(
                obj, views, root / "front-accent-report.json", ROLES, margin_ratio=0.1
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[:9], [accent] * 9)
            self.assertGreater(report["authoritative_front_accent_recolored_vertices"], 0)
            self.assertEqual(report["accent_minimum_height_ratio"], 0.08)

    def test_front_reference_keeps_face_and_hand_but_removes_garment_bleed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            size = 20
            colors = [[ROLES["primary"] for _ in range(size)] for _ in range(size)]
            for row in range(14, 20):
                for column in range(7, 13):
                    colors[row][column] = ROLES["light"]
            for row in range(5, 8):
                for column in range(3, 6):
                    colors[row][column] = ROLES["light"]
                for column in range(14, 17):
                    colors[row][column] = ROLES["light"]

            vertices = [
                f"v 1 {column} {row} {_rgb(colors[row][column])}"
                for row in range(size)
                for column in range(size)
            ]
            faces = []
            for row in range(size - 1):
                for column in range(size - 1):
                    first = row * size + column
                    right = first + 1
                    upper = first + size
                    upper_right = upper + 1
                    faces.extend(((first, upper, right), (right, upper, upper_right)))
            obj = root / "portrait.obj"
            obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )

            reference = Image.new("RGBA", (200, 200), (*tuple(int(ROLES["primary"][i:i + 2], 16) for i in (1, 3, 5)), 255))
            skin = tuple(int(ROLES["light"][i:i + 2], 16) for i in (1, 3, 5))
            # Model y grows left-to-right and model z grows bottom-to-top.
            reference.paste((*skin, 255), (28, 119, 62, 153))
            reference_path = root / "front.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj, reference_path, root / "projection.json", ROLES
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]
            expected_skin = skin
            expected_primary = tuple(int(ROLES["primary"][i:i + 2], 16) for i in (1, 3, 5))
            self.assertEqual(report["status"], "projected")
            self.assertEqual(output[17 * size + 9], expected_skin)
            self.assertEqual(output[6 * size + 4], expected_skin)
            self.assertEqual(output[6 * size + 15], expected_primary)
            self.assertEqual(output[10 * size + 10], expected_primary)
            self.assertGreater(report["protected_face_vertices"], 0)
            self.assertGreater(report["recolored_vertices"], 0)

    def test_front_reference_restores_accent_without_recoloring_occluded_back(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # Two triangles share the same projected area. Positive X is the
            # visible front chest; negative X models an occluded rear surface.
            vertices = [
                f"v 1 0.4 1.5 {_rgb(ROLES['structure'])}",
                f"v 1 0.6 1.5 {_rgb(ROLES['structure'])}",
                f"v 1 0.5 2 {_rgb(ROLES['structure'])}",
                f"v -1 0.4 1.5 {_rgb(ROLES['structure'])}",
                f"v -1 0.6 1.5 {_rgb(ROLES['structure'])}",
                f"v -1 0.5 2 {_rgb(ROLES['structure'])}",
                # Garment cleanup can also turn a real blouse patch white; the
                # exact-palette front reference may safely restore it.
                f"v 1 0.45 1.6 {_rgb(ROLES['primary'])}",
                f"v 1 0.55 1.6 {_rgb(ROLES['primary'])}",
                f"v 1 0.5 1.9 {_rgb(ROLES['primary'])}",
                # A small high skin triangle supplies the protected face root.
                f"v 1 0 4 {_rgb(ROLES['light'])}",
                f"v 1 1 4 {_rgb(ROLES['light'])}",
                f"v 1 0 5 {_rgb(ROLES['light'])}",
                # Base vertices anchor the portrait's normal Z range.
                f"v 0 0 0 {_rgb(ROLES['structure'])}",
                f"v 0 1 0 {_rgb(ROLES['structure'])}",
                f"v 0 0.5 0 {_rgb(ROLES['structure'])}",
            ]
            obj = root / "layered.obj"
            obj.write_text(
                "\n".join(
                    vertices + ["f 1 2 3", "f 4 5 6", "f 7 8 9", "f 10 11 12", "f 13 14 15"]
                ) + "\n",
                encoding="ascii",
            )
            accent = tuple(int(ROLES["accent"][index:index + 2], 16) for index in (1, 3, 5))
            reference = Image.new("RGBA", (200, 200), (*accent, 255))
            reference_path = root / "front.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj, reference_path, root / "projection.json", ROLES,
                repair_skin=False, restore_accent=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            self.assertEqual(output[:3], [accent] * 3)
            self.assertEqual(output[3:6], [structure] * 3)
            self.assertEqual(output[6:9], [accent] * 3)
            self.assertEqual(report["recolored_by_target"][ROLES["accent"]], 6)
            self.assertEqual(report["accent_recolored_vertices"], 6)
            self.assertEqual(report["accent_recolored_by_source"][ROLES["primary"]], 3)

    def test_exact_palette_reference_restores_printable_face_details_after_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            y_values = (0.0, 1.2, 2.0, 2.8, 4.0)
            vertices = [
                f"v 1 {y} {z} {_rgb(ROLES['light'])}"
                for z in range(6)
                for y in y_values
            ]
            faces = []
            width = len(y_values)
            for row in range(5):
                for column in range(width - 1):
                    first = row * width + column
                    faces.extend(
                        ((first, first + width, first + 1),
                         (first + 1, first + width, first + width + 1))
                    )
            obj = root / "face.obj"
            obj.write_text(
                "\n".join(
                    vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]
                ) + "\n",
                encoding="ascii",
            )

            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            primary = tuple(int(ROLES["primary"][index:index + 2], 16) for index in (1, 3, 5))
            reference = Image.new("RGBA", (200, 200), (*skin, 255))
            reference.paste((*structure, 255), (64, 36, 72, 44))
            # Even if the provider reference contains a locally bright sample,
            # an already-white face vertex must not survive as a doll eye or
            # oversized tooth island.
            reference.paste((*primary, 255), (96, 36, 104, 44))
            reference.paste((*primary, 255), (128, 36, 136, 44))
            reference_path = root / "face-reference.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj,
                reference_path,
                root / "face-projection.json",
                ROLES,
                repair_skin=False,
                restore_face_details=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[4 * width + 1], structure)
            self.assertEqual(output[4 * width + 3], primary)
            self.assertGreaterEqual(report["face_detail_recolored_vertices"], 2)
            self.assertTrue(report["restore_face_details"])

    def test_realistic_face_normalization_clears_false_islands_without_adding_white(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            y_values = (0.0, 1.2, 2.0, 2.8, 4.0)
            vertices = []
            for z in range(6):
                for column, y in enumerate(y_values):
                    role = "light"
                    if z == 4 and column == 2:
                        role = "primary"
                    elif z == 4 and column == 3:
                        role = "structure"
                    vertices.append(f"v 1 {y} {z} {_rgb(ROLES[role])}")
            faces = []
            width = len(y_values)
            for row in range(5):
                for column in range(width - 1):
                    first = row * width + column
                    faces.extend(
                        ((first, first + width, first + 1),
                         (first + 1, first + width, first + width + 1))
                    )
            obj = root / "face.obj"
            obj.write_text(
                "\n".join(
                    vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]
                ) + "\n",
                encoding="ascii",
            )

            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            primary = tuple(int(ROLES["primary"][index:index + 2], 16) for index in (1, 3, 5))
            reference = Image.new("RGBA", (200, 200), (*skin, 255))
            reference.paste((*structure, 255), (64, 36, 72, 44))
            reference_path = root / "face-reference.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj,
                reference_path,
                root / "face-normalization.json",
                ROLES,
                repair_skin=False,
                normalize_face_details=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[4 * width + 1], structure)
            self.assertEqual(output[4 * width + 2], skin)
            self.assertEqual(output[4 * width + 3], skin)
            self.assertNotIn(primary, output[4 * width:5 * width])
            self.assertGreaterEqual(report["face_detail_cleared_vertices"], 2)
            self.assertGreaterEqual(report["face_detail_restored_vertices"], 1)
            self.assertTrue(report["normalize_face_details"])

    def test_face_normalization_does_not_project_through_the_visible_surface(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            vertices = [
                # The first triangle is visible; the second occupies the same
                # projected cells one millimetre behind it.
                *(f"v 2 {y} {z} {_rgb(ROLES['light'])}" for y, z in ((-.1, 8), (.1, 8), (0, 8.2))),
                *(f"v 1 {y} {z} {_rgb(ROLES['light'])}" for y, z in ((-.1, 8), (.1, 8), (0, 8.2))),
                # Low-X samples keep the broad quantile permissive, proving the
                # per-pixel depth gate—not the old threshold—blocks the rear.
                *(f"v -10 {-10 + index} 8 {_rgb(ROLES['light'])}" for index in range(21)),
                f"v 0 -10 0 {_rgb(ROLES['light'])}",
                f"v 0 10 0 {_rgb(ROLES['light'])}",
                f"v 0 0 10 {_rgb(ROLES['light'])}",
            ]
            obj = root / "layered-face.obj"
            obj.write_text(
                "\n".join(vertices + ["f 1 2 3", "f 4 5 6", "f 28 29 30"]) + "\n",
                encoding="ascii",
            )
            reference = Image.new("RGBA", (200, 200), (*structure, 255))
            reference_path = root / "face-reference.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj,
                reference_path,
                root / "face-depth-report.json",
                ROLES,
                repair_skin=False,
                normalize_face_details=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[:3], [structure] * 3)
            self.assertEqual(output[3:6], [skin] * 3)
            self.assertGreaterEqual(report["face_occluded_vertices"], 3)

    def test_face_normalization_clears_upper_forehead_texture_bleed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            width = 5
            vertices = []
            for z in range(11):
                for column, y in enumerate((-2.0, -1.0, 0.0, 1.0, 2.0)):
                    role = "structure" if z == 9 and column == 2 else "light"
                    vertices.append(f"v 1 {y} {z} {_rgb(ROLES[role])}")
            faces = []
            for row in range(10):
                for column in range(width - 1):
                    first = row * width + column
                    faces.extend(
                        ((first, first + width, first + 1),
                         (first + 1, first + width, first + width + 1))
                    )
            obj = root / "forehead.obj"
            obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            reference = Image.new("RGBA", (200, 200), (*skin, 255))
            reference_path = root / "forehead-reference.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj,
                reference_path,
                root / "forehead-report.json",
                ROLES,
                repair_skin=False,
                normalize_face_details=True,
                reference_is_geometry_aligned=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[9 * width + 2], skin)
            self.assertGreaterEqual(report["face_detail_cleared_vertices"], 1)

    def test_geometry_aligned_face_normalization_keeps_compact_teeth_and_removes_white_eyes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            width = 5
            vertices = []
            for z in range(11):
                for column, y in enumerate((-2.0, -1.0, 0.0, 1.0, 2.0)):
                    role = "light"
                    if z == 9 and column == 2:
                        role = "primary"
                    elif z == 8 and column == 2:
                        role = "structure"
                    vertices.append(f"v 1 {y} {z} {_rgb(ROLES[role])}")
            faces = []
            for row in range(10):
                for column in range(width - 1):
                    first = row * width + column
                    faces.extend(
                        ((first, first + width, first + 1),
                         (first + 1, first + width, first + width + 1))
                    )
            obj = root / "smile.obj"
            obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            primary = tuple(int(ROLES["primary"][index:index + 2], 16) for index in (1, 3, 5))
            reference = Image.new("RGBA", (200, 200), (*skin, 255))
            # A compact geometry-aligned smile band is useful identity evidence,
            # while a bright eye/forehead island remains an unwanted seam.
            reference.paste((*primary, 255), (94, 54, 106, 66))
            reference.paste((*primary, 255), (94, 14, 106, 26))
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            # Geometry-aligned dark eye evidence is authoritative and should
            # remain printable after the generic cleanup pass.
            reference.paste((*structure, 255), (97, 38, 103, 42))
            reference_path = root / "smile-reference.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj,
                reference_path,
                root / "smile-report.json",
                ROLES,
                repair_skin=False,
                normalize_face_details=True,
                reference_is_geometry_aligned=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[7 * width + 2], primary)
            self.assertEqual(output[8 * width + 2], structure)
            self.assertEqual(output[9 * width + 2], skin)
            self.assertTrue(report["reference_is_geometry_aligned"])

    def test_bright_only_face_cleanup_uses_aligned_reference_as_final_truth(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            width = 5
            vertices = []
            for z in range(11):
                for column, y in enumerate((-2.0, -1.0, 0.0, 1.0, 2.0)):
                    role = "light"
                    if z == 8 and column in {1, 2}:
                        role = "structure"
                    elif z == 7 and column == 2:
                        role = "primary"
                    vertices.append(f"v 1 {y} {z} {_rgb(ROLES[role])}")
            faces = []
            for row in range(10):
                for column in range(width - 1):
                    first = row * width + column
                    faces.extend(
                        ((first, first + width, first + 1),
                         (first + 1, first + width, first + width + 1))
                    )
            obj = root / "bright-only-face.obj"
            obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            reference = Image.new("RGBA", (200, 200), (*skin, 255))
            reference.paste((*structure, 255), (94, 34, 106, 46))
            reference_path = root / "bright-only-reference.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj,
                reference_path,
                root / "bright-only-report.json",
                ROLES,
                repair_skin=False,
                normalize_face_details=True,
                reference_is_geometry_aligned=True,
                clear_bright_face_materials_only=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[7 * width + 2], skin)
            self.assertEqual(output[8 * width + 1], skin)
            self.assertEqual(output[8 * width + 2], structure)
            self.assertTrue(report["clear_bright_face_materials_only"])
            self.assertGreaterEqual(report["face_detail_cleared_vertices"], 1)

    def test_geometry_aligned_two_pixel_brow_is_not_erased_as_a_tiny_island(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            width = 5
            vertices = [
                f"v 1 {y} {z} {_rgb(ROLES['light'])}"
                for z in range(11)
                for y in (-2.0, -1.0, 0.0, 1.0, 2.0)
            ]
            faces = []
            for row in range(10):
                for column in range(width - 1):
                    first = row * width + column
                    faces.extend(
                        ((first, first + width, first + 1),
                         (first + 1, first + width, first + width + 1))
                    )
            obj = root / "thin-brow.obj"
            obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            reference = Image.new("RGBA", (200, 200), (*skin, 255))
            # A two-pixel trace is the minimum printable aligned brow/lid;
            # isolated single pixels must not expand into heavy eyeliner.
            reference.paste((*structure, 255), (96, 39, 105, 41))
            reference_path = root / "thin-brow-reference.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj,
                reference_path,
                root / "thin-brow-report.json",
                ROLES,
                repair_skin=False,
                normalize_face_details=True,
                reference_is_geometry_aligned=True,
                clear_bright_face_materials_only=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[8 * width + 2], structure)
            self.assertGreaterEqual(report["face_detail_restored_vertices"], 1)

    def test_geometry_aligned_black_mouth_is_cleared_even_when_reference_shadow_is_dark(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            width = 5
            vertices = []
            for z in range(11):
                for column, y in enumerate((-2.0, -1.0, 0.0, 1.0, 2.0)):
                    role = "structure" if z == 7 and column == 2 else "light"
                    vertices.append(f"v 1 {y} {z} {_rgb(ROLES[role])}")
            faces = []
            for row in range(10):
                for column in range(width - 1):
                    first = row * width + column
                    faces.extend(
                        ((first, first + width, first + 1),
                         (first + 1, first + width, first + width + 1))
                    )
            obj = root / "dark-mouth.obj"
            obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            reference = Image.new("RGBA", (200, 200), (*skin, 255))
            reference.paste((*structure, 255), (90, 52, 111, 69))
            reference_path = root / "dark-mouth-reference.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj,
                reference_path,
                root / "dark-mouth-report.json",
                ROLES,
                repair_skin=False,
                normalize_face_details=True,
                reference_is_geometry_aligned=True,
                clear_bright_face_materials_only=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[7 * width + 2], skin)
            self.assertGreaterEqual(report["face_detail_cleared_vertices"], 1)

    def test_sculptural_face_finish_keeps_reference_backed_eyes_teeth_and_upper_hairline(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            width = 5
            vertices = []
            for z in range(21):
                for column, y in enumerate((-2.0, -1.0, 0.0, 1.0, 2.0)):
                    role = "light"
                    x = 1.0
                    if z in {18, 19}:
                        role = "structure"
                    elif column == 2 and z == 14:
                        role = "structure"
                    elif column == 2 and z == 16:
                        # A pupil is legitimately recessed behind the local
                        # brow/cheek envelope and starts as skin material.
                        x = 0.6
                    elif column == 2 and z == 15:
                        role = "primary"
                    vertices.append(f"v {x} {y} {z} {_rgb(ROLES[role])}")
            faces = []
            for row in range(20):
                for column in range(width - 1):
                    first = row * width + column
                    faces.extend(
                        ((first, first + width, first + 1),
                         (first + 1, first + width, first + width + 1))
                    )
            obj = root / "sculptural-face.obj"
            obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            primary = tuple(int(ROLES["primary"][index:index + 2], 16) for index in (1, 3, 5))
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            reference = Image.new("RGBA", (200, 200), (*skin, 255))
            for top, bottom in ((17, 24), (27, 34), (37, 44), (57, 64)):
                reference.paste((*structure, 255), (92, top, 108, bottom))
            reference.paste((*primary, 255), (92, 47, 108, 54))
            reference_path = root / "sculptural-face-reference.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj,
                reference_path,
                root / "sculptural-face-report.json",
                ROLES,
                repair_skin=False,
                normalize_face_details=True,
                reference_is_geometry_aligned=True,
                clear_bright_face_materials_only=True,
                sculptural_face_finish=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[14 * width + 2], skin)
            self.assertEqual(output[15 * width + 2], primary)
            self.assertEqual(output[16 * width + 2], structure)
            self.assertEqual(output[17 * width + 2], structure)
            self.assertEqual(output[18 * width + 2], structure)
            self.assertEqual(output[19 * width + 2], structure)
            self.assertTrue(report["sculptural_face_finish"])
            self.assertGreaterEqual(report["sculptural_eye_line_vertices"], 2)
            self.assertGreaterEqual(report["sculptural_tooth_vertices"], 1)
            self.assertGreaterEqual(report["protected_structure_component_min_vertices"], 8)

            relief_obj = root / "sculptural-face-relief-only.obj"
            relief_obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )
            relief_report = project_front_portrait_materials(
                relief_obj,
                reference_path,
                root / "sculptural-face-relief-only-report.json",
                ROLES,
                repair_skin=False,
                normalize_face_details=True,
                reference_is_geometry_aligned=True,
                clear_bright_face_materials_only=True,
                sculptural_face_finish=True,
                sculptural_face_relief_only=True,
            )
            relief_output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in relief_obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            for row in (14, 15, 16, 17):
                self.assertEqual(relief_output[row * width + 2], skin)
            self.assertEqual(relief_output[18 * width + 2], structure)
            self.assertEqual(relief_output[19 * width + 2], structure)
            self.assertTrue(relief_report["sculptural_face_relief_only"])
            self.assertEqual(relief_report["sculptural_eye_line_vertices"], 0)
            self.assertEqual(relief_report["sculptural_tooth_vertices"], 0)
            self.assertGreaterEqual(relief_report["face_detail_cleared_vertices"], 2)

    def test_geometry_aligned_green_brow_is_normalized_to_structure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            width = 5
            vertices = []
            for z in range(11):
                for column, y in enumerate((-2.0, -1.0, 0.0, 1.0, 2.0)):
                    role = "accent" if z == 8 and column == 2 else "light"
                    vertices.append(f"v 1 {y} {z} {_rgb(ROLES[role])}")
            faces = []
            for row in range(10):
                for column in range(width - 1):
                    first = row * width + column
                    faces.extend(
                        ((first, first + width, first + 1),
                         (first + 1, first + width, first + width + 1))
                    )
            obj = root / "accent-brow.obj"
            obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            accent = tuple(int(ROLES["accent"][index:index + 2], 16) for index in (1, 3, 5))
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            reference = Image.new("RGBA", (200, 200), (*skin, 255))
            reference.paste((*accent, 255), (90, 32, 111, 53))
            reference_path = root / "accent-brow-reference.png"
            reference.save(reference_path)

            report = project_front_portrait_materials(
                obj,
                reference_path,
                root / "accent-brow-report.json",
                ROLES,
                repair_skin=False,
                normalize_face_details=True,
                reference_is_geometry_aligned=True,
                clear_bright_face_materials_only=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[8 * width + 2], structure)
            self.assertGreaterEqual(report["face_detail_restored_vertices"], 1)

    def test_approximate_reference_does_not_clear_the_upper_hairline(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            width = 5
            vertices = []
            for z in range(11):
                for column, y in enumerate((-2.0, -1.0, 0.0, 1.0, 2.0)):
                    role = "structure" if z == 9 and column == 2 else "light"
                    vertices.append(f"v 1 {y} {z} {_rgb(ROLES[role])}")
            faces = []
            for row in range(10):
                for column in range(width - 1):
                    first = row * width + column
                    faces.extend(
                        ((first, first + width, first + 1),
                         (first + 1, first + width, first + width + 1))
                    )
            obj = root / "approximate-hairline.obj"
            obj.write_text(
                "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
                encoding="ascii",
            )
            skin = tuple(int(ROLES["light"][index:index + 2], 16) for index in (1, 3, 5))
            structure = tuple(int(ROLES["structure"][index:index + 2], 16) for index in (1, 3, 5))
            reference = Image.new("RGBA", (200, 200), (*skin, 255))
            reference_path = root / "approximate-reference.png"
            reference.save(reference_path)

            project_front_portrait_materials(
                obj,
                reference_path,
                root / "approximate-report.json",
                ROLES,
                repair_skin=False,
                normalize_face_details=True,
            )
            output = [
                tuple(round(float(value) * 255) for value in line.split()[4:7])
                for line in obj.read_text(encoding="ascii").splitlines()
                if line.startswith("v ")
            ]

            self.assertEqual(output[9 * width + 2], structure)


if __name__ == "__main__":
    unittest.main()
