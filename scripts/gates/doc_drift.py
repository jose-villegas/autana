#!/usr/bin/env python3
"""Rank tracked Markdown documents by age and changes to cited source files."""
import argparse
import datetime as dt
import pathlib
import subprocess

from check_doc_citations import citations, documentation, path_exists


def git_value(root, format, *args):
    result = subprocess.run(["git", "log", "-1", format, *args], cwd=root,
                            check=True, capture_output=True, text=True)
    return result.stdout.strip()


def git_date(root, *args):
    return dt.date.fromisoformat(git_value(root, "--format=%cs", *args))


def reviews(root):
    result = {}
    path = pathlib.Path(root) / "docs/doc_review_ledger.txt"
    if not path.exists():
        return result
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t", 2)
        if len(fields) < 2:
            raise ValueError(f"{path}:{number}: expected path and YYYY-MM-DD")
        result[fields[0]] = dt.date.fromisoformat(fields[1])
    return result


def cited_files(root):
    result = {}
    for citation in citations(root):
        if citation.kind == "path" and path_exists(root, citation.value):
            result.setdefault(citation.doc, set()).add(citation.value)
    return result


def changed_since(root, revision, paths):
    if not paths:
        return []
    return [path for path in sorted(paths) if subprocess.run(
        ["git", "log", "-1", "--format=%H", f"{revision}..HEAD", "--", path],
        cwd=root, check=True, capture_output=True, text=True).stdout.strip()]


def report(root, today=None):
    root = pathlib.Path(root)
    today = today or dt.date.today()
    tracked = set(subprocess.run(["git", "ls-files", "docs"], cwd=root, check=True,
                                 capture_output=True, text=True).stdout.splitlines())
    reviewed = reviews(root)
    cited = cited_files(root)
    rows = []
    for path in documentation(root):
        name = path.relative_to(root).as_posix()
        if name not in tracked:
            continue
        changed = git_date(root, "--", name)
        effective = max(changed, reviewed.get(name, changed))
        revision = git_value(root, "--format=%H", "--", name)
        if reviewed.get(name, changed) > changed:
            revision = "HEAD"
        files = changed_since(root, revision, cited.get(name, set()))
        age = (today - effective).days
        rows.append({"doc": name, "age": age, "files": files,
                     "rank": age + 30 * len(files)})
    return sorted(rows, key=lambda row: (row["rank"], row["age"], row["doc"]), reverse=True)


def print_rows(rows):
    print("RANK  AGE  DRIFT  DOCUMENT                                      CHANGED FILES")
    for row in rows:
        files = ", ".join(row["files"][:3])
        if len(row["files"]) > 3:
            files += f" (+{len(row['files']) - 3})"
        print(f"{row['rank']:4}  {row['age']:3}  {len(row['files']):5}  {row['doc']:<44} {files}")


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--top", type=int)
    parser.add_argument("--stale-days", type=int)
    parser.add_argument("--root", default=".")
    args = parser.parse_args(argv)
    rows = report(args.root)
    if args.stale_days is not None:
        rows = [row for row in rows if row["age"] >= args.stale_days]
    if args.top is not None:
        rows = rows[:args.top]
    print_rows(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
