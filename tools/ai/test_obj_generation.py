#!/usr/bin/env python3
import importlib.util
import json
import math
import os
import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from PIL import Image


TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


SIDECAR = load_module("orca_ai_sidecar_obj_generation", TOOLS_AI / "orca_ai_sidecar.py")
TRIPO = load_module("tripo_client_low_poly", TOOLS_AI / "tripo_client.py")


class TripoHighDetailRequestTests(unittest.TestCase):
    def setUp(self):
        self.environment = mock.patch.dict(
            os.environ,
            {"TRIPO_API_KEY": "test-key", "TRIPO_MODEL": "test-model"},
        )
        self.environment.start()

    def tearDown(self):
        self.environment.stop()

    def assert_high_detail_payload(self, payload, face_limit=300000):
        self.assertEqual(payload["model"], "test-model")
        self.assertFalse(payload["smart_low_poly"])
        self.assertEqual(payload["face_limit"], face_limit)
        self.assertTrue(payload["texture"])
        self.assertFalse(payload["pbr"])
        self.assertEqual(payload["texture_quality"], "detailed")
        self.assertEqual(payload["geometry_quality"], "detailed")
        self.assertFalse(payload["quad"])
        self.assertTrue(payload["export_uv"])

    def test_text_generation_requests_colored_high_detail_model(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "text-id"}) as post:
            self.assertEqual(TRIPO.create_text_task("one watertight figurine"), "text-id")

        path, payload = post.call_args.args
        self.assertEqual(path, "/generation/text-to-model")
        self.assertEqual(payload["prompt"], "one watertight figurine")
        self.assert_high_detail_payload(payload)
        self.assertNotIn("texture_alignment", payload)

    def test_image_generation_requests_original_image_texture_alignment(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "image-id"}) as post:
            self.assertEqual(TRIPO.create_image_task("file-token"), "image-id")

        path, payload = post.call_args.args
        self.assertEqual(path, "/generation/image-to-model")
        self.assertEqual(payload["input"], "file-token")
        self.assert_high_detail_payload(payload)
        self.assertEqual(payload["texture_alignment"], "original_image")

    def test_multiview_generation_uses_named_canonical_views(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "multiview-id"}) as post:
            task_id = TRIPO.create_multiview_task({
                "right": "right-token",
                "front": "front-token",
                "back": "back-token",
            })
        self.assertEqual(task_id, "multiview-id")
        path, payload = post.call_args.args
        self.assertEqual(path, "/generation/multiview-to-model")
        self.assertEqual(payload["inputs"], [
            {"front": "front-token"},
            {"back": "back-token"},
            {"right": "right-token"},
        ])
        self.assert_high_detail_payload(payload)

    def test_multiview_generation_requires_front_and_another_view(self):
        with mock.patch.object(TRIPO, "_post_json") as post:
            with self.assertRaisesRegex(TRIPO.TripoError, "front"):
                TRIPO.create_multiview_task({"left": "left-token", "back": "back-token"})
            with self.assertRaisesRegex(TRIPO.TripoError, "additional"):
                TRIPO.create_multiview_task({"front": "front-token"})
        post.assert_not_called()

    def test_supported_face_target_is_forwarded(self):
        with mock.patch.object(TRIPO, "_post_json", return_value={"task_id": "text-id"}) as post:
            TRIPO.create_text_task("printable figure", 1000000)
        self.assert_high_detail_payload(post.call_args.args[1], 1000000)

    def test_unsupported_face_target_is_rejected_before_request(self):
        with mock.patch.object(TRIPO, "_post_json") as post:
            with self.assertRaisesRegex(TRIPO.TripoError, "face target"):
                TRIPO.create_text_task("printable figure", 20000)
        post.assert_not_called()


class TripoArtifactSecurityTests(unittest.TestCase):
    def test_exact_official_cdn_allows_proxy_fake_ip(self):
        address = [(2, 1, 6, "", ("198.18.0.1", 443))]
        with mock.patch.object(TRIPO.socket, "getaddrinfo", return_value=address):
            TRIPO._validate_artifact_url("https://openapi.cdn.tripo3d.com/tasks/model.zip?signature=test")

    def test_cdn_lookalike_is_rejected_before_dns(self):
        with mock.patch.object(TRIPO.socket, "getaddrinfo") as resolve:
            with self.assertRaisesRegex(TRIPO.TripoError, "unsafe artifact"):
                TRIPO._validate_artifact_url("https://openapi.cdn.tripo3d.com.attacker.invalid/model.zip")
        resolve.assert_not_called()

    def test_non_https_official_cdn_is_rejected(self):
        with self.assertRaisesRegex(TRIPO.TripoError, "unsafe artifact"):
            TRIPO._validate_artifact_url("http://openapi.cdn.tripo3d.com/model.zip")


