"""runner_vendor picks a candidate runner of the lock's vendor, and re-runs a run only when none was."""
import contextlib
import io
import json
import os
import re
import sys
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import runner_vendor  # noqa: E402

AMD, INTEL = "AuthenticAMD", "GenuineIntel"


RUN_SHA, NEWER_SHA = "a" * 40, "b" * 40


def fake_api(attempt, jobs, branch_sha=RUN_SHA):
    """A request stand-in: `jobs` maps a name to (conclusion, annotation titles); POSTs are recorded.

    The run was made from RUN_SHA on feature/x, whose head is now `branch_sha`."""
    posts = []
    table = {index: (name, *entry) for index, (name, entry) in enumerate(jobs.items())}

    def request(method, path, body=None):
        if method == "POST":
            posts.append(path)
            return {}
        if path.endswith("/annotations"):
            return [{"title": title} for title in table[int(path.split("/")[2])][2]]
        if "/jobs" in path:
            return {"jobs": [{"id": index, "name": name, "conclusion": conclusion}
                             for index, (name, conclusion, _) in table.items()]}
        if path.startswith("/branches/"):
            if branch_sha is None:
                raise urllib.error.HTTPError(path, 404, "Not Found", {}, None)
            return {"commit": {"sha": branch_sha}} if path == "/branches/feature%2Fx" else {}
        return {"run_attempt": attempt, "head_sha": RUN_SHA, "head_branch": "feature/x"}
    return request, posts


class RunnerVendorTests(unittest.TestCase):
    def rerun(self, attempt, jobs, branch_sha=RUN_SHA):
        request, posts = fake_api(attempt, jobs, branch_sha)
        with contextlib.redirect_stdout(io.StringIO()):
            runner_vendor.rerun(7, request=request)
        return posts

    def test_a_run_failed_only_by_vendor_is_re_run(self):
        jobs = {"shard 2": ("failure", [runner_vendor.WRONG_VENDOR]), "shard 0": ("success", []),
                "join": ("skipped", [])}
        self.assertEqual(self.rerun(1, jobs), ["/actions/runs/7/rerun-failed-jobs"])

    def test_any_other_failure_is_left_for_a_person(self):
        jobs = {"shard 2": ("failure", [runner_vendor.WRONG_VENDOR]), "lock": ("failure", [None])}
        self.assertEqual(self.rerun(1, jobs), [])
        self.assertEqual(self.rerun(1, {"lock": ("success", [])}), [])

    def test_the_tries_are_capped(self):
        jobs = {"shard 2": ("failure", [runner_vendor.WRONG_VENDOR])}
        self.assertEqual(self.rerun(runner_vendor.VENDOR_TRIES - 1, jobs), ["/actions/runs/7/rerun-failed-jobs"])
        self.assertEqual(self.rerun(runner_vendor.VENDOR_TRIES, jobs), [])

    def test_a_run_its_branch_has_moved_past_is_left(self):
        # A re-run holds the branch's Bakes concurrency slot, so the newer head's run would wait on it.
        jobs = {"shard 2": ("failure", [runner_vendor.WRONG_VENDOR])}
        self.assertEqual(self.rerun(1, jobs, branch_sha=NEWER_SHA), [])
        self.assertEqual(self.rerun(1, jobs, branch_sha=None), [])  # the branch was deleted

    def test_a_matrix_stand_in_failing_with_its_candidates_is_re_run(self):
        # The required check's one-name job fails whenever every candidate lost to the vendor check.
        jobs = {"Bake (candidate 1)": ("failure", [runner_vendor.WRONG_VENDOR]), "Bake": ("failure", [None])}
        self.assertEqual(self.rerun(1, jobs), ["/actions/runs/7/rerun-failed-jobs"])
        self.assertEqual(self.rerun(1, {"Bake": ("failure", [None])}), [])



def unit(shard):
    return f"Trace reference shard {shard}"


def marker(shard, candidate, attempt, ok, created):
    return runner_vendor.marker_name(unit(shard), candidate, attempt, ok), created


def shard_artifact(shard, candidate, attempt):
    return f"reference-shard-{shard}-{attempt}-{candidate}"


