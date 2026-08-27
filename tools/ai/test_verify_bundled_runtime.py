from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

import verify_bundled_runtime as runtime_verifier


class BundledRuntimeVerifierTests(unittest.TestCase):
    def test_path_is_within_accepts_descendant(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            module = root / "Lib" / "site-packages" / "PIL" / "__init__.py"
            module.parent.mkdir(parents=True)
            module.write_text("", encoding="utf-8")
            self.assertTrue(runtime_verifier.path_is_within(module, root))

    def test_path_is_within_rejects_sibling(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            parent = Path(temporary_directory)
            root = parent / "python"
            sibling = parent / "user-site" / "PIL" / "__init__.py"
            root.mkdir()
            sibling.parent.mkdir(parents=True)
            sibling.write_text("", encoding="utf-8")
            self.assertFalse(runtime_verifier.path_is_within(sibling, root))


if __name__ == "__main__":
    unittest.main()
