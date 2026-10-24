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
