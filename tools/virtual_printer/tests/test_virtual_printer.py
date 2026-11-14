from __future__ import annotations

import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fastapi.testclient import TestClient
from PIL import Image
from hf_virtual_printer.config import PrinterError, default_config, validate_config
from hf_virtual_printer.gcode import decode, read_metadata
from hf_virtual_printer.session import VirtualPrinterSession
from hf_virtual_printer.server import create_app
from hf_virtual_printer.camera import CameraRenderer


class SimulatorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="HF 虚拟打印机 ")
        self.root = Path(self.temp.name)
        self.session = VirtualPrinterSession(self.root)
        self.client = TestClient(create_app(self.session, engine=False))
        self.client.__enter__()

    def tearDown(self):
        self.client.__exit__(None, None, None)
        self.session.close()
        self.temp.cleanup()

    def upload(self, text=b"G90\nG1 X100 Y100 F600\n", name="test.gcode", **fields):
        return self.client.post("/server/files/upload", data={"root": "gcodes", **fields}, files={"file": (name, text)})

    def run_ticks(self, count=100, seconds=.1):
        for _ in range(count):
            self.session.advance(seconds)

    def test_defaults_and_protocol_identity(self):
        identity = self.client.get("/server/info").json()["result"]
        self.assertEqual(identity["machine_name"], self.session.config["name"])
        self.assertTrue(identity["hf_virtual_printer"])
        self.assertEqual(identity["instance_uuid"], self.session.instance_uuid)
        self.assertEqual(self.session.config["bed_size"], [256, 256, 256])
        self.assertEqual(len(self.session.config["nozzles"]), 1)
        self.assertEqual(len(self.session.config["slots"]), 4)

    def test_templates_and_extreme_valid_hardware_profiles(self):
        templates = self.client.get("/sim/api/v1/templates").json()
        for profile in templates.values():
            self.assertEqual(validate_config(profile), profile)
        config = default_config("multihead")
        config["nozzles"] *= 2
        config["slots"] *= 4
        config["tools"] = [{"nozzle": i % 8, "slot": i % 16} for i in range(32)]
        self.session.configure(config)
        self.assertIn("extruder7", self.session.objects())
        self.assertEqual(len(self.session.lane_data()), 16)
        config = default_config()
        config["slots"] = []
        config["tools"] = [{"nozzle": 0, "slot": None}]
        self.session.configure(config)
        self.assertEqual(self.session.lane_data(), {})

    def test_config_rejects_invalid_mapping_and_nonfinite_values(self):
        for mutate in [lambda c: c.update(name=""), lambda c: c.update(nozzles=[]),
                       lambda c: c["tools"][0].update(nozzle=9), lambda c: c["tools"][0].update(slot=-1),
                       lambda c: c["nozzles"][0].update(diameter=float("nan")),
                       lambda c: c["slots"][0].update(color="#ZZZZZZ"), lambda c: c.update(lan_enabled="true")]:
            with self.subTest(mutate=mutate):
                config = default_config()
                mutate(config)
                with self.assertRaises(PrinterError):
                    validate_config(config)

    def test_malformed_requests_fail_explicitly_without_server_errors(self):
        for value in [{"params": []}, {"filename": 42}, {"params": None}]:
            self.assertEqual(self.client.post("/printer/print/start", json=value).status_code, 400)
        for key in ["schema", "simulation_speed"]:
            config = default_config()
            config[key] = True
            self.assertEqual(self.client.post("/sim/api/v1/config/validate", json=config).status_code, 400)
        self.assertEqual(self.client.post("/sim/api/v1/action", json={"action": "speed", "value": True}).status_code, 400)
        for command in ["G1 Xbad", "M104 Snan", "START_PRINT BED_TEMP=bad"]:
            self.assertEqual(self.client.post("/printer/gcode/script", json={"script": command}).status_code, 400)

    def test_compact_gcode_and_coordinate_offset_preserve_physical_motion(self):
        self.session.script("G1X20Y30E1\nG92 X0 Y0\nG1 X10 Y5 E2")
        self.assertEqual(self.session.position, [30, 35, 0])
        self.assertEqual(self.session.objects()["gcode_move"]["gcode_position"], [10, 5, 0, 2])
        self.assertEqual(self.session.filament_used, 2)
        self.session.script("G28")
        self.assertEqual(self.session.coordinate_offset, [0, 0, 0])

    def test_arc_paths_and_layers_are_kept_without_slicer_comments(self):
        self.upload(b"G1 X30 Y30 Z.2\nG2 X50 Y30 I10 E1 F6000\nG1 Z.4\nG1 X40 E2\n", print="true")
        self.run_ticks(20)
        self.assertEqual(self.session.state, "complete")
        self.assertEqual(self.session.layer, 2)
        self.assertGreater(len(self.session.segments), 30)
        self.assertGreater(max(p[1] for segment in self.session.segments for p in segment[:2]), 39)

    def test_paused_task_rejects_manual_tool_changes_and_motion(self):
        self.upload(print="true")
        self.session.pause()
        for command in ["T1", "ACTIVATE_EXTRUDER EXTRUDER=extruder", "G1 X1", "G92 X0"]:
            self.assertEqual(self.client.post("/printer/gcode/script", json={"script": command}).status_code, 409)

    def test_protocol_websocket_rejects_remote_and_foreign_origins(self):
        from starlette.websockets import WebSocketDisconnect
        with self.assertRaises(WebSocketDisconnect):
            with self.client.websocket_connect("/websocket", headers={"Origin": "https://unrelated.invalid"}):
                pass
        remote = TestClient(create_app(self.session, engine=False), client=("192.0.2.1", 50000))
        with self.assertRaises(WebSocketDisconnect):
            with remote.websocket_connect("/websocket"):
                pass

    def test_camera_retains_old_layers_after_execution_buffer_rolls_over(self):
        camera = CameraRenderer(self.session)
        self.session.script("G1 X100 Y100 E10")
        camera.render()
        first = camera.paths.copy()
        self.session.segments.clear()
        camera.render()
        self.assertEqual(first.tobytes(), camera.paths.tobytes())
        self.session.configure(default_config())
        camera.render()
        self.assertIsNone(camera.paths.getbbox())

    def test_upload_is_atomic_and_upload_only_never_starts_print(self):
        response = self.upload(name="中文 带空格.gcode")
        self.assertEqual(response.status_code, 200)
        self.assertNotIn("result", response.json())
        self.assertFalse(response.json()["print_started"])
        self.assertEqual(self.session.state, "standby")
        name = response.json()["item"]["path"]
        self.assertEqual(self.client.get("/server/files/gcodes/" + name).content, b"G90\nG1 X100 Y100 F600\n")
        self.assertEqual(self.client.get("/server/files/list").json()["result"][0]["path"], name)

    def test_upload_and_print_starts_once_and_busy_still_accepts_upload_only(self):
        self.assertTrue(self.upload(print="true").json()["print_started"])
        self.assertEqual(self.session.state, "printing")
        self.assertEqual(self.client.post("/printer/print/start", json={"filename": "test.gcode"}).status_code, 409)
        self.assertEqual(self.upload(name="next.gcode").status_code, 200)
        self.assertEqual(self.upload().status_code, 409)
        self.assertEqual(self.client.delete("/server/files/gcodes/test.gcode").status_code, 409)

    def test_aborted_upload_preserves_existing_file_and_removes_partial(self):
        self.upload(b"G28\n")

        class BrokenStream:
            count = 0
            def read(self, _):
                self.count += 1
                if self.count == 1:
                    return b"G90\n"
                raise OSError("client disconnected")
        with self.assertRaises(OSError):
            self.session.accept_upload(BrokenStream(), "test.gcode")
        self.assertEqual((self.root / "gcodes/test.gcode").read_bytes(), b"G28\n")
        self.assertEqual(list(self.root.glob("*.part")), [])

    def test_upload_rejects_non_gcode_empty_binary_and_path_traversal(self):
        for content, name in [(b"", "a.gcode"), (b"abc\x00", "a.gcode"), (b"abc", "a.3mf"), (b"G28", "../escape.gcode"), (b"G28", "C:/escape.gcode")]:
            with self.subTest(name=name, content=content):
                self.assertEqual(self.upload(content, name).status_code, 400)
        self.assertFalse((self.root.parent / "escape.gcode").exists())

    def test_storage_and_upload_faults_preserve_saved_files(self):
        self.upload()
        self.session.set_fault("storage_full", True)
        self.assertEqual(self.client.get("/server/files/directory?path=gcodes").json()["result"]["disk_usage"]["free"], 0)
        self.assertEqual(self.upload(name="full.gcode").status_code, 507)
        self.session.recover()
        self.session.set_fault("upload_failed", True)
        self.assertEqual(self.upload(name="fail.gcode").status_code, 503)
        self.assertEqual(len(self.session.file_list()), 1)

    def test_pause_freezes_motion_and_resume_completes_with_camera_progress(self):
        self.upload(print="true")
        self.run_ticks(2)
        camera = CameraRenderer(self.session)
        before = camera.jpeg()
        self.assertEqual(self.client.post("/printer/print/pause").status_code, 200)
        position, progress = self.session.position[:], self.session.progress
        self.run_ticks(10)
        self.assertEqual(self.session.position, position)
        self.assertEqual(self.session.progress, progress)
        self.assertEqual(self.client.post("/printer/print/resume").status_code, 200)
        self.run_ticks(200)
        camera.render()
        self.assertEqual(self.session.state, "complete")
        self.assertEqual(self.session.progress, 1)
        self.assertNotEqual(before, camera.jpeg())
        self.assertEqual(Image.open(io.BytesIO(camera.jpeg())).size, (640, 360))

    def test_motion_respects_relative_coordinates_and_extrusion_modes(self):
        self.upload(b"G90\nM82\nG1 X10 Y10 E2 F6000\nG91\nM83\nG1 X5 Y3 E1\nG90\nG92 E0\nG1 X20 Y20 E4\n", print="true")
        self.run_ticks(20)
        self.assertEqual(self.session.position, [20, 20, 0])
        self.assertAlmostEqual(self.session.filament_used, 7, places=4)
        self.assertLess(self.session.config["slots"][0]["remaining_g"], 1000)

    def test_arc_motion_reaches_endpoint(self):
        self.upload(b"G90\nG1 X20 Y10 F6000\nG3 X10 Y20 I-10 J0 E3\n", print="true")
        self.run_ticks(30)
        self.assertEqual(self.session.state, "complete")
        self.assertEqual(self.session.position, [10, 20, 0])

    def test_head_and_multicolor_tools_are_distinct(self):
        config = default_config("multihead")
        self.session.configure(config)
        self.session.script("T2\nSET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=210")
        self.assertEqual(self.session.active_nozzle, 2)
        self.assertEqual(self.session.active_slot, 2)
        self.assertEqual(self.session.objects()["toolhead"]["extruder"], "extruder2")
        self.assertEqual(self.session.nozzles[2]["target"], 210)
        self.assertEqual(self.session.nozzles[0]["target"], 0)
        self.session.configure(default_config())
        self.session.script("T3")
        self.assertEqual(self.session.active_nozzle, 0)
        self.assertEqual(self.session.active_slot, 3)

    def test_heater_wait_advances_temperature_before_motion(self):
        self.upload(b"M109 S200\nG1 X50 F6000\n", print="true")
        self.run_ticks(3)
        self.assertEqual(self.session.position[0], 0)
        self.assertGreater(self.session.nozzles[0]["temperature"], 25)
        self.run_ticks(100)
        self.assertEqual(self.session.state, "complete")
        self.assertEqual(self.session.position[0], 50)
        self.assertEqual(self.session.nozzles[0]["target"], 0)

    def test_manual_controls_validate_ranges_and_update_status(self):
        for command in ["M104 S999", "G1 X999", "T99", "UNKNOWN_HEATER FOO=1", "M220 S0", "G1 X1 F0"]:
            with self.subTest(command=command):
                self.assertEqual(self.client.post("/printer/gcode/script", json={"script": command}).status_code, 400)
        self.session.script("G28\nG91\nG1 X10 F600\nG90\nM106 S128\nM140 S60")
        self.assertEqual(self.session.position, [10, 0, 0])
        self.assertEqual(self.session.homed, "XYZ")
        self.assertAlmostEqual(self.session.fan, 128 / 255)
        self.assertEqual(self.session.bed["target"], 60)
        self.assertFalse(self.session.mesh_calibrated)
        self.session.script("BED_MESH_CALIBRATE")
        self.run_ticks(4)
        self.assertTrue(self.session.mesh_calibrated)

    def test_manual_motion_rejected_during_print_and_config_locked_when_paused(self):
        self.upload(print="true")
        self.assertEqual(self.client.post("/printer/gcode/script", json={"script": "G1 X1"}).status_code, 409)
        self.session.pause()
        self.assertEqual(self.client.put("/sim/api/v1/config", json=default_config()).status_code, 409)
        self.session.cancel()
        self.assertEqual(self.client.put("/sim/api/v1/config", json=default_config()).status_code, 200)

    def test_unknown_file_macros_warn_once_but_invalid_motion_fails(self):
        self.upload(b"VENDOR_MACRO\nVENDOR_MACRO\nG1 X999\n", print="true")
        self.run_ticks(3)
        self.assertEqual(self.session.state, "error")
        self.assertIn("超出", self.session.message)
        self.assertEqual(sum(event["category"] == "unsupported" for event in self.session.events), 1)

    def test_missing_file_and_invalid_phase_actions_are_explicit(self):
        self.assertEqual(self.client.post("/printer/print/start", json={"filename": "missing.gcode"}).status_code, 404)
        for action in ["pause", "resume", "cancel"]:
            self.assertEqual(self.client.post("/printer/print/" + action).status_code, 409)
        self.assertEqual(self.client.get("/not-a-real-api").status_code, 404)

    def test_runout_and_overtemperature_have_recoverable_states(self):
        self.upload(print="true")
        self.session.set_fault("runout", True)
        self.assertEqual(self.session.state, "paused")
        self.assertEqual(self.client.post("/printer/print/resume").status_code, 409)
        self.session.recover()
        self.session.resume()
        self.session.set_fault("overtemperature", True)
        self.assertEqual(self.session.state, "error")
        self.assertEqual(self.session.nozzles[0]["target"], 0)
        self.session.recover()
        self.assertEqual(self.session.state, "standby")
        self.session.start("test.gcode")

    def test_actual_empty_slot_pauses_and_requires_refill(self):
        self.session.config["slots"][0]["remaining_g"] = .00001
        self.upload(b"G1 X100 E100 F600\n", print="true")
        self.run_ticks(3)
        self.assertEqual(self.session.state, "paused")
        self.session.recover()
        with self.assertRaises(PrinterError):
            self.session.resume()
        self.session.refill(0, 1000)
        self.session.resume()
        self.assertEqual(self.session.state, "printing")

    def test_offline_and_camera_fault_do_not_disable_console(self):
        self.session.set_fault("offline", True)
        self.assertEqual(self.client.get("/server/info").status_code, 503)
        self.assertEqual(self.client.get("/sim/api/v1/state").status_code, 200)
        self.assertEqual(self.client.post("/sim/api/v1/action", json={"action": "recover"}).status_code, 200)
        self.session.set_fault("camera_offline", True)
        self.assertEqual(self.client.get("/webcam/?action=snapshot").status_code, 503)
        self.assertEqual(self.client.get("/server/info").status_code, 200)

    def test_webcam_urls_preserve_port_and_snapshot_is_valid_jpeg(self):
        client = TestClient(create_app(self.session, engine=False), base_url="http://127.0.0.1:7128")
        urls = client.get("/server/webcams/list").json()["result"]["webcams"][0]
        self.assertEqual(urls["snapshot_url"], "http://127.0.0.1:7128/webcam/?action=snapshot")
        response = client.get("/webcam/?action=snapshot")
        self.assertEqual(response.headers["content-type"], "image/jpeg")
        self.assertEqual(Image.open(io.BytesIO(response.content)).size, (640, 360))

    def test_websocket_identification_subscription_query_and_unknown_method(self):
        with self.client.websocket_connect("/websocket") as ws:
            ws.send_json({"jsonrpc": "2.0", "id": 0, "method": "server.connection.identify", "params": {"client_name": "OrcaSlicer"}})
            self.assertIn("connection_id", ws.receive_json()["result"])
            ws.send_json({"jsonrpc": "2.0", "id": 1, "method": "printer.objects.subscribe", "params": {"objects": {"print_stats": None, "extruder": ["temperature"]}}})
            response = ws.receive_json()
            self.assertEqual(response["id"], 1)
            self.assertEqual(set(response["result"]["status"]["extruder"]), {"temperature"})
            ws.send_json({"jsonrpc": "2.0", "id": 2, "method": "not.available"})
            self.assertEqual(ws.receive_json()["error"]["code"], -32601)

    def test_profile_import_does_not_replace_installation_identity(self):
        old_id = self.session.instance_uuid
        config = self.client.get("/sim/api/v1/config").json()
        self.assertNotIn("instance_uuid", config)
        config["name"] = "同事的设备"
        self.assertEqual(self.client.put("/sim/api/v1/config", json=config).status_code, 200)
        self.assertEqual(self.session.instance_uuid, old_id)
        other = VirtualPrinterSession(self.root / "other")
        self.assertNotEqual(other.instance_uuid, old_id)
        other.close()

    def test_restart_preserves_files_and_marks_active_job_interrupted(self):
        self.upload(print="true")
        original_id = self.session.instance_uuid
        self.session.close()
        restarted = VirtualPrinterSession(self.root)
        self.assertEqual(restarted.instance_uuid, original_id)
        self.assertEqual(restarted.state, "standby")
        self.assertEqual(restarted.history[-1]["state"], "interrupted")
        self.assertEqual(len(restarted.file_list()), 1)
        restarted.close()

    def test_header_metadata_drives_layer_and_time_consumers(self):
        self.upload(b"; estimated printing time (normal mode) = 1h 2m 3s\n; total layer number: 12\nG28\n")
        meta = self.client.get("/server/files/metadata?filename=test.gcode").json()["result"]
        self.assertEqual(meta["estimated_time"], 3723)
        self.assertEqual(meta["layer_count"], 12)

    def test_camera_and_path_history_are_bounded(self):
        for i in range(20000):
            self.session.segments.append(([0, 0, i / 100], [20, 20, i / 100], "#FF7748"))
        self.assertEqual(len(self.session.segments), 18000)
        CameraRenderer(self.session).render()
        for i in range(500):
            self.session.log("test", str(i))
        self.assertEqual(len(self.session.events), 160)

    def test_cross_origin_management_requests_are_rejected(self):
        response = self.client.post("/sim/api/v1/action", json={"action": "recover"}, headers={"Origin": "https://unrelated.invalid"})
        self.assertEqual(response.status_code, 403)

    def test_upload_size_limit_does_not_need_a_gigabyte_allocation(self):
        import hf_virtual_printer.session as session_module
        original = session_module.MAX_FILE
        try:
            session_module.MAX_FILE = 20
            self.assertEqual(self.upload(b"G1 X1\n" * 10).status_code, 413)
        finally:
            session_module.MAX_FILE = original
        self.assertEqual(self.session.file_list(), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
