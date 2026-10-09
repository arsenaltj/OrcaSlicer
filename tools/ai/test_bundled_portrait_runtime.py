import json
from pathlib import Path
import tempfile
import unittest

from bundled_portrait_runtime import digest, install_raster, verify
from stage_bundled_portrait_runtime import copy_file


class BundledPortraitRuntimeTests(unittest.TestCase):
    def test_staging_preserves_native_runtime_and_omits_static_link_libraries(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "native.lib"; source.write_bytes(b"compile-only")
            copy_file(source, root / "stage/native.lib")
            self.assertFalse((root / "stage/native.lib").exists())
            source = root / "native.dll"; source.write_bytes(b"runtime")
            copy_file(source, root / "stage/native.dll")
            self.assertEqual((root / "stage/native.dll").read_bytes(), b"runtime")

    def fixture(self,root):
        file=root/"python"/"runtime.dll";file.parent.mkdir();file.write_bytes(b"fixture")
        record={"sha256":digest(file),"size":file.stat().st_size}
        manifest={"schema":"orca.offline-portrait-runtime/v1","network":"offline","provider_calls":False,
            "model_download":False,"files":{"python/runtime.dll":record},"modules":{},"packages":{}}
        (root/"modules").mkdir()
        for name in ("glb_artifact.py", "local_semantic_worker.py", "local_semantic_request.py",
                     "local_contour_proposals.py", "local_surface_contours.py"):
            module=root/"modules"/name;module.write_bytes(b"fixture")
            manifest["modules"][name]={"sha256":digest(module),"size":module.stat().st_size}
        (root/"runtime-manifest.json").write_text(json.dumps(manifest))
        return file,manifest

    def test_missing_dependency_and_hash_drift_disable_runtime(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);file,_=self.fixture(root)
            self.assertEqual(verify(root,imports=False)["file_count"],1)
            file.write_bytes(b"changed")
            with self.assertRaises(ValueError):verify(root,imports=False)
            file.unlink()
            with self.assertRaises(ValueError):verify(root,imports=False)

    def test_a_present_bundle_without_its_source_reader_is_incomplete(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);_,manifest=self.fixture(root)
            del manifest["modules"]["glb_artifact.py"]
            (root/"modules/glb_artifact.py").unlink()
            (root/"runtime-manifest.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError,"incomplete_portrait_modules"):
                verify(root,imports=False)

    def test_manifest_paths_cannot_leave_installation(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);_,manifest=self.fixture(root)
            manifest["files"]["../outside.dll"]={"size":0,"sha256":"0"*64}
            (root/"runtime-manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):verify(root,imports=False)

    def test_installer_seals_accelerator_in_the_actual_module_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);_,manifest=self.fixture(root)
            source=root/"compiled-raster.dll";source.write_bytes(b"compiled fixture")
            install_raster(root,source)
            self.assertTrue(verify(root,imports=False)["raster_required"])
            target=root/"modules/local_semantic_raster.dll"
            self.assertEqual(target.read_bytes(),source.read_bytes())
            manifest=json.loads((root/"runtime-manifest.json").read_text())
            self.assertEqual(manifest["modules"][target.name],manifest["files"]["modules/"+target.name])
            install_raster(root,source)  # Repeat installation with identical inputs is safe.
            target.write_bytes(b"wrong dll")
            with self.assertRaisesRegex(ValueError,"runtime_hash_mismatch"):
                verify(root,imports=False)

    def test_required_accelerator_cannot_fall_back_to_the_legacy_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);_,manifest=self.fixture(root)
            (root/"legacy").mkdir()
            (root/"legacy/local_semantic_raster.dll").write_bytes(b"legacy")
            manifest["raster_required"]=True
            (root/"runtime-manifest.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError,"incomplete_portrait_accelerator"):
                verify(root,imports=False)

    def test_online_policy_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);_,manifest=self.fixture(root);manifest["provider_calls"]=True
            (root/"runtime-manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):verify(root,imports=False)


if __name__=="__main__":unittest.main()
