from contextlib import redirect_stdout
import io
from pathlib import Path
import runpy
import unittest

from review_quality_probe_20261113 import progress_percent


class ProgressPercentTests(unittest.TestCase):
    def test_zero_total_reports_zero(self):
        for completed in (0, 1):
            with self.subTest(completed=completed):
                self.assertEqual(progress_percent(completed, 0), 0)

    def test_nonzero_total_preserves_integer_percentage(self):
        for completed, total, expected in ((0, 5, 0), (1, 3, 33), (5, 5, 100)):
            with self.subTest(completed=completed, total=total):
                self.assertEqual(progress_percent(completed, total), expected)

    def test_main_reports_zero_without_error(self):
        output = io.StringIO()
        script_path = Path(__file__).with_name('review_quality_probe_20261113.py')
        with redirect_stdout(output):
            runpy.run_path(str(script_path), run_name='__main__')
        self.assertEqual(output.getvalue(), '0\n')


if __name__ == '__main__':
    unittest.main()
