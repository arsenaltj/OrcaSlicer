from __future__ import annotations

import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hf_virtual_printer.config import PrinterError, default_config
from hf_virtual_printer.session import VirtualPrinterSession


class ApexTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hf-apex-")
        self.session = VirtualPrinterSession(Path(self.temp.name))
        self.session.configure(default_config("apex"))

    def tearDown(self):
        self.session.close()
        self.temp.cleanup()

    def test_saved_capability_does_not_copy_identity(self):
        s = self.session
        cap = s.capability_config()
        self.assertEqual((cap["model"], cap["extruderCount"], cap["physicalSlotCount"]), ("Honor Forge", 6, 6))
        self.assertEqual(cap["printableVolume"], [300, 270, 290])
        self.assertEqual(cap["toolMappings"], [{"tool": i, "nozzle": i, "slot": i} for i in range(6)])
        self.assertFalse(cap["actualFilamentLoadingVerified"])
        self.assertTrue(cap["simulation"])
        identity = s.instance_uuid
        s.close()
        self.session = VirtualPrinterSession(Path(self.temp.name))
        self.assertEqual(self.session.capability_config(), cap)
        self.assertEqual(self.session.instance_uuid, identity)
        self.assertEqual(self.session.lane_data()["lane5"]["tools"], [5])
        self.assertEqual(self.session.objects()["configfile"]["config"], cap)

    def test_macro_new_legacy_and_invalid_are_atomic(self):
        s = self.session
        s.script("START_PRINT EXTRUDER=220 BED=35 CHAMBER=0 INITIAL_TOOL=4 MESH_MIN_X=20 PROBE_COUNT_X=3")
        self.assertEqual(s.active_tool, 4)
        self.assertEqual((s.nozzles[4]["target"], s.bed["target"]), (220, 35))
        before = s.snapshot()
        for macro in ["EXTRUDER=999 BED=50", "INITIAL_TOOL=6", "INITIAL_TOOL=1.5", "BED=nan",
                      "BED=40 BED=50", "BED=50 BED_TEMP=60", "CHAMBER=121"]:
            with self.subTest(macro=macro), self.assertRaises(PrinterError):
                s.script("START_PRINT " + macro)
            self.assertEqual(s.snapshot()["nozzles"], before["nozzles"])
            self.assertEqual(s.bed["target"], 35)
            self.assertEqual(s.active_tool, 4)
        s.script("PRINT_START BED_TEMP=60 EXTRUDER_TEMP=215")
        self.assertEqual((s.nozzles[4]["target"], s.bed["target"]), (215, 60))

    def test_raw_six_tools_heat_wait_sha_consumption_and_completion(self):
        s = self.session
        raw = ("START_PRINT EXTRUDER=220 BED=35 CHAMBER=0 INITIAL_TOOL=4\nG90\nM83\n" +
               "".join(f"T{i}\nG1 X{20+i} Y20 Z0.2 E1 F600\n" for i in range(6))).encode()
        s.accept_upload(io.BytesIO(raw), "six.gcode")
        s.start("six.gcode")
        s.advance(.1)
        self.assertEqual(s.filament_used, 0)  # Macro heaters are still below target.
        for _ in range(1000):
            s.advance(1)
            if s.state != "printing":
                break
        self.assertEqual(s.state, "complete")
        record = s.history[-1]
        self.assertEqual(record["gcode_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(s.file_path("six.gcode").read_bytes(), raw)
        self.assertEqual(record["tools_used"], list(range(6)))
        self.assertTrue(all(v > 0 for v in record["tool_extrusion_mm"]))
        self.assertEqual(record["slot_extrusion_mm"], record["tool_extrusion_mm"])

    def test_command_receipt_replay_never_restarts(self):
        s = self.session
        s.accept_upload(io.BytesIO(b"G28\n"), "id.gcode")
        s.start("id.gcode", "qa-001")
        job = s.job_id
        s.start("id.gcode", "qa-001")
        self.assertEqual(s.objects()["print_stats"]["job_id"], job)
        s.cancel()
        s.start("id.gcode", "qa-001")
        self.assertEqual(s.state, "cancelled")
        s.close()
        self.session = VirtualPrinterSession(Path(self.temp.name))
        self.session.start("id.gcode", "qa-001")
        self.assertEqual(self.session.state, "standby")
        self.session.accept_upload(io.BytesIO(b"G90\n"), "id.gcode")
        with self.assertRaises(PrinterError):
            self.session.start("id.gcode", "qa-001")


if __name__ == "__main__":
    unittest.main()
