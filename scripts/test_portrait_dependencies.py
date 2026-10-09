"""Dependency delivery checks use tiny archives and never contact providers."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

import portrait_dependencies as deps


class DependencyDeliveryTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.payload = b"test weight bytes"
        self.manifest = {"schema": deps.SCHEMA, "version": "test-v1", "files": {
            "weights/model.pt": {"bytes": len(self.payload), "sha256": hashlib.sha256(self.payload).hexdigest()}}}

    def archive(self, extra=None):
        path = self.root / "test.zip"
        raw = json.dumps(self.manifest).encode()
        with zipfile.ZipFile(path, "w") as output:
            output.writestr(deps.MANIFEST, raw)
            output.writestr("weights/model.pt", self.payload)
            if extra:
                output.writestr(*extra)
        return path, {"schema": deps.SCHEMA, "version": "test-v1", "platform": "windows-x64-cp312-cpu",
                      "archive": {"name": path.name, "url": "https://example.invalid/test.zip", **deps.record(path)},
                      "manifest": {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}}

    def test_roundtrip_and_detect_modified_dependency(self):
        archive, lock = self.archive()
        destination = self.root / "new machine"
        deps.extract(archive, destination, lock)
        deps.verify(destination, lock)
        (destination / "weights/model.pt").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "mismatch"):
            deps.verify(destination, lock)

    def test_corrupt_archive_rejected_before_extraction(self):
        archive, lock = self.archive()
        archive.write_bytes(archive.read_bytes() + b"tampered")
        destination = self.root / "new"
        with self.assertRaisesRegex(ValueError, "mismatch"):
            deps.extract(archive, destination, lock)
        self.assertFalse(destination.exists())

    def test_manifest_hash_is_independently_checked(self):
        archive, lock = self.archive()
        lock["manifest"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "manifest hash"):
            deps.extract(archive, self.root / "new", lock)

    def test_traversal_and_ambiguous_windows_paths_rejected(self):
        for name in ("../escape", "/absolute", "C:/file", "weights\\file", "a//b", "a/./b", "a./b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                deps.relative_path(name)
        archive, lock = self.archive(("../escape", b"bad"))
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            deps.extract(archive, self.root / "new", lock)
        self.assertFalse((self.root / "escape").exists())

    def test_unlisted_file_rejected(self):
        archive, lock = self.archive(("python/unexpected.py", b"bad"))
        with self.assertRaisesRegex(ValueError, "unlisted"):
            deps.extract(archive, self.root / "new", lock)

    def test_case_collision_rejected_on_windows(self):
        archive, lock = self.archive(("Weights/Model.pt", b"bad"))
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            deps.extract(archive, self.root / "new", lock)

    def test_no_overwrite_of_existing_files(self):
        archive, lock = self.archive()
        destination = self.root / "existing"
        destination.mkdir()
        existing = destination / deps.MANIFEST
        existing.write_text("keep")
        with self.assertRaises(FileExistsError):
            deps.extract(archive, destination, lock)
        self.assertEqual(existing.read_text(), "keep")

    def test_unexpected_local_source_not_bundled_silently(self):
        archive, lock = self.archive()
        destination = self.root / "new"
        deps.extract(archive, destination, lock)
        (destination / "private_config.json").write_text("{}")
        with self.assertRaisesRegex(ValueError, "Unexpected"):
            deps.verify(destination, lock)

    def test_cache_hit_is_offline_and_corrupt_cache_is_not_replaced(self):
        archive, lock = self.archive()
        self.assertEqual(deps.download(lock, self.root), archive)
        archive.write_bytes(b"keep corrupt evidence")
        with self.assertRaisesRegex(ValueError, "mismatch"):
            deps.download(lock, self.root)
        self.assertEqual(archive.read_bytes(), b"keep corrupt evidence")

    def test_cmake_paths_use_literals_and_include_wheel_cache(self):
        destination = self.root / "with space"
        destination.mkdir()
        deps.write_init(destination)
        source = (destination / deps.INIT).read_text()
        self.assertIn(f"[==[{destination.as_posix()}/weights]==]", source)
        self.assertIn("ORCA_AI_PORTRAIT_SITE_PACKAGES", source)
        self.assertIn("ORCA_SEMANTIC_RUNTIME_DIR", source)
        self.assertIn("ORCA_AI_PYTHON_STABLE_ABI_DLL", source)
        self.assertIn("${CMAKE_BINARY_DIR}/_deps/orca_ai_wheels", source)
        self.assertNotIn("ORCA_BEAUTY_RUNTIME_ROOT", source)


if __name__ == "__main__":
    unittest.main()
