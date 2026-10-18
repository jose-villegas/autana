"""Token authority and hold guards under a controlled clock."""
import contextlib
import io
import os
import socket
import unittest
from unittest import mock

from test_lock_guard import Store, BOARD_A, device, device_lock, durations_rows
from test_device_lock import Clock


class HoldGuardTests(Store):
    def setUp(self):
        super().setUp()
        self.clock = Clock()
        self.store.now = self.clock.now
        self.store.is_alive = lambda pid: True
        self.env = mock.patch.dict(os.environ, {device.TOKEN_ENV: ""})
        starts = mock.patch.object(device_lock, "process_start", return_value=None)
        starts.start()
        self.addCleanup(starts.stop)
        self.env.start()
        self.addCleanup(self.env.stop)

    @contextlib.contextmanager
    def scope(self):
        with mock.patch.multiple(device.lock_scope, enter=mock.DEFAULT, leave=mock.DEFAULT,
                                 members=mock.DEFAULT, reap=mock.DEFAULT) as scope:
            scope["members"].return_value = []
            scope["reap"].return_value = ([], [])
            yield

    def take(self, **options):
        held = self.store.acquire(BOARD_A, "holder", "run")
        self.assertTrue(self.store.hold(BOARD_A, held["token"], "bench", **options))
        return held["token"]

    def main(self, *words):
        with mock.patch.object(device.device_lock, "LockStore", return_value=self.store), \
                mock.patch.object(device, "board_for_lock", return_value=(BOARD_A, None)):
            return device.main(["--board", BOARD_A, "--wait", "0", *words])

    def assert_token_refused(self, words, message):
        before = self.store.read_json(self.store.lock_path(BOARD_A))
        with contextlib.redirect_stderr(io.StringIO()) as error:
            self.assertEqual(self.main(*words), 1)
        self.assertIn(message, error.getvalue())
        self.assertEqual(self.store.tickets(BOARD_A), [])
        self.assertEqual(self.store.read_json(self.store.lock_path(BOARD_A)), before)

    def test_dead_token_fails_before_flash_build(self):
        os.environ[device.TOKEN_ENV] = "wrong"
        with mock.patch.object(device, "build_image") as build:
            self.assertEqual(self.main("flash", "--variant", "dev", "--worktree", "."), 1)
        build.assert_not_called()
        self.assertEqual(self.store.tickets(BOARD_A), [])

    def test_borrow_constructor_rejects_dead_token(self):
        os.environ[device.TOKEN_ENV] = "wrong"
        with self.assertRaises(device.TokenRefused):
            device.HeldLock(self.store, BOARD_A, "borrower", "run", 0)
        self.assertEqual(self.store.tickets(BOARD_A), [])

    def test_borrow_uses_validated_snapshot(self):
        token = self.take(seconds=30)
        os.environ[device.TOKEN_ENV] = token
        live = self.store.live_lock
        def replace_after_snapshot(*args):
            record = live(*args)
            self.store.release(BOARD_A, token)
            self.store.acquire(BOARD_A, "replacement", "run")
            return record
        with mock.patch.object(self.store, "live_lock", side_effect=replace_after_snapshot):
            held = device.HeldLock(self.store, BOARD_A, "borrower", "run", 0)
        self.assertEqual(held.held["token"], token)
        self.assertEqual(held.held["owner"], "holder")

    def test_take_back_leaves_nonhuman_locks_and_emits_nothing(self):
        for kind in ("run", "hold", "flash"):
            with self.subTest(kind=kind):
                held = self.store.acquire(BOARD_A, "holder", kind)
                before = self.store.lock_path(BOARD_A).read_bytes()
                with mock.patch.object(device_lock.device_hook, "emit") as emit:
                    self.store.end_human(BOARD_A)
                self.assertEqual(self.store.lock_path(BOARD_A).read_bytes(), before)
                emit.assert_not_called()
                self.store.release(BOARD_A, held["token"])
        self.store.reserve_human(BOARD_A, "person", "bench")
        self.store.end_human(BOARD_A)
        self.assertIsNone(self.store.status(BOARD_A)["human"])

    def test_hold_refuses_wrong_or_lapsed_token_without_changes(self):
        token = self.take(seconds=1)
        for candidate, advance in (("wrong", 0), (token, 1)):
            self.clock.advance(advance)
            before = self.store.lock_path(BOARD_A).read_bytes()
            self.assertFalse(self.store.hold(BOARD_A, candidate, "changed", seconds=30))
            self.assertEqual(self.store.lock_path(BOARD_A).read_bytes(), before)

    def test_borrow_reports_loss_without_a_heartbeat_tick(self):
        for loss in ("release", "lapse", "live"):
            with self.subTest(loss=loss):
                token = self.take(seconds=1)
                os.environ[device.TOKEN_ENV] = token
                with self.scope():
                    held = device.HeldLock(self.store, BOARD_A, "borrower", "run", 0)
                    held.__enter__()
                    if loss == "release":
                        self.store.release(BOARD_A, token)
                    elif loss == "lapse":
                        self.clock.advance(1)
                    if loss == "live":
                        held.__exit__(None, None, None)
                    else:
                        with self.assertRaises(device.LockLost):
                            held.__exit__(None, None, None)
                error = durations_rows(self.store)[-1]["error"]
                self.assertEqual(bool(error), loss != "live")
                self.store.release(BOARD_A, token)
                os.environ[device.TOKEN_ENV] = ""

    def test_estimate_and_note_for_take(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(self.main("take", "--until-released", "--pid", str(os.getpid()),
                                       "--estimate", "30", "perf", "job"), 0)
        self.assert_estimated_hold()

    def test_estimate_for_run_is_visible_during_child_and_released(self):
        def child(*args, **kwargs):
            entry = device_lock.status_entry(self.store, BOARD_A, durations={})
            self.assertEqual(entry["estimated_free"], self.clock.now() + 30)
            self.assertEqual(entry["holder"]["purpose"], device.command_label("run"))
            record = self.store.status(BOARD_A)["lock"]
            self.assertEqual(record["kind"], "run")
            self.assertNotIn("until_released", record)
            self.assertTrue(self.store.check_token(BOARD_A, os.environ[device.TOKEN_ENV]))

        with self.scope(), mock.patch.object(device, "run_to_end", side_effect=child):
            self.assertEqual(self.main("run", "--estimate", "30", "--", "child"), 0)
        self.assertIsNone(self.store.status(BOARD_A)["lock"])

    def assert_estimated_hold(self):
        ticket = self.store.enqueue(BOARD_A, "waiter", "flash")
        second = self.store.enqueue(BOARD_A, "second", "flash")
        entry = device_lock.status_entry(self.store, BOARD_A, durations={"flash": 5})
        self.assertEqual(entry["holder"]["purpose"], "perf job")
        self.assertEqual(entry["estimated_free"], self.clock.now() + 30)
        self.assertEqual([waiter["estimated_start"] for waiter in entry["waiting"]],
                         [entry["estimated_free"], entry["estimated_free"] + 5])
        self.assertIn("perf job", "\n".join(device_lock.status_lines(entry, self.clock.now())))
        self.clock.advance(31)
        self.assertIsNotNone(self.store.status(BOARD_A)["lock"])
        self.assertIsNone(self.store.status(BOARD_A)["reclaimable"])
        self.assertEqual(device_lock.status_entry(self.store, BOARD_A)["estimated_free"], self.clock.now())
        self.store.cancel(BOARD_A, ticket)
        self.store.cancel(BOARD_A, second)

    def test_until_released_reclaims_reused_pid_only_on_this_host(self):
        token = self.take(pid=123)
        with mock.patch.object(device_lock, "process_start",
                               return_value=self.clock.now() + device_lock.PID_REUSE_SLACK_SECONDS + 1):
            self.assertEqual(self.store.status(BOARD_A)["reclaimable"]["reason"], "dead process")
            record = self.store.read_json(self.store.lock_path(BOARD_A))
            record["host"] = socket.gethostname() + "-remote"
            device_lock.write_json(self.store.lock_path(BOARD_A), record)
            self.assertTrue(self.store.check_token(BOARD_A, token))

    def test_take_and_hand_inside_hold_refuse_without_queueing(self):
        token = self.take(seconds=30)
        os.environ[device.TOKEN_ENV] = token
        for words in (("take", "bench"), ("hand-to-human", "--note", "bench"),
                      ("wait-free", "--cap", "0")):
            self.assert_token_refused(words, "already inside hold holder")
        with self.assertRaises(SystemExit) as error:
            self.main("run", "--keep-on-failure", "1", "--", "child")
        self.assertEqual(error.exception.code, 2)

    def test_interrupt_never_keeps_and_lost_run_never_converts(self):
        for error in (KeyboardInterrupt(), device.LockLost()):
            with mock.patch.object(device, "run_to_end", side_effect=error), \
                    self.scope(), \
                    mock.patch.object(self.store, "hold", wraps=self.store.hold) as hold, \
                    contextlib.redirect_stderr(io.StringIO()) as output:
                self.assertEqual(self.main("run", "--keep-on-failure", "1", "--", "child"),
                                 device.EXIT_INTERRUPTED if isinstance(error, KeyboardInterrupt) else 1)
            hold.assert_not_called()
            self.assertNotIn("lock held for", output.getvalue())
            self.assertIsNone(self.store.status(BOARD_A)["lock"])

    def test_lost_lock_with_keep_seconds_never_calls_hold(self):
        with self.scope(), \
                mock.patch.object(self.store, "hold", wraps=self.store.hold) as hold, \
                contextlib.redirect_stderr(io.StringIO()) as output:
            held = device.HeldLock(self.store, BOARD_A, "owner", "run", 0)
            held.__enter__()
            held.keep_seconds = 1
            self.store.release(BOARD_A, held.held["token"])
            with self.assertRaises(device.LockLost):
                held.__exit__(None, None, None)
        hold.assert_not_called()
        self.assertNotIn("lock held for", output.getvalue())

    def test_pid_and_duration_limits(self):
        for words in (("take", "--for", str(device_lock.HOLD_MAX_SECONDS + 1), "bench"),
                      ("take", "--pid", "1", "bench"),
                      ("take", "--until-released", "--pid", "0", "bench"),
                      ("take", "--until-released", "bench")):
            with self.subTest(words=words), self.assertRaises(SystemExit) as error:
                self.main(*words)
            self.assertEqual(error.exception.code, 2)

    def test_estimate_never_controls_hold_liveness(self):
        token = self.take(pid=os.getpid(), estimate=1)
        self.clock.advance(2)
        self.assertTrue(self.store.check_token(BOARD_A, token))
        entry = device_lock.status_entry(self.store, BOARD_A, durations={})
        self.assertEqual(entry["state"], "held")
        self.assertEqual(entry["estimated_free"], self.clock.now())
        self.store.is_alive = lambda pid: False
        status = self.store.status(BOARD_A)
        self.assertIsNone(status["lock"])
        self.assertEqual(status["reclaimable"]["reason"], "dead process")
        replacement = self.store.acquire(BOARD_A, "next", "flash", wait=0)
        self.assertIsNotNone(replacement)
        self.assertNotEqual(replacement["token"], token)

    def test_record_without_estimate_projects_acquired_plus_duration(self):
        held = self.store.acquire(BOARD_A, "holder", "flash")
        self.clock.advance(2)
        durations = {"flash": 10}
        expected = held["acquired_at"] + durations["flash"]
        entry = device_lock.status_entry(self.store, BOARD_A, durations=durations)
        self.assertEqual(entry["estimated_free"], expected)
        ticket = self.store.enqueue(BOARD_A, "waiter", "flash")
        estimates = device_lock.queue_estimates(self.store.status(BOARD_A), self.clock.now(), durations)
        self.assertEqual(estimates[ticket], expected)

    def test_hold_has_no_duration_estimate(self):
        self.take(seconds=30)
        entry = device_lock.status_entry(self.store, BOARD_A, durations={"hold": 1})
        self.assertIsNone(entry["estimated_free"])
        ticket = self.store.enqueue(BOARD_A, "waiter", "flash")
        estimates = device_lock.queue_estimates(self.store.status(BOARD_A), self.clock.now(),
                                                {"hold": 1, "flash": 2})
        self.assertIsNone(estimates[ticket])

    def test_refused_tokens_do_not_touch_reservation_or_queue(self):
        for kind in ("wrong", "released", "lapsed"):
            with self.subTest(kind=kind):
                token = self.take(seconds=1)
                if kind == "released":
                    self.store.release(BOARD_A, token)
                elif kind == "lapsed":
                    self.clock.advance(1)
                os.environ[device.TOKEN_ENV] = "wrong" if kind == "wrong" else token
                for words in (("take", "bench"), ("hand-to-human", "--note", "bench"),
                              ("wait-free", "--cap", "0")):
                    self.assert_token_refused(words, "not live")
                self.store.release(BOARD_A, token)
                self.store.reserve_human(BOARD_A, "person", "bench")
                for words in (("take", "bench"), ("hand-to-human", "--note", "changed"),
                              ("wait-free", "--cap", "0")):
                    self.assert_token_refused(words, "not live")
                self.store.end_human(BOARD_A)
                os.environ[device.TOKEN_ENV] = ""

    def test_maximum_hold_duration_is_accepted(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(self.main("take", "--for", str(device_lock.HOLD_MAX_SECONDS),
                                       "bench"), 0)
        record = self.store.read_json(self.store.lock_path(BOARD_A))
        self.assertEqual(record["expires_at"], self.clock.now() + device_lock.HOLD_MAX_SECONDS)

    def test_pid_reuse_slack_boundary(self):
        token = self.take(pid=123)
        for offset, live in ((device_lock.PID_REUSE_SLACK_SECONDS, True),
                             (device_lock.PID_REUSE_SLACK_SECONDS + 0.01, False)):
            with mock.patch.object(device_lock, "process_start",
                                   return_value=self.clock.now() + offset):
                self.assertEqual(self.store.check_token(BOARD_A, token), live)
