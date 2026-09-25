#!/usr/bin/env python3
"""Search the repository's documentation one section at a time.

    python scripts/docs/docs_search.py <question...>        best sections, excerpted
    python scripts/docs/docs_search.py --section PATH:LINE  one whole section
    python scripts/docs/docs_search.py --section 'PATH#heading words'
    python scripts/docs/docs_search.py --outline PATH       a document's headings
    python scripts/docs/docs_search.py --ask <question...>  a written answer, with sources
    python scripts/docs/docs_search.py --eval               retrieval score

Options: --top N (3), --more N (5), --budget CHARS (1800), --json, --lexical.

The unit of an answer is a section: a heading and the text up to the next
heading. Every tracked Markdown file is read, plus the private .dev notes when
that checkout is present, plus the header of every tracked script, so "how do
I run X" finds the script that documents itself. Each result prints the
paragraphs that carry the question, the section's path:line range to read the
rest, and the code it cites.

Two rankings are fused: BM25F over exact words, with the heading path weighted
above the body, and embedding similarity from docs_llama.py's local model when
it is set up. Without the model, search is the first alone and says so. The
index is rebuilt on every run, in about a second, so it is never stale; only
the vectors are cached. docs/tools/Docs-Search.md has the measurements.
"""
import argparse
import json
import math
import re
import subprocess
import sys
from collections import Counter, namedtuple
from pathlib import Path

import docs_llama

Section = namedtuple("Section", "path start end title headings level body cites")
Hit = namedtuple("Hit", "section score coverage similarity")

SKIPPED_PREFIXES = ("third_party/", ".claude/skills/", "launcher/components/")
DEV_SKIPPED_PREFIXES = ("records/", "hardware/text/", "agents/", "skills/")
SCRIPT_ROOTS = ("scripts/", "launcher/tools/", "launcher/test/", "launcher/main/apps/")
SCRIPT_SUFFIXES = (".py", ".sh", ".mjs")
# A plan describes code that does not exist yet, and a .dev note is history
# or workflow; both answer fewer questions than the documents of record.
PRIORS = (("docs/plans/", 0.8), (".dev/", 0.85))
# A list of links to other documents names every topic and answers none.
NAVIGATION = re.compile(r"^(related|see also|further reading|where to go next)\b", re.I)
NAVIGATION_PRIOR = 0.25
FIELD_WEIGHTS = {"headings": 4.0, "title": 2.0, "path": 1.5, "body": 1.0}
K1 = 1.2
B = 0.75
# Reciprocal-rank fusion of the embedding ranking with half-weighted BM25:
# on eval_questions.tsv, 36/45 top-3 and 30/45 top-1 against BM25's 30 and 19.
FUSION_K = 10
LEXICAL_WEIGHT = 0.5
FUSED_DEPTH = 50
WINDOW_CHARS = 650
WINDOWS_PER_SECTION = 12
WEAK_SIMILARITY = 0.75

STOPWORDS = frozenset("""
a an and are as at be but by can do does did for from has have how i if in
into is it its me my of on or so that the their them then there these this
to was what when where which who why will with would you your should could
we our get use used using one any all not no yes way need needs want
""".split())

