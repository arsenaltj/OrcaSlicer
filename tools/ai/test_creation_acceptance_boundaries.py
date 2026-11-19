"""Deterministic acceptance of creation admission, catalog races and result reuse.

Only in-process provider doubles are used. The offline runner denies external
networking; these checks do not represent supplier or desktop acceptance.
"""
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
import tempfile
import threading
import unittest
from unittest import mock

import test_advanced_multiview_models as four_view_tests
from advanced_multiview_contracts import new_id
from model_artifact_workflow import ModelArtifactWorkflow
from model_contracts import Job, RequestError
from model_provider_gateway import ObservedModelResult, ProviderGatewayError
from yoyoclaw_client import YoYoClawConfig, YoYoClawError
from yoyoclaw_provider_gateway import (
    GEOMETRY_LIBRARY, GEOMETRY_ROLE, IMAGE_LIBRARY, IMAGE_ROLE,
    YoYoClawModelProviderGateway,
)


class CatalogBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.config = YoYoClawConfig(base_url="https://supplier.invalid", user_id="offline-a")

    def gateway(self, reader, config=None, closed=None):
        def factory():
            return SimpleNamespace(config=(config or (lambda: self.config))(),
                                   whitelist_catalog=reader,
                                   close=closed or (lambda: None))
        return YoYoClawModelProviderGateway(factory)

    def test_numeric_versions_and_trailing_zero_variants_require_a_unique_choice(self):
        for names, expected in [
            (["3.1", "3.2"], "3.2"), (["3.9", "3.10"], "3.10"),
            (["3.10", "3.10.0"], ""),
        ]:
            with self.subTest(names=names):
                gateway = self.gateway(lambda _: {"models": [{"name": f"Hy-3D-{n}-YOYO"} for n in names]})
                result = gateway.catalog(GEOMETRY_ROLE)
                self.assertEqual(result["default_model"], f"{GEOMETRY_LIBRARY}/Hy-3D-{expected}-YOYO" if expected else "")
                self.assertEqual(result["state"], "ready" if expected else "empty")

    def test_unavailable_texture_unknown_and_ambiguous_rows_never_become_a_default(self):
        rows = [{"name": "Hy-3D-9.0-YOYO", "available": False},
                {"name": "Hy-3D-8.0-YOYO", "status": "retired"},
                {"name": "Hy-3D-7.0-Texture-YOYO"}, {"name": "Unversioned-model"}]
        gateway = self.gateway(lambda _: {"models": rows})
        self.assertFalse(gateway.catalog(GEOMETRY_ROLE)["single_image"])
        rows.extend([{"name": "Hy-3D-3.1-A"}, {"name": "Hy-3D-3.1-B"}])
        self.assertFalse(gateway.catalog(GEOMETRY_ROLE, fresh=True)["named_four_view"])
        rows[0] = {"name": "Hy-3D-3.1-A", "default": True}
        self.assertEqual(gateway.catalog(GEOMETRY_ROLE, fresh=True)["default_model"],
                         f"{GEOMETRY_LIBRARY}/Hy-3D-3.1-A")

    def test_same_account_concurrent_catalog_requests_share_one_read_and_release_clients(self):
        entered, release, second_client = threading.Event(), threading.Event(), threading.Event()
        created, reads, closed = [], [], []
        def reader(library):
            reads.append(library); entered.set()
            if not release.wait(5):
                raise TimeoutError("Catalog release was not signalled")
            return {"models": [{"name": "Hy-3D-3.1-YOYO"}]}
        def factory():
            created.append(object())
            if len(created) >= 2: second_client.set()
            return SimpleNamespace(config=self.config, whitelist_catalog=reader,
                                   close=lambda: closed.append(True))
        gateway = YoYoClawModelProviderGateway(factory)
        with ThreadPoolExecutor(max_workers=2) as pool:
            first = pool.submit(gateway.catalog, GEOMETRY_ROLE, fresh=True)
            try:
                self.assertTrue(entered.wait(3))
                second = pool.submit(gateway.catalog, GEOMETRY_ROLE, fresh=True)
                self.assertTrue(second_client.wait(3))
                self.assertEqual(reads, [GEOMETRY_LIBRARY])
            finally:
                release.set()
            self.assertEqual(first.result(3), second.result(3))
        self.assertEqual(len(closed), 2)

    def test_other_account_and_role_do_not_wait_for_a_pending_geometry_read(self):
        entered, release = threading.Event(), threading.Event()
        local = threading.local()
        def factory():
            config = getattr(local, "config", self.config)
            def reader(library):
                if config == self.config and library == GEOMETRY_LIBRARY:
                    entered.set()
                    if not release.wait(5): raise TimeoutError("Unreleased old catalog")
                name = "Qwen-Image-3.0-PC-YOYO" if library == IMAGE_LIBRARY else "Hy-3D-3.2-YOYO"
                return {"models": [{"name": name}]}
            return SimpleNamespace(config=config, whitelist_catalog=reader, close=lambda: None)
        gateway = YoYoClawModelProviderGateway(factory)
        def read_other():
            local.config = replace(self.config, user_id="offline-b", base_url="https://other.invalid")
            return gateway.catalog(GEOMETRY_ROLE)
        with ThreadPoolExecutor(max_workers=3) as pool:
            old = pool.submit(gateway.catalog, GEOMETRY_ROLE)
            try:
                self.assertTrue(entered.wait(3))
                other = pool.submit(read_other).result(3)
                image = pool.submit(gateway.catalog, IMAGE_ROLE).result(3)
                self.assertEqual(other["state"], "ready")
                self.assertEqual(image["state"], "ready")
            finally:
                release.set()
            self.assertEqual(old.result(3)["state"], "ready")

    def test_cached_results_are_copies_expire_and_do_not_cross_accounts(self):
        reader = mock.Mock(return_value={"models": [{"name": "Hy-3D-3.1-YOYO"}]})
        gateway = self.gateway(reader)
        with mock.patch("yoyoclaw_provider_gateway.time.monotonic", return_value=100):
            first = gateway.catalog(GEOMETRY_ROLE)
            first["models"].clear()
            self.assertTrue(gateway.catalog(GEOMETRY_ROLE)["models"])
            self.assertEqual(reader.call_count, 1)
            self.config = replace(self.config, user_id="offline-b")
            gateway.catalog(GEOMETRY_ROLE)
            self.assertEqual(reader.call_count, 2)
        with mock.patch("yoyoclaw_provider_gateway.time.monotonic", return_value=106):
            gateway.catalog(GEOMETRY_ROLE)
            self.assertEqual(reader.call_count, 3)

    def test_failed_refresh_invalidates_only_its_account_and_is_not_cached(self):
        reader = mock.Mock(return_value={"models": [{"name": "Hy-3D-3.1-YOYO"}]})
        closed = mock.Mock()
        gateway = self.gateway(reader, closed=closed)
        self.assertEqual(gateway.catalog(GEOMETRY_ROLE)["state"], "ready")
        reader.side_effect = YoYoClawError("offline 401", code="authentication_failed", status=401)
        self.assertEqual(gateway.catalog(GEOMETRY_ROLE, fresh=True)["state"], "error")
        self.assertEqual(gateway.catalog(GEOMETRY_ROLE)["state"], "error")
        self.assertEqual(reader.call_count, 3)
        self.assertEqual(closed.call_count, 3)


class ModelAdmissionBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.fixture = four_view_tests.ModelTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    def test_stopping_while_model_admission_waits_does_not_block_or_accept_late_work(self):
        f = self.fixture
        plan = f.plan()
        entered, release = threading.Event(), threading.Event()
        def capability(*_):
            entered.set()
            if not release.wait(5): raise TimeoutError("Model admission was not released")
            return {"available": True, "named_four_view_model": "supported", "model": "model-a"}
        f.models.ports = replace(f.models.ports, capability=capability)
        with ThreadPoolExecutor(max_workers=2) as pool:
            pending = pool.submit(f.execute, plan)
            try:
                self.assertTrue(entered.wait(3))
                result = pool.submit(f.models.reconcile, f.group["id"], plan["job_id"]).result(3)
                self.assertFalse(result["accepted"])
            finally:
                release.set()
            with self.assertRaises(RequestError) as error: pending.result(3)
        self.assertEqual(error.exception.code, "submission_not_accepted")
        self.assertEqual(f.queue, [])
        self.assertEqual(f.requests, [])

    def test_concurrent_retransmission_keeps_the_original_acceptance_and_queue(self):
        f = self.fixture
        plan = f.plan()
        with ThreadPoolExecutor(max_workers=2) as pool:
            jobs = list(pool.map(lambda _: f.execute(plan), range(2)))
        self.assertIs(jobs[0], jobs[1])
        self.assertEqual(len(f.queue), 1)
        f.drain()
        self.assertEqual(len(f.requests), 1)

    def test_withdrawn_frozen_model_and_expired_plan_do_not_create_a_job(self):
        f = self.fixture
        plan = f.plan()
        f.models.ports = replace(f.models.ports, capability=lambda *_: {"available": False})
        with self.assertRaises(RequestError) as error: f.execute(plan)
        self.assertEqual(error.exception.code, "unsupported_multiview")
        with mock.patch("advanced_multiview_models.time.time", return_value=plan["expires_at"] + 1):
            with self.assertRaises(RequestError) as error: f.execute(plan)
        self.assertEqual(error.exception.code, "stale_confirmation")
        self.assertEqual(f.queue, [])
        self.assertEqual(f.jobs, {})

    def test_validation_and_local_publish_failures_preserve_the_accepted_task(self):
        for stage in ("validate_topology", "promote_artifact"):
            with self.subTest(stage=stage):
                f = self.fixture
                original = getattr(f.ports, stage)
                setattr(f.ports, stage, mock.Mock(side_effect=OSError("offline local failure")))
                job = f.execute(f.plan()); f.drain()
                self.assertEqual(job.state, "failed")
                self.assertEqual(job.attempts[0]["generation_task_id"], "saved-task")
                self.assertEqual(len(job.attempts), 1)
                self.assertEqual(len(f.requests), 1 if stage == "validate_topology" else 2)
                self.assertTrue((job.directory / "inputs/front.png").is_file())
                setattr(f.ports, stage, original)


class ObservedResultBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.job = Job(id=new_id(), source="image", directory=Path(self.temp.name),
                       provider="yoyoclaw", geometry_model="Hy-3D-3.1-YOYO", face_limit=0)
        self.gateway = mock.Mock()
        self.gateway.wait_for_task.return_value = {"asset": "fresh"}
        self.gateway.download_artifact.side_effect = lambda payload, path, *_args, **_kw: path.write_bytes(b"offline OBJ")
        self.ports = SimpleNamespace(lock=threading.RLock(), persist=mock.Mock(),
                                    stop_boundary=mock.Mock(), model_gateway=lambda _: self.gateway,
                                    prepare_obj=mock.Mock(return_value=self.job.directory / "model.obj"))
        self.workflow = ModelArtifactWorkflow(self.ports)

    def observed(self, **changes):
        fields = dict(provider="yoyoclaw", model_id=self.job.geometry_model,
                      task_id="accepted-task", payload={"asset": "observed"})
        return ObservedModelResult(**{**fields, **changes})

    def test_same_verified_result_is_downloaded_without_a_second_terminal_query(self):
        result = self.observed()
        self.workflow.download_generation_artifact(self.job, "accepted-task", observed=result)
        self.gateway.wait_for_task.assert_not_called()
        self.assertEqual(self.gateway.download_artifact.call_args.args[0], result.payload)
        self.gateway.start_or_reuse_model_task.assert_not_called()

    def test_foreign_task_model_provider_or_empty_result_cannot_supply_an_asset(self):
        for changes in ({"task_id": "foreign"}, {"model_id": "Hy-3D-9.0-YOYO"},
                        {"provider": "tripo"}, {"payload": {}}):
            with self.subTest(changes=changes):
                self.gateway.reset_mock()
                self.workflow.download_generation_artifact(self.job, "accepted-task", observed=self.observed(**changes))
                self.gateway.wait_for_task.assert_called_once_with("accepted-task", stop_event=self.job.stop_event,
                                                                   model_id=self.job.geometry_model)
                self.assertEqual(self.gateway.download_artifact.call_args.args[0], {"asset": "fresh"})
                self.gateway.start_or_reuse_model_task.assert_not_called()

    def test_expired_asset_refreshes_only_the_same_task_once(self):
        for status in (403, 410):
            with self.subTest(status=status):
                self.gateway.reset_mock()
                # The second response supplies a real local file to the owning workflow.
                def download(payload, path, *_args, **_kw):
                    if payload["asset"] == "observed":
                        raise ProviderGatewayError("expired", code="download_failed", category="download", status=status)
                    path.write_bytes(b"offline OBJ")
                self.gateway.download_artifact.side_effect = download
                self.workflow.download_generation_artifact(self.job, "accepted-task", observed=self.observed())
                self.gateway.wait_for_task.assert_called_once()
                self.assertEqual(self.gateway.wait_for_task.call_args.args, ("accepted-task",))
                self.assertEqual(self.gateway.download_artifact.call_count, 2)
                self.gateway.start_or_reuse_model_task.assert_not_called()

    def test_unrelated_download_failure_does_not_refresh_or_create(self):
        self.gateway.download_artifact.side_effect = ProviderGatewayError(
            "local download failure", code="download_failed", category="download", status=500)
        with self.assertRaises(ProviderGatewayError):
            self.workflow.download_generation_artifact(self.job, "accepted-task", observed=self.observed())
        self.gateway.wait_for_task.assert_not_called()
        self.gateway.start_or_reuse_model_task.assert_not_called()

    def test_legacy_tripo_obj_keeps_its_original_conversion_path(self):
        self.job.provider = "tripo"; self.job.output_format = "obj"
        with mock.patch.object(self.workflow, "download_conversion", return_value=Path("retained.obj")) as conversion:
            result = self.workflow.download_generation_artifact(self.job, "accepted-task", observed=self.observed())
        self.assertEqual(result, Path("retained.obj"))
        conversion.assert_called_once_with(self.job, "accepted-task", "obj", 1, False)
        self.gateway.wait_for_task.assert_not_called()


if __name__ == "__main__":
    unittest.main()
