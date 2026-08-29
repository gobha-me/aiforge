#!/usr/bin/env python3
"""Stateful, evaluation-only bridge from AIForge's process tool to DrawForge."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


PROTOCOL = "drawforge.experimental/v1"
MAX_PAYLOAD_BYTES = 1_048_576


class ToolError(Exception):
    pass


def load_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ToolError(f"{path.name} must contain a JSON object")
    return value


def write_object(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".new")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def frame(request: dict[str, Any]) -> dict[str, Any]:
    return {"protocol": PROTOCOL, "request": request}


def load_frames(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    frames = []
    for line in path.read_text(encoding="utf-8").splitlines():
        value = json.loads(line)
        if not isinstance(value, dict) or value.get("protocol") != PROTOCOL:
            raise ToolError(f"invalid frozen transcript: {path.name}")
        frames.append(value)
    return frames


def invoke(binary: Path, frames: list[dict[str, Any]]) -> dict[str, Any]:
    payload = "".join(json.dumps(item, separators=(",", ":")) + "\n" for item in frames)
    with tempfile.TemporaryDirectory(prefix="aiforge-drawforge-eval-") as raw:
        completed = subprocess.run(
            [str(binary), "jsonl", "--artifact-dir", raw],
            input=payload,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
    responses = [json.loads(line) for line in completed.stdout.splitlines()]
    if not responses:
        raise ToolError("DrawForge produced no response")
    return responses[-1]


def append_event(metadata: dict[str, Any], event: str) -> None:
    events = metadata.setdefault("events", [])
    if not isinstance(events, list):
        raise ToolError("run events are malformed")
    events.append(event)


def initial_state(task_id: str) -> dict[str, Any]:
    events = ["context_reset"] if task_id == "continue-after-compaction" else []
    return {"interactions": 0, "next_attempt": 1, "frames": [], "concurrent": False, "events": events}


def save_attempt(run: Path, state: dict[str, Any], suffix: str, payload: str) -> Path:
    attempts = run / "attempts"
    attempts.mkdir(exist_ok=True)
    number = state["next_attempt"]
    if not isinstance(number, int) or not 1 <= number <= 3:
        raise ToolError("submission budget exhausted")
    path = attempts / f"{number:03d}.{suffix}"
    path.write_text(payload if payload.endswith("\n") else payload + "\n", encoding="utf-8")
    state["next_attempt"] = number + 1
    return path


def direct_submit(run: Path, metadata: dict[str, Any], state: dict[str, Any], payload: str) -> dict[str, Any]:
    save_attempt(run, state, "svg", payload)
    task_id = metadata["task_id"]
    if task_id == "recover-invalid-edit" and "submission_rejected_invalid" not in state["events"]:
        state["events"].append("submission_rejected_invalid")
        return {"status": "error", "code": "invalid_submission", "retryable": True,
                "message": "the injected route adapter rejected the first submission"}
    if task_id == "recover-stale-revision" and "submission_rejected_stale" not in state["events"]:
        state["events"].extend(["submission_rejected_stale", "source_refreshed"])
        refreshed = (run / "concurrent-source.svg").read_text(encoding="utf-8")
        return {"status": "error", "code": "stale_revision", "retryable": True,
                "message": "the source changed concurrently; preserve this refreshed source on retry",
                "refreshed_source": refreshed}
    if task_id == "continue-after-compaction" and "source_inspected" not in state["events"]:
        state["events"].append("source_inspected")
    state["events"].append("submission_accepted")
    return {"status": "ok", "accepted": True, "attempt": state["next_attempt"] - 1}


def semantic_submit(
    run: Path, binary: Path, metadata: dict[str, Any], state: dict[str, Any], payload: str
) -> dict[str, Any]:
    request = json.loads(payload)
    if not isinstance(request, dict):
        raise ToolError("semantic payload must be one DrawForge request object")
    kind = request.get("kind")
    is_apply = kind == "apply"
    task_id = metadata["task_id"]
    if is_apply and task_id == "recover-invalid-edit" and "submission_rejected_invalid" not in state["events"]:
        save_attempt(run, state, "jsonl", json.dumps(frame(request), separators=(",", ":")))
        state["events"].append("submission_rejected_invalid")
        return {"status": "error", "code": "invalid_submission", "retryable": True,
                "message": "the injected adapter rejected the first mutation before commit"}
    if is_apply and task_id == "recover-stale-revision" and "submission_rejected_stale" not in state["events"]:
        save_attempt(run, state, "jsonl", json.dumps(frame(request), separators=(",", ":")))
        state["events"].extend(["submission_rejected_stale", "source_refreshed"])
        state["concurrent"] = True
        return {"status": "error", "code": "stale_revision", "retryable": True,
                "message": "the source changed concurrently; inspect the refreshed revision and retry"}

    source_name = "concurrent-source.jsonl" if state["concurrent"] else "source.jsonl"
    frames = load_frames(run / source_name) + list(state["frames"]) + [frame(request)]
    response = invoke(binary, frames)
    if response.get("status") != "ok":
        return response
    if kind == "inspect" and task_id == "continue-after-compaction" and "source_inspected" not in state["events"]:
        state["events"].append("source_inspected")
    if kind in {"create_document", "apply"}:
        state["frames"].append(frame(request))
    if is_apply:
        transcript = "".join(
            json.dumps(item, separators=(",", ":"), sort_keys=True) + "\n" for item in state["frames"]
        )
        save_attempt(run, state, "jsonl", transcript)
        state["events"].append("submission_accepted")
    return response


def main() -> int:
    try:
        if len(sys.argv) != 3 or sys.argv[1] != "submit":
            raise ToolError("usage: tool.py submit PAYLOAD")
        payload = sys.argv[2]
        if not payload or len(payload.encode("utf-8")) > MAX_PAYLOAD_BYTES:
            raise ToolError("payload is empty or exceeds 1 MiB")
        run_raw = os.environ.get("DRAWFORGE_EVAL_RUN")
        binary_raw = os.environ.get("DRAWFORGE_EVAL_BINARY")
        if not run_raw or not binary_raw:
            raise ToolError("evaluation environment is incomplete")
        run = Path(run_raw).resolve(strict=True)
        binary = Path(binary_raw).resolve(strict=True)
        metadata = load_object(run / "run.json")
        state_path = run / ".aiforge-eval-state.json"
        state = load_object(state_path) if state_path.exists() else initial_state(metadata["task_id"])
        state["interactions"] += 1
        if state["interactions"] > 12:
            raise ToolError("tool interaction budget exhausted")
        if metadata["route"] == "direct-svg":
            result = direct_submit(run, metadata, state, payload)
        elif metadata["route"] == "semantic":
            result = semantic_submit(run, binary, metadata, state, payload)
        else:
            raise ToolError("unsupported evaluation route")
        metadata["events"] = state["events"]
        metadata["usage"]["tool_interactions"] = state["interactions"]
        write_object(state_path, state)
        write_object(run / "run.json", metadata)
        print(json.dumps(result, separators=(",", ":"), sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, ToolError, subprocess.SubprocessError) as error:
        print(json.dumps({"status": "error", "code": "adapter_failure", "retryable": False,
                          "message": str(error)[:512]}, separators=(",", ":"), sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