class PrintablePaletteTests(unittest.TestCase):
    def test_palette_is_normalized_and_deduplicated_in_slot_order(self):
        self.assertEqual(
            SIDECAR._normalize_palette(["#ff0000", "#00FF00", "#FF0000"]),
            ("#FF0000", "#00FF00"),
        )

    def test_palette_can_be_disabled_with_an_empty_array(self):
        self.assertEqual(SIDECAR._normalize_palette([]), ())

    def test_palette_must_be_an_array(self):
        for value in (None, "#FF0000"):
            with self.subTest(value=value):
                with self.assertRaises(SIDECAR.RequestError):
                    SIDECAR._normalize_palette(value)

    def test_palette_rejects_invalid_colors_and_too_many_slots(self):
        for value in (["red"], ["#12345G"], ["#000000"] * 17):
            with self.subTest(value=value):
                with self.assertRaises(SIDECAR.RequestError):
                    SIDECAR._normalize_palette(value)

    def test_preview_uses_every_exact_palette_color_without_dithering(self):
        with tempfile.TemporaryDirectory() as directory:
            preview = Path(directory) / "preview.png"
            image = Image.new("RGB", (40, 20))
            for x in range(40):
                color = (230, 40, 40) if x < 20 else (30, 220, 40)
                for y in range(20):
                    image.putpixel((x, y), color)
            image.save(preview)

            usage = SIDECAR._quantize_image_to_palette(preview, ("#FF0000", "#00FF00"))

            with Image.open(preview) as result:
                colors = set(result.convert("RGB").getdata())
            self.assertEqual(colors, {(255, 0, 0), (0, 255, 0)})
            self.assertEqual(set(usage), {"#FF0000", "#00FF00"})

    def test_preview_preserves_large_regions_without_palette_speckles(self):
        with tempfile.TemporaryDirectory() as directory:
            preview = Path(directory) / "preview.png"
            image = Image.new("RGB", (18, 9), (255, 0, 0))
            for x in range(9, 18):
                for y in range(9):
                    image.putpixel((x, y), (0, 255, 0))
            image.save(preview)

            SIDECAR._quantize_image_to_palette(preview, ("#FF0000", "#00FF00"))

            with Image.open(preview) as result:
                self.assertEqual(set(result.convert("RGB").getdata()), {(255, 0, 0), (0, 255, 0)})

    def test_preview_filter_cannot_create_colors_outside_palette(self):
        with tempfile.TemporaryDirectory() as directory:
            preview = Path(directory) / "preview.png"
            palette = ((255, 0, 0), (0, 255, 0), (0, 0, 255))
            image = Image.new("RGB", (9, 9))
            image.putdata([palette[min(2, x // 3)] for y in range(9) for x in range(9)])
            image.save(preview)

            SIDECAR._quantize_image_to_palette(preview, ("#FF0000", "#00FF00", "#0000FF"))

            with Image.open(preview) as result:
                colors = set(result.convert("RGB").getdata())
            self.assertEqual(colors, set(palette))

    def test_preview_keeps_background_clean_without_forcing_unused_colors_onto_base(self):
        with tempfile.TemporaryDirectory() as directory:
            preview = Path(directory) / "preview.png"
            image = Image.new("RGB", (120, 120), (215, 215, 215))
            for x in range(35, 85):
                for y in range(12, 98):
                    image.putpixel((x, y), (205, 160, 123))
            for x in range(50, 70):
                for y in range(45, 65):
                    image.putpixel((x, y), (245, 220, 195))
            for x in range(35, 85):
                for y in range(12, 35):
                    image.putpixel((x, y), (18, 18, 18))
            for x in range(18, 102):
                for y in range(92, 112):
                    image.putpixel((x, y), (120, 175, 105))
            image.save(preview)
            palette = (
                "#DCDBD7", "#FFFFFF", "#CDA07B", "#DAAE8C", "#242421", "#83B771", "#EA0006", "#FFFF0C", "#0102FF"
            )

            usage = SIDECAR._quantize_image_to_palette(preview, palette, "q_cartoon")

            with Image.open(preview) as result:
                rgb = result.convert("RGB")
                self.assertEqual(rgb.getpixel((0, 0)), (220, 219, 215))
                self.assertEqual(rgb.getpixel((119, 119)), (220, 219, 215))
                self.assertIn(rgb.getpixel((60, 50)), {(205, 160, 123), (218, 174, 140)})
                base_colors = {rgb.getpixel((x, 102)) for x in range(20, 100)}
            self.assertGreaterEqual(len(usage), 4)
            self.assertLess(set(usage), set(palette))
            self.assertFalse({(234, 0, 6), (255, 255, 12), (1, 2, 255)} & base_colors)

    def test_preview_protects_face_skin_without_recoloring_warm_armor(self):
        with tempfile.TemporaryDirectory() as directory:
            preview = Path(directory) / "preview.png"
            image = Image.new("RGB", (160, 200), (187, 188, 192))
            for x in range(62, 98):
                for y in range(20, 62):
                    image.putpixel((x, y), (219, 177, 160))
            for x in range(38, 122):
                for y in range(72, 145):
                    image.putpixel((x, y), (235, 225, 210))
            for x in range(38, 60):
                for y in range(90, 130):
                    image.putpixel((x, y), (210, 80, 45))
            for x in range(50, 110):
                for y in range(90, 126):
                    image.putpixel((x, y), (120, 175, 105))
            for x in range(25, 135):
                for y in range(150, 188):
                    image.putpixel((x, y), (36, 36, 33))
            image.save(preview)
            palette = ("#DCDBD7", "#DAAE8C", "#EA0006", "#83B771", "#242421")

            SIDECAR._quantize_image_to_palette(preview, palette, "low_poly")

            with Image.open(preview) as result:
                rgb = result.convert("RGB")
                self.assertEqual(rgb.getpixel((80, 40)), (218, 174, 140))
                self.assertEqual(rgb.getpixel((110, 80)), (220, 219, 215))
                self.assertEqual(rgb.getpixel((45, 110)), (234, 0, 6))
                self.assertEqual(rgb.getpixel((80, 105)), (131, 183, 113))

    def test_style_ids_are_strict_and_default_to_sculpture(self):
        self.assertEqual(SIDECAR._normalize_style(None), "sculpture")
        self.assertEqual(SIDECAR._normalize_style("q_cartoon"), "q_cartoon")
        self.assertEqual(SIDECAR._normalize_style("low_poly"), "low_poly")
        self.assertEqual(SIDECAR._normalize_style("cel_shaded"), "cel_shaded")
        self.assertEqual(SIDECAR._normalize_style("enamel_inlay"), "enamel_inlay")
        self.assertEqual(SIDECAR._normalize_style("sculpture"), "sculpture")
        with self.assertRaises(SIDECAR.RequestError):
            SIDECAR._normalize_style("classical")

    def test_image_instruction_is_optional(self):
        self.assertEqual(SIDECAR._normalize_image_instruction(None), SIDECAR.DEFAULT_IMAGE_INSTRUCTION)
        self.assertEqual(SIDECAR._normalize_image_instruction("   "), SIDECAR.DEFAULT_IMAGE_INSTRUCTION)
        self.assertEqual(SIDECAR._normalize_image_instruction(" preserve pose "), "preserve pose")
        self.assertIn("Preserve the exact crop, framing", SIDECAR.DEFAULT_IMAGE_INSTRUCTION)
        self.assertIn("do not add, remove, reveal, reconstruct, or extend anything", SIDECAR.DEFAULT_IMAGE_INSTRUCTION)


class ObjGenerationTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.output_root = Path(self.temporary_directory.name) / "generated_models"
        self.environment = mock.patch.dict(os.environ, {"ORCASLICER_AI_OUTPUT_DIR": str(self.output_root)})
        self.environment.start()
        self.palette = ("#FF0000", "#00FF00", "#0000FF", "#FFFF00")
        self.job = SIDECAR._new_job("text", self.palette)

    def tearDown(self):
        self.environment.stop()
        self.temporary_directory.cleanup()

    def _write_package(self, archive, *, obj=None, mtl=None, texture=True, extra=None):
        obj = obj or (
            "mtllib model.mtl\n"
            "v 0 0 0\n"
            "v 1 0 0\n"
            "v 0 1 0\n"
            "v 0 0 1\n"
            "vt 0 1\n"
            "vt 1 1\n"
            "vt 0 0\n"
            "vt 1 0\n"
            "usemtl painted\n"
            "f 1/1 3/3 2/2\n"
            "f 1/4 2/2 4/3\n"
            "f 1/1 4/3 3/3\n"
            "f 2/2 3/3 4/3\n"
        )
        mtl = mtl or "newmtl painted\nmap_Kd base.png\n"
        texture_path = Path(self.temporary_directory.name) / "base.png"
        image = Image.new("RGB", (2, 2))
        image.putdata([(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)])
        image.save(texture_path)
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            bundle.writestr("model.obj", obj)
            bundle.writestr("model.mtl", mtl)
            if texture:
                bundle.write(texture_path, "base.png")
            for name, data in extra or []:
                bundle.writestr(name, data)

    def _vertex_color_torus(
        self, *, omit_faces=(), add_tiny_component=False, duplicate_faces=(), major_segments=8, minor_segments=8
    ):
        vertices = []
        faces = []
        for major in range(major_segments):
            u = 2.0 * math.pi * major / major_segments
            for minor in range(minor_segments):
                v = 2.0 * math.pi * minor / minor_segments
                radius = 10.0 + 3.0 * math.cos(v)
                vertices.append((radius * math.cos(u), radius * math.sin(u), 3.0 * math.sin(v)))
        for major in range(major_segments):
            next_major = (major + 1) % major_segments
            for minor in range(minor_segments):
                next_minor = (minor + 1) % minor_segments
                a = major * minor_segments + minor
                b = next_major * minor_segments + minor
                c = next_major * minor_segments + next_minor
                d = major * minor_segments + next_minor
                faces.extend(((a, b, c), (a, c, d)))
        faces = [face for index, face in enumerate(faces) if index not in set(omit_faces)]
        faces.extend(faces[index] for index in duplicate_faces)
        if add_tiny_component:
            start = len(vertices)
            vertices.extend(((0, 0, 0), (0.1, 0, 0), (0, 0.1, 0), (0, 0, 0.1)))
            faces.extend(
                (
                    (start, start + 2, start + 1),
                    (start, start + 1, start + 3),
                    (start, start + 3, start + 2),
                    (start + 1, start + 2, start + 3),
                )
            )
        vertex_lines = [f"v {x:.9g} {y:.9g} {z:.9g} 1 0 0" for x, y, z in vertices]
        face_lines = [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]
        return "\n".join(vertex_lines + face_lines) + "\n"

    def _vertex_color_grid(self, size, color_for_vertex):
        vertices = [
            f"v {x} {y} 0 {color_for_vertex(x, y)}"
            for y in range(size)
            for x in range(size)
        ]
        faces = []
        for y in range(size - 1):
            for x in range(size - 1):
                left = y * size + x
                right = left + 1
                upper = left + size
                upper_right = upper + 1
                faces.extend(((left, upper, right), (right, upper, upper_right)))
        return "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n"

    def test_new_job_uses_persistent_output_directory(self):
        self.assertEqual(self.job.directory.parent, self.output_root.resolve())
        self.assertTrue(self.job.directory.is_dir())
        self.assertTrue((self.job.directory / SIDECAR.JOB_STATE_FILENAME).is_file())

    def test_ready_job_round_trips_through_persistent_state(self):
        preview = self.job.directory / "preview.png"
        preview.write_bytes(b"\x89PNG\r\n\x1a\npreview")
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="ascii")
        self.job.prepared_prompt = "persistent printable object"
        self.job.user_prompt = "original object"
        self.job.preview_path = preview
        self.job.preview_content_type = "image/png"
        self.job.artifact_path = artifact
        self.job.artifact_format = "obj"
        self.job.state = "ready"
        self.job.phase = "ready"
        SIDECAR._persist_job(self.job)

        restored = SIDECAR._load_job(self.job.directory)

        self.assertIsNotNone(restored)
        self.assertEqual(restored.state, "ready")
        self.assertEqual(restored.prepared_prompt, "persistent printable object")
        self.assertEqual(restored.user_prompt, "original object")
        self.assertEqual(restored.preview_path, preview)
        self.assertEqual(restored.artifact_path, artifact)
        self.assertEqual(restored.palette, self.palette)

    def test_restore_reuses_paid_generation_reference_without_creating_a_new_task(self):
        self.job.state = "running"
        self.job.phase = "generating"
        self.job.prepared_prompt = "resume printable object"
        self.job.attempts = [{"attempt": 1, "generation_task_id": "paid-generation-id", "status": "running"}]
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "queued")
        self.assertEqual(restored.phase, "resuming")
        submit.assert_called_once_with(restored, SIDECAR._generate_job, "resume printable object", True)

    def test_restore_never_repeats_ambiguous_paid_request_without_saved_reference(self):
        self.job.state = "running"
        self.job.phase = "generating"
        self.job.prepared_prompt = "ambiguous printable object"
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "failed")
        self.assertIn("Start a new generation manually", restored.message)
        submit.assert_not_called()

    def test_restore_retries_download_with_existing_remote_ids(self):
        self.job.state = "failed"
        self.job.phase = "failed"
        self.job.progress = 95
        self.job.prepared_prompt = "resume artifact download"
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
            "status": "rejected",
            "error": "Tripo returned an unsafe artifact location.",
        }]
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "queued")
        self.assertEqual(restored.phase, "resuming")
        submit.assert_called_once_with(restored, SIDECAR._generate_job, "resume artifact download", True)

    def test_restore_retries_invalid_obj_package_with_existing_remote_ids(self):
        self.job.state = "failed"
        self.job.phase = "failed"
        self.job.progress = 95
        self.job.prepared_prompt = "resume invalid local package"
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
            "status": "rejected",
            "error": "Tripo returned an invalid OBJ package.",
        }]
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "queued")
        self.assertEqual(restored.phase, "resuming")
        submit.assert_called_once_with(restored, SIDECAR._generate_job, "resume invalid local package", True)

    def test_face_target_uses_per_tier_tolerance(self):
        for face_count, face_limit in (
            (90000, 100000),
            (95338, 100000),
            (125000, 100000),
            (270000, 300000),
            (375000, 300000),
        ):
            with self.subTest(face_count=face_count, face_limit=face_limit):
                SIDECAR._validate_face_target(face_count, face_limit)

        for face_count, face_limit in (
            (89999, 100000),
            (125001, 100000),
            (269999, 300000),
            (375001, 300000),
        ):
            with self.subTest(face_count=face_count, face_limit=face_limit):
                with self.assertRaises(SIDECAR.TripoError):
                    SIDECAR._validate_face_target(face_count, face_limit)

    def test_restore_accepts_legacy_strict_face_error_without_new_paid_task(self):
        self.job.state = "failed"
        self.job.phase = "failed"
        self.job.face_limit = 100000
        self.job.progress = 95
        self.job.prepared_prompt = "resume accepted near-target artifact"
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
            "status": "rejected",
            "error": "The generated OBJ contains 95338 triangles; at least 100000 are required.",
        }]
        SIDECAR._persist_job(self.job)
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS.clear()

        with mock.patch.object(SIDECAR, "_submit") as submit:
            SIDECAR._restore_jobs()

        restored = SIDECAR._JOBS[self.job.id]
        self.assertEqual(restored.state, "queued")
        self.assertEqual(restored.phase, "resuming")
        submit.assert_called_once_with(
            restored,
            SIDECAR._generate_job,
            "resume accepted near-target artifact",
            True,
        )

    def test_restore_rejects_legacy_face_error_outside_tolerance(self):
        self.assertFalse(
            SIDECAR._legacy_face_error_is_recoverable(
                "The generated OBJ contains 85000 triangles; at least 100000 are required.",
                100000,
            )
        )

    def test_resumed_download_reuses_valid_local_obj(self):
        attempt_directory = self.job.directory / "attempt-01"
        attempt_directory.mkdir()
        candidate = attempt_directory / "model-vertex-color.obj"
        candidate.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="ascii")
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
        }]

        with (
            mock.patch.object(SIDECAR, "_validate_artifact") as validate,
            mock.patch.object(SIDECAR, "wait_for_task") as wait,
            mock.patch.object(SIDECAR, "download_task_artifact") as download,
        ):
            result = SIDECAR._download_conversion(self.job, "existing-generation", "obj", 1, True)

        self.assertEqual(result, candidate)
        validate.assert_called_once_with(candidate, "obj", allow_repairable_obj=True)
        wait.assert_not_called()
        download.assert_not_called()

    def test_resumed_download_uses_new_recovery_workspace_when_local_obj_is_invalid(self):
        attempt_directory = self.job.directory / "attempt-01"
        attempt_directory.mkdir()
        candidate = attempt_directory / "model-vertex-color.obj"
        candidate.write_text("invalid", encoding="ascii")
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
        }]

        def write_download(_result, destination, _limit):
            destination.write_bytes(b"download")

        recovered = attempt_directory / "recovery-01" / "model-vertex-color.obj"
        with (
            mock.patch.object(SIDECAR, "_validate_artifact", side_effect=SIDECAR.TripoError("invalid")),
            mock.patch.object(SIDECAR, "wait_for_task", return_value={"output": {}}) as wait,
            mock.patch.object(SIDECAR, "download_task_artifact", side_effect=write_download) as download,
            mock.patch.object(SIDECAR, "_prepare_obj_artifact", return_value=recovered) as prepare,
        ):
            result = SIDECAR._download_conversion(self.job, "existing-generation", "obj", 1, True)

        self.assertEqual(result, recovered)
        wait.assert_called_once()
        destination = download.call_args.args[1]
        self.assertEqual(destination.parent, attempt_directory / "recovery-01")
        prepare.assert_called_once_with(destination, attempt_directory / "recovery-01", self.job.palette)

    def test_generation_downloads_obj_artifact(self):
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="utf-8")

        with (
            mock.patch.object(SIDECAR, "create_text_task", return_value="generation-id"),
            mock.patch.object(SIDECAR, "wait_for_task", return_value={}),
            mock.patch.object(SIDECAR, "_download_conversion", return_value=artifact) as download,
            mock.patch.object(SIDECAR, "_validate_obj_topology", return_value=(300000, 2, 0)),
        ):
            SIDECAR._generate_job(self.job, "printable object")

        download.assert_called_once_with(self.job, "generation-id", "obj", 1)
        self.assertEqual(self.job.state, "ready")
        self.assertEqual(self.job.artifact_format, "obj")
        self.assertEqual(self.job.artifact_path, artifact)

    def test_resumed_generation_reuses_remote_ids_without_paid_creation(self):
        artifact = self.job.directory / "model-vertex-color.obj"
        artifact.write_text("v 0 0 0 1 0 0\nf 1 1 1\n", encoding="utf-8")
        self.job.attempts = [{
            "attempt": 1,
            "generation_task_id": "existing-generation",
            "conversion_task_id": "existing-conversion",
            "status": "running",
            "error": "old recoverable download failure",
        }]
        with (
            mock.patch.object(SIDECAR, "create_text_task") as create_generation,
            mock.patch.object(SIDECAR, "create_conversion") as create_conversion,
            mock.patch.object(SIDECAR, "wait_for_task", return_value={}),
            mock.patch.object(SIDECAR, "_download_conversion", return_value=artifact) as download,
            mock.patch.object(SIDECAR, "_validate_obj_topology", return_value=(300000, 2, 0)),
        ):
            SIDECAR._generate_job(self.job, "printable object", True)

        create_generation.assert_not_called()
        create_conversion.assert_not_called()
        download.assert_called_once_with(self.job, "existing-generation", "obj", 1, True)
        self.assertEqual(self.job.state, "ready")
        self.assertEqual(self.job.attempts[0]["error"], "")

    def test_obj_conversion_failure_does_not_fall_back(self):
        error = SIDECAR.TripoError("OBJ conversion failed")
        with (
            mock.patch.object(SIDECAR, "create_text_task", return_value="generation-id"),
            mock.patch.object(SIDECAR, "wait_for_task", return_value={}),
            mock.patch.object(SIDECAR, "_download_conversion", side_effect=error) as download,
        ):
            SIDECAR._generate_job(self.job, "printable object")

        download.assert_called_once_with(self.job, "generation-id", "obj", 1)
        self.assertEqual(self.job.state, "failed")
        self.assertEqual(self.job.message, "OBJ conversion failed")
        self.assertEqual(len(self.job.attempts), 1)
        self.assertEqual(self.job.attempts[0]["status"], "rejected")

    def test_quality_failure_does_not_create_a_second_paid_task(self):
        quality_error = SIDECAR.TripoError(
            "Tripo generated a non-watertight or non-manifold mesh. Regenerate before importing into OrcaSlicer."
        )
        with (
            mock.patch.object(SIDECAR, "create_text_task", return_value="generation-1") as create,
            mock.patch.object(SIDECAR, "wait_for_task", return_value={}),
            mock.patch.object(SIDECAR, "_download_conversion", side_effect=quality_error) as download,
        ):
            SIDECAR._generate_job(self.job, "printable object")

        create.assert_called_once_with("printable object", 300000)
        self.assertEqual(download.call_count, 1)
        self.assertEqual(self.job.state, "failed")
        self.assertEqual([attempt["status"] for attempt in self.job.attempts], ["rejected"])
        attempts = (self.job.directory / "attempts.json").read_text(encoding="utf-8")
        self.assertIn("generation-1", attempts)

    def test_quality_failure_exhausts_single_attempt_budget(self):
        quality_error = SIDECAR.TripoError("The generated OBJ exceeds the 1000000-triangle limit.")
        with (
            mock.patch.object(SIDECAR, "create_text_task", return_value="generation-1") as create,
            mock.patch.object(SIDECAR, "wait_for_task", return_value={}),
            mock.patch.object(SIDECAR, "_download_conversion", side_effect=quality_error) as download,
        ):
            SIDECAR._generate_job(self.job, "printable object")

        self.assertEqual(create.call_count, SIDECAR.MAX_GENERATION_ATTEMPTS)
        self.assertEqual(download.call_count, SIDECAR.MAX_GENERATION_ATTEMPTS)
        self.assertEqual(self.job.state, "failed")
        self.assertEqual(len(self.job.attempts), SIDECAR.MAX_GENERATION_ATTEMPTS)
        self.assertTrue(all(attempt["status"] == "rejected" for attempt in self.job.attempts))

    def test_obj_is_normalized_to_z_up_100mm_and_on_bed(self):
        source = self.job.directory / "normalize.obj"
        source.write_text(
            "v -0.2 -0.5 -0.1 1 0 0\n"
            "v 0.2 -0.5 0.1 0 1 0\n"
            "v 0 0.5 0 0 0 1\n"
            "f 1 2 3\n",
            encoding="ascii",
        )

        SIDECAR._normalize_obj_for_orca(source)

        vertices = [
            tuple(float(value) for value in line.split()[1:4])
            for line in source.read_text(encoding="ascii").splitlines()
            if line.startswith("v ")
        ]
        self.assertAlmostEqual(min(vertex[2] for vertex in vertices), 0.0)
        self.assertAlmostEqual(max(vertex[2] for vertex in vertices), 100.0)
        self.assertLessEqual(max(abs(vertex[0]) for vertex in vertices), 20.0)
        self.assertLessEqual(max(abs(vertex[1]) for vertex in vertices), 10.0)
        self.assertEqual(self.job.artifact_format, "")
        self.assertIsNone(self.job.artifact_path)

    def test_zip_texture_is_baked_into_vertex_colors(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        self.assertEqual(artifact.name, "model-vertex-color.obj")
        self.assertTrue((self.job.directory / "artifact-raw.zip").is_file())
        self.assertTrue((self.job.directory / "package" / "model.obj").is_file())
        lines = artifact.read_text(encoding="ascii").splitlines()
        vertices = [line for line in lines if line.startswith("v ")]
        self.assertEqual(len(vertices), 4)
        output_colors = {tuple(round(float(value) * 255) for value in line.split()[4:7]) for line in vertices}
        self.assertLessEqual(output_colors, {(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)})
        forbidden = ("mtllib ", "usemtl ", "vt ", "vn ")
        self.assertFalse(any(line.lower().startswith(forbidden) for line in lines))
        metrics = json.loads((self.job.directory / "vertex-color-metrics.json").read_text(encoding="utf-8"))
        self.assertEqual(metrics["face_count"], 4)
        self.assertEqual(metrics["three_color_faces"], 0)
        cleanup = json.loads((self.job.directory / "vertex-color-cleanup.json").read_text(encoding="utf-8"))
        self.assertIn(cleanup["status"], {"not_needed", "consolidated"})
        boundary = json.loads((self.job.directory / "color-boundary-cleanup.json").read_text(encoding="utf-8"))
        self.assertIn(boundary["status"], {"not_needed", "regularized"})
        quality = json.loads((self.job.directory / SIDECAR.MODEL_QUALITY_FILENAME).read_text(encoding="utf-8"))
        self.assertEqual(quality["status"], "review")
        self.assertEqual(quality["metrics"]["face_count"], 4)
        self.assertTrue(quality["metrics"]["target_palette_metrics_available"])
        self.assertEqual(quality["metrics"]["target_palette_color_count"], 4)
        self.assertEqual(quality["metrics"]["meaningful_target_palette_color_count"], 2)
        self.assertFalse(quality["metrics"]["target_palette_diversity_ok"])
        self.assertAlmostEqual(quality["metrics"]["target_palette_surface_coverage_ratio"], 1.0)
        self.assertEqual(len(quality["evidence"]["target_palette_surface_usage"]), 4)
        self.assertIn("too_few_meaningful_target_palette_colors", quality["warnings"])
        SIDECAR._validate_obj_vertex_colors(artifact)
        SIDECAR._validate_obj_topology(artifact)

    def test_model_quality_report_write_failure_is_a_controlled_generation_error(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)
        failure = SIDECAR.ModelQualityError("report_write_failed", "quality report unavailable")

        with mock.patch.object(SIDECAR, "write_model_quality_report", side_effect=failure):
            with self.assertRaisesRegex(SIDECAR.TripoError, "quality report unavailable"):
                SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_zip_texture_preserves_natural_vertex_colors_without_printable_palette(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, ())

        lines = artifact.read_text(encoding="ascii").splitlines()
        output_colors = {
            tuple(round(float(value) * 255) for value in line.split()[4:7])
            for line in lines
            if line.startswith("v ")
        }
        self.assertGreaterEqual(len(output_colors), 2)
        self.assertTrue(any(channel not in range(0, 256, 51) for color in output_colors for channel in color))
        self.assertFalse((self.job.directory / "vertex-color-cleanup.json").exists())
        self.assertFalse((self.job.directory / "color-boundary-cleanup.json").exists())
        SIDECAR._validate_obj_vertex_colors(artifact)
        SIDECAR._validate_obj_topology(artifact)

    def test_object_group_and_material_boundaries_are_preserved_as_groups(self):
        raw = self.job.directory / "artifact-raw.download"
        obj = (
            "mtllib model.mtl\n"
            "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
            "vt 0 1\nvt 1 1\nvt 0 0\n"
            "o body shell\ng head part\nusemtl painted\n"
            "f 1/1 3/3 2/2\nf 1/1 2/2 4/3\nf 1/1 4/3 3/3\nf 2/2 3/3 4/3\n"
        )
        self._write_package(raw, obj=obj)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, ())

        lines = artifact.read_text(encoding="ascii").splitlines()
        self.assertIn("o body_shell", lines)
        self.assertIn("g head_part", lines)
        self.assertIn("g material_painted", lines)

    def test_uv_seams_do_not_duplicate_geometry_vertices(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        vertices = [line for line in artifact.read_text(encoding="ascii").splitlines() if line.startswith("v ")]
        self.assertEqual(len(vertices), 4)
        self.assertEqual(SIDECAR._validate_obj_topology(artifact), (4, 1, 0))

    def test_disconnected_parts_are_preserved_before_import(self):
        raw = self.job.directory / "artifact-raw.download"
        vertices = "\n".join(
            f"v {x} {y} {z}" for x, y, z in (
                (0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1),
                (3, 0, 0), (4, 0, 0), (3, 1, 0), (3, 0, 1),
            )
        )
        faces = "\n".join((
            "f 1/1 3/3 2/2", "f 1/1 2/2 4/3", "f 1/1 4/3 3/3", "f 2/2 3/3 4/3",
            "f 5/1 7/3 6/2", "f 5/1 6/2 8/3", "f 5/1 8/3 7/3", "f 6/2 7/3 8/3",
        ))
        obj = f"mtllib model.mtl\n{vertices}\nvt 0 1\nvt 1 1\nvt 0 0\nusemtl painted\n{faces}\n"
        self._write_package(raw, obj=obj)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)
        self.assertEqual(SIDECAR._validate_obj_topology(artifact), (8, 2, 0))

    def test_tiny_detached_component_is_preserved_and_reported(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(self._vertex_color_torus(add_tiny_component=True), encoding="ascii")

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        self.assertEqual(SIDECAR._validate_obj_topology(artifact), (132, 2, 0))
        report = json.loads((self.job.directory / "mesh-repair.json").read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "preserved")
        self.assertEqual(report["original_components"], 2)
        self.assertEqual(report["removed_components"], 0)

    def test_bounded_detached_noise_is_removed_from_dense_model(self):
        source = self.job.directory / "dense-with-noise.obj"
        size = 151
        vertices = [f"v {x} {y} 0 1 0 0" for y in range(size) for x in range(size)]
        faces = []
        for y in range(size - 1):
            for x in range(size - 1):
                left = y * size + x
                right = left + 1
                upper = left + size
                upper_right = upper + 1
                faces.extend(((left, upper, right), (right, upper, upper_right)))
        noise_start = len(vertices)
        vertices.extend((
            "v 75 75 1 0 1 0",
            "v 75.05 75 1 0 1 0",
            "v 75 75.05 1 0 1 0",
            "v 75 75 1.05 0 1 0",
        ))
        faces.extend((
            (noise_start, noise_start + 2, noise_start + 1),
            (noise_start, noise_start + 1, noise_start + 3),
            (noise_start, noise_start + 3, noise_start + 2),
            (noise_start + 1, noise_start + 2, noise_start + 3),
        ))
        source.write_text(
            "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
            encoding="ascii",
        )

        report = SIDECAR._remove_small_detached_obj_components(
            source, self.job.directory / "dense-mesh-repair.json"
        )

        self.assertEqual(report["status"], "removed")
        self.assertEqual(report["removed_components"], 1)
        self.assertEqual(report["removed_vertices"], 4)
        self.assertEqual(report["removed_faces"], 4)
        self.assertEqual(report["kept_faces"], (size - 1) * (size - 1) * 2)
        self.assertNotIn("v 75 75 1 0 1 0", source.read_text(encoding="ascii"))

    def test_unreferenced_vertex_is_removed_without_touching_main_component(self):
        source = self.job.directory / "unreferenced-vertex.obj"
        source.write_text(
            "v 0 0 0 1 0 0\n"
            "v 1 0 0 1 0 0\n"
            "v 0 1 0 1 0 0\n"
            "v 0 0 1 1 0 0\n"
            "v 99 99 99 0 1 0\n"
            "f 1 3 2\n"
            "f 1 2 4\n"
            "f 1 4 3\n"
            "f 2 3 4\n",
            encoding="ascii",
        )

        report = SIDECAR._remove_small_detached_obj_components(
            source, self.job.directory / "unreferenced-mesh-repair.json"
        )

        self.assertEqual(report["status"], "removed")
        self.assertEqual(report["removed_components"], 0)
        self.assertEqual(report["removed_vertices"], 1)
        self.assertEqual(SIDECAR._validate_obj_topology(source), (4, 1, 0))

    def test_tiny_vertex_color_component_is_merged_into_strongest_neighbor(self):
        source = self.job.directory / "tiny-color-island.obj"
        size = 101
        center = (size // 2) * size + size // 2
        vertices = [
            f"v {x} {y} 0 " + ("0 0 1" if y * size + x == center else "1 0 0")
            for y in range(size)
            for x in range(size)
        ]
        faces = []
        for y in range(size - 1):
            for x in range(size - 1):
                left = y * size + x
                right = left + 1
                upper = left + size
                upper_right = upper + 1
                faces.extend(((left, upper, right), (right, upper, upper_right)))
        source.write_text(
            "\n".join(vertices + [f"f {a + 1} {b + 1} {c + 1}" for a, b, c in faces]) + "\n",
            encoding="ascii",
        )

        report = SIDECAR._consolidate_tiny_obj_color_components(
            source, self.job.directory / "tiny-color-cleanup.json"
        )

        self.assertEqual(report["status"], "consolidated")
        self.assertEqual(report["merged_components"], 1)
        self.assertEqual(report["recolored_vertices"], 1)
        self.assertEqual(report["final_vertex_color_usage"], {"#FF0000": size * size})
        SIDECAR._validate_obj_palette(source, ("#FF0000", "#0000FF"))

    def test_isolated_color_boundary_spike_is_regularized(self):
        source = self.job.directory / "boundary-spike.obj"
        size = 21
        center = size // 2
        source.write_text(
            self._vertex_color_grid(
                size,
                lambda x, y: "0 0 1" if (x, y) == (center, center) else "1 0 0",
            ),
            encoding="ascii",
        )

        report = SIDECAR._regularize_obj_color_boundaries(
            source, self.job.directory / "boundary-spike-cleanup.json"
        )

        self.assertEqual(report["status"], "regularized")
        self.assertEqual(report["recolored_vertices"], 1)
        self.assertGreater(report["before"]["mixed_face_count"], 0)
        self.assertEqual(report["after"]["mixed_face_count"], 0)
        self.assertLess(
            report["after"]["mixed_face_surface_area_mm2"],
            report["before"]["mixed_face_surface_area_mm2"],
        )
        self.assertNotIn(" 0.000000 0.000000 1.000000", source.read_text(encoding="ascii"))
        self.assertEqual(SIDECAR._obj_vertex_color_metrics(source)["three_color_faces"], 0)

    def test_coherent_color_boundary_is_preserved(self):
        source = self.job.directory / "coherent-boundary.obj"
        size = 21
        source.write_text(
            self._vertex_color_grid(size, lambda x, _y: "1 0 0" if x < size // 2 else "0 0 1"),
            encoding="ascii",
        )
        original = source.read_bytes()

        report = SIDECAR._regularize_obj_color_boundaries(
            source, self.job.directory / "coherent-boundary-cleanup.json"
        )

        self.assertEqual(report["status"], "not_needed")
        self.assertEqual(report["recolored_vertices"], 0)
        self.assertEqual(report["before"]["mixed_face_count"], report["after"]["mixed_face_count"])
        self.assertEqual(source.read_bytes(), original)

    def test_meaningful_palette_color_is_protected_from_boundary_cleanup(self):
        source = self.job.directory / "meaningful-spike.obj"
        size = 5
        center = size // 2
        source.write_text(
            self._vertex_color_grid(
                size,
                lambda x, y: "0 0 1" if (x, y) == (center, center) else "1 0 0",
            ),
            encoding="ascii",
        )

        with (
            mock.patch.object(SIDECAR, "MAX_COLOR_BOUNDARY_SURFACE_AREA_RATIO", 1.0),
            mock.patch.object(SIDECAR, "MAX_COLOR_BOUNDARY_SOURCE_AREA_RATIO", 1.0),
        ):
            report = SIDECAR._regularize_obj_color_boundaries(
                source, self.job.directory / "meaningful-spike-cleanup.json"
            )

        self.assertEqual(report["status"], "not_needed")
        self.assertEqual(report["recolored_vertices"], 0)
        self.assertGreater(report["protected_meaningful_candidates"], 0)
        self.assertIn("v 2 2 0 0 0 1", source.read_text(encoding="ascii"))

    def test_repairable_open_edges_are_deferred_to_orca(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(self._vertex_color_torus(omit_faces=(0,)), encoding="ascii")

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        self.assertEqual(SIDECAR._validate_obj_topology(artifact, allow_repairable=True), (127, 1, 3))
        with self.assertRaisesRegex(SIDECAR.TripoError, "watertight"):
            SIDECAR._validate_obj_topology(artifact)

    def test_inconsistent_face_winding_is_normalized_before_orca_import(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            "v 0 0 0 1 0 0\n"
            "v 1 0 0 0 1 0\n"
            "v 0 1 0 0 0 1\n"
            "v 0 0 1 1 1 0\n"
            "f 1 2 3\n"
            "f 1 2 4\n"
            "f 1 4 3\n"
            "f 2 3 4\n",
            encoding="ascii",
        )

        with self.assertRaisesRegex(SIDECAR.TripoError, "inconsistently wound"):
            SIDECAR._validate_obj_topology(raw)

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        self.assertEqual(SIDECAR._validate_obj_topology(artifact), (4, 1, 0))
        report = json.loads((self.job.directory / "mesh-repair.json").read_text(encoding="utf-8"))
        self.assertEqual(report["topology_status"], "repaired")
        self.assertEqual(report["original_inconsistent_winding_edges"], 3)
        self.assertEqual(report["flipped_winding_faces"], 1)
        self.assertEqual(report["remaining_inconsistent_winding_edges"], 0)
        self.assertEqual(report["remaining_invalid_edges"], 0)

    def test_small_non_manifold_defect_is_repaired_with_palette_colors(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            self._vertex_color_torus(duplicate_faces=(0,), major_segments=128, minor_segments=64),
            encoding="ascii",
        )

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        face_count, components, invalid_edges = SIDECAR._validate_obj_topology(artifact)
        self.assertLessEqual(face_count, SIDECAR.MAX_MODEL_FACES)
        self.assertEqual((components, invalid_edges), (1, 0))
        report = json.loads((self.job.directory / "mesh-repair.json").read_text(encoding="utf-8"))
        self.assertEqual(report["topology_status"], "repaired")
        self.assertGreater(report["removed_non_manifold_faces"], 0)
        self.assertGreater(report["filled_boundary_loops"], 0)
        self.assertEqual(report["remaining_invalid_edges"], 0)
        SIDECAR._validate_obj_palette(artifact, self.palette)

    def test_separated_small_non_manifold_defects_are_repaired_independently(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            self._vertex_color_torus(duplicate_faces=(0, 8192), major_segments=128, minor_segments=64),
            encoding="ascii",
        )

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        face_count, components, invalid_edges = SIDECAR._validate_obj_topology(artifact)
        self.assertLessEqual(face_count, SIDECAR.MAX_MODEL_FACES)
        self.assertEqual((components, invalid_edges), (1, 0))
        report = json.loads((self.job.directory / "mesh-repair.json").read_text(encoding="utf-8"))
        self.assertEqual(report["topology_status"], "repaired")
        self.assertEqual(report["non_manifold_regions"], 2)
        self.assertLess(
            report["max_non_manifold_region_diagonal_ratio"],
            SIDECAR.MAX_LOCAL_REPAIR_DIAGONAL_RATIO,
        )
        self.assertEqual(report["remaining_invalid_edges"], 0)
        SIDECAR._validate_obj_palette(artifact, self.palette)

    def test_excessive_open_edges_are_rejected_before_import(self):
        raw = self.job.directory / "artifact-raw.download"
        vertices = ["v 0 0 0 1 0 0"]
        faces = []
        for index in range(30):
            angle = 2.0 * math.pi * index / 30
            vertices.extend(
                (
                    f"v {math.cos(angle):.9g} {math.sin(angle):.9g} 0 1 0 0",
                    f"v {math.cos(angle):.9g} {math.sin(angle):.9g} 1 1 0 0",
                )
            )
            faces.append(f"f 1 {2 * index + 2} {2 * index + 3}")
        raw.write_text("\n".join(vertices + faces) + "\n", encoding="ascii")

        with self.assertRaisesRegex(SIDECAR.TripoError, "watertight"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_open_mesh_is_rejected_before_import(self):
        raw = self.job.directory / "artifact-raw.download"
        obj = (
            "mtllib model.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "vt 0 1\nvt 1 1\nvt 0 0\nusemtl painted\nf 1/1 2/2 3/3\n"
        )
        self._write_package(raw, obj=obj)

        with self.assertRaisesRegex(SIDECAR.TripoError, "watertight"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_plain_vertex_color_obj_is_preserved(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nv 0 0 1 1 1 0\n"
            "f 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n",
            encoding="ascii",
        )

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

        self.assertTrue((self.job.directory / "artifact-raw.obj").is_file())
        SIDECAR._validate_obj_vertex_colors(artifact)

    def test_plain_natural_vertex_colors_are_not_quantized(self):
        raw = self.job.directory / "artifact-raw.download"
        raw.write_text(
            "o colored_part\n"
            "v 0 0 0 0.123 0.456 0.789\nv 1 0 0 0.2 0.3 0.4\n"
            "v 0 1 0 0.5 0.6 0.7\nv 0 0 1 0.8 0.9 0.1\n"
            "f 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n",
            encoding="ascii",
        )

        artifact = SIDECAR._prepare_obj_artifact(raw, self.job.directory, ())

        text = artifact.read_text(encoding="ascii")
        self.assertIn("0.123000 0.456000 0.789000", text)
        self.assertIn("o colored_part", text)

    def test_zip_path_traversal_is_rejected(self):
        raw = self.job.directory / "artifact-raw.download"
        with zipfile.ZipFile(raw, "w") as bundle:
            bundle.writestr("../escape.obj", "v 0 0 0\n")
        with self.assertRaisesRegex(SIDECAR.TripoError, "unsafe path"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)
        self.assertFalse((self.job.directory.parent / "escape.obj").exists())

    def test_zip_symbolic_link_is_rejected(self):
        raw = self.job.directory / "artifact-raw.download"
        link = zipfile.ZipInfo("model.obj")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(raw, "w") as bundle:
            bundle.writestr(link, "target.obj")
        with self.assertRaisesRegex(SIDECAR.TripoError, "symbolic link"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_zip_file_count_limit_is_enforced(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)
        with mock.patch.object(SIDECAR, "MAX_ARCHIVE_FILES", 2):
            with self.assertRaisesRegex(SIDECAR.TripoError, "number of files"):
                SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_zip_unpacked_size_limit_is_enforced(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw)
        with mock.patch.object(SIDECAR, "MAX_UNPACKED_BYTES", 4):
            with self.assertRaisesRegex(SIDECAR.TripoError, "too large"):
                SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_missing_obj_is_rejected(self):
        raw = self.job.directory / "artifact-raw.download"
        with zipfile.ZipFile(raw, "w") as bundle:
            bundle.writestr("model.mtl", "newmtl painted\n")
        with self.assertRaisesRegex(SIDECAR.TripoError, "exactly one OBJ"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_missing_material_is_rejected(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw, obj="v 0 0 0\nvt 0 0\nmtllib missing.mtl\nf 1/1 1/1 1/1\n")
        with self.assertRaisesRegex(SIDECAR.TripoError, "missing its material"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_missing_base_color_texture_is_rejected(self):
        raw = self.job.directory / "artifact-raw.download"
        self._write_package(raw, texture=False)
        with self.assertRaisesRegex(SIDECAR.TripoError, "base-color texture"):
            SIDECAR._prepare_obj_artifact(raw, self.job.directory, self.palette)

    def test_cleanup_keeps_generated_files(self):
        artifact = self.job.directory / "kept.obj"
        artifact.write_text("kept", encoding="ascii")
        SIDECAR._cleanup_job(self.job)
        self.assertTrue(artifact.is_file())

    def test_deleted_job_keeps_generated_files(self):
        artifact = self.job.directory / "kept-after-delete.obj"
        artifact.write_text("kept", encoding="ascii")
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS[self.job.id] = self.job
            self.job.delete_requested = True
        SIDECAR._finish_deleted(self.job)
        self.assertNotIn(self.job.id, SIDECAR._JOBS)
        self.assertTrue(artifact.is_file())
        self.assertFalse((self.job.directory / SIDECAR.JOB_STATE_FILENAME).exists())

    def test_zz_shutdown_keeps_generated_files(self):
        artifact = self.job.directory / "kept-after-shutdown.obj"
        artifact.write_text("kept", encoding="ascii")
        with SIDECAR._JOBS_LOCK:
            SIDECAR._JOBS[self.job.id] = self.job
        SIDECAR.shutdown_sidecar()
        self.assertTrue(artifact.is_file())


if __name__ == "__main__":
    unittest.main()
