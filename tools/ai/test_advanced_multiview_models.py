"""Offline four-view modeling and legacy-consumer regressions; no real suppliers."""
from copy import deepcopy
from io import BytesIO
from pathlib import Path
from types import SimpleNamespace
import tempfile
import threading
import unittest
from unittest import mock

from PIL import Image

from advanced_multiview_application import AdvancedMultiviewApplication
from advanced_multiview_contracts import VIEWS, new_id
from advanced_multiview_design_workflow import AdvancedMultiviewDesignWorkflow, DesignImageResult
from advanced_multiview_models import AdvancedModelPorts, AdvancedMultiviewModels
from advanced_multiview_model_contracts import approved_paths
from advanced_multiview_runtime import AdvancedMultiviewRuntime
from model_contracts import RequestError
from model_job_application import ModelJobApplication
from model_job_repository import _load_job, _persist_job
from model_provider_gateway import ProviderGatewayError


def image():
    output = BytesIO(); Image.new("RGB", (256, 256), "navy").save(output, "PNG")
    return output.getvalue()


class ModelTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        from advanced_multiview_repository import AdvancedMultiviewRepository
        self.repo = AdvancedMultiviewRepository(Path(self.temp.name))
        self.group = self.repo.create(new_id(), {"prompt": "One object"}, {})
        def generate(_request, path):
            path.write_bytes(image()); return DesignImageResult()
        self.design = AdvancedMultiviewDesignWorkflow(self.repo, generate)
        self.images = AdvancedMultiviewApplication(self.repo,
            capabilities=lambda *_: {"initial_text": "supported"},
            submit=lambda work: work(), run_design=self.design.run)
        self.initial = self.images.prepare(self.group["id"], {"command_id": new_id(), "kind": "initial", "expected_group_revision": 0})
        self.images.execute(self.group["id"], {"command_id": self.initial["id"], "plan_id": self.initial["id"],
                            "confirmation_fingerprint": self.initial["confirmation_fingerprint"]})
        self.group = self.repo.load(self.group["id"])
        self.jobs, self.queue, self.requests = {}, [], []
        def start(request, **kwargs):
            self.requests.append(request)
            if not kwargs.get("existing_task_id"):
                kwargs["authorization"].consume("tripo", "model_generation")
            return SimpleNamespace(task_id=kwargs.get("existing_task_id") or "saved-task", reused=bool(kwargs.get("existing_task_id")))
        self.gateway = SimpleNamespace(start_or_reuse_model_task=mock.Mock(side_effect=start), wait_for_task=mock.Mock())
        def record(job, attempt, **values):
            while len(job.attempts) < attempt: job.attempts.append({})
            job.attempts[attempt - 1].update(values)
        def download(job, *_):
            path = job.directory / "candidate.glb"; path.write_bytes(b"controlled artifact"); return path
        def fail(job, message):
            job.state = job.phase = "failed"; job.message = message; _persist_job(job)
        self.ports = SimpleNamespace(lock=threading.RLock(), persist=_persist_job, record_attempt=record,
            gateway=lambda _: self.gateway, stop_boundary=lambda _: None, progress_callback=lambda *_: None,
            download_artifact=download, validate_topology=lambda *_args, **_kwargs: (1000000, 1, 1),
            analysis_artifact=lambda value: value, promote_artifact=lambda src, dst: dst.write_bytes(src.read_bytes()),
            write_color_intent=lambda *_: None, fail=fail, mark_stopped=mock.Mock(), finish_deleted=mock.Mock(),
            is_shutting_down=lambda: False, ensure_multiview=mock.Mock(), automatic_visual_review=mock.Mock())
        self.models = AdvancedMultiviewModels(self.repo, AdvancedModelPorts(self.jobs, self.ports.lock, self.ports,
            lambda *args: self.queue.append(args), lambda job: {"id": job.id, "state": job.state},
            lambda provider, model: {"named_four_view_model": "supported" if provider == "tripo" else "unknown",
                                     "available": True, "model": "model-a"}))
        self.images.model_busy = self.models.busy

    def plan(self, **fields):
        group = self.repo.load(self.group["id"])
        return self.models.prepare(group["id"], {"command_id": new_id(), "expected_group_revision": group["revision"],
                                    "draft": group["latest_submitted_draft"], **fields})

    def execute(self, plan):
        return self.models.execute(plan["group_id"], {"command_id": plan["id"], "plan_id": plan["id"],
                                  "confirmation_fingerprint": plan["confirmation_fingerprint"]})

    def drain(self):
        while self.queue:
            job, worker, *args = self.queue.pop(0); worker(job, *args)

    def test_confirmed_model_freezes_four_named_inputs_and_submits_one_task(self):
        job = self.execute(self.plan())
        self.assertEqual(set(approved_paths(job.approved_multiview_snapshot, job.directory)), set(VIEWS))
        self.assertEqual(self.requests, [])
        self.drain()
        self.assertEqual(job.state, "ready")
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.requests[0].source, "multiview")
        self.assertEqual(set(self.requests[0].image_paths), set(VIEWS))
        self.ports.ensure_multiview.assert_not_called()
        self.ports.automatic_visual_review.assert_not_called()

    def test_repeated_confirmation_returns_the_same_job_and_never_queues_twice(self):
        plan = self.plan(); first = self.execute(plan); second = self.execute(plan)
        self.assertIs(first, second); self.assertEqual(len(self.queue), 1)
        self.drain(); self.execute(plan)
        self.assertEqual(len(self.requests), 1)

    def test_new_job_version_roundtrips_and_cannot_be_forged_as_single_image(self):
        job = self.execute(self.plan()); restored = _load_job(job.directory)
        self.assertEqual(restored.approved_multiview_snapshot, job.approved_multiview_snapshot)
        import json
        path = job.directory / "job.json"; value = json.loads(path.read_text("utf-8"))
        self.assertEqual(value["version"], 2)
        value["version"] = 1; path.write_text(json.dumps(value), "utf-8")
        self.assertIsNone(_load_job(job.directory))

    def test_changed_or_missing_four_view_files_block_submission(self):
        for view in ("back", "left"):
            with self.subTest(view=view):
                job = self.execute(self.plan()); (job.directory / f"inputs/{view}.png").write_bytes(b"changed")
                self.drain(); self.assertEqual(job.state, "failed")
        self.assertEqual(self.requests, [])

    def test_missing_design_or_unsupported_supplier_does_not_create_a_model_job(self):
        with self.assertRaises(RequestError): self.plan(provider="yoyoclaw")
        group = self.repo.load(self.group["id"]); group["views"]["back"]["current_version_id"] = None; self.repo.save(group)
        with self.assertRaises(RequestError): self.plan()
        self.assertEqual(self.jobs, {})

    def test_pending_design_regeneration_blocks_model_plans(self):
        group = self.repo.load(self.group["id"])
        group["pending_views"] = ["front"]
        group["changed_upload_views"] = ["front"]
        self.repo.save(group)
        with self.assertRaises(RequestError) as error:
            self.plan()
        self.assertEqual(error.exception.code, "design_regeneration_required")
        self.assertEqual(self.jobs, {})

    def test_changed_draft_and_stale_confirmation_do_not_submit(self):
        with self.assertRaises(RequestError): self.plan(draft={"prompt": "different"})
        plan = self.plan(); group = self.repo.load(self.group["id"]); self.repo.save(group)
        with self.assertRaises(RequestError): self.execute(plan)
        self.assertEqual(self.queue, [])

    def test_active_model_blocks_design_commands_and_new_model_plans(self):
        self.execute(self.plan())
        with self.assertRaises(RequestError): self.plan()
        group = self.repo.load(self.group["id"])
        with self.assertRaises(RequestError):
            self.images.prepare(group["id"], {"command_id": new_id(), "kind": "edit", "expected_group_revision": group["revision"]})

    def test_legacy_creation_commands_reject_advanced_source_before_any_mutation(self):
        job = self.execute(self.plan())
        app = ModelJobApplication(SimpleNamespace(get_job=lambda _: job))
        for name in ("generate", "retry_preparation", "confirm_palette", "set_generation_options", "reuse_design", "retexture"):
            with self.subTest(name=name), self.assertRaises(RequestError) as result:
                getattr(app, name)(job.id, {})
            self.assertEqual(result.exception.code, "advanced_plan_required")

    def test_resume_reuses_original_remote_task_and_four_frozen_inputs(self):
        job = self.execute(self.plan()); self.drain(); job.state = "failed"
        self.models.resume(self.group["id"], job.id); self.drain()
        self.assertEqual(job.state, "ready")
        self.assertEqual(len(self.requests), 2)
        self.assertEqual(self.gateway.start_or_reuse_model_task.call_args.kwargs["existing_task_id"], "saved-task")
        self.assertIsNone(self.gateway.start_or_reuse_model_task.call_args.kwargs["authorization"])

    def test_ambiguous_submission_cannot_create_a_replacement_during_resume(self):
        self.gateway.start_or_reuse_model_task.side_effect = ProviderGatewayError("lost response", code="ambiguous", category="provider", ambiguous=True)
        job = self.execute(self.plan()); self.drain()
        with self.assertRaises(RequestError): self.models.resume(self.group["id"], job.id)
        with self.assertRaises(RequestError): self.plan()
        self.assertEqual(self.gateway.start_or_reuse_model_task.call_count, 1)

    def test_job_acceptance_survives_failed_group_link_commit_without_second_submission(self):
        plan = self.plan()
        with mock.patch.object(self.repo, "save", side_effect=RequestError("disk_full", "full", 503)):
            with self.assertRaises(RequestError): self.execute(plan)
        self.assertEqual(self.queue, [])
        job = self.execute(plan)
        self.assertEqual(len(self.repo.load(self.group["id"])["model_links"]), 1)
        self.assertEqual(self.queue, [])
        self.assertEqual(job.id, plan["job_id"])
        self.assertEqual(job.state, "stopped")
        self.assertFalse(self.models.busy(self.group["id"]))
        self.plan()  # A known unsubmitted receipt does not permanently block the group.

    def test_model_reconcile_repairs_link_without_submitting(self):
        plan = self.plan()
        with mock.patch.object(self.repo, "save", side_effect=RequestError("disk_full", "full", 503)):
            with self.assertRaises(RequestError): self.execute(plan)
        response = self.models.reconcile(self.group["id"], plan["job_id"])
        self.assertTrue(response["accepted"])
        self.assertEqual(len(self.repo.load(self.group["id"])["model_links"]), 1)
        self.assertEqual(self.queue, [])
        self.assertEqual(self.requests, [])
        self.assertFalse(self.models.reconcile(self.group["id"], new_id())["accepted"])

    def test_stop_and_failed_resume_queue_keep_original_task(self):
        job = self.execute(self.plan())
        self.models.stop(self.group["id"], job.id)
        self.assertTrue(job.stop_event.is_set())
        self.assertEqual(job.state, "stopping")
        self.queue.clear(); job.state = "failed"; job.attempts = [{"generation_task_id": "original"}]
        with mock.patch.object(self.models, "ports", AdvancedModelPorts(self.jobs, self.ports.lock, self.ports,
                mock.Mock(side_effect=RequestError("unavailable", "busy", 503)), lambda job: {}, lambda *_: {})):
            with self.assertRaises(RequestError): self.models.resume(self.group["id"], job.id)
        self.assertEqual(job.state, "stopped")
        self.assertEqual(job.attempts[0]["generation_task_id"], "original")

    def test_reconciliation_reports_original_receipt_and_never_generates(self):
        runtime = AdvancedMultiviewRuntime.__new__(AdvancedMultiviewRuntime)
        runtime.repository, runtime.workflow, runtime.application = self.repo, self.design, self.images
        result = runtime.reconcile(self.group["id"], self.initial["id"])
        self.assertTrue(result["accepted"])
        self.assertFalse(runtime.reconcile(self.group["id"], new_id())["accepted"])
        self.assertEqual(self.requests, [])

    def test_negative_model_reconcile_rejects_delayed_submission(self):
        plan = self.plan()
        self.assertFalse(self.models.reconcile(self.group["id"], plan["job_id"])["accepted"])
        with self.assertRaises(RequestError) as result: self.execute(plan)
        self.assertEqual(result.exception.code, "submission_not_accepted")
        self.assertEqual(self.queue, [])
        self.assertEqual(self.jobs, {})

    def test_corrupt_original_model_receipt_blocks_reconcile_and_overwrite(self):
        plan = self.plan(); directory = self.repo.root.parent / plan["job_id"]
        directory.mkdir(); receipt = directory / "job.json"; receipt.write_text("corrupt", "utf-8")
        for action in (lambda: self.models.reconcile(self.group["id"], plan["job_id"]), lambda: self.execute(plan)):
            with self.assertRaises(RequestError) as result: action()
            self.assertEqual(result.exception.code, "snapshot_unavailable")
            self.assertEqual(receipt.read_text("utf-8"), "corrupt")
        self.assertEqual(self.queue, [])


if __name__ == "__main__": unittest.main()
