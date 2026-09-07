"""Exercise production input collection without credentials or provider calls."""

import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import threading
import time


def run(production, root):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("AIFORGE_") and key != "VENICE_API_KEY"}
    environment.update(XDG_CONFIG_HOME=str(root / "config"),
                       XDG_STATE_HOME=str(root / "state"),
                       XDG_CACHE_HOME=str(root / "cache"))
    command = [production, "explain"]
    # Hold the writer open throughout cancellation, including full read buffers.
    for size in (0, 17, 4096, 8192):
        for repeated in (False, True):
            with subprocess.Popen(command, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  env=environment) as process:
                try:
                    if size:
                        process.stdin.write(b"x" * size)
                        process.stdin.flush()
                    time.sleep(0.15)
                    start = time.monotonic()
                    process.send_signal(signal.SIGINT)
                    if repeated:
                        for _ in range(4):
                            time.sleep(0.002)
                            if process.poll() is None:
                                process.send_signal(signal.SIGINT)
                    # wait(), unlike communicate(), leaves producer stdin open.
                    assert process.wait(timeout=2) == 130, (size, repeated)
                    assert time.monotonic() - start < 2
                    assert process.stdout.read() == b""
                    assert process.stderr.read() == b"aiforge: request cancelled\n"
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    # Keep supplying input during SIGINT, below the byte limit.
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=environment) as process:
        finished = threading.Event()

        def produce():
            while not finished.is_set():
                try:
                    process.stdin.write(b"x" * 4096)
                    process.stdin.flush()
                except BrokenPipeError:
                    return
                finished.wait(0.005)

        writer = threading.Thread(target=produce)
        writer.start()
        try:
            time.sleep(0.15)
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=2) == 130
            assert process.stdout.read() == b""
            assert process.stderr.read() == b"aiforge: request cancelled\n"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            finished.set()
            writer.join(timeout=2)
            assert not writer.is_alive()
            try:
                process.stdin.close()
            except BrokenPipeError:
                pass

    # EOF, exact limit and overflow controls; stop before any backend setup.
    for size in (0, 17, 4096, 1024 * 1024 - len("explain"), 1024 * 1024 + 1):
        result = subprocess.run(command, input=b"x" * size, capture_output=True,
                                env=environment, timeout=3)
        assert result.stdout == b""
        if size > 1024 * 1024:
            assert result.returncode == 2
            assert b"standard input exceeds 1 MiB" in result.stderr
        else:
            assert result.returncode == 1
            assert b"model is not configured" in result.stderr
    # Regular files share the descriptor path, and EOF must remain prompt.
    input_path = root / "input.txt"
    input_path.write_bytes(b"completed file input")
    with input_path.open("rb") as input_file:
        result = subprocess.run(command, stdin=input_file, capture_output=True,
                                env=environment, timeout=3)
    assert result.returncode == 1
    assert result.stdout == b""
    assert b"model is not configured" in result.stderr
    # An open directory descriptor polls readable but cannot be read as input.
    descriptor = os.open(root, os.O_RDONLY)
    try:
        result = subprocess.run(command, stdin=descriptor, capture_output=True,
                                env=environment, timeout=3)
        assert result.returncode == 1
        assert result.stdout == b""
        assert b"standard input could not be read" in result.stderr
    finally:
        os.close(descriptor)
    assert not (root / "state").exists(), "input collection created session state"


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="aiforge-stdin-") as directory:
        run(sys.argv[1], pathlib.Path(directory))
