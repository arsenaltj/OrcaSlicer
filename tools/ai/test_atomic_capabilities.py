from __future__ import annotations

from dataclasses import fields
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

from PIL import Image, ImageDraw

from capability_catalog import CAPABILITIES, RECIPES, build_catalog
from design_workflow import DesignWorkflow, DesignWorkflowPorts
from model_contracts import Job, RequestError
from model_job_application import ModelJobApplication, ModelJobApplicationPorts
from model_job_repository import _persist_job
from model_provider_gateway import PaidTaskAuthorization
from model_request import _validate_image_file


class AtomicCapabilityTests(unittest.TestCase):
    def test_discovery_preserves_unknown_native_availability_and_is_detached(self):
        catalog = build_catalog({"image": False, "geometry": False})
        items = {item["id"]: item for item in catalog["capabilities"]}
        self.assertTrue(items["reference.policy"]["available"])
        self.assertFalse(items["model.generate"]["available"])
        self.assertIsNone(items["slice.apply"]["available"])
        self.assertEqual(items["color.workbench"]["isolation"], "host_bound")
        self.assertEqual(len(items), len(CAPABILITIES))
        catalog["recipes"][0]["steps"].clear()
        self.assertTrue(RECIPES[0]["steps"])

    def test_recipes_preserve_user_checkpoints(self):
        for recipe in RECIPES:
            self.assertFalse(recipe["automatic_execution"])
            self.assertTrue(recipe["checkpoints"])
            self.assertTrue(set(recipe["steps"]) <= {item.id for item in CAPABILITIES})
        for item in CAPABILITIES:
            if "paid_3d" in item.effects or "change_project" in item.effects:
                self.assertTrue(item.confirmation, item.id)

    def test_application_import_has_no_server_or_executor_side_effect(self):
        code = '''import socket, sys
def denied(*args, **kwargs): raise AssertionError("No provider or server may be contacted")
socket.socket.connect = denied
sys.path.insert(0, sys.argv[1])
from model_job_application import ModelJobApplication
from design_workflow import DesignWorkflow
from model_generation_workflow import ModelGenerationWorkflow
assert "orca_ai_sidecar" not in sys.modules
assert "http.server" not in sys.modules
print("independent")
'''
        result = subprocess.run([sys.executable, "-I", "-B", "-c", code, str(Path(__file__).parent)],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("independent", result.stdout)

    def test_text_design_can_be_composed_without_http_and_waits_for_model_confirmation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jobs = {}
            lock = threading.RLock()
            events = []
            queued = []

            def forbidden(*args, **kwargs):
                raise AssertionError("Unexpected capability invocation")

            def image(prompt, output, *args, **kwargs):
                events.append("image")
                canvas = Image.new("RGB", (384, 384), "white")
                draw = ImageDraw.Draw(canvas)
                draw.rectangle((60, 60, 325, 325), fill="#287842")
                draw.ellipse((100, 100, 225, 225), fill="#e69b23")
                canvas.save(output)

            def fail(job, message):
                job.state = "failed"
                job.message = str(message)

            design_values = {field.name: forbidden for field in fields(DesignWorkflowPorts)}
            design_values.update(lock=lock, persist=_persist_job, stop_boundary=lambda job: None,
                                 fail=fail, fail_preprocess=fail, mark_stopped=lambda job: None,
                                 finish_deleted=lambda job: None, assess_reference=lambda job: None,
                                 generate_image=image, image_provider_status=lambda: {"available": True},
                                 fallback_enabled=lambda: False, validate_image=_validate_image_file)
            design = DesignWorkflow(DesignWorkflowPorts(**design_values))

            def new_job(source, palette=(), roles=None, style="sculpture", custom_style="", print_settings=None, **kwargs):
                output = root / str(len(jobs) + 1)
                output.mkdir()
                return Job(str(len(jobs) + 1), source, output, style=style, custom_style=custom_style,
                           provider=kwargs.get("provider", "tripo"))

            def model_worker(*args):
                raise AssertionError("Creating a design must never execute model generation")

            def submit(job, worker, *args):
                if worker == design.preprocess_text:
                    worker(job, *args)
                else:
                    queued.append((worker, args))

            values = {field.name: forbidden for field in fields(ModelJobApplicationPorts)}
            values.update(jobs=jobs, lock=lock, new_job=new_job, persist=_persist_job,
                          get_job=jobs.get, submit=submit, preprocess_text=design.preprocess_text,
                          present=lambda job: {"id": job.id, "state": job.state},
                          image_provider_status=lambda: {"available": True}, fallback_enabled=lambda: False,
                          gateway=lambda provider: mock.Mock(model_generation_available=lambda: True),
                          can_retry_hunyuan=lambda job: False, can_retry_unsubmitted=lambda job: False,
                          latest_task_id=lambda job: "", assess_reference=lambda job: None,
                          clear_artifact=lambda job: setattr(job, "artifact_path", None),
                          validate_image=_validate_image_file, generate=model_worker)
            application = ModelJobApplication(ModelJobApplicationPorts(**values))
            result = application.create_text_job({"request_id": "offline", "prompt": "a green vase", "style": "realistic"})
            job = jobs[result.payload["job"]["id"]]
            self.assertEqual(job.state, "awaiting_confirmation", job.message)
            self.assertEqual(events, ["image"])
            self.assertEqual(queued, [])
            self.assertTrue(job.raw_preview_path.is_file())
            confirmed = application.execute_job("generate", job.id, {"prepared_prompt": job.prepared_prompt})
            self.assertEqual(confirmed.payload["job"]["state"], "queued")
            self.assertEqual(len(queued), 1)
            self.assertIs(queued[0][0], model_worker)
            self.assertIsInstance(queued[0][1][-1], PaidTaskAuthorization)
            self.assertFalse(queued[0][1][-1].consumed)
            with self.assertRaises(RequestError):
                application.execute_job("generate", job.id, {"prepared_prompt": job.prepared_prompt})
            self.assertEqual(len(queued), 1)

    def test_delete_defers_running_cleanup_and_removes_completed_state_without_provider_calls(self):
        def denied(*args, **kwargs):
            raise AssertionError("Deletion must not contact a provider or queue work")
        jobs = {}
        cleanup = mock.Mock()
        values = {field.name: denied for field in fields(ModelJobApplicationPorts)}
        values.update(jobs=jobs, lock=threading.RLock(), remove_state=cleanup)
        application = ModelJobApplication(ModelJobApplicationPorts(**values))
        running = Job("running", "text", Path("unused"), state="running")
        completed = Job("completed", "text", Path("unused"), state="completed")
        jobs.update(running=running, completed=completed)
        self.assertEqual(application.delete(running.id).status, 204)
        self.assertIs(jobs[running.id], running)
        self.assertTrue(running.delete_requested)
        self.assertTrue(running.stop_event.is_set())
        self.assertEqual(running.state, "stopping")
        cleanup.assert_not_called()
        self.assertEqual(application.delete(completed.id).status, 204)
        self.assertNotIn(completed.id, jobs)
        cleanup.assert_called_once_with(completed)
        with self.assertRaises(RequestError) as error:
            application.delete("missing")
        self.assertEqual(error.exception.code, "job_not_found")
        cleanup.assert_called_once_with(completed)

    def test_unknown_command_cannot_execute_an_entrypoint(self):
        def denied(*args, **kwargs):
            raise AssertionError("Command lookup must not reach a capability")
        values = {field.name: denied for field in fields(ModelJobApplicationPorts)}
        application = ModelJobApplication(ModelJobApplicationPorts(**values))
        with self.assertRaises(RequestError) as error:
            application.execute_job("model_mesh_repair._repair_small_obj_topology_defects", "id", {})
        self.assertEqual(error.exception.code, "not_found")


if __name__ == "__main__":
    unittest.main()
