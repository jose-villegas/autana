import isolation  # noqa: F401  (first: keeps the suite out of real records)
import contextlib
import errno
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
import lock_scope

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
        held = self.lock.acquire("COM5", "one", "flash")
        self.assertEqual(held["owner"], "one")
        self.clock.advance(3)
        self.assertTrue(self.lock.heartbeat("COM5", held["token"]))
        self.assertEqual(self.lock.status("COM5")["lock"]["heartbeat_at"],
                         1003.0)
        self.assertTrue(self.lock.release("COM5", held["token"]))
        self.assertIsNone(self.lock.status("COM5")["lock"])

    def test_acquired_event(self):
        with isolation.project(lock_hook="echo hook"), \
                mock.patch.object(device_hook.subprocess, "run",
                                  return_value=subprocess.CompletedProcess([], 0)) as run, \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            self.lock.acquire("COM5", "one", "flash")
        self.assertEqual(run.call_args.args[0], "echo hook")
        self.assertEqual(run.call_args.kwargs["env"]["AUTANA_LOCK_EVENT"], "acquired")
        self.assertEqual(stderr.getvalue(), "")

    def test_a_hook_in_the_environment_is_ignored(self):
        with mock.patch.dict(os.environ, {"AUTANA_LOCK_HOOK": "echo hook"}), \
                mock.patch.object(device_hook.subprocess, "run") as run:
            self.lock.acquire("COM5", "one", "flash")
        run.assert_not_called()

    def test_a_projects_hook_fires_only_for_a_command_run_from_it(self):
        with isolation.project(lock_hook="echo hook"), \
                mock.patch.object(device_hook.subprocess, "run") as run:
            self.lock.acquire("COM5", "one", "flash")
        self.assertEqual(run.call_count, 1)
        with isolation.project(), mock.patch.object(device_hook.subprocess, "run") as run:
            self.lock.release("COM5", self.lock.status("COM5")["lock"]["token"])
        run.assert_not_called()

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
            mock.call("acquired", "COM5", "one", "autana flash", note=""),
            mock.call("released", "COM5", "one", "autana flash"),
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
            mock.call("waiting", "COM5", "two", "autana flash"),
            mock.call("gave-up", "COM5", "two", "autana flash"),
            mock.call("acquired", "COM5", "three", "autana send",
                      note="reclaimed from one (heartbeat expiry)"),
        ])

    def test_unset_hook_runs_nothing(self):
        with mock.patch.object(device_hook.subprocess, "run") as run:
            self.lock.acquire("COM5", "one", "flash")
        run.assert_not_called()

    def test_empty_hook_runs_nothing(self):
        with isolation.project(lock_hook=""), \
                mock.patch.object(device_hook.subprocess, "run") as run, \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            self.lock.acquire("COM5", "one", "flash")
        run.assert_not_called()
        self.assertEqual(stderr.getvalue(), "")

    def test_real_sleeping_hook_times_out(self):
        # The bound is the hook's own sleep, not a guess at how fast this
        # machine starts a process: a loaded one takes seconds to, and only
        # a lock that waited the hook out would take the whole minute.
        command = f'"{sys.executable}" -c "import time; time.sleep(60)"'
        with isolation.project(lock_hook=command), \
                mock.patch.object(device_hook, "HOOK_TIMEOUT_SECONDS", 0.2), \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            start = time.monotonic()
            held = self.lock.acquire("COM5", "one", "flash")
            elapsed = time.monotonic() - start
        self.assertIsNotNone(held)
        self.assertLess(elapsed, 30)
        self.assertEqual(stderr.getvalue(), "")

    def test_missing_command_warns_once_and_keeps_result(self):
        with isolation.project(lock_hook="autana-hook-command-does-not-exist-88219"), \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            held = self.lock.acquire("COM5", "one", "flash")
        self.assertIsNotNone(held)
        self.assertEqual(stderr.getvalue(), "")

    def test_hook_output_is_hidden_from_caller(self):
        caller = "import device_hook; device_hook.emit('acquired', 'COM5')"
        for status in (0, 7):
            with self.subTest(status=status):
                command = (f'"{sys.executable}" -c "import sys; '
                           f'print(12345); print(67890, file=sys.stderr); sys.exit({status})"')
                environment = os.environ.copy()
                environment["PYTHONPATH"] = str(DEVICE)
                with isolation.project(lock_hook=command) as project:
                    environment["_AUTANA_PROJECT"] = str(project)
                    result = subprocess.run([sys.executable, "-c", caller],
                                            env=environment, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertNotIn("12345", result.stderr)
                self.assertNotIn("67890", result.stderr)
                self.assertEqual(result.stderr.count("warning: device lock hook failed:"),
                                 0)

    def test_suite_ignores_the_hook_of_the_project_it_was_started_in(self):
        if os.environ.get("_AUTANA_HOOK_SUITE_CHILD"):
            self.skipTest("suite child")
        sentinel = Path(self.temp.name) / "sentinel"
        command = (f'"{sys.executable}" -c "import os; '
                   "open(os.environ['_AUTANA_HOOK_SENTINEL'], 'w').close()\"")
        with isolation.project(lock_hook=command) as project:
            environment = os.environ.copy()
            environment.update({"_AUTANA_PROJECT": str(project),
                                "_AUTANA_HOOK_SENTINEL": str(sentinel),
                                "_AUTANA_HOOK_SUITE_CHILD": "1"})
            result = subprocess.run([sys.executable, "-m", "unittest", "discover",
                                     "-s", str(DEVICE / "tests"), "-p", "test_device_lock.py"],
                                    env=environment, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr[-1000:])
        self.assertFalse(sentinel.exists())

    def test_hook_errors_leave_lock_operation_successful(self):
        for result in (subprocess.CompletedProcess([], 4),
                       subprocess.TimeoutExpired("hook", 3), RuntimeError()):
            with self.subTest(result=result), isolation.project(lock_hook="missing-command"), \
                    mock.patch.object(device_hook.subprocess, "run") as run, \
                    contextlib.redirect_stderr(io.StringIO()) as stderr:
                if isinstance(result, BaseException):
                    run.side_effect = result
                else:
                    run.return_value = result
                held = self.lock.acquire("COM5", "one", "flash")
            self.assertIsNotNone(held)
            self.assertEqual(stderr.getvalue(), "")
            self.assertEqual(run.call_args.kwargs["timeout"], device_hook.HOOK_TIMEOUT_SECONDS)
            self.lock.release("COM5", held["token"])

    def test_real_hook_records_environment(self):
        output = Path(self.temp.name) / "events.txt"
        command = (f'"{sys.executable}" -c "import os; '
                   "print('|'.join(os.environ[k] for k in "
                   "('AUTANA_LOCK_EVENT','AUTANA_LOCK_BOARD','AUTANA_LOCK_OWNER',"
                   "'AUTANA_LOCK_PURPOSE','AUTANA_LOCK_NOTE')), "
                   "file=open(os.environ['_AUTANA_TEST_HOOK_LOG'],'a'))\"")
        with isolation.project(lock_hook=command), \
                mock.patch.dict(os.environ, {"_AUTANA_TEST_HOOK_LOG": str(output)}):
            held = self.lock.acquire("COM5", "one", "flash")
            self.lock.release("COM5", held["token"])
            self.lock.set_human("COM5", "person", "panel")
            self.lock.clear_human("COM5")
        self.assertEqual(output.read_text().splitlines(), [
            "acquired|COM5|one|autana flash|", "released|COM5|one|autana flash|",
            "human-reserved|COM5|person||panel", "human-cleared|COM5|person||panel",
        ])

    def test_waiters_are_fifo(self):
        first = self.lock.enqueue("COM5", "one", "run")
        second = self.lock.enqueue("COM5", "two", "flash")
        self.assertIsNone(self.lock.claim("COM5", second))
        held = self.lock.claim("COM5", first)
        self.assertEqual(held["owner"], "one")
        self.lock.release("COM5", held["token"])
        held = self.lock.claim("COM5", second)
        self.assertEqual(held["owner"], "two")

    def test_stale_lock_is_reclaimed_with_record(self):
        old = self.lock.acquire("COM5", "lost", "listen")
        self.clock.advance(601)
        ticket = self.lock.enqueue("COM5", "next", "flash")
        held = self.lock.claim("COM5", ticket, stale_seconds=600)
        self.assertEqual(held["owner"], "next")
        self.assertIn("reclaimed lock from lost for autana listen (heartbeat expiry)",
                      held.log)
        self.assertNotEqual(old["token"], held["token"])

    def test_a_reclaim_is_one_acquired_event_naming_who_it_was_taken_from(self):
        self.lock.acquire("COM5", "old", "listen")
        self.clock.advance(601)
        with mock.patch.object(device_hook, "emit") as emit:
            held = self.lock.acquire("COM5", "new", "flash")
        self.assertIsNotNone(held)
        self.assertEqual(emit.call_args_list, [
            mock.call("acquired", "COM5", "new", "autana flash",
                      note="reclaimed from old (heartbeat expiry)"),
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
            mock.call("waiting", "COM5", "new", "autana flash"),
            mock.call("gave-up", "COM5", "new", "autana flash"),
        ])

    def test_a_first_claim_that_uses_the_whole_wait_still_announces_it(self):
        self.lock.acquire("COM5", "old", "listen")
        claim = self.lock.claim
        heard = []

        def slow_claim(*args, **kwargs):
            self.clock.advance(1)
            return claim(*args, **kwargs)

        with mock.patch.object(device_hook, "emit") as emit, \
                mock.patch.object(self.lock, "claim", side_effect=slow_claim), \
                mock.patch.object(device_lock.time, "sleep"):
            self.assertIsNone(self.lock.acquire("COM5", "new", "flash", wait=0.2,
                                                on_wait=heard.append))
        self.assertEqual(len(heard), 1)
        self.assertEqual(emit.call_args_list, [
            mock.call("waiting", "COM5", "new", "autana flash"),
            mock.call("gave-up", "COM5", "new", "autana flash"),
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
            mock.call("waiting", "COM5", "new", "autana flash"),
            mock.call("released", "COM5", "old", "autana listen"),
            mock.call("acquired", "COM5", "new", "autana flash", note=""),
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
            mock.call("waiting", "COM5", "new", "autana flash"),
            mock.call("gave-up", "COM5", "new", "autana flash"),
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
                         {"acquired", "released", "human-reserved",
                          "human-cleared", "waiting", "gave-up"})

    def test_dead_lock_holder_on_this_host_is_reclaimed(self):
        ticket = self.lock.enqueue("COM5", "dead", "flash", pid=1)
        self.lock.write_json(self.lock.lock_path("COM5"), {
            "acquired_at": 1000, "heartbeat_at": 1000,
            "expected_build_id": "", "host": device_lock.socket.gethostname(),
            "owner": "dead", "pid": 99, "port": "COM5", "purpose": "flash",
            "token": "old", "protocol": device_lock.LOCK_PROTOCOL,
        })
        held = self.lock.claim("COM5", ticket)
        self.assertIn("reclaimed lock from dead for flash (dead process)",
                      held.log)

    def test_live_lock_holder_is_not_reclaimed(self):
        ticket = self.lock.enqueue("COM5", "live", "flash", pid=1)
        self.lock.write_json(self.lock.lock_path("COM5"), {
            "acquired_at": 1000, "heartbeat_at": 1000,
            "expected_build_id": "", "host": device_lock.socket.gethostname(),
            "owner": "live", "pid": 1, "port": "COM5", "purpose": "flash",
            "token": "old", "protocol": device_lock.LOCK_PROTOCOL,
        })
        self.assertIsNone(self.lock.claim("COM5", ticket, stale_seconds=600))

    def test_dead_lock_holder_on_other_host_is_not_reclaimed_by_pid(self):
        ticket = self.lock.enqueue("COM5", "remote", "flash", pid=1)
        self.lock.write_json(self.lock.lock_path("COM5"), {
            "acquired_at": 1000, "heartbeat_at": 1000,
            "expected_build_id": "", "host": "another-host",
            "owner": "remote", "pid": 99, "port": "COM5", "purpose": "flash",
            "token": "old", "protocol": device_lock.LOCK_PROTOCOL,
        })
        self.assertIsNone(self.lock.claim("COM5", ticket, stale_seconds=600))

    def write_holder(self, pid, host=None):
        self.lock.write_json(self.lock.lock_path("COM5"), {
            "acquired_at": 1000, "heartbeat_at": 1000,
            "expected_build_id": "",
            "host": host or device_lock.socket.gethostname(),
            "owner": "gone", "pid": pid, "port": "COM5", "purpose": "screenshot",
            "token": "old", "protocol": device_lock.LOCK_PROTOCOL,
        })

    def printed_status(self):
        return "\n".join(device_lock.status_lines(
            device_lock.status_entry(self.lock, "COM5", durations={}), self.clock.now()))

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
        self.assertIn("(dead process)", self.lock.claim("COM5", ticket).log)

    def test_crashed_waiter_does_not_block_queue(self):
        ticket = self.lock.enqueue("COM5", "crashed", "flash", pid=99)
        self.assertTrue((self.lock.queue_dir("COM5") / (ticket + ".json")).exists())
        next_ticket = self.lock.enqueue("COM5", "next", "listen", pid=1)
        held = self.lock.claim("COM5", next_ticket)
        self.assertEqual(held["owner"], "next")

    def test_human_reservation_blocks_acquisition(self):
        self.lock.set_human("COM5", "maintainer", "checking display")
        ticket = self.lock.enqueue("COM5", "agent", "flash")
        self.assertIsNone(self.lock.claim("COM5", ticket))
        self.assertEqual(self.lock.status("COM5")["human"]["note"],
                         "checking display")

    def test_timed_out_acquire_removes_its_ticket(self):
        self.lock.set_human("COM5", "maintainer", "checking display")
        self.assertIsNone(self.lock.acquire("COM5", "agent", "flash", wait=0))
        self.assertEqual(self.lock.status("COM5")["queue"], [])


class HumanReservationExpiryTests(unittest.TestCase):
    """A person's reservation has no heartbeat, so it lapses after
    HUMAN_RESERVATION_SECONDS unless the same owner reserves again."""

    HOUR = 3600

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.clock = Clock()
        self.lock = device_lock.LockStore(
            Path(self.temp.name), self.clock.now,
            lambda pid: pid in (1, device_lock.os.getpid()))

    def tearDown(self):
        self.temp.cleanup()

    def claim(self, owner="agent"):
        ticket = self.lock.enqueue("COM5", owner, "flash", pid=1)
        held = self.lock.claim("COM5", ticket)
        if held is None:
            self.lock.cancel("COM5", ticket)
        return held

    def test_reserving_says_whether_it_renewed(self):
        self.assertEqual(self.lock.set_human("COM5", "maintainer", "bench")[1], False)
        self.assertEqual(self.lock.set_human("COM5", "maintainer", "bench")[1], True)

    def test_the_lifetime_is_one_hour(self):
        self.assertEqual(device_lock.HUMAN_RESERVATION_SECONDS, self.HOUR)

    def test_a_reservation_just_short_of_an_hour_still_blocks(self):
        self.lock.set_human("COM5", "maintainer", "bench")
        self.clock.advance(self.HOUR - 1)
        self.assertIsNone(self.claim())
        self.assertEqual(self.lock.status("COM5")["human"]["owner"], "maintainer")

    def test_an_hour_old_reservation_is_released_and_the_board_is_claimed(self):
        self.lock.set_human("COM5", "maintainer", "bench")
        self.clock.advance(self.HOUR)
        held = self.claim()
        self.assertEqual(held["owner"], "agent")
        self.assertFalse(self.lock.human_path("COM5").exists())

    def test_a_lapsed_reservation_fires_no_event_of_its_own(self):
        self.lock.set_human("COM5", "maintainer", "bench")
        self.clock.advance(self.HOUR)
        with mock.patch.object(device_hook, "emit") as emit:
            self.claim()
        self.assertEqual(emit.call_args_list, [
            mock.call("acquired", "COM5", "agent", "autana flash", note=""),
        ])

    def test_status_treats_an_expired_reservation_as_released_and_says_so(self):
        self.lock.set_human("COM5", "maintainer", "bench")
        self.clock.advance(self.HOUR + 120)
        entry = device_lock.status_entry(self.lock, "COM5", durations={})
        self.assertEqual(entry["state"], "unlocked")
        self.assertEqual(entry["lapsed"], {"owner": "maintainer", "purpose": "bench",
                                           "reason": "reservation expired",
                                           "at": 1000.0 + self.HOUR})
        text = "\n".join(device_lock.status_lines(entry, self.clock.now()))
        self.assertIn("human reservation from maintainer: bench expired 2m ago and is released",
                      text)

    def test_status_shows_the_time_left(self):
        self.lock.set_human("COM5", "maintainer", "bench")
        self.clock.advance(1200)
        entry = device_lock.status_entry(self.lock, "COM5", durations={})
        self.assertEqual((entry["since"], entry["expires_at"]), (1000.0, 1000.0 + self.HOUR))
        self.assertIn("40m left", "\n".join(device_lock.status_lines(entry, self.clock.now())))

    def test_a_lapsed_reservation_still_shows_while_another_lock_is_held(self):
        self.lock.set_human("COM5", "maintainer", "bench")
        self.clock.advance(self.HOUR + 120)
        self.lock.write_json(self.lock.lock_path("COM5"), {
            "acquired_at": self.clock.now(), "heartbeat_at": self.clock.now(),
            "host": "another-host", "owner": "alice", "pid": 1, "port": "COM5",
            "purpose": "flash", "token": "t", "protocol": device_lock.LOCK_PROTOCOL})
        entry = device_lock.status_entry(self.lock, "COM5", durations={})
        self.assertEqual((entry["state"], entry["lapsed"]["reason"]),
                         ("held", "reservation expired"))

    def test_status_json_carries_timestamps_and_no_relative_durations(self):
        self.lock.set_human("COM5", "maintainer", "bench")
        self.clock.advance(self.HOUR + 120)
        lapsed = device_lock.status_entry(self.lock, "COM5", durations={})
        self.lock.set_human("COM5", "maintainer", "bench")
        held = device_lock.status_entry(self.lock, "COM5", durations={})
        derived = {"elapsed_seconds", "remaining_seconds", "ago_seconds"}
        self.assertFalse(derived & set(held) | derived & set(lapsed["lapsed"]))

    def test_reserving_again_renews_it(self):
        first, _ = self.lock.set_human("COM5", "maintainer", "bench")
        self.clock.advance(self.HOUR - 60)
        self.assertEqual(self.lock.set_human("COM5", "maintainer", "still at the bench")[0], first)
        self.clock.advance(self.HOUR - 60)
        self.assertIsNone(self.claim())
        human = self.lock.status("COM5")["human"]
        self.assertEqual((human["id"], human["note"]), (first, "still at the bench"))
        self.clock.advance(61)
        self.assertEqual(self.claim()["owner"], "agent")

    def test_renewing_keeps_the_original_start_and_announces_nothing_new(self):
        self.lock.set_human("COM5", "maintainer", "bench")
        self.clock.advance(300)
        with mock.patch.object(device_hook, "emit") as emit:
            self.lock.set_human("COM5", "maintainer", "bench")
        emit.assert_not_called()
        human = self.lock.status("COM5")["human"]
        self.assertEqual(human["since_at"], 1000.0)
        self.assertEqual(human["expires_at"], 1300.0 + self.HOUR)

    def test_a_later_hand_from_another_process_renews_under_its_own_name(self):
        first, _ = self.lock.set_human("COM5", "ville@bench:100", "bench")
        self.clock.advance(600)
        self.assertEqual(self.lock.set_human("COM5", "ville@bench:200", "bench")[0], first)
        human = self.lock.status("COM5")["human"]
        self.assertEqual((human["owner"], human["expires_at"]), ("ville@bench:200", 1600.0 + self.HOUR))

    def test_taking_back_then_reserving_is_a_new_reservation(self):
        first, _ = self.lock.set_human("COM5", "maintainer", "bench")
        self.lock.clear_human("COM5")
        self.assertNotEqual(self.lock.set_human("COM5", "maintainer", "bench")[0], first)

    def test_reserving_after_it_lapsed_starts_a_new_reservation(self):
        first, _ = self.lock.set_human("COM5", "maintainer", "bench")
        self.clock.advance(self.HOUR)
        with mock.patch.object(device_hook, "emit") as emit:
            second, _ = self.lock.set_human("COM5", "maintainer", "bench")
        self.assertNotEqual(second, first)
        self.assertEqual(emit.call_args_list, [
            mock.call("human-reserved", "COM5", "maintainer", note="bench"),
        ])

    def test_a_record_from_before_reservations_expired_lapses_an_hour_after_it_began(self):
        self.lock.write_json(self.lock.human_path("COM5"), {
            "board": "COM5", "id": "old", "note": "bench", "owner": "maintainer",
            "since_at": 1000.0, "protocol": 1})
        self.clock.advance(self.HOUR - 1)
        self.assertIsNone(self.claim())
        self.clock.advance(1)
        self.assertEqual(self.claim()["owner"], "agent")

    def test_take_back_still_clears_a_live_reservation(self):
        self.lock.set_human("COM5", "maintainer", "bench")
        self.lock.clear_human("COM5")
        self.assertEqual(self.claim()["owner"], "agent")

    def test_a_waiter_behind_an_expiring_reservation_gets_the_board(self):
        self.lock.set_human("COM5", "maintainer", "bench")
        with mock.patch.object(device_lock.time, "sleep",
                               side_effect=lambda seconds: self.clock.advance(self.HOUR)):
            held = self.lock.acquire("COM5", "agent", "flash", wait=2 * self.HOUR)
        self.assertEqual(held["owner"], "agent")


class LockRootTests(unittest.TestCase):
    """Where the lock lives must not depend on anything a job sets for itself."""

    def without_override(self, **environment):
        base = {key: value for key, value in os.environ.items()
                if key not in ("_AUTANA_DEVICE_LOCK_ROOT", "TMPDIR", "TEMP", "TMP")}
        return mock.patch.dict(os.environ, dict(base, **environment), clear=True)

    def test_an_override_names_the_root(self):
        with self.without_override(_AUTANA_DEVICE_LOCK_ROOT="/somewhere"):
            self.assertEqual(device_lock.default_root(), Path("/somewhere"))

    @unittest.skipIf(os.name == "nt", "Windows keeps the account's own temp folder")
    def test_linux_ignores_tmpdir(self):
        with self.without_override(TMPDIR="/tmp/job-1"):
            first = device_lock.default_root()
        with self.without_override(TMPDIR="/tmp/job-2"):
            second = device_lock.default_root()
        self.assertEqual(first, second)
        self.assertEqual(first, Path("/tmp") / f"autana-device-{os.getuid()}")

    @unittest.skipIf(os.name != "nt", "the Windows root is the account's %TEMP%")
    def test_windows_keeps_the_account_temp_folder(self):
        with self.without_override():
            self.assertEqual(device_lock.default_root(),
                             Path(tempfile.gettempdir()) / "autana-device")


class BusyTextTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.clock = Clock()
        self.lock = device_lock.LockStore(Path(self.temp.name), self.clock.now)

    def tearDown(self):
        self.temp.cleanup()

    def busy(self):
        return device_lock.busy_text(self.lock.status("COM5"), self.clock.now())

    def test_a_holder_is_named_with_its_purpose(self):
        self.lock.acquire("COM5", "alice@bench:9", "flash")
        self.assertIn("board held by alice@bench:9 for autana flash since", self.busy())

    def test_a_record_without_an_expiry_or_start_names_no_time_left(self):
        self.lock.write_json(self.lock.human_path("COM5"), {
            "board": "COM5", "id": "x", "note": "bench", "owner": "maintainer"})
        self.assertEqual(self.busy(), "board reserved by maintainer: bench "
                                      "(`autana lock take-back` ends it)")

    def test_a_record_from_before_expiry_shows_its_hour_not_zero(self):
        self.lock.write_json(self.lock.human_path("COM5"), {
            "board": "COM5", "id": "x", "note": "bench", "owner": "maintainer",
            "since_at": self.clock.now()})
        self.assertIn("(1h left;", self.busy())

    def test_a_reservation_is_named_with_the_time_left_and_the_way_out(self):
        self.lock.set_human("COM5", "maintainer", "bench")
        self.clock.advance(600)
        self.assertEqual(self.busy(), "board reserved by maintainer: bench (50m left; "
                                      "`autana lock take-back` ends it)")


class HumanReservationRealClockTests(unittest.TestCase):
    """The same rule against the real clock: no injected time, records
    written the way a lapsed hour leaves them."""

    BOARD = "90:70:69:FE:A3:08"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.lock = device_lock.LockStore(self.root)

    def tearDown(self):
        self.temp.cleanup()

    def status_text(self):
        now = self.lock.now()
        return "\n".join(device_lock.status_lines(
            device_lock.status_entry(self.lock, self.BOARD, now=now), now))

    def acquire(self):
        return self.lock.acquire(self.BOARD, "agent", "flash")

    def write_reservation(self, age_seconds):
        self.lock.write_json(self.lock.human_path(self.BOARD), {
            "board": self.BOARD, "id": "person", "note": "bench", "owner": "maintainer",
            "since_at": time.time() - age_seconds, "protocol": 1})

    def test_an_unnamed_owner_is_user_at_host_colon_pid_not_unknown(self):
        owner = device_lock.default_owner()
        self.assertRegex(owner, r"^.+@.+:\d+$")
        self.assertNotEqual(owner, "unknown")

    def test_an_hour_old_reservation_is_reported_released_and_does_not_block(self):
        self.write_reservation(3601)
        self.assertIn("expired", self.status_text())
        self.assertIsNotNone(self.acquire())

    def test_a_recent_reservation_blocks_and_shows_time_left(self):
        self.write_reservation(60)
        self.assertIn("left", self.status_text())
        self.assertIsNone(self.acquire())

    def test_reserving_again_renews_it(self):
        self.write_reservation(3000)
        self.assertRegex(self.status_text(), r"(9m|10m) left")
        self.lock.set_human(self.BOARD, "maintainer", "bench")
        self.assertRegex(self.status_text(), r"(59m|1h) left")
        self.assertIsNone(self.acquire())


class FakeScope:
    """What lock_scope answers, without an OS: name, start time, and any
    processes found besides the holder's own."""

    def __init__(self, start=None, name="", extra=()):
        self.start, self.name, self.extra = start, name, list(extra)
        self.asked = []

    def process_start(self, pid):
        return self.start

    def process_name(self, pid):
        return self.name

    def survivors_extra(self, record):
        self.asked.append(record)
        return self.extra


class PreviousHolderTests(unittest.TestCase):
    """When a command wins the lock and the port still will not open, it says
    who held the board before it and what of that holder still runs."""

    BOARD = "90:70:69:FE:A3:08"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.clock = Clock()
        self.clock.value = time.time()
        self.lock = device_lock.LockStore(Path(self.temp.name), self.clock.now)

    def tearDown(self):
        self.temp.cleanup()

    def previous_holder(self, pid=None):
        ticket = self.lock.enqueue(self.BOARD, "sam@bench:41", "flash", pid=pid)
        held = self.lock.claim(self.BOARD, ticket)
        self.lock.release(self.BOARD, held["token"])
        return held

    def sleeper(self, **environment):
        child = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(120)"],
            env=dict(os.environ, **environment), stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: (child.kill(), child.wait()))
        return child

    def text(self):
        return device_lock.previous_holder_text(self.lock, self.BOARD, lock_scope)

    def point_record_at(self, pid):
        record = self.lock.read_json(self.lock.last_path(self.BOARD))
        record["pid"] = pid
        self.lock.write_json(self.lock.last_path(self.BOARD), record)

    def test_it_names_the_previous_owner_and_its_live_process_and_leaves_it_running(self):
        held = self.previous_holder(pid=os.getpid())
        child = self.sleeper(_AUTANA_DEVICE_LOCK_TOKEN=held["token"])
        self.point_record_at(child.pid)
        self.clock.advance(185)
        text = self.text()
        self.assertIn("sam@bench:41 (autana flash)", text)
        self.assertIn("ended 3m ago", text)
        self.assertIn(str(child.pid), text)
        self.assertIn("never stops another holder's processes", text)
        time.sleep(0.2)
        self.assertIsNone(child.poll())

    def test_the_holder_itself_is_named_though_it_carries_no_token(self):
        """A holder sets the token after it starts, so /proc never shows it in
        the holder's own environment: only its recorded pid can name it."""
        self.previous_holder(pid=os.getpid())
        child = self.sleeper()
        self.point_record_at(child.pid)
        self.assertIn(f"Still running from it: {child.pid}", self.text())

    def test_a_pid_that_began_after_the_lock_was_taken_is_not_named(self):
        """The holder is enqueued before it acquires; a process that started
        later has only reused the number, and the text invites ending it."""
        self.clock.value = time.time() - 100
        self.previous_holder(pid=os.getpid())
        child = self.sleeper()
        self.point_record_at(child.pid)
        text = self.text()
        self.assertNotIn(str(child.pid), text)
        self.assertNotIn("Still running", text)

    def test_it_says_so_when_nothing_of_the_previous_holder_still_runs(self):
        self.previous_holder(pid=os.getpid())
        text = self.text()
        self.assertIn("sam@bench:41", text)
        self.assertNotIn("Still running", text)
        self.assertIn("program outside autana", text)

    def test_it_names_a_holder_reclaimed_from(self):
        store = device_lock.LockStore(Path(self.temp.name), self.clock.now,
                                      lambda pid: pid == os.getpid())
        first = store.enqueue(self.BOARD, "hung@bench:7", "listen", pid=os.getpid())
        self.assertIsNotNone(store.claim(self.BOARD, first))
        self.clock.advance(601)
        second = store.enqueue(self.BOARD, "next@bench:8", "flash", pid=os.getpid())
        self.assertIsNotNone(store.claim(self.BOARD, second))
        text = device_lock.previous_holder_text(store, self.BOARD, FakeScope())
        self.assertIn("hung@bench:7 (autana listen)", text)
        self.assertIn("reclaimed", text)

    def test_an_unrecorded_holder_is_admitted(self):
        self.assertIn("not recorded", self.text())

    def test_a_holder_on_another_host_is_not_looked_up_here(self):
        self.previous_holder(pid=os.getpid())
        record = self.lock.read_json(self.lock.last_path(self.BOARD))
        record["host"] = "some-other-machine"
        self.lock.write_json(self.lock.last_path(self.BOARD), record)
        scope = FakeScope(extra=[1])
        text = device_lock.previous_holder_text(self.lock, self.BOARD, scope)
        self.assertEqual(scope.asked, [])
        self.assertNotIn("Still running", text)

    def test_the_scope_adds_its_extras_once_beside_the_holders_pid(self):
        self.previous_holder(pid=os.getpid())
        self.point_record_at(1)
        store = device_lock.LockStore(Path(self.temp.name), self.clock.now, lambda pid: pid == 1)
        text = device_lock.previous_holder_text(
            store, self.BOARD, FakeScope(name="python3", extra=[1, 77]))
        self.assertIn("Still running from it: 1 (python3), 77 (python3).", text)

    def test_a_record_without_a_lock_time_keeps_its_pid(self):
        self.previous_holder(pid=os.getpid())
        record = self.lock.read_json(self.lock.last_path(self.BOARD))
        record.update(pid=1, acquired_at=None)
        self.lock.write_json(self.lock.last_path(self.BOARD), record)
        store = device_lock.LockStore(Path(self.temp.name), self.clock.now, lambda pid: pid == 1)
        text = device_lock.previous_holder_text(store, self.BOARD, FakeScope(start=time.time() + 999))
        self.assertIn("Still running from it: 1", text)


class ProtocolTests(unittest.TestCase):
    """LOCK_PROTOCOL: the mutex is guard() (an O_CREAT|O_EXCL file); these JSON
    records are the state it protects. A protocol mismatch is never a reason
    to stop - two machines with different-aged autana installs still have to
    work the same board - only information shown in status and while waiting."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.clock = Clock()
        self.lock = device_lock.LockStore(
            Path(self.temp.name), self.clock.now,
            lambda pid: pid in (1, device_lock.os.getpid()))

    def tearDown(self):
        self.temp.cleanup()

    def write_foreign_lock(self, protocol, pid=99, autana_version="9.9.9"):
        """pid 99 is dead per this test class's is_alive (only 1 and this
        process's own pid are alive) - the default names a reclaimable
        holder; pass pid=1 for a live one a claim must leave standing."""
        record = {
            "acquired_at": 1000, "heartbeat_at": 1000, "expected_build_id": "",
            "host": device_lock.socket.gethostname(), "owner": "other-autana",
            "pid": pid, "purpose": "flash", "token": "old",
        }
        if autana_version is not None:
            record["autana_version"] = autana_version
        if protocol is not None:
            record["protocol"] = protocol
        self.lock.write_json(self.lock.lock_path("COM5"), record)

    def test_a_live_holder_of_a_different_protocol_is_just_waited_for(self):
        """No exception, no refusal - a claim behind a live foreign-protocol
        holder is queued exactly like any other live holder."""
        self.write_foreign_lock(protocol=99, pid=1)
        ticket = self.lock.enqueue("COM5", "me", "flash", pid=1)
        self.assertIsNone(self.lock.claim("COM5", ticket))
        # Never touched: the foreign record still stands, unguessed at.
        self.assertEqual(self.lock.read_json(self.lock.lock_path("COM5"))["owner"],
                         "other-autana")

    def test_a_live_holder_with_no_protocol_field_is_queued_behind_and_named(self):
        """A lock written before LOCK_PROTOCOL existed (no "protocol" or
        "autana_version" key at all) reads as protocol 0, unknown version -
        still just waited for, and status/the wait notice both name it,
        never crash reading a field it lacks."""
        self.write_foreign_lock(protocol=None, pid=1, autana_version=None)
        ticket = self.lock.enqueue("COM5", "me", "flash", pid=1)
        self.assertIsNone(self.lock.claim("COM5", ticket))
        entry = device_lock.status_entry(self.lock, "COM5", durations={})
        self.assertEqual(entry["holder"]["protocol"], 0)
        self.assertIsNone(entry["holder"]["autana_version"])
        text = "\n".join(device_lock.status_lines(entry, self.clock.now()))
        self.assertIn("lock protocol 0", text)
        self.assertIn("autana unknown", text)

    def test_a_dead_holder_is_reclaimed_regardless_of_protocol(self):
        """The board must never wedge on a dead peer just because it spoke a
        different protocol: dead/stale is judged from PROTOCOL_CORE_FIELDS
        alone (host, pid, heartbeat_at), so it never needs a protocol match."""
        self.write_foreign_lock(protocol=99, pid=99)
        ticket = self.lock.enqueue("COM5", "me", "flash", pid=1)
        held = self.lock.claim("COM5", ticket)
        self.assertEqual(held["owner"], "me")
        self.assertIn("reclaimed lock from other-autana for flash (dead process)", held.log)

    def test_a_matching_protocol_claims_normally(self):
        self.write_foreign_lock(protocol=device_lock.LOCK_PROTOCOL)
        ticket = self.lock.enqueue("COM5", "me", "flash", pid=1)
        held = self.lock.claim("COM5", ticket)
        self.assertEqual(held["owner"], "me")

    def test_a_live_matching_protocol_holder_is_not_reclaimed(self):
        self.write_foreign_lock(protocol=device_lock.LOCK_PROTOCOL, pid=1)
        ticket = self.lock.enqueue("COM5", "me", "flash", pid=1)
        self.assertIsNone(self.lock.claim("COM5", ticket))

    def test_status_names_a_live_holders_protocol_and_version(self):
        self.write_foreign_lock(protocol=device_lock.LOCK_PROTOCOL, pid=1)
        entry = device_lock.status_entry(self.lock, "COM5", durations={})
        text = "\n".join(device_lock.status_lines(entry, self.clock.now()))
        self.assertIn(f"lock protocol {device_lock.LOCK_PROTOCOL}", text)
        self.assertIn("autana 9.9.9", text)

    def test_a_foreign_lock_is_not_ours_to_heartbeat_or_release(self):
        """No token of ours ever matches a foreign lock's, so these return the
        same "not mine" answer as any other lock we do not hold - no crash,
        whatever its protocol."""
        self.write_foreign_lock(protocol=99)
        self.assertFalse(self.lock.heartbeat("COM5", "not-mine"))
        self.assertFalse(self.lock.release("COM5", "not-mine"))
        self.assertFalse(self.lock.check_token("COM5", "not-mine"))

    def test_written_records_carry_the_current_protocol_and_version(self):
        held = self.lock.acquire("COM5", "me", "flash")
        self.assertEqual(held["protocol"], device_lock.LOCK_PROTOCOL)
        self.assertEqual(held["autana_version"], device_lock.__version__)
        self.lock.enqueue("COM5", "waiting", "flash")
        [ticket] = [t for t in self.lock.tickets("COM5") if t["owner"] == "waiting"]
        self.assertEqual(ticket["protocol"], device_lock.LOCK_PROTOCOL)
        reservation_id, _ = self.lock.set_human("COM6", "person", "note")
        self.assertTrue(reservation_id)
        human = self.lock.read_json(self.lock.human_path("COM6"))
        self.assertEqual(human["protocol"], device_lock.LOCK_PROTOCOL)


class RecordLabelTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.lock = device_lock.LockStore(Path(self.temp.name))

    # What the installed autana reads off a record with [] rather than .get():
    # a newer autana must keep writing all of it, or an older one on PATH fails
    # (release indexes purpose after it removes the lock).
    OLD_READER_LOCK_KEYS = {"owner", "board", "pid", "host", "heartbeat_at", "token", "purpose",
                            "kind", "acquired_at", "protocol"}
    OLD_READER_TICKET_KEYS = {"owner", "board", "pid", "ticket", "sequence", "purpose", "kind"}

    def test_a_record_written_now_has_every_key_an_older_reader_indexes(self):
        self.lock.acquire("COM5", "one", "flash", purpose="autana tune")
        self.lock.enqueue("COM5", "two", "listen")
        record = self.lock.read_json(self.lock.lock_path("COM5"))
        queued = self.lock.tickets("COM5")[0]
        self.assertLessEqual(self.OLD_READER_LOCK_KEYS, set(record))
        self.assertLessEqual(self.OLD_READER_TICKET_KEYS, set(queued))
        self.assertEqual(record["purpose"], "autana tune")
        self.assertEqual(queued["purpose"], "autana listen")
        for gone in ("expected_build_id", "log", "command"):
            self.assertNotIn(gone, record)

    def test_an_older_reader_can_release_and_show_a_lock_written_now(self):
        held = self.lock.acquire("COM5", "one", "flash")
        lock = self.lock.read_json(self.lock.lock_path("COM5"))
        self.assertEqual(lock["purpose"], "autana flash")
        self.assertTrue(self.lock.release("COM5", held["token"]))
        last = self.lock.read_json(self.lock.last_path("COM5"))
        self.assertEqual(last["purpose"], "autana flash")

    def test_the_reclaim_line_comes_back_from_acquire_not_from_the_record(self):
        self.lock.acquire("COM5", "gone", "listen")
        record = self.lock.read_json(self.lock.lock_path("COM5"))
        self.lock.write_json(self.lock.lock_path("COM5"), dict(record, host="elsewhere",
                                                               heartbeat_at=1))
        held = self.lock.acquire("COM5", "next", "flash")
        self.assertIn("reclaimed lock from gone for autana listen (heartbeat expiry)", held.log)
        self.assertNotIn("log", self.lock.read_json(self.lock.lock_path("COM5")))

    def test_a_lock_says_autana_kind_unless_told_more(self):
        self.assertEqual(device_lock.purpose_of("flash"), "autana flash")


class LockRecordShapeTests(unittest.TestCase):
    """A golden snapshot of every field a lock record carries, keyed by the
    protocol that shape belongs to. A deliberate field addition or removal
    is exactly the case LOCK_PROTOCOL exists for: this failing is the
    reminder to bump LOCK_PROTOCOL (device_lock.py's own docstring on it says
    why), add a new entry here for the new protocol, and keep the old one -
    never edit an existing entry to make a red test green without doing that."""

    GOLDEN_KEYS = {
        2: {
            "lock": frozenset({
                "acquired_at", "board", "heartbeat_at", "host", "owner", "pid", "kind", "purpose",
                "token", "protocol", "autana_version",
            }),
            "ticket": frozenset({
                "board", "created_at", "owner", "pid", "kind", "purpose", "sequence",
                "ticket", "protocol", "autana_version",
            }),
            "human": frozenset({
                "board", "id", "note", "owner", "since_at", "expires_at", "protocol",
                "autana_version",
            }),
            "last": frozenset({
                "board", "owner", "purpose", "pid", "host", "token", "acquired_at", "ended_at", "how",
                "protocol", "autana_version",
            }),
        },
        1: {
            "lock": frozenset({
                "acquired_at", "board", "expected_build_id", "heartbeat_at", "host",
                "log", "owner", "pid", "purpose", "kind", "token", "protocol",
                "autana_version",
            }),
            "ticket": frozenset({
                "board", "created_at", "owner", "pid", "purpose", "kind", "sequence",
                "ticket", "protocol", "autana_version",
            }),
            "human": frozenset({
                "board", "id", "note", "owner", "since_at", "protocol", "autana_version",
            }),
        },
    }

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.lock = device_lock.LockStore(Path(self.temp.name))

    def tearDown(self):
        self.temp.cleanup()

    def golden(self, kind):
        return self.GOLDEN_KEYS[device_lock.LOCK_PROTOCOL][kind]

    def test_the_lock_records_keys_match_the_golden_set(self):
        held = self.lock.acquire("COM5", "me", "flash")
        self.assertEqual(set(held), self.golden("lock"))

    def test_the_ticket_records_keys_match_the_golden_set(self):
        self.lock.enqueue("COM5", "me", "flash")
        [ticket] = self.lock.tickets("COM5")
        self.assertEqual(set(ticket), self.golden("ticket"))

    def test_the_human_records_keys_match_the_golden_set(self):
        self.lock.set_human("COM5", "me", "note")
        human = self.lock.read_json(self.lock.human_path("COM5"))
        self.assertEqual(set(human), self.golden("human"))

    def test_the_last_holder_records_keys_match_the_golden_set(self):
        held = self.lock.acquire("COM5", "me", "flash")
        self.lock.release("COM5", held["token"])
        last = self.lock.read_json(self.lock.last_path("COM5"))
        self.assertEqual(set(last), self.golden("last"))

    def test_the_last_holder_names_no_board_of_its_own(self):
        held = self.lock.acquire("COM5", "me", "flash")
        self.lock.release("COM5", held["token"])
        self.assertEqual(self.lock.boards(), [])

    def test_core_fields_a_lock_reader_needs_are_promised_and_present(self):
        """reclaim_reason()/is_stale() decide whether a lock is live, stale or
        dead from pid/host/heartbeat_at alone, and boards() reads board off
        any record - PROTOCOL_CORE_FIELDS must promise these regardless of
        protocol, and this protocol's own golden set must actually carry
        them. purpose/acquired_at decide nothing and are read with .get()."""
        needed = {"owner", "pid", "host", "heartbeat_at", "board", "protocol"}
        self.assertLessEqual(needed, set(device_lock.PROTOCOL_CORE_FIELDS))
        self.assertLessEqual(needed, self.golden("lock"))

    def test_core_fields_a_ticket_reader_needs_are_promised_and_present(self):
        """tickets() sorts on sequence and _claim() matches on ticket -
        both decide something and so must be promised regardless of
        protocol. purpose decides nothing and is read with .get()."""
        needed = {"owner", "pid", "sequence", "ticket", "board", "protocol"}
        self.assertLessEqual(needed, set(device_lock.PROTOCOL_CORE_FIELDS))
        self.assertLessEqual(needed, self.golden("ticket"))

    def test_core_fields_a_human_reader_needs_are_promised_and_present(self):
        """boards() reads board off any record; note/since_at decide nothing
        and are read with .get()."""
        needed = {"owner", "board", "protocol"}
        self.assertLessEqual(needed, set(device_lock.PROTOCOL_CORE_FIELDS))
        self.assertLessEqual(needed, self.golden("human"))


if __name__ == "__main__":
    unittest.main()
