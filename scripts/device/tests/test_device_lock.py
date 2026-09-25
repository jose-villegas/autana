import contextlib
import errno
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

DEVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEVICE))
import device_lock
import device_hook


def setUpModule():
    global saved_hook
    saved_hook = os.environ.pop("AUTANA_LOCK_HOOK", None)


def tearDownModule():
    if saved_hook is not None:
        os.environ["AUTANA_LOCK_HOOK"] = saved_hook


class Clock:
    def __init__(self):
        self.value = 1000.0

    def now(self):
        return self.value

    def advance(self, seconds):
        self.value += seconds


class LivenessTests(unittest.TestCase):
    """process_alive() decides whether one agent may take the board from
    another, so every case that is not a proven absence must answer alive."""

    def kill_raising(self, error):
        def fake(unused_pid, unused_signal):
            raise error

        return fake

    def test_posix_a_pid_that_does_not_exist_is_dead(self):
        self.assertFalse(device_lock.posix_process_alive(
            4242, self.kill_raising(ProcessLookupError())))

    def test_posix_a_process_we_may_not_query_is_alive(self):
        self.assertTrue(device_lock.posix_process_alive(
            4242, self.kill_raising(PermissionError(errno.EPERM, "denied"))))

    def test_posix_an_unexpected_error_answers_alive(self):
        self.assertTrue(device_lock.posix_process_alive(
            4242, self.kill_raising(OSError(errno.EIO, "io"))))

    def detached_sleeper(self):
        """A live process on a DIFFERENT console from this one - the case an
        agent under Git Bash and one under PowerShell or Codex are in."""
        flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"],
                                 creationflags=flags, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: (child.kill(), child.wait()))
        time.sleep(0.5)
        return child

    @unittest.skipUnless(os.name == "nt", "Windows process table")
    def test_windows_a_live_process_on_another_console_is_alive(self):
        child = self.detached_sleeper()
        self.assertTrue(device_lock.process_alive(child.pid))

    @unittest.skipUnless(os.name == "nt", "Windows process table")
    def test_windows_never_probes_liveness_with_os_kill(self):
        # Signal 0 is CTRL_C_EVENT on Windows: probing with it can deliver
        # Ctrl+C to a process sharing this console.
        child = self.detached_sleeper()

        def refuse(unused_pid, unused_signal):
            raise AssertionError("os.kill used as a liveness probe on Windows")

        real = device_lock.os.kill
        device_lock.os.kill = refuse
        self.addCleanup(setattr, device_lock.os, "kill", real)
        device_lock.process_alive(child.pid)

    @unittest.skipUnless(os.name == "nt", "Windows process table")
    def test_windows_an_exited_process_is_dead(self):
        child = self.detached_sleeper()
        child.kill()
        child.wait()
        self.assertFalse(device_lock.process_alive(child.pid))

    @unittest.skipUnless(os.name == "nt", "Windows process table")
    def test_windows_a_pid_that_never_existed_is_dead(self):
        self.assertFalse(device_lock.process_alive(0x7FFFFFF0))


class LockTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.clock = Clock()
        self.lock = device_lock.LockStore(
            Path(self.temp.name), self.clock.now,
            lambda pid: pid in (1, device_lock.os.getpid()))

    def tearDown(self):
        self.temp.cleanup()

    def test_acquire_heartbeat_and_release(self):
        held = self.lock.acquire("COM5", "one", "flash", "dev")
        self.assertEqual(held["owner"], "one")
        self.clock.advance(3)
        self.assertTrue(self.lock.heartbeat("COM5", held["token"]))
        self.assertEqual(self.lock.status("COM5")["lock"]["heartbeat_at"],
                         1003.0)
        self.assertTrue(self.lock.release("COM5", held["token"]))
        self.assertIsNone(self.lock.status("COM5")["lock"])

    def test_acquired_event(self):
        with mock.patch.dict(os.environ, {"AUTANA_LOCK_HOOK": "echo hook"}), \
                mock.patch.object(device_hook.subprocess, "run",
                                  return_value=subprocess.CompletedProcess([], 0)) as run, \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            self.lock.acquire("COM5", "one", "flash")
        self.assertEqual(run.call_args.kwargs["env"]["AUTANA_LOCK_EVENT"], "acquired")
        self.assertEqual(stderr.getvalue(), "")

    def test_state_events_once_with_facts(self):
        with mock.patch.object(device_hook, "emit") as emit:
            held = self.lock.acquire("COM5", "one", "flash")
            self.lock.heartbeat("COM5", held["token"])
            self.lock.release("COM5", "wrong")
            self.lock.release("COM5", held["token"])
            self.lock.set_human("COM5", "person", "panel")
            self.lock.clear_human("COM5")
            self.lock.clear_human("COM5")
            self.lock.heartbeat("COM5", held["token"])
        self.assertEqual(emit.call_args_list, [
            mock.call("acquired", "COM5", "one", "flash"),
            mock.call("released", "COM5", "one", "flash"),
            mock.call("human-reserved", "COM5", "person", note="panel"),
            mock.call("human-cleared", "COM5", "person", note="panel"),
        ])

    def test_waiting_once_over_polls_and_reclaim_acquires(self):
        held = self.lock.acquire("COM5", "one", "listen")
        with mock.patch.object(device_hook, "emit") as emit, \
                mock.patch.object(device_lock.time, "sleep",
                                  side_effect=lambda seconds: self.clock.advance(seconds)):
            self.assertIsNone(self.lock.acquire("COM5", "two", "flash", wait=0.3))
            self.clock.advance(601)
            reclaimed = self.lock.acquire("COM5", "three", "send")
        self.assertIsNotNone(reclaimed)
        self.assertNotEqual(reclaimed["token"], held["token"])
        self.assertEqual(emit.call_args_list, [
            mock.call("waiting", "COM5", "two", "flash"),
            mock.call("gave-up", "COM5", "two", "flash"),
            mock.call("lost", "COM5", "one", "listen", note="heartbeat expiry"),
            mock.call("acquired", "COM5", "three", "send"),
        ])

    def test_unset_hook_runs_nothing(self):
        with mock.patch.object(device_hook.subprocess, "run") as run:
            self.lock.acquire("COM5", "one", "flash")
        run.assert_not_called()

    def test_empty_hook_runs_nothing(self):
        with mock.patch.dict(os.environ, {"AUTANA_LOCK_HOOK": ""}), \
                mock.patch.object(device_hook.subprocess, "run") as run, \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            self.lock.acquire("COM5", "one", "flash")
        run.assert_not_called()
        self.assertEqual(stderr.getvalue(), "")

    def test_real_sleeping_hook_times_out(self):
        command = f'"{sys.executable}" -c "import time; time.sleep(10)"'
        with mock.patch.dict(os.environ, {"AUTANA_LOCK_HOOK": command}), \
                mock.patch.object(device_hook, "HOOK_TIMEOUT_SECONDS", 0.2), \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            start = time.monotonic()
            held = self.lock.acquire("COM5", "one", "flash")
            elapsed = time.monotonic() - start
        self.assertIsNotNone(held)
        self.assertLess(elapsed, 1.2)
        self.assertEqual(stderr.getvalue().count("warning: device lock hook failed:"), 1)

    def test_missing_command_warns_once_and_keeps_result(self):
        with mock.patch.dict(os.environ, {"AUTANA_LOCK_HOOK":
                                      "autana-hook-command-does-not-exist-88219"}), \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            held = self.lock.acquire("COM5", "one", "flash")
        self.assertIsNotNone(held)
        self.assertEqual(stderr.getvalue().count("warning: device lock hook failed:"), 1)

    def test_hook_output_is_hidden_from_caller(self):
        caller = "import device_hook; device_hook.emit('acquired', 'COM5')"
        for status in (0, 7):
            with self.subTest(status=status):
                command = (f'"{sys.executable}" -c "import sys; '
                           f'print(12345); print(67890, file=sys.stderr); sys.exit({status})"')
                environment = os.environ.copy()
                environment["AUTANA_LOCK_HOOK"] = command
                environment["PYTHONPATH"] = str(DEVICE)
                result = subprocess.run([sys.executable, "-c", caller],
                                        env=environment, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertNotIn("12345", result.stderr)
                self.assertNotIn("67890", result.stderr)
                self.assertEqual(result.stderr.count("warning: device lock hook failed:"),
                                 0 if status == 0 else 1)

    def test_suite_ignores_inherited_hook(self):
        if os.environ.get("AUTANA_HOOK_SUITE_CHILD"):
            self.skipTest("suite child")
        sentinel = Path(self.temp.name) / "sentinel"
        command = (f'"{sys.executable}" -c "import os; '
                   "open(os.environ['AUTANA_HOOK_SENTINEL'], 'w').close()\"")
        environment = os.environ.copy()
        environment.update({"AUTANA_LOCK_HOOK": command,
                            "AUTANA_HOOK_SENTINEL": str(sentinel),
                            "AUTANA_HOOK_SUITE_CHILD": "1"})
        result = subprocess.run([sys.executable, "-m", "unittest", "discover",
                                 "-s", str(DEVICE / "tests"), "-p", "test_device_lock.py"],
                                env=environment, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr[-1000:])
        self.assertFalse(sentinel.exists())

    def test_hook_errors_leave_lock_operation_successful(self):
        for result in (subprocess.CompletedProcess([], 4),
                       subprocess.TimeoutExpired("hook", 3), RuntimeError()):
            with self.subTest(result=result), mock.patch.dict(
                    os.environ, {"AUTANA_LOCK_HOOK": "missing-command"}), \
                    mock.patch.object(device_hook.subprocess, "run") as run, \
                    contextlib.redirect_stderr(io.StringIO()) as stderr:
                if isinstance(result, BaseException):
                    run.side_effect = result
                else:
                    run.return_value = result
                held = self.lock.acquire("COM5", "one", "flash")
            self.assertIsNotNone(held)
            self.assertEqual(stderr.getvalue().count("\n"), 1)
            self.assertIn("warning: device lock hook failed:", stderr.getvalue())
            self.assertEqual(run.call_args.kwargs["timeout"], device_hook.HOOK_TIMEOUT_SECONDS)
            self.lock.release("COM5", held["token"])

    def test_real_hook_records_environment(self):
        output = Path(self.temp.name) / "events.txt"
        command = (f'"{sys.executable}" -c "import os; '
                   "print('|'.join(os.environ[k] for k in "
                   "('AUTANA_LOCK_EVENT','AUTANA_LOCK_PORT','AUTANA_LOCK_OWNER',"
                   "'AUTANA_LOCK_PURPOSE','AUTANA_LOCK_NOTE')), "
                   "file=open(os.environ['AUTANA_LOCK_LOG'],'a'))\"")
        with mock.patch.dict(os.environ, {"AUTANA_LOCK_HOOK": command,
                                          "AUTANA_LOCK_LOG": str(output)}):
            held = self.lock.acquire("COM5", "one", "flash")
            self.lock.release("COM5", held["token"])
            self.lock.set_human("COM5", "person", "panel")
            self.lock.clear_human("COM5")
        self.assertEqual(output.read_text().splitlines(), [
            "acquired|COM5|one|flash|", "released|COM5|one|flash|",
            "human-reserved|COM5|person||panel", "human-cleared|COM5|person||panel",
        ])

    def test_waiters_are_fifo(self):
        first = self.lock.enqueue("COM5", "one", "run")
        second = self.lock.enqueue("COM5", "two", "flash")
        self.assertIsNone(self.lock.claim("COM5", second, ""))
        held = self.lock.claim("COM5", first, "")
        self.assertEqual(held["owner"], "one")
        self.lock.release("COM5", held["token"])
        held = self.lock.claim("COM5", second, "")
        self.assertEqual(held["owner"], "two")

    def test_stale_lock_is_reclaimed_with_record(self):
        old = self.lock.acquire("COM5", "lost", "listen", "")
        self.clock.advance(601)
        ticket = self.lock.enqueue("COM5", "next", "flash")
        held = self.lock.claim("COM5", ticket, "", stale_seconds=600)
        self.assertEqual(held["owner"], "next")
        self.assertIn("reclaimed lock from lost for listen (heartbeat expiry)",
                      held["log"])
        self.assertNotEqual(old["token"], held["token"])

    def test_reclaim_emits_lost_before_acquired(self):
        self.lock.acquire("COM5", "old", "listen")
        self.clock.advance(601)
        with mock.patch.object(device_hook, "emit") as emit:
            held = self.lock.acquire("COM5", "new", "flash")
        self.assertIsNotNone(held)
        self.assertEqual(emit.call_args_list, [
            mock.call("lost", "COM5", "old", "listen", note="heartbeat expiry"),
            mock.call("acquired", "COM5", "new", "flash"),
        ])

    def test_wait_zero_emits_no_event(self):
        self.lock.acquire("COM5", "old", "listen")
        with mock.patch.object(device_hook, "emit") as emit:
            self.assertIsNone(self.lock.acquire("COM5", "new", "flash", wait=0))
        emit.assert_not_called()

    def test_wait_timeout_emits_waiting_then_gave_up(self):
        self.lock.acquire("COM5", "old", "listen")
        with mock.patch.object(device_hook, "emit") as emit, \
                mock.patch.object(device_lock.time, "sleep",
                                  side_effect=lambda seconds: self.clock.advance(seconds)):
            self.assertIsNone(self.lock.acquire("COM5", "new", "flash", wait=0.3))
        self.assertEqual(emit.call_args_list, [
            mock.call("waiting", "COM5", "new", "flash"),
            mock.call("gave-up", "COM5", "new", "flash"),
        ])

    def test_wait_then_win_emits_waiting_then_acquired(self):
        old = self.lock.acquire("COM5", "old", "listen")

        def release_and_advance(seconds):
            self.lock.release("COM5", old["token"])
            self.clock.advance(seconds)

        with mock.patch.object(device_hook, "emit") as emit, \
                mock.patch.object(device_lock.time, "sleep", side_effect=release_and_advance):
            held = self.lock.acquire("COM5", "new", "flash", wait=0.3)
        self.assertIsNotNone(held)
        self.assertEqual(emit.call_args_list, [
            mock.call("waiting", "COM5", "new", "flash"),
            mock.call("released", "COM5", "old", "listen"),
            mock.call("acquired", "COM5", "new", "flash"),
        ])

    def test_cancelled_wait_emits_gave_up(self):
        self.lock.acquire("COM5", "old", "listen")

        def cancel_ticket(seconds):
            ticket = self.lock.tickets("COM5")[0]["ticket"]
            self.lock.cancel("COM5", ticket)
            self.clock.advance(seconds)

        with mock.patch.object(device_hook, "emit") as emit, \
                mock.patch.object(device_lock.time, "sleep", side_effect=cancel_ticket):
            self.assertIsNone(self.lock.acquire("COM5", "new", "flash", wait=0.3))
        self.assertEqual(emit.call_args_list, [
            mock.call("waiting", "COM5", "new", "flash"),
            mock.call("gave-up", "COM5", "new", "flash"),
        ])

    def test_events_run_without_guard(self):
        def check_guard(event, port, owner="", purpose="", note=""):
            self.assertFalse(self.lock.guard_path(port).exists(), event)

        with mock.patch.object(device_hook, "emit", side_effect=check_guard) as emit:
            old = self.lock.acquire("COM5", "old", "listen")
            self.clock.advance(601)
            new = self.lock.acquire("COM5", "new", "flash")
            self.lock.release("COM5", new["token"])
            self.lock.set_human("COM5", "person", "panel")
            self.lock.clear_human("COM5")
            self.lock.acquire("COM5", "last", "listen")
            with mock.patch.object(device_lock.time, "sleep",
                                  side_effect=lambda seconds: self.clock.advance(seconds)):
                self.lock.acquire("COM5", "waiter", "flash", wait=0.2)
        self.assertEqual({call.args[0] for call in emit.call_args_list},
                         {"acquired", "lost", "released", "human-reserved",
                          "human-cleared", "waiting", "gave-up"})

    def test_dead_lock_holder_on_this_host_is_reclaimed(self):
        ticket = self.lock.enqueue("COM5", "dead", "flash", pid=1)
        self.lock.write_json(self.lock.lock_path("COM5"), {
            "acquired_at": 1000, "heartbeat_at": 1000,
            "expected_build_id": "", "host": device_lock.socket.gethostname(),
            "owner": "dead", "pid": 99, "port": "COM5", "purpose": "flash",
            "token": "old",
        })
        held = self.lock.claim("COM5", ticket, "")
        self.assertIn("reclaimed lock from dead for flash (dead process)",
                      held["log"])

    def test_live_lock_holder_is_not_reclaimed(self):
        ticket = self.lock.enqueue("COM5", "live", "flash", pid=1)
        self.lock.write_json(self.lock.lock_path("COM5"), {
            "acquired_at": 1000, "heartbeat_at": 1000,
            "expected_build_id": "", "host": device_lock.socket.gethostname(),
            "owner": "live", "pid": 1, "port": "COM5", "purpose": "flash",
            "token": "old",
        })
        self.assertIsNone(self.lock.claim("COM5", ticket, "", stale_seconds=600))

    def test_dead_lock_holder_on_other_host_is_not_reclaimed_by_pid(self):
        ticket = self.lock.enqueue("COM5", "remote", "flash", pid=1)
        self.lock.write_json(self.lock.lock_path("COM5"), {
            "acquired_at": 1000, "heartbeat_at": 1000,
            "expected_build_id": "", "host": "another-host",
            "owner": "remote", "pid": 99, "port": "COM5", "purpose": "flash",
            "token": "old",
        })
        self.assertIsNone(self.lock.claim("COM5", ticket, "", stale_seconds=600))

    def write_holder(self, pid, host=None):
        self.lock.write_json(self.lock.lock_path("COM5"), {
            "acquired_at": 1000, "heartbeat_at": 1000,
            "expected_build_id": "",
            "host": host or device_lock.socket.gethostname(),
            "owner": "gone", "pid": pid, "port": "COM5", "purpose": "screenshot",
            "token": "old",
        })

    def printed_status(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            device_lock.print_status(self.lock.status("COM5"))
        return out.getvalue()

    def test_status_does_not_report_a_dead_holder_as_holding_the_board(self):
        self.write_holder(pid=99)
        self.assertIsNone(self.lock.status("COM5")["lock"])
        printed = self.printed_status()
        self.assertNotIn("held by", printed)
        self.assertIn("stale lock from gone for screenshot (dead process)", printed)

    def test_status_does_not_report_an_expired_heartbeat_as_holding_the_board(self):
        self.write_holder(pid=1)
        self.clock.advance(device_lock.DEFAULT_STALE_SECONDS + 1)
        self.assertIsNone(self.lock.status("COM5")["lock"])
        self.assertIn("(heartbeat expiry)", self.printed_status())

    def test_status_still_reports_a_live_holder(self):
        self.write_holder(pid=1)
        self.assertEqual(self.lock.status("COM5")["lock"]["owner"], "gone")
        self.assertTrue(self.printed_status().startswith("held by gone for screenshot"))

    def test_status_leaves_reclaiming_a_dead_holder_to_claim(self):
        self.write_holder(pid=99)
        self.lock.status("COM5")
        self.assertTrue(self.lock.lock_path("COM5").exists())
        ticket = self.lock.enqueue("COM5", "next", "screenshot", pid=1)
        self.assertIn("(dead process)", self.lock.claim("COM5", ticket, "")["log"])

    def test_crashed_waiter_does_not_block_queue(self):
        ticket = self.lock.enqueue("COM5", "crashed", "flash", pid=99)
        self.assertTrue((self.lock.queue_dir("COM5") / (ticket + ".json")).exists())
        next_ticket = self.lock.enqueue("COM5", "next", "listen", pid=1)
        held = self.lock.claim("COM5", next_ticket, "")
        self.assertEqual(held["owner"], "next")

    def test_human_reservation_blocks_acquisition(self):
        self.lock.set_human("COM5", "maintainer", "checking display")
        ticket = self.lock.enqueue("COM5", "agent", "flash")
        self.assertIsNone(self.lock.claim("COM5", ticket, ""))
        self.assertEqual(self.lock.status("COM5")["human"]["note"],
                         "checking display")

    def test_timed_out_acquire_removes_its_ticket(self):
        self.lock.set_human("COM5", "maintainer", "checking display")
        self.assertIsNone(self.lock.acquire("COM5", "agent", "flash", wait=0))
        self.assertEqual(self.lock.status("COM5")["queue"], [])


if __name__ == "__main__":
    unittest.main()
