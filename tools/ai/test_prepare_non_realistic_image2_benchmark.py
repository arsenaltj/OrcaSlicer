import unittest

from tools.ai import prepare_non_realistic_image2_benchmark as prepare


class PrepareNonRealisticImage2BenchmarkTests(unittest.TestCase):
    def test_build_manifest_excludes_realistic_and_keeps_requested_order(self):
        source = {
            "palettes": {"warm": ["#C95B43", "#253B5E", "#F2E5C4", "#D6A72C"]},
            "cases": [
                {"id": "second", "source": "b.png", "instruction": "b"},
                {"id": "first", "source": "a.png", "instruction": "a"},
            ],
        }

        manifest = prepare.build_manifest(source, ("first", "second"))

        self.assertEqual([case["id"] for case in manifest["cases"][:2]], ["first", "second"])
        self.assertNotIn("realistic", manifest["style_runs"])
        self.assertEqual(
            set(manifest["style_runs"]),
            {"sculpture", "cartoon", "low_poly", "relief", "diorama"},
        )
        self.assertTrue(all(case.get("input_mode") == "text" for case in manifest["cases"][2:]))

    def test_build_manifest_rejects_missing_and_duplicate_cases(self):
        source = {"palettes": {"warm": ["#000000"]}, "cases": [{"id": "one"}]}
        with self.assertRaisesRegex(prepare.NightlyManifestError, "Missing"):
            prepare.build_manifest(source, ("missing",))
        with self.assertRaisesRegex(prepare.NightlyManifestError, "unique"):
            prepare.build_manifest(source, ("one", "one"))


if __name__ == "__main__":
    unittest.main()