WORD = re.compile(r"[A-Za-z][A-Za-z0-9_]*|\d+")
CAMEL = re.compile(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
FENCE = re.compile(r"^\s*(```+|~~~+)")
CITATION = re.compile(r"`([^`\n]+)`")
CITED_PATH = re.compile(r"^[\w./-]+/[\w.-]+\.\w+$|^[\w-]+\.(?:c|h|py|sh|md|json|cmake)$")
CITED_CODE = re.compile(r"^[A-Za-z_]\w*\(\)$|^[A-Z][A-Z0-9_]{3,}$")


def stem(word):
    for suffix, keep in (("ies", "y"), ("sses", "ss"), ("ing", ""), ("ed", ""), ("es", ""), ("s", "")):
        if word.endswith(suffix) and len(word) - len(suffix) >= 3:
            if suffix == "s" and word.endswith(("ss", "us", "is")):
                return word
            return word[: -len(suffix)] + keep
    return word


def tokens(text):
    """Lowercased stems, and an identifier both whole and split into its parts."""
    out = []
    for word in WORD.findall(text):
        parts = [p for chunk in word.split("_") for p in CAMEL.findall(chunk)]
        whole = word.lower()
        if whole not in STOPWORDS and len(whole) > 1:
            out.append(stem(whole))
        if len(parts) > 1:
            out.extend(stem(p.lower()) for p in parts
                       if len(p) > 1 and p.lower() not in STOPWORDS)
    return out


def git_files(root, *patterns):
    result = subprocess.run(["git", "-C", str(root), "ls-files", "--cached", "--others",
                             "--exclude-standard", *patterns],
                            capture_output=True, text=True, encoding="utf-8")
    return result.stdout.split() if result.returncode == 0 else []


def corpus_files(root):
    """(label, file) for every document: tracked Markdown, .dev notes, script headers."""
    root = Path(root)
    files = [(p, root / p) for p in git_files(root, "*.md")
             if not p.startswith(SKIPPED_PREFIXES)]
    dev = root / ".dev"
    if dev.is_dir():
        files += [(".dev/" + p, dev / p) for p in git_files(dev, "*.md")
                  if not p.startswith(DEV_SKIPPED_PREFIXES)]
    files += [(p, root / p) for p in git_files(root, *(f"*{s}" for s in SCRIPT_SUFFIXES))
              if p.startswith(SCRIPT_ROOTS) and "/tests/" not in p
              and not Path(p).name.startswith("test_")]
    return list(dict.fromkeys(files))


def corpus_signature(root):
    signature = []
    for label, file in corpus_files(root):
        try:
            stat = file.stat()
        except OSError:
            continue
        signature.append((label, stat.st_mtime_ns, stat.st_size))
    return tuple(signature)


def cites(text):
    found = []
    for quoted in CITATION.findall(text):
        quoted = quoted.strip()
        if (CITED_PATH.match(quoted) or CITED_CODE.match(quoted)) and quoted not in found:
            found.append(quoted)
    return tuple(found)


def markdown_sections(label, text):
    lines = text.splitlines()
    title = Path(label).stem.replace("-", " ")
    sections, stack, fence = [], [], None
    start, level, body = 1, 0, []

    def close(end):
        content = "\n".join(body).strip()
        if content or stack:
            names = tuple(name for _, name in stack)
            sections.append(Section(label, start, end, title, names, level,
                                    content, cites(content)))

    for number, line in enumerate(lines, 1):
        marker = FENCE.match(line)
        if marker:
            fence = None if fence and marker.group(1)[0] == fence else (fence or marker.group(1)[0])
        heading = None if fence or marker else HEADING.match(line)
        if not heading:
            body.append(line)
            continue
        close(number - 1)
        level, name = len(heading.group(1)), heading.group(2).strip()
        if level == 1 and not sections and not stack:
            title = name
        stack = [(lvl, n) for lvl, n in stack if lvl < level] + [(level, name)]
        start, body = number, []
    close(len(lines))
    return [s._replace(title=title) for s in sections]


def script_header(text):
    """The module docstring of a Python file, or the leading comment of a shell one."""
    docstring = re.match(r'\A(?:#![^\n]*\n)?(?:#[^\n]*\n|\s)*(?:"""|\'\'\')(.*?)(?:"""|\'\'\')', text, re.S)
    if docstring:
        return docstring.group(1).strip("\n"), docstring.group(0).count("\n") + 1
    lines = []
    for line in text.splitlines():
        if line.startswith("#!"):
            lines.append("")
        elif line.startswith(("#", "//")):
            lines.append(line.lstrip("#/").removeprefix(" "))
        elif line.strip() == "" and len(lines) < 2:
            lines.append("")
        else:
            break
    body = "\n".join(lines).strip("\n")
    return body, len(lines)


def script_sections(label, text):
    body, end = script_header(text)
    if len(body) < 80:
        return []
    return [Section(label, 1, max(end, 1), label, (), 0, body, cites(body))]


def read_sections(label, file):
    try:
        text = file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    return markdown_sections(label, text) if label.endswith(".md") else script_sections(label, text)


def prior(section):
    value = next((p for prefix, p in PRIORS if section.path.startswith(prefix)), 1.0)
    if section.headings and NAVIGATION.match(section.headings[-1]):
        value *= NAVIGATION_PRIOR
    return value


def unique(sections):
    """The first copy of every section, so a mirrored file answers once."""
    seen, out = set(), []
    for section in sections:
        key = (section.headings, section.body)
        if key not in seen:
            seen.add(key)
            out.append(section)
    return out


class Index:
    def __init__(self, root, semantic=True):
        self.root = Path(root)
        self.files = corpus_files(self.root)
        self.sections = unique(s for label, file in self.files for s in read_sections(label, file))
        self.fields = [self.section_fields(s) for s in self.sections]
        self.average = {f: sum(len(d[f]) for d in self.fields) / max(len(self.fields), 1) or 1
                        for f in FIELD_WEIGHTS}
        self.frequency = Counter(t for d in self.fields for t in set().union(*map(set, d.values())))
        self.vectors = None if semantic else False

    @staticmethod
    def section_fields(section):
        return {"headings": tokens(" ".join(section.headings)),
                "title": tokens(section.title),
                "path": tokens(section.path.replace("/", " ").replace(".", " ")),
                "body": tokens(section.body)}

    def idf(self, term):
        n = self.frequency.get(term, 0)
        return math.log(1 + (len(self.sections) - n + 0.5) / (n + 0.5))

    def score(self, fields, terms, phrases, section):
        total, matched = 0.0, 0
        counts = {f: Counter(fields[f]) for f in FIELD_WEIGHTS}
        for term in terms:
            weighted = sum(w * counts[f][term] / (1 - B + B * len(fields[f]) / self.average[f])
                           for f, w in FIELD_WEIGHTS.items())
            if weighted:
                matched += 1
                total += self.idf(term) * weighted * (K1 + 1) / (weighted + K1)
        if not matched:
            return 0.0, 0.0
        body = " ".join(fields["body"])
        heads = " ".join(fields["headings"])
        total += sum(0.6 * self.idf(p.split()[1]) for p in phrases if p in body or p in heads)
        coverage = matched / len(terms)
        return total * (0.4 + 0.6 * coverage) * prior(section), coverage

    def lexical(self, terms, ordered):
        phrases = {f"{a} {b}" for a, b in zip(ordered, ordered[1:])}
        scored = {}
        for number, (section, fields) in enumerate(zip(self.sections, self.fields)):
            value, coverage = self.score(fields, terms, phrases, section)
            if value > 0:
                scored[number] = (value, coverage)
        return scored

    def windows(self, number):
        section = self.sections[number]
        head = breadcrumb(section)[:200] + "\n"
        body = section.body
        return [head + body[at:at + WINDOW_CHARS]
                for at in range(0, max(len(body), 1), WINDOW_CHARS)][:WINDOWS_PER_SECTION]

    def semantic(self, question):
        """Similarity of every section to the question, or None with no model set up."""
        if self.vectors is False:
            return None
        if self.vectors is None:
            if not docs_llama.installed("embed") or not docs_llama.start():
                self.vectors = False
                return None
            owners, texts = [], []
            for number in range(len(self.sections)):
                for text in self.windows(number):
                    owners.append(number)
                    texts.append(text)
            cache = docs_llama.VectorCache()
            try:
                self.vectors = list(zip(owners, cache.vectors(texts)))
            except (OSError, ValueError, KeyError) as problem:
                return self.without_model(problem)
            cache.save()
        try:
            query = docs_llama.embed([question], query=True)[0]
        except (OSError, ValueError, KeyError) as problem:
            return self.without_model(problem)
        best = {}
        for owner, vector in self.vectors:
            similarity = sum(a * b for a, b in zip(query, vector))
            if similarity > best.get(owner, -1.0):
                best[owner] = similarity
        return best

    def without_model(self, problem):
        print(f"docs_search: embedding model failed ({problem}); exact words only",
              file=sys.stderr)
        self.vectors = False
        return None

    def search(self, question, limit=8, per_file=2, semantic=True):
        terms = list(dict.fromkeys(tokens(question)))
        ordered = tokens(question)
        lexical = self.lexical(terms, ordered) if terms else {}
        similarity = self.semantic(question) if semantic else None
        fused = Counter()
        lexical_order = sorted(lexical, key=lambda n: -lexical[n][0])[:FUSED_DEPTH]
        if similarity is None:
            fused.update({n: lexical[n][0] for n in lexical_order})
        else:
            dense_order = sorted(similarity, key=lambda n: -similarity[n])[:FUSED_DEPTH]
            for rank, number in enumerate(dense_order):
                fused[number] += prior(self.sections[number]) / (FUSION_K + rank)
            for rank, number in enumerate(lexical_order):
                fused[number] += LEXICAL_WEIGHT / (FUSION_K + rank)
        hits, per = [], Counter()
        for number, value in fused.most_common():
            section = self.sections[number]
            if per[section.path] < per_file:
                per[section.path] += 1
                coverage = lexical.get(number, (0, 0.0))[1]
                hits.append(Hit(section, value, coverage,
                                None if similarity is None else similarity.get(number)))
            if len(hits) == limit:
                break
        return hits, terms

    def unknown_terms(self, terms):
        return [t for t in terms if not self.frequency.get(t)]

    def find_section(self, reference):
        """PATH:LINE (the section holding that line) or PATH#heading words."""
        path, _, where = reference.partition("#") if "#" in reference else reference.rpartition(":")
        if not where and ":" not in reference and "#" not in reference:
            path, where = reference, ""
        path = path.replace("\\", "/").removeprefix("./")
        candidates = [s for s in self.sections if s.path == path]
        if not candidates:
            return None
        if where.isdigit():
            line = int(where)
            inside = [s for s in candidates if s.start <= line <= max(s.end, s.start)]
            return inside[0] if inside else None
        wanted = where.lower().strip()
        for s in candidates:
            if s.headings and wanted in s.headings[-1].lower():
                return s
        return candidates[0] if not wanted else None

    def children(self, section):
        return [s for s in self.sections if s.path == section.path and s.start > section.start
                and s.level > section.level and s.headings[:len(section.headings)] == section.headings]

    def subtree_end(self, section):
        later = [s for s in self.sections if s.path == section.path and s.start > section.start
                 and s.level <= section.level]
        return later[0].start - 1 if later else max(s.end for s in self.sections if s.path == section.path)


def blocks(body):
    """Paragraphs, lists, tables and fenced code, each kept whole."""
    out, current, fence = [], [], None
    for line in body.splitlines():
        marker = FENCE.match(line)
        if fence:
            current.append(line)
            if marker and marker.group(1)[0] == fence:
                out.append("\n".join(current))
                current, fence = [], None
            continue
        if marker:
            if current:
                out.append("\n".join(current))
            current, fence = [line], marker.group(1)[0]
        elif line.strip() in ("", "---"):
            if current:
                out.append("\n".join(current))
            current = []
        else:
            current.append(line)
    if current:
        out.append("\n".join(current))
    return [b for b in out if not b.lstrip().startswith("```mermaid")]


def excerpt(section, terms, budget, index):
    """The blocks that carry the most of the question, in document order, within budget."""
    wanted = set(terms)
    chosen, used = [], 0
    candidates = blocks(section.body)
    ranked = sorted(range(len(candidates)), key=lambda i: -sum(
        index.idf(t) for t in set(tokens(candidates[i])) & wanted) + i * 1e-3)
    for i in ranked:
        text = candidates[i]
        if not set(tokens(text)) & wanted and chosen:
            break
        if used + len(text) > budget:
            if not chosen:
                chosen.append((i, clip(text, budget)))
            continue
        chosen.append((i, text))
        used += len(text)
    return "\n\n".join(text for _, text in sorted(chosen))


def clip(text, budget):
    cut = text[:budget]
    stop = max(cut.rfind(". "), cut.rfind("\n"))
    return (cut[:stop + 1] if stop > budget // 2 else cut).rstrip() + " ..."


def location(section):
    return f"{section.path}:{section.start}-{max(section.end, section.start)}"


def breadcrumb(section):
    return " > ".join((section.title,) + tuple(h for h in section.headings if h != section.title))


def answer(index, question, top=3, more=5, budget=1800):
    hits, terms = index.search(question, limit=top + more)
    unknown = index.unknown_terms(terms)
    shares = [0.5, 0.3, 0.2] + [0.15] * max(top - 3, 0)
    results = []
    for rank, hit in enumerate(hits[:top]):
        results.append({"location": location(hit.section), "heading": breadcrumb(hit.section),
                        "score": round(hit.score, 2), "coverage": round(hit.coverage, 2),
                        "excerpt": excerpt(hit.section, terms, int(budget * shares[rank]), index),
                        "cites": list(hit.section.cites[:6])})
    best = hits[0] if hits else None
    weak = not best or (best.coverage < 0.5
                        and (best.similarity is None or best.similarity < WEAK_SIMILARITY))
    return {"question": question, "terms": terms, "unknown": unknown, "weak": weak,
            "semantic": bool(index.vectors), "results": results,
            "more": [{"location": location(h.section), "heading": breadcrumb(h.section)}
                     for h in hits[top:]]}


def format_answer(reply):
    lines = []
    if not reply["semantic"]:
        lines.append("exact words only; `python scripts/docs/docs_llama.py setup` adds meaning")
    if reply["unknown"]:
        lines.append("no document uses: " + ", ".join(reply["unknown"]))
    if not reply["results"]:
        lines.append("nothing matched; try other words, or --outline a likely document")
        return "\n".join(lines)
    if reply["weak"]:
        lines.append("weak match: nothing below is close to the question; check before relying on it")
    for number, result in enumerate(reply["results"], 1):
        lines.append(f"[{number}] {result['location']}  {result['heading']}")
        lines.extend("    " + line if line else "" for line in result["excerpt"].splitlines())
        if result["cites"]:
            lines.append("    cites: " + ", ".join(result["cites"]))
        lines.append("")
    if reply["more"]:
        lines.append("more:")
        lines.extend(f"  {m['location']}  {m['heading']}" for m in reply["more"])
    lines.append("whole section: --section PATH:LINE")
    return "\n".join(lines)


ASK_RULES = (
    "You answer questions about one firmware repository using only the numbered "
    "documentation sections given. Answer in at most 120 words. After each claim put "
    "the section number it came from, like [2]. If the sections do not answer the "
    "question, say so in one sentence and name the section most worth reading. "
    "Never add facts that are not in the sections.")


def ask(index, question, stream=None, budget=5000):
    """A short answer written by the local chat model from the best sections, with its sources."""
    reply = answer(index, question, top=4, more=0, budget=budget)
    context = "\n\n".join(f"[{n}] {r['location']}  {r['heading']}\n{r['excerpt']}"
                          for n, r in enumerate(reply["results"], 1))
    messages = [{"role": "system", "content": ASK_RULES},
                {"role": "user", "content": f"{context}\n\nQuestion: {question}"}]
    text = docs_llama.chat(messages, stream=stream)
    return text, reply


def format_section(index, section, deep=False):
    end = index.subtree_end(section) if deep else section.end
    lines = [f"{section.path}:{section.start}-{end}  {breadcrumb(section)}", ""]
    if deep:
        text = read_lines(index, section.path, section.start + (1 if section.level else 0), end)
        lines.append(text.strip())
    else:
        lines.append(section.body)
        children = index.children(section)
        if children:
            lines += ["", "subsections (--deep prints them too):"]
            lines += [f"  {s.path}:{s.start}  {'  ' * (s.level - section.level - 1)}{s.headings[-1]}"
                      f"  ({len(s.body)} chars)" for s in children]
    return "\n".join(lines)


def read_lines(index, label, first, last):
    file = dict(index.files).get(label)
    text = file.read_text(encoding="utf-8", errors="replace").splitlines() if file else []
    return "\n".join(text[first - 1:last])


def format_outline(index, path):
    path = path.replace("\\", "/").removeprefix("./")
    sections = [s for s in index.sections if s.path == path]
    if not sections:
        return None
    lines = [f"{path}  {sections[0].title}"]
    lines += [f"  {s.start:>5}  {'  ' * max(s.level - 1, 0)}{s.headings[-1] if s.headings else '(intro)'}"
              f"  ({len(s.body)} chars)" for s in sections]
    return "\n".join(lines)


def load_eval():
    rows = []
    for line in (Path(__file__).with_name("eval_questions.tsv")).read_text(encoding="utf-8").splitlines():
        question, path, heading = line.split("\t")
        rows.append((question, path, heading.lower()))
    return rows


def evaluate(index, depth=3):
    """(question, rank or None) per row whose document is present; a hit is the section or one inside it."""
    present = {s.path for s in index.sections}
    ranks = []
    for question, path, heading in load_eval():
        if path not in present:
            continue
        hits, _ = index.search(question, limit=depth, per_file=depth)
        rank = next((i for i, h in enumerate(hits, 1) if h.section.path == path
                     and any(heading in name.lower() for name in h.section.headings + (h.section.title,))),
                    None)
        ranks.append((question, rank))
    return ranks


def repo_root():
    result = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    return Path(result.stdout.strip()) if result.returncode == 0 else Path(__file__).resolve().parents[2]


def main(argv=None, root=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(prog="docs_search", description=__doc__.split("\n")[0])
    parser.add_argument("question", nargs="*")
    parser.add_argument("--section")
    parser.add_argument("--deep", action="store_true")
    parser.add_argument("--outline")
    parser.add_argument("--eval", action="store_true")
    parser.add_argument("--ask", action="store_true")
    parser.add_argument("--top", type=int, default=3)
    parser.add_argument("--more", type=int, default=5)
    parser.add_argument("--budget", type=int, default=1800)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--lexical", action="store_true")
    args = parser.parse_args(argv)
    index = Index(root or repo_root(), semantic=not args.lexical)
    if args.eval:
        ranks = evaluate(index)
        for question, rank in ranks:
            print(f"{rank or '-':>2}  {question}")
        found = sum(1 for _, r in ranks if r)
        print("hybrid" if index.vectors else "lexical", end=": ")
        print(f"top-3 recall {found}/{len(ranks)}, top-1 {sum(1 for _, r in ranks if r == 1)}")
        return 0
    if args.outline:
        text = format_outline(index, args.outline)
        print(text or f"no document {args.outline}")
        return 0 if text else 1
    if args.section:
        section = index.find_section(args.section)
        if not section:
            print(f"no section {args.section}; --outline PATH lists them")
            return 1
        print(format_section(index, section, args.deep))
        return 0
    if not args.question:
        parser.print_usage()
        return 2
    if args.ask:
        if not docs_llama.installed("chat") or not docs_llama.start():
            print("no chat model: python scripts/docs/docs_llama.py setup --chat")
            return 1
        text, reply = ask(index, " ".join(args.question),
                          stream=lambda piece: print(piece, end="", flush=True))
        print("\n\nsources:")
        print("\n".join(f"  [{n}] {r['location']}  {r['heading']}"
                        for n, r in enumerate(reply["results"], 1)))
        return 0
    reply = answer(index, " ".join(args.question), args.top, args.more, args.budget)
    print(json.dumps(reply, indent=1) if args.json else format_answer(reply))
    return 0


if __name__ == "__main__":
    sys.exit(main())