class CandidateTests(unittest.TestCase):
    """Of a job's candidates, one of the lock's vendor does it, and the job fails only when none can."""

    def decides(self, artifacts, running, verdicts, attempt=1):
        """`verdicts` maps (candidate, on the lock's vendor) to what `decide` must say for shard 4."""
        for (candidate, ok), verdict in verdicts.items():
            with self.subTest(candidate=candidate, ok=ok):
                self.assertEqual(runner_vendor.decide(artifacts, running, unit(4), candidate, attempt, ok), verdict)

    def test_the_earliest_marked_on_the_locks_vendor_traces(self):
        artifacts = [marker(4, 2, 1, True, "10:00:01"), marker(4, 0, 1, True, "10:00:05")]
        self.decides(artifacts, {0, 1, 2}, {(2, True): runner_vendor.WORK, (0, True): runner_vendor.OTHER})

    def test_a_right_candidate_that_cannot_see_its_marker_yet_traces_alone(self):
        self.decides([], {0, 1, 2}, {(1, True): runner_vendor.WORK})

    def test_a_wrong_candidate_finishes_green_while_a_sibling_can_still_trace(self):
        wrong = [marker(4, 0, 1, False, "10:00:00")]
        self.decides(wrong + [marker(4, 1, 1, True, "10:00:02")], {0}, {(0, False): runner_vendor.OTHER})
        # Candidate 2's job has not marked yet: it may be the lock's vendor.
        self.decides(wrong, {0, 2}, {(0, False): runner_vendor.OTHER})

    def test_a_shard_is_lost_when_no_candidate_left_can_trace_it(self):
        wrong = [marker(4, 0, 1, False, "10:00:00"), marker(4, 1, 1, False, "10:00:03")]
        # Candidate 1 is still deciding and candidate 2 finished green before candidate 1 marked.
        self.decides(wrong, {0, 1}, {(0, False): runner_vendor.LOST})
        # A re-run attempt runs only the failed candidate.
        self.decides([marker(4, 1, 2, False, "11:00:00")], {1}, {(1, False): runner_vendor.LOST}, attempt=2)

    def test_markers_of_another_shard_or_attempt_do_not_count(self):
        others = [marker(5, 1, 1, True, "10:00:00"), marker(4, 1, 1, True, "10:00:00")]
        self.decides(others, {0}, {(0, False): runner_vendor.LOST, (0, True): runner_vendor.WORK}, attempt=2)

    def test_markers_of_another_job_of_the_attempt_do_not_count(self):
        others = [marker(14, 1, 1, True, "10:00:00"),
                  (runner_vendor.marker_name("Plan the reference shards", 0, 1, True), "10:00:00")]
        self.decides(others, {0}, {(0, False): runner_vendor.LOST, (0, True): runner_vendor.WORK})

    def test_the_earliest_marked_traces_whatever_order_the_markers_list_in(self):
        # Every candidate on the lock's vendor reads after its own marker is listed; whichever siblings each
        # sees, the earliest-created one traces.
        early, late = marker(4, 3, 1, True, "10:00:01"), marker(4, 0, 1, True, "10:00:04")
        self.decides([early], {0, 3}, {(3, True): runner_vendor.WORK})
        self.decides([late, early], {0, 3}, {(3, True): runner_vendor.WORK, (0, True): runner_vendor.OTHER})

    def test_equal_marker_times_break_on_the_lower_candidate(self):
        artifacts = [marker(4, 2, 1, True, "10:00:00"), marker(4, 1, 1, True, "10:00:00")]
        self.decides(artifacts, {1, 2}, {(1, True): runner_vendor.WORK, (2, True): runner_vendor.OTHER})

    def test_a_shard_an_earlier_attempt_traced_is_done(self):
        artifacts = [(shard_artifact(14, 0, 1), "10:00:00"), marker(4, 0, 1, True, "10:00:00")]
        self.assertFalse(runner_vendor.done(artifacts, "reference-shard-4-*"))
        self.assertTrue(runner_vendor.done(artifacts + [(shard_artifact(4, 2, 1), "11:00:00")], "reference-shard-4-*"))
        self.assertFalse(runner_vendor.done(artifacts, ""))

    def test_a_marker_names_its_job_whatever_the_jobs_name_holds(self):
        name = runner_vendor.marker_name("Plan the reference shards", 3, 2, False)
        fields = runner_vendor.MARKER.fullmatch(name)
        self.assertEqual(tuple(fields.groups()), ("plan-the-reference-shards", "3", "2", "wrong"))
        self.assertNotEqual(runner_vendor.slug(unit(1)), runner_vendor.slug(unit(14)))

    def test_the_candidates_keep_the_matrix_under_its_cap(self):
        from bake import bake
        full = list(range(runner_vendor.CANDIDATES))
        self.assertEqual(runner_vendor.candidates(1), full)
        most = bake.MATRIX_JOBS // runner_vendor.CANDIDATES
        self.assertEqual(runner_vendor.candidates(most), full)
        self.assertEqual(len(runner_vendor.candidates(most + 1)), runner_vendor.CANDIDATES - 1)
        self.assertEqual(runner_vendor.candidates(bake.MATRIX_JOBS), [0])
        for count in (1, 18, most, most + 1, bake.MATRIX_JOBS // 2 + 1, bake.MATRIX_JOBS):
            with self.subTest(count=count):
                self.assertLessEqual(count * len(runner_vendor.candidates(count)), bake.MATRIX_JOBS)

    def env(self, folder, attempt="1"):
        out_path = Path(folder) / "out"
        out_path.write_text("")
        return out_path, {"GITHUB_RUN_ID": "7", "GITHUB_RUN_ATTEMPT": attempt, "GITHUB_OUTPUT": str(out_path)}

    def mark(self, artifacts, lock, host, attempt="2", candidate=3, traces=True):
        reads = []

        def request(method, path, body=None):
            reads.append(path)
            return {"artifacts": [{"name": name, "created_at": created} for name, created in artifacts]}
        with tempfile.TemporaryDirectory() as folder:
            out_path, env = self.env(folder, attempt)
            target = Path(folder) / "new" / "vendor"
            with mock.patch.dict(os.environ, env), contextlib.redirect_stdout(io.StringIO()):
                runner_vendor.mark(unit(4), candidate, target, lock, host, traces, "reference-shard-4-*", request=request)
            self.reads = reads
            files = sorted(path.name for path in target.iterdir()) if target.exists() else []
            return files, dict(line.split("=", 1) for line in out_path.read_text().splitlines())

    def test_mark_names_this_candidates_job_vendor_and_attempt(self):
        for host, ok in ((AMD, True), (INTEL, False)):
            with self.subTest(host=host):
                files, outputs = self.mark([], AMD, host)
                self.assertEqual(files, [runner_vendor.marker_name(unit(4), 3, 2, ok)])
                self.assertEqual(outputs, {"marker": files[0]})
                fields = runner_vendor.MARKER.fullmatch(files[0])
                self.assertEqual(tuple(fields.groups()), ("trace-reference-shard-4", "3", "2", "ok" if ok else "wrong"))

    def test_mark_skips_a_shard_an_earlier_attempt_traced(self):
        files, outputs = self.mark([(shard_artifact(4, 0, 1), "10:00:00")], AMD, AMD)
        self.assertEqual((files, outputs), ([], {"marker": "", "work": "false"}))

    def test_a_job_that_traces_nothing_is_candidate_0s_on_any_vendor(self):
        for candidate, work in ((0, "true"), (1, "false")):
            with self.subTest(candidate=candidate):
                files, outputs = self.mark([], AMD, INTEL, candidate=candidate, traces=False)
                self.assertEqual((files, outputs), ([], {"marker": "", "work": work}))
                self.assertEqual(self.reads, [])

    def test_start_sizes_each_job_by_its_traced_work(self):
        with tempfile.TemporaryDirectory() as folder:
            out_path, env = self.env(folder)
            with mock.patch.dict(os.environ, env):
                runner_vendor.start({"cpu": 3, "reference-plan": 0})
            self.assertEqual(out_path.read_text().splitlines(),
                             [f"cpu-candidates={list(range(runner_vendor.CANDIDATES))}",
                              "reference-plan-candidates=[0]"])

    def claim(self, artifacts, jobs, marker_name, candidate=0, unlisted_reads=0, attempt="1"):
        calls = []

        def request(method, path, body=None):
            calls.append(path.split("?")[0])
            if "/artifacts" in path:
                listed = [item for item in artifacts if item[0] != marker_name or
                          calls.count(path.split("?")[0]) > unlisted_reads]
                return {"artifacts": [{"name": name, "created_at": created} for name, created in listed]}
            return {"jobs": jobs}
        with tempfile.TemporaryDirectory() as folder:
            out_path, env = self.env(folder, attempt)
            out = io.StringIO()
            with mock.patch.dict(os.environ, env), contextlib.redirect_stdout(out):
                code = runner_vendor.claim(unit(4), candidate, marker_name, request=request,
                                           sleep=lambda seconds: calls.append(("sleep", seconds)))
            self.calls = calls
            return code, out.getvalue(), out_path.read_text()

    def test_claim_reads_until_its_own_marker_is_listed(self):
        # Candidate 1 is the earliest on the lock's vendor; while its marker is not listed, candidate 2's is.
        mine, created = marker(4, 1, 1, True, "10:00:00")
        code, _, outputs = self.claim([(mine, created), marker(4, 2, 1, True, "10:00:03")], [], mine, candidate=1,
                                      unlisted_reads=2)
        self.assertEqual((code, outputs), (0, "work=true\n"))
        reads = [call for call in self.calls if call == "/actions/runs/7/artifacts"]
        self.assertEqual(len(reads), 3)
        self.assertEqual(self.calls[0], ("sleep", runner_vendor.MARKER_POLL_SECONDS))

    def test_claim_reads_its_vendor_from_its_marker_name(self):
        wrong, created = marker(4, 1, 1, False, "10:00:00")
        code, _, outputs = self.claim([(wrong, created)], [], wrong, candidate=1)
        self.assertEqual((code, outputs), (1, "work=false\n"))

    def test_running_candidates_counts_every_unfinished_job_of_the_shard(self):
        jobs = [{"name": f"Trace reference shard 4 (candidate {index})", "status": status}
                for index, status in enumerate(("queued", "in_progress", "waiting", "completed"))]
        jobs += [{"name": "Trace reference shard 14 (candidate 0)", "status": "queued"},
                 {"name": "Join and pack the reference sets", "status": "queued"}]
        pages = [jobs[:3] + [{"name": "x", "status": "completed"}] * (runner_vendor.github_api.PER_PAGE - 3), jobs[3:]]

        def request(method, path, body=None):
            return {"jobs": pages[int(path.rsplit("page=", 1)[1]) - 1]}
        with mock.patch.dict(os.environ, {"GITHUB_RUN_ID": "7", "GITHUB_RUN_ATTEMPT": "1"}):
            self.assertEqual(runner_vendor.running_candidates(unit(4), request=request), {0, 1, 2})

    def test_a_lost_shard_fails_with_the_title_the_rerun_takes(self):
        name, created = marker(4, 0, 1, False, "10:00:00")
        jobs = [{"name": "Trace reference shard 4 (candidate 0)", "status": "in_progress"},
                {"name": "Trace reference shard 4 (candidate 1)", "status": "completed"},
                {"name": "Trace reference shard 3 (candidate 1)", "status": "queued"}]
        code, out, outputs = self.claim([(name, created)], jobs, name)
        self.assertEqual(code, 1)
        self.assertIn(f"::error title={runner_vendor.WRONG_VENDOR}::", out)
        self.assertEqual(outputs, "work=false\n")
        # The run's other candidates finished green, so the lost one is its only failure and is re-run.
        run = {"Trace reference shard 4 (candidate 0)": ("failure", [runner_vendor.WRONG_VENDOR]),
               "Trace reference shard 4 (candidate 1)": ("success", []),
               "Join and pack the reference sets": ("skipped", [])}
        request, posts = fake_api(1, run)
        with contextlib.redirect_stdout(io.StringIO()):
            runner_vendor.rerun(7, request=request)
        self.assertEqual(posts, ["/actions/runs/7/rerun-failed-jobs"])

    def test_a_queued_sibling_keeps_a_wrong_candidate_green(self):
        name, created = marker(4, 0, 1, False, "10:00:00")
        jobs = [{"name": "Trace reference shard 4 (candidate 0)", "status": "in_progress"},
                {"name": "Trace reference shard 4 (candidate 2)", "status": "queued"}]
        code, _, outputs = self.claim([(name, created)], jobs, name)
        self.assertEqual((code, outputs), (0, "work=false\n"))

    def test_the_tracing_candidate_says_so(self):
        name, created = marker(4, 1, 1, True, "10:00:00")
        code, _, outputs = self.claim([(name, created)], [], name, candidate=1)
        self.assertEqual((code, outputs), (0, "work=true\n"))


class BakesWorkflowTests(unittest.TestCase):
    """A re-run happens only when the vendor check is a run's sole failure, so nothing after the lock may run,
    and fail, when a bake before it failed: a skipped lock is no failure."""

    def test_a_job_after_the_lock_waits_out_every_failure_the_lock_does(self):
        import yaml
        jobs = yaml.safe_load((runner_vendor.ROOT / ".github/workflows/bakes.yml").read_text())["jobs"]
        guarded = [name for name in jobs["lock"]["needs"] if f"needs.{name}.result != 'failure'" in jobs["lock"]["if"]]
        self.assertTrue(guarded)
        for name, job in jobs.items():
            needs = [job["needs"]] if isinstance(job.get("needs"), str) else job.get("needs", [])
            if "lock" not in needs:
                continue
            with self.subTest(job=name):
                for before in guarded:
                    self.assertIn(before, needs)
                    self.assertIn(f"needs.{before}.result != 'failure'", job["if"])

    @staticmethod
    def jobs():
        import yaml
        return yaml.safe_load((runner_vendor.ROOT / ".github/workflows/bakes.yml").read_text())["jobs"]

    @staticmethod
    def traces(job):
        """Whether `job` runs a bake that traces: `bake.py bake` or `bake.py shards`, but not the join of
        traced shards (`--shards`), which packs them."""
        return any(re.search(r"bake\.py (bake|shards)\b", line) and "--shards " not in line
                   for step in job["steps"] for line in step.get("run", "").splitlines())

    def test_the_cpu_matrix_reports_one_check_name(self):
        # Branch protection requires the cpu bake by its bare name, but a matrix reports as "<name> (candidate N)":
        # one job of the bare name stands for it, runs whatever the matrix did, and passes on success or skipped.
        jobs = self.jobs()
        bare = jobs["cpu"]["name"].split(" (")[0]
        self.assertNotEqual(bare, jobs["cpu"]["name"])
        summary = [job for job in jobs.values() if job.get("name") == bare]
        self.assertEqual(len(summary), 1)
        self.assertEqual(summary[0]["needs"], ["inputs", "cpu"])
        self.assertIn("always()", summary[0]["if"])
        run = "".join(step.get("run", "") for step in summary[0]["steps"])
        self.assertIn("success", run)
        self.assertIn("skipped", run)
        # A skipped matrix passes only when the inputs job found no cpu work, not when the run was cancelled.
        self.assertIn("needs.inputs.outputs.cpu", str(summary[0]["steps"]))

    def test_every_job_that_traces_starts_as_vendor_candidates(self):
        jobs = self.jobs()
        tracing = [name for name, job in jobs.items() if self.traces(job)]
        self.assertTrue(tracing)
        for name in tracing:
            job = jobs[name]
            with self.subTest(job=name):
                steps = [step.get("id") or step.get("name") or step.get("uses") for step in job["steps"]]
                self.assertIn("candidate", steps)
                start = job["steps"][steps.index("candidate")]
                self.assertEqual(start["uses"], "./.github/actions/vendor-candidate")
                self.assertEqual(start["with"]["candidate"], "${{ matrix.candidate }}")
                # claim finds a candidate's siblings by this name.
                self.assertEqual(job["name"], f"{start['with']['unit']} (candidate ${{{{ matrix.candidate }}}})")
                match = runner_vendor.CANDIDATE_JOB.fullmatch(job["name"].replace("${{ matrix.candidate }}", "2"))
                self.assertEqual(match[2], "2")
                self.assertEqual(job["permissions"]["actions"], "read")
                self.assertFalse(job["strategy"]["fail-fast"])
                for step in job["steps"][steps.index("candidate") + 1:]:
                    gate = step.get("if", "")
                    self.assertIn("steps.candidate.outputs.work == 'true'", gate, step)
                    self.assertNotIn("||", gate, step)
                # Nothing that bakes or uploads runs before the start.
                for step in job["steps"][:steps.index("candidate")]:
                    self.assertNotIn("upload-artifact", step.get("uses", ""), step)
                    self.assertFalse(self.traces({"steps": [step]}), step)

    def candidate_jobs(self):
        jobs = self.jobs()
        return {name: job for name, job in jobs.items()
                if any(step.get("uses") == "./.github/actions/vendor-candidate" for step in job["steps"])}

    def test_two_candidates_that_both_work_upload_under_their_own_names(self):
        """A marker that lists late lets two candidates work; upload-artifact fails on a name taken already."""
        for name, job in self.candidate_jobs().items():
            ids = {step.get("id"): step for step in job["steps"]}
            for step in job["steps"]:
                if "upload-artifact" not in step.get("uses", ""):
                    continue
                artifact = step["with"]["name"]
                source = re.fullmatch(r"\$\{\{ steps\.([\w-]+)\.outputs\.[\w-]+ \}\}", artifact)
                if source:  # named by a step's output: the step's environment builds it
                    artifact = " ".join(ids[source[1]].get("env", {}).values())
                with self.subTest(job=name, artifact=artifact):
                    self.assertIn("${{ matrix.candidate }}", artifact)
                    self.assertIn("${{ github.run_attempt }}", artifact)
            # A local action that uploads under a name of its own replaces the other worker's copy.
            import yaml
            for step in job["steps"]:
                local = step.get("uses", "")
                if not local.startswith("./") or local.endswith("vendor-candidate"):
                    continue
                action = yaml.safe_load((runner_vendor.ROOT / local / "action.yml").read_text())
                for inner in action["runs"].get("steps", []):
                    if "upload-artifact" in inner.get("uses", ""):
                        with self.subTest(job=name, action=local):
                            self.assertTrue(inner["with"].get("overwrite"), inner)

    def test_a_shard_an_earlier_attempt_traced_is_not_traced_again(self):
        job = self.jobs()["reference-shard"]
        start = next(step for step in job["steps"] if step.get("id") == "candidate")
        upload = next(step for step in job["steps"] if "upload-artifact" in step.get("uses", ""))
        for shard, other in ((1, 14), (14, 1)):
            with self.subTest(shard=shard):
                pattern = self.at(start["with"]["done"], shard)
                self.assertTrue(runner_vendor.done([(self.at(upload["with"]["name"], shard), "")], pattern))
                self.assertFalse(runner_vendor.done([(self.at(upload["with"]["name"], other), "")], pattern))

    @staticmethod
    def at(text, shard, plan="reference-plan-1-0"):
        """`text` with the reference matrix and plan filled in: attempt 1, candidate 2."""
        return (text.replace("${{ matrix.shard }}", str(shard)).replace("${{ github.run_attempt }}", "1")
                .replace("${{ matrix.candidate }}", "2").replace("${{ needs.reference-plan.outputs.artifact }}", plan))

    def test_a_shard_counts_only_for_the_plan_it_traced(self):
        # Probe selection may differ between two runners of the lock's vendor, so a re-run attempt that plans
        # again traces every shard again; the join takes only the shards of the plan this attempt's shards read.
        jobs = self.jobs()
        start = next(step for step in jobs["reference-shard"]["steps"] if step.get("id") == "candidate")
        upload = next(step for step in jobs["reference-shard"]["steps"] if "upload-artifact" in step.get("uses", ""))
        fetch = next(step for step in jobs["reference"]["steps"] if "download-artifact" in step.get("uses", ""))
        traced = self.at(upload["with"]["name"], 1, plan="reference-plan-4-3")
        self.assertTrue(runner_vendor.done([(traced, "")], self.at(start["with"]["done"], 1, plan="reference-plan-4-3")))
        self.assertFalse(runner_vendor.done([(traced, "")], self.at(start["with"]["done"], 1, plan="reference-plan-6-2")))
        for plan, taken in (("reference-plan-4-3", True), ("reference-plan-6-2", False)):
            with self.subTest(plan=plan):
                self.assertEqual(runner_vendor.done([(traced, "")], self.at(fetch["with"]["pattern"], 1, plan)), taken)

    @staticmethod
    def kinds_of(run, files):
        """The bake kinds the `bake.py list --missing` lines of `run` list into each of `files` (the
        $RUNNER_TEMP/<file>.txt or $(...) a value is counted from)."""
        listed = {}
        for line in run.splitlines():
            kinds = re.findall(r"--kind (\w+)", line)
            if "list --missing" in line:
                for target in re.findall(r'tee (?:-a )?"\$RUNNER_TEMP/(\w+)\.txt"', line) or [line]:
                    listed.setdefault(target, set()).update(kinds)
        return set().union(*(listed[name] for name in files))

    def test_each_job_is_sized_by_the_traced_work_its_own_start_sees(self):
        """`traces` in a job and the count `start` sizes its matrix by cover the same bake kinds, and every
        traced kind is counted for some job."""
        jobs = self.jobs()
        start_run = next(step for step in jobs["inputs"]["steps"] if step.get("id") == "start")["run"]
        sized = dict(re.findall(r'([\w-]+)="\$(\w+)"', start_run.splitlines()[-1]))
        counted = {}
        for line in start_run.splitlines()[:-1]:
            name, command = line.split("=", 1)
            counted[name.strip()] = self.kinds_of(command, [command])
        from bake import bake
        self.assertEqual(set().union(*counted.values()), set(bake.TRACED_KINDS))
        for name, job in self.candidate_jobs().items():
            traces = next(step for step in job["steps"] if step.get("id") == "candidate")["with"].get("traces")
            if name not in sized:
                self.assertIsNone(traces, name)  # always traces: the action's default
                continue
            with self.subTest(job=name):
                step_id, key = re.fullmatch(r"\$\{\{ steps\.([\w-]+)\.outputs\.(\w+) != '0' \}\}", traces).groups()
                run = next(step for step in job["steps"] if step.get("id") == step_id)["run"]
                value = next(line for line in run.splitlines() if f'"{key}=' in line)
                files = re.findall(r'\$RUNNER_TEMP/(\w+)\.txt', value)
                self.assertEqual(self.kinds_of(run, files), counted[sized[name]])

    def test_the_action_works_once_it_knows_and_claims_only_with_a_marker(self):
        import yaml
        action = yaml.safe_load((runner_vendor.ROOT / ".github/actions/vendor-candidate/action.yml").read_text())
        self.assertEqual(action["inputs"]["traces"]["default"], "true")
        self.assertEqual(action["outputs"]["work"]["value"], "${{ steps.claim.outputs.work || steps.mark.outputs.work }}")
        mark, upload, claim = action["runs"]["steps"]
        self.assertEqual((mark["id"], claim["id"]), ("mark", "claim"))
        self.assertIn("upload-artifact", upload["uses"])
        self.assertEqual(upload["with"]["name"], "${{ steps.mark.outputs.marker }}")
        self.assertEqual(upload["with"]["retention-days"], 1)
        for step in (upload, claim):
            self.assertEqual(step["if"], "steps.mark.outputs.marker != ''")
        self.assertEqual(claim["env"]["VENDOR_MARKER"], "${{ steps.mark.outputs.marker }}")

    def test_a_job_starts_one_runner_when_it_traces_nothing(self):
        """Each tracing job's candidate matrix comes from a runner_vendor step of a job it needs, which sizes it
        by the job's traced work; when that job did not run, the matrix is every candidate."""
        jobs = self.jobs()
        for name in [name for name, job in jobs.items() if self.traces(job)]:
            job = jobs[name]
            with self.subTest(job=name):
                match = re.fullmatch(r"\$\{\{ fromJSON\(needs\.([\w-]+)\.outputs\.([\w-]+)(?: \|\| '(.*)')?\) \}\}",
                                     job["strategy"]["matrix"]["candidate"])
                self.assertIsNotNone(match, job["strategy"]["matrix"]["candidate"])
                source, key, fallback = match.groups()
                needs = [job["needs"]] if isinstance(job["needs"], str) else job["needs"]
                self.assertIn(source, needs)
                step_id = re.fullmatch(r"\$\{\{ steps\.([\w-]+)\.outputs\.([\w-]+) \}\}", jobs[source]["outputs"][key])
                self.assertEqual(step_id[2], key)
                run = next(step for step in jobs[source]["steps"] if step.get("id") == step_id[1])["run"]
                if key == "candidates":
                    self.assertIn("runner_vendor.py plan", run)
                else:
                    self.assertEqual(key, f"{name}-candidates")
                    self.assertIn(f" {name}=", run[run.index("runner_vendor.py start"):])
                    self.assertEqual(json.loads(fallback), list(range(runner_vendor.CANDIDATES)))

    def test_every_tracing_job_is_sized_on_every_event(self):
        """The job that sizes the matrices runs on every event; only its pull request check is gated."""
        inputs = self.jobs()["inputs"]
        self.assertNotIn("if", inputs)
        start = next(step for step in inputs["steps"] if step.get("id") == "start")
        self.assertIn("github.event_name != 'pull_request'", start["if"])

    def test_the_cli_passes_its_arguments_in_order(self):
        with mock.patch.object(runner_vendor, "claim", return_value=0) as claim, \
                mock.patch.dict(os.environ, {"VENDOR_MARKER": "m"}):
            runner_vendor.main(["claim", unit(4), "2"])
        claim.assert_called_once_with(unit(4), 2, "m")
        with mock.patch.object(runner_vendor, "start", return_value=0) as start:
            runner_vendor.main(["start", "cpu=3", "reference-plan=0"])
        start.assert_called_once_with({"cpu": 3, "reference-plan": 0})
        with mock.patch.object(runner_vendor, "plan", return_value=0) as plan:
            runner_vendor.main(["plan", "[0, 1]"])
        plan.assert_called_once_with("[0, 1]")
        with mock.patch.object(runner_vendor, "mark", return_value=0) as mark, \
                mock.patch("bake.bake.read_lock_tables", return_value={"mesh_cpu_vendor": AMD}), \
                mock.patch("bake.bake.cpu_vendor", return_value=INTEL):
            runner_vendor.main(["mark", unit(4), "2", "dir"])
            runner_vendor.main(["mark", unit(4), "2", "dir", "false"])
            runner_vendor.main(["mark", unit(4), "2", "dir", "true", "reference-shard-4-*"])
        self.assertEqual(mark.call_args_list, [
            mock.call(unit(4), 2, "dir", AMD, INTEL, True, ""),
            mock.call(unit(4), 2, "dir", AMD, INTEL, False, ""),
            mock.call(unit(4), 2, "dir", AMD, INTEL, True, "reference-shard-4-*")])
        for bad in (["claim", "4"], ["mark", unit(4), "2"], ["mark", unit(4), "2", "d", "true", "p", "x"],
                    ["start"], ["start", "cpu"], ["start", "cpu=x"], ["start", "cpu=-1"]):
            with self.subTest(argv=bad), self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                runner_vendor.main(bad)

    def test_plan_outputs_the_count_and_candidates(self):
        with tempfile.TemporaryDirectory() as folder:
            out_path = Path(folder) / "out"
            out_path.write_text("")
            with mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(out_path)}):
                runner_vendor.plan("[0, 1, 2]")
            self.assertEqual(out_path.read_text().splitlines(),
                             ["count=3", f"candidates={list(range(runner_vendor.CANDIDATES))}"])


if __name__ == "__main__":
    unittest.main()
