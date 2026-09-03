#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

from PIL import Image


TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))

from color_intent import (  # noqa: E402
    COLOR_INTENT_FILENAME,
    SCHEMA_ID,
    ColorIntentError,
    build_color_intent_manifest,
    sha256_file,
    validate_color_intent_manifest,
    write_color_intent_manifest,
)
from printable_palette import PALETTE_ROLES  # noqa: E402


FALLBACKS = ("#E8DDD0", "#202124", "#E2A26F", "#2F6B5F", "#3267A8", "#9B3F77")
DESIRED = ("#D9C5B8", "#343138", "#C7865B", "#477D6A", "#426FAD", "#A95086")


def write_reference_pair(directory: Path, count: int, *, omit_last: bool = False) -> tuple[Path, Path]:
    width = count * 8
    appearance = Image.new("RGBA", (width, 8), (0, 0, 0, 0))
    material = Image.new("RGBA", (width, 8), (0, 0, 0, 0))
    for index in range(count - int(omit_last)):
        desired = tuple(int(DESIRED[index][offset:offset + 2], 16) for offset in (1, 3, 5))
        fallback = tuple(int(FALLBACKS[index][offset:offset + 2], 16) for offset in (1, 3, 5))
        for x in range(index * 8, (index + 1) * 8):
            for y in range(8):
                appearance.putpixel((x, y), (*desired, 255))
                material.putpixel((x, y), (*fallback, 255))
    appearance_path = directory / "appearance.png"
    material_path = directory / "material.png"
    appearance.save(appearance_path)
    material.save(material_path)
    return appearance_path, material_path


class ColorIntentTests(unittest.TestCase):
    def fixture(self, directory: str, count: int, *, omit_last: bool = False):
        root = Path(directory)
        artifact = root / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="ascii")
        geometry = root / "geometry-reference.png"
        Image.new("RGB", (16, 16), "gray").save(geometry)
        appearance, material = write_reference_pair(root, count, omit_last=omit_last)
        palette = FALLBACKS[:count]
        roles = {role: palette[index] for index, role in enumerate(PALETTE_ROLES[:count])}
        return artifact, geometry, appearance, material, palette, roles

    def test_builds_deterministic_one_through_six_color_intent(self):
        for count in range(1, 7):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as directory:
                artifact, geometry, appearance, material, palette, roles = self.fixture(directory, count)

                manifest = build_color_intent_manifest(
                    artifact,
                    appearance,
                    material,
                    palette,
                    roles,
                    geometry_reference_path=geometry,
                )

                self.assertEqual(manifest["schema"], SCHEMA_ID)
                self.assertEqual(manifest["mode"], "discrete_filament")
                self.assertEqual(manifest["artifact"]["sha256"], sha256_file(artifact))
                self.assertEqual(manifest["references"]["geometry"]["sha256"], sha256_file(geometry))
                self.assertEqual(manifest["references"]["appearance_source"]["sha256"], sha256_file(appearance))
                self.assertEqual(manifest["references"]["material_preview"]["sha256"], sha256_file(material))
                self.assertEqual(len(manifest["targets"]), count)
                for index, target in enumerate(manifest["targets"]):
                    self.assertEqual(target["role"], PALETTE_ROLES[index])
                    self.assertEqual(target["fallback_color"], FALLBACKS[index])
                    self.assertEqual(target["desired_color"], DESIRED[index])
                    self.assertEqual(target["sample_count"], 64)
                validate_color_intent_manifest(manifest, artifact_path=artifact)

    def test_empty_region_falls_back_to_printable_color(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact, geometry, appearance, material, palette, roles = self.fixture(directory, 2, omit_last=True)

            manifest = build_color_intent_manifest(
                artifact, appearance, material, palette, roles,
                geometry_reference_path=geometry,
            )

        self.assertEqual(manifest["targets"][1]["desired_color"], FALLBACKS[1])
        self.assertEqual(manifest["targets"][1]["sample_count"], 0)

    def test_rejects_invalid_or_ambiguous_palette_roles(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact, geometry, appearance, material, palette, roles = self.fixture(directory, 2)
            cases = (
                ((palette[0], palette[0]), roles),
                (("red", palette[1]), roles),
                (palette, {"primary": palette[0]}),
                (palette, {"primary": palette[0], "structure": palette[0]}),
                (palette, {**roles, "detail": palette[1]}),
            )
            for candidate_palette, candidate_roles in cases:
                with self.subTest(palette=candidate_palette, roles=candidate_roles), self.assertRaises(ColorIntentError):
                    build_color_intent_manifest(
                        artifact,
                        appearance,
                        material,
                        candidate_palette,
                        candidate_roles,
                        geometry_reference_path=geometry,
                    )

    def test_validation_rejects_wrong_schema_hash_and_missing_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact, geometry, appearance, material, palette, roles = self.fixture(directory, 1)
            manifest = build_color_intent_manifest(
                artifact, appearance, material, palette, roles,
                geometry_reference_path=geometry,
            )
            wrong_schema = json.loads(json.dumps(manifest))
            wrong_schema["schema"] = "orcaslicer.color-intent.v2"
            with self.assertRaises(ColorIntentError):
                validate_color_intent_manifest(wrong_schema, artifact_path=artifact)
            wrong_hash = json.loads(json.dumps(manifest))
            wrong_hash["artifact"]["sha256"] = "A" * 64
            with self.assertRaises(ColorIntentError):
                validate_color_intent_manifest(wrong_hash, artifact_path=artifact)
            artifact.unlink()
            with self.assertRaises(ColorIntentError):
                validate_color_intent_manifest(manifest, artifact_path=artifact)

    def test_atomic_write_replaces_only_with_a_valid_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact, geometry, appearance, material, palette, roles = self.fixture(directory, 2)
            destination = Path(directory) / COLOR_INTENT_FILENAME
            result = write_color_intent_manifest(
                destination,
                artifact,
                appearance,
                material,
                palette,
                roles,
                geometry_reference_path=geometry,
            )
            original = destination.read_bytes()

            self.assertEqual(result.path, destination)
            self.assertEqual(result.schema, SCHEMA_ID)
            self.assertEqual(result.sha256, sha256_file(destination))
            self.assertFalse(destination.with_name(destination.name + ".part").exists())
            with self.assertRaises(ColorIntentError):
                write_color_intent_manifest(
                    destination,
                    artifact,
                    appearance,
                    material,
                    (palette[0], palette[0]),
                    roles,
                    geometry_reference_path=geometry,
                )
            self.assertEqual(destination.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
