from __future__ import annotations

import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hf_virtual_printer.config import default_config
from hf_virtual_printer.gcode import read_metadata
from hf_virtual_printer.session import VirtualPrinterSession


class LayerMarkerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hf-layer-markers-')
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def metadata(self, raw):
        path = self.root / 'layers.gcode'
        path.write_bytes(raw)
        return read_metadata(path)

    def test_supported_complete_layer_count_headers(self):
        for header in ['; total layer number: 50', '; total_layer_count = 50',
                       ';LAYER_COUNT:50', '  ; lAyEr_CoUnT = 50  ']:
            with self.subTest(header=header):
                self.assertEqual(self.metadata((header + '\nG28\n').encode())['layer_count'], 50)

    def test_config_footer_cannot_override_layer_count(self):
        raw = (b'; total layer number: 50\nG28\n'
               b'; interlocking_beam_layer_count = 2\n'
               b'; total_layer_count_backup = 3\n'
               b'; custom text LAYER_COUNT: 4\n')
        self.assertEqual(self.metadata(raw)['layer_count'], 50)
        self.assertIsNone(self.metadata(raw.split(b'\n', 1)[1])['layer_count'])

    def test_layer_count_requires_complete_integer_value(self):
        for header in [b';LAYER_COUNT:50extra\n', b';LAYER_COUNT:50.5\n',
                       b'; total layer number: -2\n']:
            with self.subTest(header=header):
                self.assertIsNone(self.metadata(header)['layer_count'])

    def test_metadata_read_remains_bounded_to_256_kib(self):
        raw = b';LAYER_COUNT:50\n' + b'; padding\n' * 30000 + b';LAYER_COUNT:2\n'
        path = self.root / 'large.gcode'
        path.write_bytes(raw)
        reads = []

        class HeaderStream(io.BytesIO):
            def read(self, size=-1):
                reads.append(size)
                return super().read(size)

        with patch.object(Path, 'open', return_value=HeaderStream(raw)):
            self.assertEqual(read_metadata(path)['layer_count'], 50)
        self.assertEqual(reads, [256 * 1024])
        self.assertIsNone(self.metadata(b'; padding\n' * 30000 + b';LAYER_COUNT:2\n')['layer_count'])

    def execute(self, raw, directory):
        s = VirtualPrinterSession(directory)
        self.addCleanup(s.close)
        s.configure(default_config('apex'))
        s.accept_upload(io.BytesIO(raw), 'layers.gcode')
        s.start('layers.gcode', 'layer-marker-case')
        for _ in range(100):
            s.advance(1)
            if s.state != 'printing':
                break
        self.assertEqual(s.state, 'complete')
        self.assertEqual(s.file_position, len(raw))
        return s

    def test_supported_events_count_layers_but_config_names_do_not(self):
        variants = [(';LAYER_CHANGE', '; layer_change'),
                    ('; layer num/total_layer_count: 1/2', '; layer num/total_layer_count: 2/2'),
                    (';LAYER:0', '; LAYER: 1')]
        for index, markers in enumerate(variants):
            with self.subTest(markers=markers):
                raw = (';LAYER_COUNT:2\nG90\nM83\nT4\n' + markers[0] +
                       '\nG1 X10 Y10 Z0.2 E1 F600\n' + markers[1] +
                       '\nG1 X20 Y10 Z0.4 E1 F600\n'
                       '; layer_change_gcode =\n;LAYER_CHANGE_CUSTOM\n'
                       '; layer num/total_layer_count_backup: 9/9\n'
                       ';LAYER:99extra\n').encode()
                s = self.execute(raw, self.root / str(index))
                self.assertEqual((s.metadata['layer_count'], s.layer), (2, 2))
                self.assertEqual(s.history[-1]['layer'], 2)
                self.assertEqual(s.tools_used, {4})
                self.assertEqual(s.tool_extrusion[4], 2)

    def test_config_names_alone_do_not_create_explicit_layers(self):
        raw = (b'; layer_change_gcode =\n;LAYER_CHANGE_CUSTOM\n'
               b'; layer num/total_layer_count_backup: 9/9\n'
               b';LAYER:1extra\nG28\n')
        s = self.execute(raw, self.root / 'config-only')
        self.assertEqual(s.layer, 0)
        self.assertFalse(s._explicit_layers)


if __name__ == '__main__':
    unittest.main()
