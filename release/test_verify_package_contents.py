import io
import bz2
import gzip
import hashlib
import json
from pathlib import Path
import tempfile
import tarfile
import unittest
from unittest.mock import patch
import zipfile

from verify_package_contents import inspect
import verify_package_contents as inspection


class PackageInspectionTests(unittest.TestCase):
    def package(self, members):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            for name, value in members.items():
                archive.writestr(name, value)
        return data.getvalue()

    def check(self, members):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "package.zip"
            path.write_bytes(self.package(members))
            return inspect(path)

    def test_ordinary_config_and_placeholders_are_distinguished(self):
        report = self.check({"orca_ai_internal_defaults.json": json.dumps({
            "OPENAI_PRO_URL": "https://example.invalid", "model": "fixture",
            "OPENAI_PRO_API": "your_key", "TRIPO_API_KEY": ""})})
        self.assertEqual(report["status"], "NOT_DETECTED_WITHIN_SCOPE")
        self.assertIn("ORDINARY_CONFIGURATION", {f["category"] for f in report["config_fields"]})

    def test_nonplaceholder_credential_is_blocked_without_disclosing_value(self):
        secret = "fixture-value-that-is-not-a-real-credential"
        report = self.check({"orca_ai_internal_defaults.json": json.dumps({"OPENAI_PRO_API": secret})})
        self.assertEqual(report["status"], "BLOCKED_FINDINGS")
        self.assertNotIn(secret, json.dumps(report))

    def test_nested_payload_and_utf16_assignment_are_inspected(self):
        nested = self.package({"config.env": 'TRIPO_API_KEY="fixture-opaque-value"'.encode("utf-16")})
        self.assertEqual(self.check({"runtime.zip": nested})["status"], "BLOCKED_FINDINGS")

    def test_missing_extractor_and_corrupt_zip_fail_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "installer.exe"
            path.write_bytes(b"MZ fixture")
            self.assertEqual(inspect(path, str(Path(temp) / "absent"))["status"], "UNKNOWN")
        self.assertEqual(self.check({"nested.zip": b"bad archive"})["status"], "UNKNOWN")

    def test_traversal_and_malformed_provider_config_fail_closed(self):
        self.assertEqual(self.check({"../config": "x"})["status"], "UNKNOWN")
        self.assertEqual(self.check({"orca_ai_internal_defaults.json": "{bad"})["status"], "UNKNOWN")

    def test_artifacts_do_not_share_results(self):
        dirty = self.check({"config.json": '{"api_key":"fixture-only-value"}'})
        clean = self.check({"config.json": '{"model":"fixture"}'})
        self.assertEqual(dirty["status"], "BLOCKED_FINDINGS")
        self.assertEqual(clean["status"], "NOT_DETECTED_WITHIN_SCOPE")
        self.assertNotEqual(dirty["sha256"], clean["sha256"])

    def test_audited_tooltip_emoji_candidate_is_recorded_as_public_mapping(self):
        bundle = Path(__file__).resolve().parents[1] / "resources/tooltip/main.js"
        report = self.check({"resources/tooltip/main.js": bundle.read_bytes()})
        self.assertEqual(report["status"], "NOT_DETECTED_WITHIN_SCOPE")
        self.assertEqual(report["config_fields"][0]["category"], "PUBLIC_EMOJI_NAME_MAPPING")

    def test_tooltip_classification_does_not_cover_changed_files_or_other_fields(self):
        bundle = (Path(__file__).resolve().parents[1] / "resources/tooltip/main.js").read_bytes()
        cases = [
            {"resources/tooltip/main.js": bundle + b'\nconst api_key="fixture-opaque-value";'},
            {"resources/tooltip/main.js": bundle.replace("\u3299\ufe0f".encode(), b"fixture-opaque-value")},
            {"config.js": bundle},
            {"resources/tooltip/main.js": 'const secret="\u3299\ufe0f";'},
            {"resources/tooltip/main.js": bundle, "provider.json": '{"secret":"fixture-opaque-value"}'},
        ]
        for members in cases:
            with self.subTest(paths=list(members)):
                self.assertEqual(self.check(members)["status"], "BLOCKED_FINDINGS")

    def test_compressed_resources_are_scanned_without_executing_them(self):
        credential = b'api_key="fixture-opaque-value"'
        self.assertEqual(self.check({"config.env.gz": gzip.compress(credential)})["status"], "BLOCKED_FINDINGS")
        self.assertEqual(self.check({"config.env.bz2": bz2.compress(credential)})["status"], "BLOCKED_FINDINGS")
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w:gz") as archive:
            item = tarfile.TarInfo("config.env"); item.size = len(credential)
            archive.addfile(item, io.BytesIO(credential))
        self.assertEqual(self.check({"resources.tar.gz": data.getvalue()})["status"], "BLOCKED_FINDINGS")

    def test_compressed_traversal_links_and_size_limits_fail_closed(self):
        for name, kind in (("../outside", tarfile.REGTYPE), ("linked", tarfile.SYMTYPE)):
            data = io.BytesIO()
            with tarfile.open(fileobj=data, mode="w:gz") as archive:
                item = tarfile.TarInfo(name); item.type = kind
                archive.addfile(item)
            self.assertEqual(self.check({"resources.tar.gz": data.getvalue()})["status"], "UNKNOWN")
        with patch.object(inspection, "MAX_FILE", 64):
            self.assertEqual(self.check({"bomb.dat.gz": gzip.compress(b"a" * 1000)})["status"], "UNKNOWN")

    def test_large_binary_streaming_catches_tokens_across_chunk_boundaries(self):
        token = "sk-" + "a" * 24
        with patch.object(inspection, "MAX_FILE", 64), patch.object(inspection, "SCAN_CHUNK", 11):
            report = self.check({"runtime.dll": b"." * 80 + token.encode("utf-16-le") + b"b" * 80})
            self.assertEqual(report["status"], "BLOCKED_FINDINGS")
            self.assertNotIn(token, json.dumps(report))
            self.assertEqual(self.check({"runtime.dll": b"a" * 200})["status"], "NOT_DETECTED_WITHIN_SCOPE")

    def hardlink_archive(self, links, payload=b'api_key="fixture-opaque-value"', extra=None):
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w:gz") as archive:
            item = tarfile.TarInfo("config.env"); item.size = len(payload)
            archive.addfile(item, io.BytesIO(payload))
            for name, target in links:
                item = tarfile.TarInfo(name); item.type = tarfile.LNKTYPE; item.linkname = target
                archive.addfile(item)
            if extra is not None:
                archive.addfile(extra)
        return data.getvalue()

    def test_internal_hardlink_aliases_scan_target_under_each_alias(self):
        report = self.check({"resources.tar.gz": self.hardlink_archive([
            ("alias.env", "config.env"), ("chain.env", "./alias.env")])})
        self.assertEqual(report["status"], "BLOCKED_FINDINGS")
        self.assertFalse(report["gaps"])
        self.assertEqual({row["member"].split("!")[-1] for row in report["findings"]},
                         {"config.env", "alias.env", "chain.env"})
        self.assertNotIn("fixture-opaque-value", json.dumps(report))

    def test_invalid_hardlink_targets_fail_closed(self):
        cases = [
            [("alias.env", "../config.env")], [("alias.env", "/config.env")],
            [("alias.env", "C:/config.env")], [("alias.env", "missing.env")],
            [("alias.env", "alias.env")], [("alias.env", "other.env"), ("other.env", "alias.env")],
            [("../alias.env", "config.env")], [("alias.env", "config.env"), ("./alias.env", "config.env")],
            [(f"alias{i}.env", f"alias{i+1}.env") for i in range(65)] + [("alias65.env", "config.env")],
        ]
        for links in cases:
            with self.subTest(links=links[:2]):
                self.assertEqual(self.check({"resources.tar.gz": self.hardlink_archive(links)})["status"], "UNKNOWN")
        symbolic = tarfile.TarInfo("symbolic.env"); symbolic.type = tarfile.SYMTYPE; symbolic.linkname = "config.env"
        report = self.check({"resources.tar.gz": self.hardlink_archive([("alias.env", "symbolic.env")], extra=symbolic)})
        self.assertEqual(report["status"], "UNKNOWN")
        self.assertTrue(any(row["reason"] == "unsafe_archive_hardlink" for row in report["gaps"]))

    def test_hardlink_aliases_count_toward_total_budget(self):
        data = self.hardlink_archive([("alias.bin", "config.env")], payload=b"x" * 512)
        with patch.object(inspection, "MAX_TOTAL", len(data) + 768):
            self.assertEqual(self.check({"resources.tar.gz": data})["status"], "UNKNOWN")

    def test_large_model_zip_is_inspected_and_total_budget_stays_enforced(self):
        model = self.package({"padding.bin": b"a" * 200, "config.env": b'api_key="fixture-opaque-value"'})
        with patch.object(inspection, "MAX_FILE", 128):
            self.assertEqual(self.check({"model.pt": model})["status"], "BLOCKED_FINDINGS")
        with patch.object(inspection, "MAX_TOTAL", 100):
            self.assertEqual(self.check({"model.pt": model})["status"], "UNKNOWN")

    def test_audited_dependency_requires_exact_path_file_and_literal(self):
        name = "resources/beauty-runtime/python/Lib/site-packages/library.py"
        data = b'api_key="my_token"'
        record = {"file_sha256": hashlib.sha256(data).hexdigest(),
            "fields": {"api_key": [hashlib.sha256(b"my_token").hexdigest()]}}
        with patch.object(inspection, "AUDITED_LIBRARY_LITERALS", {name: record}):
            clean = self.check({name: data})
            self.assertEqual(clean["status"], "NOT_DETECTED_WITHIN_SCOPE")
            self.assertEqual(clean["config_fields"][0]["category"], "AUDITED_PUBLIC_LIBRARY_LITERAL")
            dependency_path = "python/Lib/site-packages/library.py"
            self.assertEqual(self.check({dependency_path: data})["status"], "NOT_DETECTED_WITHIN_SCOPE")
            self.assertEqual(self.check({dependency_path: data + b'\npassword="opaque-value"'})["status"], "BLOCKED_FINDINGS")
            self.assertEqual(self.check({"unrelated/" + dependency_path: data})["status"], "BLOCKED_FINDINGS")
            self.assertEqual(self.check({"config.py": data})["status"], "BLOCKED_FINDINGS")
            self.assertEqual(self.check({name: data + b'\npassword="opaque-value"'})["status"], "BLOCKED_FINDINGS")
            record["fields"]["api_key"] = []
            self.assertEqual(self.check({name: data})["status"], "BLOCKED_FINDINGS")


