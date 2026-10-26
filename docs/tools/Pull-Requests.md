# Pull request descriptions

A description is read in under a minute: a paragraph saying what changes and why, one table, a diagram only when there is a sequence or dependency, and a last line `Ticket: <id>` naming the backlog ticket. [`.github/pull_request_template.md`](../../.github/pull_request_template.md) holds that layout, and the GitHub form opens with it.

`scripts/pr/pr_body.py new` prints the template with its table filled from the branch, so only the paragraph, the table cells and the diagram are left to write:

```sh
python scripts/pr/pr_body.py new --kind change -o body.md
python scripts/pr/pr_body.py check body.md && gh pr create --body-file body.md
```

| Kind | For | Table it fills |
|---|---|---|
| `change` (default) | features, fixes, tooling, bakes | `Piece \| Before \| After`, a row per changed area |
| `move` | renames, splits, moves | `From \| To` from git's rename detection, then `change` rows for the rest |
| `perf` | timing work and goal changes | perf_compare's rows from `--perf <its --out dir>`, a `Goal \| Old \| New` row, then `change` rows |
| `docs` | documentation | `File \| Before \| After` in lines and words, then `change` rows for the rest |

- **Rows:** changed files are grouped by top-level directory, and the largest groups are split into subdirectories while the table stays within `MAX_ROWS` rows (set in `scripts/pr/pr_body.py`).
- **Lines:** every kind ends its table with the added, removed and net line counts against the merge base of `--base` (default `origin/main`).
- **Ticket:** `--ticket <id>`, else the first segment of the branch name (`feature/ks4hn-hash` → `ks4hn`) that `bd show` finds. Without either, `new` stops and asks for `--ticket`.
- **Unfilled parts** stay as HTML comments starting with `fill`, which GitHub renders as nothing; a leftover `block` comment also fails `check`.

`check` fails on a marker left in, on a Ticket line that [`scripts/gates/check-pr-ticket.sh`](../../scripts/gates/check-pr-ticket.sh) rejects, and on a ```` ```mermaid ```` block that [`check-mermaid.mjs`](Mermaid-Diagrams.md) cannot render. The pull request check runs the ticket gate alone.

## Run the pull request's gates locally

The runner reads the gate commands from `.github/workflows`, in workflow and
step order. List them first, then save a separate log for each step:

```sh
python scripts/ci/local_gates.py list
python scripts/ci/local_gates.py run --out build/local-gates --base origin/main
python scripts/ci/local_gates.py run --out build/local-gates --tier rel,dev,diag
```

| Tier | Selection |
|---|---|
| `host` | Default when `--tier` is omitted; includes container gates using the local ESP-IDF environment |
| `rel`, `dev`, `diag` | Selected with `--tier`; builds the variant before its gates |
| `qemu` | Selected with `--tier qemu`; needs the patched emulator |

`--tier` selects exactly the listed tiers. Builds run incrementally.
Unselected tiers print `NOT RUN` and never count as passing. Steps use their
workflow shell. ESP-IDF gates need the pinned local toolchain.

The exclusion reasons live in `OUT_OF_SCOPE` in
[`local_gates.py`](../../scripts/ci/local_gates.py). Host-wide installs are
printed and skipped; checkout-local dependency installs run.

With `GH_TOKEN` or `gh auth token`, CI's input manifests select affected jobs.
Changes include the working tree and untracked files from the merge-base.
`--base` takes a remote branch such as `origin/main`. Missing manifests or
credentials run the jobs and print why.

A failing gate or an unmapped workflow step returns failure. If a command or
Python module is missing, the runner prints skipped install commands from
that job beside the failing log path.
