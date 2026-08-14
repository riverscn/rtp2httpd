"""E2E coverage for the opt-in RefPlayer helper lifetime contract."""

from __future__ import annotations

import os
import signal
import subprocess
import time

from helpers import find_free_port, wait_for_status_payload


_LIFETIME_ENV = "RTP2HTTPD_REFPLAYER_LIFETIME_STDIN"


def _worker_pid(port: int) -> int:
    payload = wait_for_status_payload(
        "127.0.0.1",
        port,
        lambda value: len(value.get("workers", [])) == 1 and value["workers"][0]["pid"] > 0,
    )
    return payload["workers"][0]["pid"]


def _wait_for_exit(pid: int, timeout: float = 6.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.05)
    raise AssertionError("process %d did not exit" % pid)


def _stop_process(process: subprocess.Popen[bytes], worker_pid: int | None = None) -> None:
    if process.stdin is not None and not process.stdin.closed:
        process.stdin.close()
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=6)
        except subprocess.TimeoutExpired:
            try:
                owns_process_group = os.getpgid(process.pid) == process.pid
            except ProcessLookupError:
                owns_process_group = False
            if owns_process_group:
                os.killpg(process.pid, signal.SIGKILL)
            elif process.poll() is None:
                process.kill()
            process.wait(timeout=2)
    if worker_pid is not None:
        try:
            if os.getpgid(worker_pid) == process.pid:
                os.kill(worker_pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def test_refplayer_lifetime_pipe_owns_process_group_and_stops_all_processes(r2h_binary):
    port = find_free_port()
    environment = os.environ.copy()
    environment[_LIFETIME_ENV] = "1"
    process = subprocess.Popen(
        [str(r2h_binary), "-C", "-l", str(port), "-v", "4", "-w", "1"],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=environment,
    )
    worker_pid = None
    try:
        worker_pid = _worker_pid(port)
        assert os.getpgid(process.pid) == process.pid
        assert os.getpgid(worker_pid) == process.pid

        assert process.stdin is not None
        process.stdin.close()

        assert process.wait(timeout=6) == 0
        _wait_for_exit(worker_pid)
    finally:
        _stop_process(process, worker_pid)


def test_closed_stdin_does_not_stop_server_without_refplayer_lifetime_opt_in(r2h_binary):
    port = find_free_port()
    environment = os.environ.copy()
    environment.pop(_LIFETIME_ENV, None)
    process = subprocess.Popen(
        [str(r2h_binary), "-C", "-l", str(port), "-v", "4", "-w", "1"],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=environment,
    )
    worker_pid = None
    try:
        assert process.stdin is not None
        process.stdin.close()
        worker_pid = _worker_pid(port)
        time.sleep(0.2)
        assert process.poll() is None
        assert os.getpgid(worker_pid) == os.getpgid(process.pid)
    finally:
        _stop_process(process, worker_pid)


def test_refplayer_worker_exits_if_supervisor_is_killed(r2h_binary):
    port = find_free_port()
    environment = os.environ.copy()
    environment[_LIFETIME_ENV] = "1"
    process = subprocess.Popen(
        [str(r2h_binary), "-C", "-l", str(port), "-v", "4", "-w", "1"],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=environment,
    )
    worker_pid = None
    try:
        worker_pid = _worker_pid(port)
        assert os.getpgid(process.pid) == process.pid
        assert os.getpgid(worker_pid) == process.pid

        os.kill(process.pid, signal.SIGKILL)
        assert process.wait(timeout=2) == -signal.SIGKILL
        _wait_for_exit(worker_pid, timeout=3)
    finally:
        _stop_process(process, worker_pid)
