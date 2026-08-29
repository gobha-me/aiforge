#!/usr/bin/env python3
"""Offline failure-matrix tests for the DrawForge evaluation bridge."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).with_name("tool.py")
RUNNER: Path | None = None


class BridgeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="aiforge-drawforge-test-")
        self.root = Path(self.temporary.name)
        self.binary = self.root / "drawforge-fake.py"
        self.binary.write_text(
            "#!/usr/bin/env python3\n"
            "import json, sys\n"
            "for line in sys.stdin:\n"
            " print(json.dumps({'status':'ok','result':{'revision':1}}))\n",
            encoding="utf-8",
        )
        self.binary.chmod(0o700)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def prepare(self, route: str, task_id: str) -> Path:
        run = self.root / f"{route}-{task_id}"
        run.mkdir()
        (run / "attempts").mkdir()
        (run / "run.json").write_text(
            json.dumps(
                {
                    "route": route,
                    "task_id": task_id,
                    "usage": {"tool_interactions": 0},
                    "events": [],
                }
            ),
            encoding="utf-8",
        )
        return run

    def submit(self, run: Path, payload: str) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment.update(
            DRAWFORGE_EVAL_RUN=str(run),
            DRAWFORGE_EVAL_BINARY=str(self.binary),
        )
        return subprocess.run(
            [sys.executable, str(TOOL), "submit", payload],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            check=False,
        )

    def metadata(self, run: Path) -> dict[str, object]:
        return json.loads((run / "run.json").read_text(encoding="utf-8"))

    def test_invalid_direct_submission_is_rejected_without_commit(self) -> None:
        run = self.prepare("direct-svg", "recover-invalid-edit")
        first = self.submit(run, "<svg id='first'/>")
        second = self.submit(run, "<svg id='second'/>")
        self.assertEqual(json.loads(first.stdout)["code"], "invalid_submission")
        self.assertTrue(json.loads(second.stdout)["accepted"])
        self.assertEqual(len(list((run / "attempts").glob("*.svg"))), 2)
        self.assertEqual(
            self.metadata(run)["events"],
            ["submission_rejected_invalid", "submission_accepted"],
        )

    def test_stale_direct_submission_exposes_concurrent_source(self) -> None:
        run = self.prepare("direct-svg", "recover-stale-revision")
        (run / "concurrent-source.svg").write_text("<svg id='current'/>", encoding="utf-8")
        result = json.loads(self.submit(run, "<svg id='stale'/>").stdout)
        self.assertEqual(result["code"], "stale_revision")
        self.assertEqual(result["refreshed_source"], "<svg id='current'/>")
        self.assertEqual(
            self.metadata(run)["events"],
            ["submission_rejected_stale", "source_refreshed"],
        )

    def test_semantic_create_does_not_consume_submission_attempt(self) -> None:
        run = self.prepare("semantic", "create-status-badge")
        created = self.submit(run, json.dumps({"kind": "create_document"}))
        applied = self.submit(run, json.dumps({"kind": "apply"}))
        self.assertEqual(created.returncode, 0)
        self.assertEqual(applied.returncode, 0)
        attempts = list((run / "attempts").glob("*.jsonl"))
        self.assertEqual([path.name for path in attempts], ["001.jsonl"])
        frames = attempts[0].read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(frames), 2)
        self.assertEqual(self.metadata(run)["events"], ["submission_accepted"])

    def test_invalid_semantic_apply_does_not_commit(self) -> None:
        run = self.prepare("semantic", "recover-invalid-edit")
        (run / "source.jsonl").write_text("", encoding="utf-8")
        first = json.loads(self.submit(run, json.dumps({"kind": "apply", "id": 1})).stdout)
        second = json.loads(self.submit(run, json.dumps({"kind": "apply", "id": 2})).stdout)
        self.assertEqual(first["code"], "invalid_submission")
        self.assertEqual(second["status"], "ok")
        transcript = (run / "attempts" / "002.jsonl").read_text(encoding="utf-8")
        self.assertNotIn('"id":1', transcript)
        self.assertIn('"id":2', transcript)

    def test_malformed_semantic_payload_fails_closed(self) -> None:
        run = self.prepare("semantic", "revise-named-sun")
        result = self.submit(run, "not-json")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["code"], "adapter_failure")
        self.assertFalse((run / ".aiforge-eval-state.json").exists())

    def test_interaction_ceiling_is_enforced(self) -> None:
        run = self.prepare("semantic", "revise-named-sun")
        for _ in range(12):
            self.assertEqual(self.submit(run, json.dumps({"kind": "inspect"})).returncode, 0)
        result = self.submit(run, json.dumps({"kind": "inspect"}))
        self.assertEqual(result.returncode, 2)
        self.assertIn("interaction budget exhausted", result.stdout)
        self.assertEqual(self.metadata(run)["usage"]["tool_interactions"], 12)

    def test_runner_refuses_started_run_without_cost_evidence(self) -> None:
        self.assertIsNotNone(RUNNER)
        run = self.prepare("direct-svg", "revise-named-sun")
        metadata = self.metadata(run)
        metadata.update(schema_version=2, corpus_id="drawforge-semantic-svg-v2")
        metadata["usage"]["cost_usd"] = None
        (run / "run.json").write_text(json.dumps(metadata), encoding="utf-8")
        (run / ".aiforge-eval-state.json").write_text("{}", encoding="utf-8")
        result = subprocess.run(
            [
                str(RUNNER),
                "--run",
                str(run),
                "--matrix-root",
                str(self.root),
                "--drawforge",
                str(self.binary),
                "--helper",
                str(TOOL),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("started matrix run lacks provider-reported USD cost", result.stderr)

    def test_runner_refuses_concurrent_matrix_execution(self) -> None:
        self.assertIsNotNone(RUNNER)
        run = self.prepare("direct-svg", "revise-named-sun")
        (self.root / ".aiforge-drawforge-eval.lock").mkdir()
        result = subprocess.run(
            [
                str(RUNNER),
                "--run",
                str(run),
                "--matrix-root",
                str(self.root),
                "--drawforge",
                str(self.binary),
                "--helper",
                str(TOOL),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("another matrix runner is active", result.stderr)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--runner":
        RUNNER = Path(sys.argv[2])
        del sys.argv[1:]
    unittest.main()
