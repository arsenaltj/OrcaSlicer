"""Offline selected-view modeling and legacy-consumer regressions; no real suppliers."""
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
from advanced_multiview_contracts import VIEWS, new_id, fingerprint
from advanced_multiview_design_workflow import AdvancedMultiviewDesignWorkflow, DesignImageResult
from advanced_multiview_models import AdvancedModelPorts, AdvancedMultiviewModels
from advanced_multiview_model_contracts import approved_paths
from advanced_multiview_runtime import AdvancedMultiviewRuntime
from model_contracts import RequestError
from model_job_application import ModelJobApplication
from model_job_repository import _load_job, _persist_job
from model_provider_gateway import ProviderGatewayError


def image(color="navy"):
    output = BytesIO(); Image.new("RGB", (256, 256), color).save(output, "PNG")
    return output.getvalue()


class ModelTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        from advanced_multiview_repository import AdvancedMultiviewRepository
        self.repo = AdvancedMultiviewRepository(Path(self.temp.name))
        self.group = self.repo.create(new_id(), {"prompt": "One object"}, {})
        def generate(_request, path):
            path.write_bytes(image({"front":"red","left":"green","back":"blue","right":"purple"}[_request.view])); return DesignImageResult()
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

    def selected_plan(self, selected, **fields):
        group = self.repo.snapshot(self.group["id"])
        fields.setdefault("geometry_quality", "standard")
        return self.plan(selected_views=selected, expected_input_epoch=group["input_epoch"],
                         visible_versions={view: {"version_id": group["views"][view]["current"]["id"],
                                                 "sha256": group["views"][view]["current"]["sha256"]}
                                           for view in selected if view in VIEWS and group["views"][view]["current"]}, **fields)

    def test_both_providers_freeze_and_route_all_eight_front_inclusive_subsets(self):
        from dataclasses import replace
        for provider in ("tripo", "yoyoclaw"):
            self.models.ports = replace(self.models.ports, capability=lambda _provider, model:
                {"available": True, "supported_image_counts": [1,2,3,4], "model": model or "model-a"})
            def start(request, **kwargs):
                self.requests.append(request)
                kwargs["authorization"].consume(provider,"model_generation")
                return SimpleNamespace(task_id=f"{provider}-task-{len(self.requests)}",reused=False)
            self.gateway.start_or_reuse_model_task.side_effect = start
            for mask in range(8):
                selected = ["front"] + [view for slot,view in enumerate(VIEWS[1:]) if mask & (1 << slot)]
                with self.subTest(provider=provider, selected=selected):
                    plan = self.selected_plan(selected,provider=provider,geometry_quality="standard")
                    snapshot = plan["approved_multiview_snapshot"]
                    self.assertEqual(snapshot["selected_views"],selected)
                    self.assertEqual(snapshot["input_mode"],"image" if mask == 0 else "multiview")
                    self.assertEqual(plan["max_new_image_calls"],0)
                    before = len(self.requests)
                    job = self.execute(plan)
                    paths = approved_paths(snapshot,job.directory)
                    self.assertEqual(list(paths),selected)
                    self.assertEqual({p.name for p in (job.directory/"inputs").iterdir()}, {f"{v}.png" for v in selected})
                    current = self.repo.snapshot(self.group["id"])
                    for view,path in paths.items():
                        original = self.repo.path(self.group["id"],current["views"][view]["current"]["path"])
                        self.assertEqual(path.read_bytes(),original.read_bytes())
                    self.drain()
                    self.assertEqual(job.state,"ready")
                    self.assertEqual(len(self.requests),before+1)
                    request = self.requests[-1]
                    self.assertEqual(request.source,snapshot["input_mode"])
                    if mask == 0:
                        self.assertEqual(request.image_path,paths["front"])
                        self.assertFalse(request.image_paths)
                    else:
                        self.assertEqual(request.image_paths,paths)
                        self.assertIsNone(request.image_path)
                    self.assertEqual(_load_job(job.directory).approved_multiview_snapshot,snapshot)
                    self.execute(plan); self.drain()
                    self.assertEqual(len(self.requests),before+1)
                    self.ports.ensure_multiview.assert_not_called()

    def test_selected_plans_reject_illegal_sets_and_inexact_visible_versions_before_submission(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports,capability=lambda *_:
            {"available":True,"supported_image_counts":[1,2,3,4],"model":"model-a"})
        for provider in ("tripo", "yoyoclaw"):
            for selected in ([],["left"],["front","front"],["front","top"],["front",*VIEWS]):
                with self.subTest(provider=provider,selected=selected), self.assertRaises(RequestError):
                    self.selected_plan(selected,provider=provider)
            valid = self.selected_plan(["front","right"],provider=provider)
            snapshot = valid["approved_multiview_snapshot"]
            visible = {view:{"version_id":record["version_id"],"sha256":record["sha256"]}
                       for view,record in snapshot["views"].items()}
            wrong_hash = deepcopy(visible); wrong_hash["right"]["sha256"] = "a"*64
            wrong_version = deepcopy(visible); wrong_version["front"]["version_id"] = new_id()
            for field,value in (("expected_input_epoch",new_id()),("visible_versions",{}),
                                ("visible_versions",{"front":visible["front"]}),
                                ("visible_versions",wrong_hash),("visible_versions",wrong_version)):
                fields = {"provider":provider,"geometry_quality":"standard","selected_views":["front","right"],
                          "expected_input_epoch":snapshot["input_epoch"],"visible_versions":deepcopy(visible)}
                fields[field] = value
                with self.subTest(provider=provider,field=field,value=value),self.assertRaises(RequestError):
                    self.plan(**fields)
            plan = deepcopy(valid); plan["approved_multiview_snapshot"]["input_mode"] = "image"
            self.repo.write_record(self.group["id"],f"model-plans/{plan['id']}.json",plan)
            with self.assertRaises(RequestError): self.execute(valid)
        self.assertEqual(self.requests,[])
        self.assertEqual(self.jobs,{})

    def test_missing_auxiliary_can_be_excluded_with_a_failed_or_unknown_attempt(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports,capability=lambda *_:
            {"available":True,"supported_image_counts":[1,2,3,4],"model":"model-a"})
        group = self.repo.load(self.group["id"])
        group["views"]["back"]["current_version_id"] = None
        group["pending_views"] = ["back"]
        self.repo.save(group)
        self.selected_plan(["front","right"])
        with self.assertRaises(RequestError): self.selected_plan(["front","back"])
        operation = self.repo.read_record(self.group["id"],f"operations/{self.initial['id']}/operation.json")
        operation["attempts"]["back"]["state"] = "ambiguous"
        self.repo.write_record(self.group["id"],f"operations/{self.initial['id']}/operation.json",operation)
        group = self.repo.load(self.group["id"])
        group["views"]["back"]["latest_attempt"]["state"] = "ambiguous"
        self.repo.save(group)
        self.selected_plan(["front", "right"])
        with self.assertRaises(RequestError):
            self.selected_plan(["front", "back"])

    def test_terminal_unknown_auxiliary_is_excluded_from_both_provider_requests_and_keeps_its_receipt(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports, capability=lambda *_:
            {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": "model-a"})
        group = self.repo.load(self.group["id"])
        group["views"]["left"]["latest_attempt"]["state"] = "ambiguous"
        updated = self.repo.save(group)
        left = deepcopy(updated["views"]["left"])
        for provider in ("tripo", "yoyoclaw"):
            def start(request, **kwargs):
                self.requests.append(request)
                kwargs["authorization"].consume(provider, "model_generation")
                return SimpleNamespace(task_id=f"{provider}-task-{len(self.requests)}", reused=False)
            self.gateway.start_or_reuse_model_task.side_effect = start
            with self.subTest(provider=provider), self.assertRaises(RequestError) as selected:
                self.selected_plan(["front", "left"], provider=provider)
            self.assertEqual(selected.exception.code, "design_result_unknown")
            with self.assertRaises(RequestError) as legacy:
                self.plan(provider=provider, geometry_quality="standard")
            self.assertEqual(legacy.exception.code, "design_result_unknown")
            for expected in (["front"], ["front", "back"]):
                with self.subTest(provider=provider, selected=expected):
                    plan = self.selected_plan(expected, provider=provider)
                    before = len(self.requests)
                    job = self.execute(plan)
                    self.assertEqual(self.execute(plan).id, job.id)
                    self.assertEqual(len(self.queue), 1)
                    frozen = job.approved_multiview_snapshot
                    self.assertEqual(frozen["selected_views"], expected)
                    self.assertEqual(frozen["input_mode"], "image" if len(expected) == 1 else "multiview")
                    self.assertEqual({p.name for p in (job.directory / "inputs").iterdir()},
                                     {view + ".png" for view in expected})
                    self.drain()
                    self.assertEqual(job.state, "ready")
                    self.assertEqual(len(self.requests), before + 1)
                    self.assertEqual(self.requests[-1].source, frozen["input_mode"])
                    self.execute(plan)
                    self.drain()
                    self.assertEqual(len(self.requests), before + 1)
        self.assertEqual(self.repo.load(self.group["id"])["views"]["left"], left)
        self.assertEqual(len(self.requests), 4)

    def test_recoverable_results_still_block_unselected_views_and_main_unknown_cannot_be_excluded(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports, capability=lambda *_:
            {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": "model-a"})
        for direction, state, reason in (("left", "result_pending", "provider_task_ambiguous"),
                                         ("left", "result_saved", "provider_task_ambiguous"),
                                         ("front", "ambiguous", "design_result_unknown")):
            with self.subTest(direction=direction, state=state):
                group = self.repo.load(self.group["id"])
                group["views"]["left"]["latest_attempt"]["state"] = "succeeded"
                group["views"][direction]["latest_attempt"]["state"] = state
                self.repo.save(group)
                with self.assertRaises(RequestError) as rejected:
                    self.selected_plan(["front"])
                self.assertEqual(rejected.exception.code, reason)
                self.assertEqual(self.queue, [])
        self.assertEqual(self.jobs, {})

    def test_accepted_plan_cannot_be_reused_after_selected_attempt_changes(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports, capability=lambda *_:
            {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": "model-a"})
        plan = self.selected_plan(["front", "back"])
        group = self.repo.load(self.group["id"])
        group["views"]["back"]["latest_attempt"]["state"] = "ambiguous"
        self.repo.save(group)
        with self.assertRaises(RequestError) as rejected:
            self.execute(plan)
        self.assertEqual(rejected.exception.code, "revision_conflict")
        self.assertEqual(self.queue, [])
        self.assertEqual(self.jobs, {})

    def test_new_snapshots_and_strict_legacy_snapshots_validate_content_not_only_fingerprints(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports,capability=lambda *_:
            {"available":True,"supported_image_counts":[1,2,3,4],"model":"model-a"})
        for selected in (["front"],["front","back"],["front","left","right"],list(VIEWS)):
            job = self.execute(self.selected_plan(selected))
            good = deepcopy(job.approved_multiview_snapshot)
            for kind in ("mode","missing-front","unknown","order","path","version","hash","epoch","empty"):
                bad = deepcopy(good)
                if kind == "mode": bad["input_mode"] = "text"
                if kind == "missing-front": bad["views"].pop("front")
                if kind == "unknown": bad["views"]["top"] = bad["views"]["front"]
                if kind == "order": bad["selected_views"] = ["front","front"]
                if kind == "path": bad["views"]["front"]["path"] = "../front.png"
                if kind == "version": bad["views"]["front"]["version_id"] = "invalid"
                if kind == "hash": bad["views"]["front"]["sha256"] = "f"*64
                if kind == "epoch": bad["input_epoch"] = "invalid"
                if kind == "empty": (job.directory/"inputs/front.png").write_bytes(b"")
                bad["fingerprint"] = fingerprint({k:v for k,v in bad.items() if k != "fingerprint"})
                with self.subTest(selected=selected,kind=kind),self.assertRaises(RequestError):
                    approved_paths(bad,job.directory)
            legacy = deepcopy(good)
            legacy["schema"] = "approved-multiview-input-v1"
            for field in ("selected_views","input_mode","input_epoch"): legacy.pop(field)
            legacy["fingerprint"] = fingerprint({k:v for k,v in legacy.items() if k != "fingerprint"})
            if len(selected) != 4:
                with self.assertRaises(RequestError): approved_paths(legacy,job.directory,verify_files=False)
            else: self.assertEqual(list(approved_paths(legacy,job.directory,verify_files=False)),list(VIEWS))
            self.queue.clear(); job.state = "stopped"

    def test_capability_counts_are_explicit_and_never_inferred_from_four_view_support(self):
        from advanced_multiview_model_contracts import supports_model_input
        for count in range(1,5):
            self.assertEqual(supports_model_input({"available":True,"named_four_view_model":"supported"},count),count == 4)
            self.assertEqual(supports_model_input({"available":True,"named_four_view_model":"supported",
                                                  "supported_image_counts":[1,3]},count),count in (1,3))
            self.assertFalse(supports_model_input({"available":False,"supported_image_counts":[1,2,3,4]},count))

    def test_selected_model_commands_are_idempotent_and_reject_changed_input_or_provider(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports, capability=lambda _provider, model:
            {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": model or "model-a"})
        for provider in ("tripo", "yoyoclaw"):
            for mask in range(8):
                selected = ["front"] + [v for slot, v in enumerate(VIEWS[1:]) if mask & (1 << slot)]
                with self.subTest(provider=provider, selected=selected):
                    command = new_id()
                    original = self.selected_plan(selected, provider=provider, command_id=command)
                    self.assertEqual(self.selected_plan(selected, provider=provider, command_id=command), original)
                    changed = ["front", "left"] if len(selected) == 1 else ["front"]
                    for fields in ({"selected": changed, "provider": provider},
                                   {"selected": selected, "provider": "yoyoclaw" if provider == "tripo" else "tripo"}):
                        with self.assertRaises(RequestError) as caught:
                            self.selected_plan(command_id=command, **fields)
                        self.assertEqual(caught.exception.code, "idempotency_conflict")
        self.assertEqual(self.jobs, {})
        self.assertEqual(self.queue, [])
        self.assertEqual(self.requests, [])

    def test_expired_or_withdrawn_selected_model_plans_never_copy_or_submit_inputs(self):
        from dataclasses import replace
        for provider in ("tripo", "yoyoclaw"):
            for selected in (["front"], ["front", "back"], ["front", "left", "right"], list(VIEWS)):
                self.models.ports = replace(self.models.ports, capability=lambda _provider, model:
                    {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": model or "model-a"})
                with self.subTest(provider=provider, selected=selected):
                    plan = self.selected_plan(selected, provider=provider)
                    with mock.patch("advanced_multiview_models.time.time", return_value=plan["expires_at"] + 1):
                        with self.assertRaises(RequestError) as expired:
                            self.execute(plan)
                    self.assertEqual(expired.exception.code, "stale_confirmation")
                    self.models.ports = replace(self.models.ports, capability=lambda *_:
                        {"available": False, "supported_image_counts": [1, 2, 3, 4], "model": "model-a"})
                    with self.assertRaises(RequestError) as withdrawn:
                        self.execute(plan)
                    self.assertEqual(withdrawn.exception.code, "unsupported_multiview")
                    self.assertFalse((self.repo.root.parent / plan["job_id"]).exists())
        self.assertEqual(self.jobs, {})
        self.assertEqual(self.queue, [])
        self.assertEqual(self.requests, [])

    def test_copy_persist_and_queue_failures_cannot_start_a_selected_model_task(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports, capability=lambda _provider, model:
            {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": model or "model-a"})
        for provider in ("tripo", "yoyoclaw"):
            for selected in (["front"], ["front", "back"], ["front", "left", "right"], list(VIEWS)):
                for failure in ("copy", "persist", "queue"):
                    with self.subTest(provider=provider, selected=selected, failure=failure):
                        plan = self.selected_plan(selected, provider=provider)
                        if failure == "copy":
                            boundary = mock.patch("advanced_multiview_models.shutil.copyfile", side_effect=OSError("copy unavailable"))
                        elif failure == "persist":
                            boundary = mock.patch.object(self.ports, "persist", side_effect=OSError("write unavailable"))
                        else:
                            boundary = mock.patch.object(self.models, "ports", replace(self.models.ports,
                                submit=mock.Mock(side_effect=OSError("queue unavailable"))))
                        with boundary, self.assertRaises(OSError):
                            self.execute(plan)
                        self.assertEqual(self.queue, [])
                        self.assertEqual(self.requests, [])
                        directory = self.repo.root.parent / plan["job_id"]
                        if failure == "queue":
                            restored = _load_job(directory)
                            self.assertIsNotNone(restored)
                            self.assertEqual(restored.state, "stopped")
                            self.assertEqual(self.execute(plan).id, restored.id)
                            self.assertEqual(self.queue, [])
                        else:
                            self.assertNotIn(plan["job_id"], self.jobs)
                            self.assertFalse((directory / "job.json").exists())

    def test_missing_or_oversized_selected_snapshot_files_fail_before_the_gateway(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports, capability=lambda _provider, model:
            {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": model or "model-a"})
        for provider in ("tripo", "yoyoclaw"):
            for selected in (["front"], ["front", "back"], ["front", "left", "right"], list(VIEWS)):
                for corruption in ("missing", "oversized"):
                    with self.subTest(provider=provider, selected=selected, corruption=corruption):
                        job = self.execute(self.selected_plan(selected, provider=provider))
                        path = job.directory / f"inputs/{selected[-1]}.png"
                        if corruption == "missing":
                            path.unlink()
                        else:
                            with path.open("wb") as output:
                                output.truncate(20 * 1024 * 1024 + 1)
                        self.drain()
                        self.assertEqual(job.state, "failed")
                        self.assertIsNone(job.artifact_path)
                        self.assertEqual(job.attempts, [])
        self.assertEqual(self.requests, [])
        self.gateway.start_or_reuse_model_task.assert_not_called()

    def test_selected_snapshot_links_cannot_escape_the_job_even_with_matching_image_content(self):
        from dataclasses import replace
        import os
        import subprocess
        probe_target = Path(self.temp.name) / "link-probe-target.png"
        probe_target.write_bytes(image())
        probe_link = Path(self.temp.name) / "link-probe.png"
        try:
            probe_link.symlink_to(probe_target)
            probe_link.unlink()
            link_kind = "file-symlink"
        except OSError:
            if os.name != "nt":
                self.skipTest("Creating filesystem links is unavailable")
            # NTFS directory junctions exercise real resolved-path escape without
            # requiring the Windows file-symlink privilege or changing settings.
            link_kind = "directory-junction"
        self.models.ports = replace(self.models.ports, capability=lambda _provider, model:
            {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": model or "model-a"})
        for provider in ("tripo", "yoyoclaw"):
            for mask in range(8):
                selected = ["front"] + [view for slot, view in enumerate(VIEWS[1:]) if mask & (1 << slot)]
                for escaped_view in selected if link_kind == "file-symlink" else ["all"]:
                    with self.subTest(provider=provider, selected=selected, escaped_view=escaped_view, link_kind=link_kind):
                        job = self.execute(self.selected_plan(selected, provider=provider))
                        if link_kind == "file-symlink":
                            source = job.directory / f"inputs/{escaped_view}.png"
                            outside = Path(self.temp.name) / f"{job.id}-{escaped_view}.png"
                            outside.write_bytes(source.read_bytes())
                            source.unlink()
                            source.symlink_to(outside)
                            self.assertTrue(source.is_symlink())
                            self.assertEqual(source.read_bytes(), outside.read_bytes())
                        else:
                            source = job.directory / "inputs"
                            outside = Path(self.temp.name) / f"{job.id}-outside-inputs"
                            source.rename(outside)
                            subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(source), str(outside)],
                                           check=True, capture_output=True, text=True)
                            self.assertEqual(source.resolve(), outside.resolve())
                            for view in selected:
                                self.assertEqual((source / f"{view}.png").read_bytes(), (outside / f"{view}.png").read_bytes())
                        snapshots = [job.approved_multiview_snapshot]
                        if len(selected) == 4:
                            legacy = deepcopy(job.approved_multiview_snapshot)
                            legacy["schema"] = "approved-multiview-input-v1"
                            for field in ("selected_views", "input_mode", "input_epoch"):
                                legacy.pop(field)
                            legacy["fingerprint"] = fingerprint({k: v for k, v in legacy.items() if k != "fingerprint"})
                            snapshots.append(legacy)
                        for snapshot in snapshots:
                            for verify_files in (False, True):
                                with self.assertRaises(RequestError) as rejected:
                                    approved_paths(snapshot, job.directory, verify_files=verify_files)
                                self.assertEqual(rejected.exception.code, "invalid_multiview_snapshot")
                        original_receipt = (job.directory / "job.json").read_bytes()
                        # Saving a forged path is rejected as well. The original
                        # receipt must survive instead of serializing that path.
                        with self.assertRaises(RequestError) as rejected:
                            self.drain()
                        self.assertEqual(rejected.exception.code, "invalid_multiview_snapshot")
                        self.assertEqual(job.state, "failed")
                        self.assertIsNone(job.artifact_path)
                        self.assertEqual(job.attempts, [])
                        self.assertEqual((job.directory / "job.json").read_bytes(), original_receipt)
                        self.assertIsNone(_load_job(job.directory))
        self.assertEqual(self.requests, [])
        self.gateway.start_or_reuse_model_task.assert_not_called()

    def test_original_model_inputs_survive_later_current_upload_changes(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports, capability=lambda _provider, model:
            {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": model or "model-a"})
        originals = []
        for selected in (["front"], ["front", "back"], ["front", "left", "right"], list(VIEWS)):
            with self.subTest(selected=selected):
                job = self.execute(self.selected_plan(selected))
                self.queue.clear()
                job.state = job.phase = "stopped"
                _persist_job(job)
                frozen = deepcopy(job.approved_multiview_snapshot)
                before = {view: path.read_bytes() for view, path in approved_paths(frozen, job.directory).items()}
                originals.append((job, frozen, before))
        group = self.repo.load(self.group["id"])
        self.repo.replace_inputs(group, group["saved_draft"], {"front": image("gold")}, ("front",))
        self.assertIsNone(self.repo.snapshot(self.group["id"])["views"]["front"]["current"])
        for job, frozen, before in originals:
            with self.subTest(selected=frozen["selected_views"]):
                self.assertEqual(_load_job(job.directory).approved_multiview_snapshot, frozen)
                self.assertEqual({view: path.read_bytes() for view, path in approved_paths(frozen, job.directory).items()}, before)
        self.assertEqual(self.requests, [])

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

    def test_yoyoclaw_default_faces_roundtrip_and_confirmed_model_stays_frozen(self):
        from dataclasses import replace
        for mask in range(8):
            views = ["front"] + [view for slot, view in enumerate(VIEWS[1:]) if mask & (1 << slot)]
            with self.subTest(selected=views):
                selected = "Hy-3D-3.10-YOYO"
                seen = []
                def capability(provider, model):
                    seen.append((provider, model))
                    return {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": model or selected}
                self.models.ports = replace(self.models.ports, capability=capability)
                plan = self.selected_plan(views, provider="yoyoclaw", face_limit=1000000)
                self.assertEqual(plan["options"]["face_limit"], 0)
                self.assertEqual(plan["options"]["geometry_model"], selected)
                selected = "Hy-3D-4.0-YOYO"
                def start(request, **kwargs):
                    self.requests.append(request)
                    kwargs["authorization"].consume("yoyoclaw", "model_generation")
                    return SimpleNamespace(task_id=f"frozen-yoyo-task-{mask}", reused=False)
                self.gateway.start_or_reuse_model_task.side_effect = start
                before = len(self.requests)
                job = self.execute(plan)
                self.execute(plan)
                self.drain()
                self.assertEqual(job.state, "ready")
                self.assertEqual(len(self.requests), before + 1)
                request = self.requests[-1]
                self.assertEqual(request.model_id, "Hy-3D-3.10-YOYO")
                self.assertEqual(request.face_limit, 0)
                self.assertEqual(request.source, "image" if len(views) == 1 else "multiview")
                self.assertEqual(seen, [("yoyoclaw", ""), ("yoyoclaw", "Hy-3D-3.10-YOYO")])
                restored = _load_job(job.directory)
                self.assertEqual(restored.face_limit, 0)
                self.assertEqual(restored.generation_profile, "quality")
                self.assertEqual(restored.approved_multiview_snapshot, plan["approved_multiview_snapshot"])
                self.assertEqual(restored.approved_multiview_snapshot["selected_views"], views)
                self.assertEqual(restored.image_metrics["model_delivery_warnings"], [])

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

    def test_new_operation_uses_confirmed_views_and_never_resumes_an_ended_task(self):
        for terminal in ("ready", "failed", "stopped"):
            with self.subTest(terminal=terminal):
                job = self.execute(self.plan()); self.drain(); job.state = terminal
                frozen = dict(job.approved_multiview_snapshot["views"])
                before = len(self.requests)
                with self.assertRaises(RequestError) as caught: self.models.resume(self.group["id"], job.id)
                self.assertEqual(caught.exception.code, "generation_restart_required")
                self.assertEqual(len(self.requests), before)
                fresh = self.execute(self.plan()); self.drain()
                self.assertNotEqual(job.id, fresh.id)
                self.assertEqual(fresh.approved_multiview_snapshot["views"], frozen)
                self.assertEqual(len(self.requests), before + 1)
                self.assertEqual(self.gateway.start_or_reuse_model_task.call_args.kwargs["existing_task_id"], "")
                self.assertIsNotNone(self.gateway.start_or_reuse_model_task.call_args.kwargs["authorization"])

    def test_download_failure_ends_the_advanced_job_until_a_new_explicit_operation(self):
        error = ProviderGatewayError("Download exhausted", code="artifact_dns_failed", category="download",
                                     operation="artifact_download", provider="tripo", retryable=False)
        with mock.patch.object(self.ports, "download_artifact", side_effect=error) as download, \
                mock.patch.object(self.ports, "fail", wraps=self.ports.fail) as fail:
            job = self.execute(self.plan())
            self.drain()
        self.assertEqual(job.state, "failed")
        self.assertIsNone(job.artifact_path)
        self.assertEqual(job.attempts[0]["provider_error_code"], "artifact_dns_failed")
        self.assertFalse(job.attempts[0]["provider_error_retryable"])
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(len(job.attempts), 1)
        self.assertEqual(self.queue, [])
        download.assert_called_once()
        fail.assert_called_once()
        with self.assertRaises(RequestError) as caught:
            self.models.resume(self.group["id"], job.id)
        self.assertEqual(caught.exception.code, "generation_restart_required")
        self.assertEqual(len(self.requests), 1)
        fresh = self.execute(self.plan())
        self.drain()
        self.assertNotEqual(job.id, fresh.id)
        self.assertEqual(fresh.state, "ready")
        self.assertEqual(len(self.requests), 2)
        self.assertEqual(self.gateway.start_or_reuse_model_task.call_args.kwargs["existing_task_id"], "")

    def test_ended_ambiguous_submission_does_not_block_a_new_user_operation(self):
        self.gateway.start_or_reuse_model_task.side_effect = ProviderGatewayError("lost response", code="ambiguous", category="provider", ambiguous=True)
        job = self.execute(self.plan()); self.drain()
        with self.assertRaises(RequestError): self.models.resume(self.group["id"], job.id)
        self.plan()  # No submission: explicit new user confirmation still required.
        self.assertEqual(self.gateway.start_or_reuse_model_task.call_count, 1)
        job.state = "running"
        with self.assertRaises(RequestError): self.plan()  # Current unresolved create remains protected.

    def test_selected_jobs_preserve_the_original_task_after_poll_download_conversion_or_analysis_failure(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports, capability=lambda _provider, model:
            {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": model or "model-a"})
        for provider in ("tripo", "yoyoclaw"):
            def start(request, **kwargs):
                self.requests.append(request)
                kwargs["authorization"].consume(provider, "model_generation")
                return SimpleNamespace(task_id=f"{provider}-saved-task-{len(self.requests)}", reused=False)
            self.gateway.start_or_reuse_model_task.side_effect = start
            for selected in (["front"], ["front", "back"], ["front", "left", "right"], list(VIEWS)):
                for failure in ("poll", "download", "conversion", "analysis"):
                    with self.subTest(provider=provider, selected=selected, failure=failure):
                        job = self.execute(self.selected_plan(selected, provider=provider))
                        frozen = deepcopy(job.approved_multiview_snapshot)
                        original_inputs = {v: p.read_bytes() for v, p in approved_paths(frozen, job.directory).items()}
                        error = ProviderGatewayError("Controlled selected task failure", provider=provider,
                            code=f"{failure}_failed", category=failure, operation=failure, retryable=False)
                        if failure == "poll":
                            boundary = mock.patch.object(self.gateway, "wait_for_task", side_effect=error)
                        elif failure in ("download", "conversion"):
                            boundary = mock.patch.object(self.ports, "download_artifact", side_effect=error)
                        else:
                            boundary = mock.patch.object(self.ports, "validate_topology", side_effect=ValueError("Model analysis failed"))
                        before = len(self.requests)
                        before_creates = self.gateway.start_or_reuse_model_task.call_count
                        with boundary:
                            self.drain()
                        self.assertEqual(len(self.requests), before + 1)
                        self.assertEqual(self.gateway.start_or_reuse_model_task.call_count, before_creates + 1)
                        self.assertEqual(job.state, "failed")
                        self.assertIsNone(job.artifact_path)
                        self.assertEqual(len(job.attempts), 1)
                        task_id = job.attempts[0]["generation_task_id"]
                        self.assertTrue(task_id.startswith(provider + "-saved-task-"))
                        self.assertEqual(job.attempts[0]["status"], "rejected")
                        if failure != "analysis":
                            self.assertEqual(job.attempts[0]["provider_error_code"], error.code)
                        restored = _load_job(job.directory)
                        self.assertEqual(restored.state, "failed")
                        self.assertEqual(restored.attempts[0]["generation_task_id"], task_id)
                        self.assertEqual(restored.approved_multiview_snapshot, frozen)
                        self.assertEqual({v: p.read_bytes() for v, p in approved_paths(frozen, job.directory).items()}, original_inputs)
                        self.assertTrue(self.models.reconcile(self.group["id"], job.id)["accepted"])
                        with self.assertRaises(RequestError) as rejected:
                            self.models.resume(self.group["id"], job.id)
                        self.assertEqual(rejected.exception.code, "generation_restart_required")
                        self.assertEqual(len(self.requests), before + 1)
                        self.assertEqual(self.queue, [])
        self.ports.ensure_multiview.assert_not_called()

    def test_model_options_preserve_selected_inputs_without_extra_design_work(self):
        from dataclasses import replace
        self.models.ports = replace(self.models.ports, capability=lambda _provider, model:
            {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": model or "model-a"})
        original = self.execute(self.plan()); self.drain()
        frozen = deepcopy(original.approved_multiview_snapshot["views"])
        self.design.generate_image = mock.Mock(side_effect=AssertionError("Model options must not generate design images"))
        variants = {
            "tripo": [
                {"face_limit": 300000}, {"face_limit": 1000000},
                {"face_limit": 2000000, "geometry_quality": "detailed"},
                {"geometry_model": "model-b"}, {"geometry_quality": "detailed"},
                {"texture_quality": "detailed"}, {"texture_quality": "extreme"},
                {"output_format": "obj"},
            ],
            "yoyoclaw": [{"face_limit": faces, "output_format": output}
                         for faces in (0, 300000, 1000000) for output in ("glb", "obj")],
        }
        expected_requests = 1
        for provider, options_list in variants.items():
            for selected in (["front"], ["front", "back"], ["front", "left", "right"], list(VIEWS)):
                for options in options_list:
                    with self.subTest(provider=provider, selected=selected, options=options):
                        plan = self.selected_plan(selected, provider=provider, **options)
                        self.assertEqual(plan["max_new_image_calls"], 0)
                        self.assertEqual(plan["max_new_model_tasks"], 1)
                        job = self.execute(plan)
                        job.image_metrics.update(generation_input_quality={"model_input_eligible": False}, palette_quality_ok=False)
                        def start(request, **kwargs):
                            self.requests.append(request)
                            self.assertFalse(kwargs.get("existing_task_id"))
                            kwargs["authorization"].consume(provider, "model_generation")
                            return SimpleNamespace(task_id="new-task-" + job.id, reused=False)
                        def download(current_job, *_):
                            path = current_job.directory / ("candidate." + current_job.output_format)
                            path.write_bytes(b"controlled artifact")
                            return path
                        self.gateway.start_or_reuse_model_task.side_effect = start
                        with mock.patch.object(self.ports, "download_artifact", side_effect=download):
                            self.drain()
                        expected_requests += 1
                        self.assertEqual(len(self.requests), expected_requests)
                        self.assertEqual(job.state, "ready")
                        self.assertNotEqual(job.id, original.id)
                        self.assertEqual(job.approved_multiview_snapshot["views"], {v: frozen[v] for v in selected})
                        request = self.requests[-1]
                        self.assertEqual(request.source, "image" if len(selected) == 1 else "multiview")
                        paths = approved_paths(job.approved_multiview_snapshot, job.directory)
                        if len(selected) == 1:
                            self.assertEqual(request.image_path, paths["front"])
                            self.assertFalse(request.image_paths)
                        else:
                            self.assertEqual(request.image_paths, paths)
                        for name in ("face_limit", "geometry_quality", "texture_quality", "output_format"):
                            self.assertEqual(getattr(request, name), plan["options"][name])
                            self.assertEqual(getattr(job, name), plan["options"][name])
                        self.assertEqual(request.model_id, plan["options"]["geometry_model"])
                        if provider == "yoyoclaw":
                            self.assertEqual(job.face_limit, 0)
                            self.assertEqual(job.generation_profile, "quality")
                            self.assertEqual(job.image_metrics["model_delivery_warnings"], [])
                        restored = _load_job(job.directory)
                        self.assertEqual(restored.approved_multiview_snapshot, job.approved_multiview_snapshot)
                        self.assertEqual(restored.output_format, job.output_format)
                        self.assertEqual(job.artifact_format, job.output_format)
                        self.execute(plan); self.drain()
                        self.assertEqual(len(self.requests), expected_requests)
        self.design.generate_image.assert_not_called()
        self.ports.ensure_multiview.assert_not_called()
        self.ports.automatic_visual_review.assert_not_called()

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

    def test_stop_before_acceptance_records_a_negative_receipt_and_rejects_the_late_post(self):
        plan = self.plan()
        self.assertFalse(self.models.reconcile(self.group["id"], plan["job_id"])["accepted"])
        with self.assertRaises(RequestError) as caught: self.execute(plan)
        self.assertEqual(caught.exception.code, "submission_not_accepted")
        self.assertEqual(self.queue, [])
        self.assertEqual(self.requests, [])
        fresh = self.plan(); job = self.execute(fresh)
        self.assertNotEqual(job.id, plan["job_id"])
        self.drain()
        self.assertEqual(len(self.requests), 1)

    def test_restart_preserves_selected_inputs_and_original_tasks_without_submitting(self):
        from dataclasses import fields, replace
        from model_job_lifecycle import ModelJobLifecycle, ModelJobLifecyclePorts
        self.models.ports = replace(self.models.ports, capability=lambda _provider, model:
            {"available": True, "supported_image_counts": [1, 2, 3, 4], "model": model or "model-a"})
        for provider in ("tripo", "yoyoclaw"):
            for mask in range(8):
                selected = ["front"] + [v for slot, v in enumerate(VIEWS[1:]) if mask & (1 << slot)]
                for state in ("running", "stopping", "stopped", "queued", "awaiting_confirmation", "failed"):
                    with self.subTest(provider=provider, selected=selected, state=state):
                        old = self.execute(self.selected_plan(selected, provider=provider)); self.queue.clear()
                        old.state = old.phase = state
                        old.attempts = [{"generation_task_id": "original-" + old.id,
                                         "status": "submitted", "ambiguous": state == "running"}]
                        _persist_job(old)
                        frozen = deepcopy(old.approved_multiview_snapshot)
                        before = {view: path.read_bytes() for view, path in approved_paths(frozen, old.directory).items()}
                        def denied(*_args, **_kwargs): raise AssertionError("Restart must not submit or analyze")
                        values = {field.name: denied for field in fields(ModelJobLifecyclePorts)}
                        values.update(jobs={}, lock=threading.RLock(), model_output_root=lambda: old.directory.parent,
                            persist_job=_persist_job, submit=mock.Mock())
                        lifecycle = ModelJobLifecycle(ModelJobLifecyclePorts(**values)); lifecycle.restore_jobs()
                        restored = lifecycle.ports.jobs[old.id]
                        self.assertEqual(restored.state, "failed" if state == "failed" else "stopped")
                        self.assertFalse(lifecycle.latest_job_is_restorable(restored))
                        self.assertEqual(restored.provider, provider)
                        self.assertEqual(restored.creation_kind, "advanced_multiview_v1")
                        self.assertEqual(restored.attempts, old.attempts)
                        self.assertEqual(restored.approved_multiview_snapshot, frozen)
                        self.assertEqual({v: p.read_bytes() for v, p in approved_paths(frozen, restored.directory).items()}, before)
                        lifecycle.ports.submit.assert_not_called()
                        old.state = "stopped"  # Mirror startup's installed objects before the next explicit operation.
        self.assertEqual(self.requests, [])
        self.gateway.start_or_reuse_model_task.assert_not_called()

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
        self.assertEqual(job.state, "failed")
        self.assertEqual(job.attempts[0]["generation_task_id"], "original")
        self.plan()

    def test_reconciliation_reports_original_receipt_and_never_generates(self):
        runtime = AdvancedMultiviewRuntime.__new__(AdvancedMultiviewRuntime)
        runtime.repository, runtime.workflow, runtime.application = self.repo, self.design, self.images
        runtime.models = self.models
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
