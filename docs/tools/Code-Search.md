# Code search

`scripts/docs/code_search.py` answers "does something already do this?" before you write a helper. It searches the public names and purposes of every tracked header and Python module, apart from vendored and generated files. It rebuilds its index on every run, in a few seconds, so nothing is committed and nothing goes stale.

```sh
python scripts/docs/code_search.py strbuf append          # everything indexed
python scripts/docs/code_search.py --shared ring buffer   # only owners used from two or more directories
python scripts/docs/code_search.py --check                # every indexed file states a purpose
python scripts/docs/code_search.py --eval                 # recall over scripts/docs/code_search_eval.tsv
```

Each search is logged on this machine through `query_log.py`, beside `autana docs`; `python scripts/docs/query_log.py --tool code` summarises what was asked and what found nothing.

## Matching and order

| A word matches | When |
|---|---|
| a public name | it starts the name or one of its parts, split at `_` and case changes (`append` finds `strbuf_appendf`) |
| a file | it is a word of the file's banner comment, its docstring or its path; words are stemmed, so `append` also finds `appending` |

| Order | Rule |
|---|---|
| Score | each matched word adds its rarity, log(files / files it matches): twice on a name, once in the file's prose, so a query split between the two adds both |
| Ties | files used from two or more directories first, then by path |
| Spread | one file shows three names before the next file's |

The first 20 results are shown, followed by a count of the rest. When nothing matches, the five names spelled closest to the query are shown instead, and the command exits 1.

## A result

```
launcher/packages/collections/include/collections/strbuf.h:48  strbuf_appendf
    static inline __attribute__((format(printf, 2, 3))) bool strbuf_appendf(strbuf_t* b, const char* format, ...)
    strbuf: text built by appending pieces into a caller's fixed buffer.
```

Each result is `path:line` and the name, the declaration on one line (a type's body as `{ ... }`), and the file's purpose: the first sentence of its banner or docstring.

## The purpose gate

`--check` fails on each indexed file that has no banner comment or docstring, naming the file, and CI runs it. A header's banner is its first comment, with nothing before it except `#pragma once`.

## Measuring it

Each row of `scripts/docs/code_search_eval.tsv` holds words someone typed, or would type, before writing a helper that already existed, the file that owned it, and the ticket where the second copy was merged into it. `--eval` reports how many owners land in the first 20 results (recall@20), in the first 5, and the mean reciprocal rank. Add a row whenever a duplicate is merged.

References and call sites are not indexed; use your editor or `git grep` for those.
