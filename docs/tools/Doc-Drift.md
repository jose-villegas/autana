# Documentation drift

`python scripts/gates/doc_drift.py --top 20` ranks every tracked Markdown document
by its age plus the number of cited source files changed since it was last
written. `--stale-days 30` instead lists every document untouched for at least
that many days, including documents that cite no code. It is a review queue,
not a correctness gate.

When a review finds a document accurate, append one line to
`docs/doc_review_ledger.txt`: its path, a tab, an ISO date, and optionally a
short note. The report uses the later of that date and the document's own last
commit, so a review leaves durable evidence without a content-only commit.
