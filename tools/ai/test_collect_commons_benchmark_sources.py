import unittest
import tempfile
from pathlib import Path
from unittest import mock

from tools.ai import collect_commons_benchmark_sources as collector


class CommonsBenchmarkSourceTests(unittest.TestCase):
    def test_atomic_json_retries_a_transient_windows_file_lock(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / "manifest.json"
            original_replace = collector.os.replace
            attempts = 0

            def transient_lock(source, destination):
                nonlocal attempts
                attempts += 1
                if attempts == 1:
                    raise PermissionError(5, "temporarily locked")
                original_replace(source, destination)

            with mock.patch.object(collector.os, "replace", side_effect=transient_lock), \
                    mock.patch.object(collector.time, "sleep"):
                collector._atomic_json(target, {"count": 100})

            self.assertEqual(attempts, 2)
            self.assertEqual(collector._read_json(target), {"count": 100})
            self.assertFalse(target.with_name("manifest.json.part").exists())

    def test_frozen_plan_contains_one_hundred_unique_cases(self):
        plan = collector._read_json(
            collector.REPOSITORY_ROOT / "Docs" / "benchmarks" / "image2-community-500-source-plan.json"
        )
        cases = collector.flatten_plan(plan)

        self.assertEqual(len(cases), 100)
        self.assertEqual(len({case.case_id for case in cases}), 100)
        self.assertTrue(all(len(case.queries) >= 2 for case in cases))
        self.assertFalse(any("child" in query.lower() for case in cases for query in case.queries))

    def test_license_allowlist_rejects_noncommercial_and_unknown_media(self):
        self.assertTrue(collector.allowed_license("Public domain", ""))
        self.assertTrue(collector.allowed_license("CC0", ""))
        self.assertTrue(collector.allowed_license("CC BY-SA 4.0", "https://creativecommons.org/licenses/by-sa/4.0/"))
        self.assertFalse(collector.allowed_license("CC BY-NC 4.0", "https://creativecommons.org/licenses/by-nc/4.0/"))
        self.assertFalse(collector.allowed_license("CC BY-ND 4.0", "https://creativecommons.org/licenses/by-nd/4.0/"))
        self.assertFalse(collector.allowed_license("Copyrighted", ""))
        self.assertFalse(collector.allowed_license("", ""))

    def test_candidate_filter_uses_bitmap_dimensions_and_license(self):
        page = {
            "pageid": 12,
            "title": "File:Golden Retriever standing.jpg",
            "imageinfo": [{
                "mime": "image/jpeg",
                "mediatype": "BITMAP",
                "width": 2400,
                "height": 1800,
                "url": "https://upload.wikimedia.org/source.jpg",
                "thumburl": "https://upload.wikimedia.org/thumb.jpg",
                "descriptionurl": "https://commons.wikimedia.org/wiki/File:Golden_Retriever_standing.jpg",
                "sha1": "abc123",
                "extmetadata": {
                    "LicenseShortName": {"value": "CC BY-SA 4.0"},
                    "LicenseUrl": {"value": "https://creativecommons.org/licenses/by-sa/4.0/"},
                    "Artist": {"value": "<b>Example Author</b>"},
                },
            }],
        }

        candidate = collector._candidate_record(page, "Golden Retriever standing photograph", 0)

        self.assertIsNotNone(candidate)
        self.assertEqual(candidate["artist"], "Example Author")
        self.assertEqual(candidate["commons_sha1"], "abc123")

        page["imageinfo"][0]["extmetadata"]["LicenseUrl"]["value"] = \
            "http://creativecommons.org/licenses/by-sa/4.0/"
        candidate = collector._candidate_record(page, "Golden Retriever standing photograph", 0)
        self.assertEqual(candidate["license_url"], "https://creativecommons.org/licenses/by-sa/4.0/")

        page["imageinfo"][0]["extmetadata"]["LicenseShortName"]["value"] = "CC BY-NC 4.0"
        self.assertIsNone(collector._candidate_record(page, "Golden Retriever", 0))

        page["imageinfo"][0]["extmetadata"]["LicenseShortName"]["value"] = "Public domain"
        page["title"] = "File:Unrelated historical family.jpg"
        self.assertIsNone(collector._candidate_record(page, "tabby cat standing photograph", 0))
        page["title"] = "File:Abraham Lincoln portrait without beard.jpg"
        self.assertIsNone(collector._candidate_record(page, "Abraham Lincoln beard portrait", 0))

    def test_benchmark_manifest_expands_to_exactly_five_outputs_per_source(self):
        records = [{
            "id": "sample",
            "source": "generated_models/sample.jpg",
            "instruction": "preserve subject",
            "label": "sample",
            "category": "product",
            "challenges": ["thin_parts"],
            "preserve": ["silhouette"],
            "community_use": "test",
            "source_page": "https://commons.wikimedia.org/wiki/File:Sample.jpg",
            "license": "CC0",
            "license_url": "http://creativecommons.org/publicdomain/zero/1.0/",
            "artist": "Example",
        }]

        manifest = collector.build_benchmark_manifest(records)
        runs = manifest["style_runs"]
        count = sum(
            entry["repetitions"]
            for entries in runs.values()
            for entry in entries
        )

        self.assertEqual(count, 5)
        self.assertEqual(set(runs), {"sculpture", "realistic", "cartoon"})
        self.assertEqual(
            manifest["cases"][0]["license_url"],
            "https://creativecommons.org/publicdomain/zero/1.0/",
        )


if __name__ == "__main__":
    unittest.main()