import bz2
import gzip
import hashlib
import lzma
import tarfile
from unittest.mock import patch
import verify_package_contents as inspector


class RuntimeArchiveInspectionTests(unittest.TestCase):
    package = PackageInspectionTests.package
    check = PackageInspectionTests.check
    def test_compressed_streams_retain_credential_detection(self):
        raw = b'{"api_key":"fixture-opaque-value"}'
        for suffix, compress in (("gz", gzip.compress), ("bz2", bz2.compress), ("xz", lzma.compress)):
            with self.subTest(suffix=suffix):
                self.assertEqual(self.check({"config.json."+suffix: compress(raw)})["status"], "BLOCKED_FINDINGS")

    def test_tar_members_are_scanned_without_extracting(self):
        content=io.BytesIO()
        with tarfile.open(fileobj=content, mode='w') as tar:
            data=b'{"password":"fixture-opaque-value"}'
            entry=tarfile.TarInfo('config.json'); entry.size=len(data)
            tar.addfile(entry, io.BytesIO(data))
        self.assertEqual(self.check({'data.tar.gz':gzip.compress(content.getvalue())})['status'], 'BLOCKED_FINDINGS')

    def test_compression_bombs_and_unsafe_tar_members_fail_closed(self):
        with patch.object(inspector, 'MAX_FILE', 1024):
            self.assertEqual(self.check({'data.txt.gz':gzip.compress(b'x'*2048)})['status'], 'UNKNOWN')
        content=io.BytesIO()
        with tarfile.open(fileobj=content, mode='w') as tar:
            entry=tarfile.TarInfo('../bad'); entry.size=1; tar.addfile(entry, io.BytesIO(b'x'))
            entry=tarfile.TarInfo('link'); entry.type=tarfile.SYMTYPE; entry.linkname='outside'; tar.addfile(entry)
        self.assertEqual(self.check({'data.tar':content.getvalue()})['status'],'UNKNOWN')

    def test_public_dependency_classification_requires_path_hash_and_literal(self):
        original=b'password="public documentation example"\n'
        member='resources/beauty-runtime/python/Lib/site-packages/example.py'
        entry={'path':'python/Lib/site-packages/example.py', 'sha256':hashlib.sha256(original).hexdigest(),
               'literals':[['password','public documentation example']]}
        with patch.object(inspector, 'PUBLIC_DEPENDENCY_LITERALS', [entry]):
            self.assertEqual(self.check({member:original})['status'],'NOT_DETECTED_WITHIN_SCOPE')
            dependency_path = 'python/Lib/site-packages/example.py'
            self.assertEqual(self.check({dependency_path:original})['status'],'NOT_DETECTED_WITHIN_SCOPE')
            self.assertEqual(self.check({dependency_path:original+b'api_key="fixture-opaque-value"'})['status'],'BLOCKED_FINDINGS')
            self.assertEqual(self.check({'unrelated/'+dependency_path:original})['status'],'BLOCKED_FINDINGS')
            for path, data in ((member, original+b'api_key="fixture-opaque-value"'),
                               ('config.py',original), (member,original.replace(b'example',b'changed'))):
                self.assertEqual(self.check({path:data})['status'],'BLOCKED_FINDINGS')
            self.assertEqual(self.check({member:original,'provider.json':'{"password":"fixture-opaque-value"}'})['status'],'BLOCKED_FINDINGS')

    def test_public_dependency_still_checks_token_bytes(self):
        data=b'password="public documentation example"\n' + b'sk-' + b'A' * 24
        member='resources/beauty-runtime/example.py'
        entry={'path':'example.py','sha256':hashlib.sha256(data).hexdigest(),
               'literals':[['password','public documentation example']]}
        with patch.object(inspector,'PUBLIC_DEPENDENCY_LITERALS',[entry]):
            self.assertEqual(self.check({member:data})['status'],'BLOCKED_FINDINGS')

    def test_tar_hardlink_reads_only_regular_in_archive_target(self):
        content=io.BytesIO()
        with tarfile.open(fileobj=content, mode='w') as tar:
            data=b'{"password":"fixture-opaque-value"}'
            entry=tarfile.TarInfo('config.json'); entry.size=len(data); tar.addfile(entry,io.BytesIO(data))
            entry=tarfile.TarInfo('linked.json'); entry.type=tarfile.LNKTYPE; entry.linkname='config.json'; tar.addfile(entry)
        report=self.check({'data.tar':content.getvalue()})
        self.assertEqual(report['status'],'BLOCKED_FINDINGS')
        self.assertEqual(len(report['findings']),2)


if __name__ == "__main__":
    unittest.main()
