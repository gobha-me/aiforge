"""Production JSONL transport and inert replay, with no provider credentials."""

import hashlib
import json
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time


def records(output):
    return [json.loads(line) for line in output.splitlines()]


def run(production, fixture, root):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("AIFORGE_") and key != "VENICE_API_KEY"}
    environment.update(HOME=str(root / "home"),
                       XDG_CONFIG_HOME=str(root / "config"),
                       XDG_STATE_HOME=str(root / "state"),
                       XDG_CACHE_HOME=str(root / "cache"))
    command = [production, "agent", "--jsonl"]
    for payload in (b"", b"{", b"{}", b"{}\n{}", b"x" * (1024 * 1024 + 1)):
        result = subprocess.run(command, input=payload, capture_output=True,
                                env=environment, timeout=3)
        assert result.returncode == 2, result.stderr
        emitted = records(result.stdout)
        assert [item["type"] for item in emitted] == ["error", "terminal"]
        assert emitted[-1]["durable_terminal"] is False
        assert b"\x1b" not in result.stdout

    for size in (0, 17, 4096):
        with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=environment) as process:
            if size:
                process.stdin.write(b"x" * size)
                process.stdin.flush()
            time.sleep(0.08)
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=2) == 130
            emitted = records(process.stdout.read())
            assert emitted[-1]["status"] == "cancelled"
    assert not (root / "state").exists(), "input errors opened persistent storage"

    database = root / "state" / "aiforge" / "sessions.sqlite3"
    database.parent.mkdir(parents=True)
    subprocess.run([fixture, str(database)], check=True, env=environment, timeout=3)
    before = hashlib.sha256(database.read_bytes()).hexdigest()
    # A broken configuration would prevent normal startup. Replay and pending
    # refusal must remain before configuration, catalog, auth and terminal setup.
    config = root / "config" / "aiforge" / "config.json"
    config.parent.mkdir(parents=True)
    config.write_text("BROKEN_CONFIG_PRIVATE_SENTINEL")
    request = b'{"version":1,"operation":"submit","session_id":"pending","profile":"dev","tools":["run_process"],"prompt":"do not execute"}'
    result = subprocess.run(command, input=request, capture_output=True,
                            env=environment, timeout=3)
    assert result.returncode == 1, result.stderr
    emitted = records(result.stdout)
    assert len(emitted) == 1 and emitted[0]["status"] == "recovery_required", emitted
    assert emitted[0]["session_id"] == "pending" and emitted[0]["run_id"] == "run"
    assert hashlib.sha256(database.read_bytes()).hexdigest() == before

    replay = b'{"version":1,"operation":"replay","session_id":"ready"}'
    result = subprocess.run(command, input=replay, capture_output=True,
                            env=environment, timeout=3)
    assert result.returncode == 0, result.stderr
    emitted = records(result.stdout)
    assert emitted[-1]["status"] == "completed"
    assert emitted[-1]["durable_terminal"] is False
    assert len(emitted) == 35
    assert b"PRIVATE_SENTINEL" not in result.stdout + result.stderr

    # A consumer that never reads stdout cannot hold the process indefinitely.
    for interrupt in (False, True):
        with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=environment) as process:
            process.stdin.write(replay)
            process.stdin.close()
            start = time.monotonic()
            if interrupt:
                time.sleep(0.15)
                process.send_signal(signal.SIGINT)
            assert process.wait(timeout=4) == (130 if interrupt else 1)
            assert time.monotonic() - start < 4
            output = process.stdout.read()
            assert output and not output.endswith(b"\n"), "partial record was not retained"
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=environment) as process:
        process.stdout.close()
        process.stdin.write(replay)
        process.stdin.close()
        assert process.wait(timeout=3) == 1, "SIGPIPE killed the process"
    assert hashlib.sha256(database.read_bytes()).hexdigest() == before


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="aiforge-agent-") as directory:
        run(sys.argv[1], sys.argv[2], pathlib.Path(directory))
