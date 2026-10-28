# C style guide

The style of first-party C under `launcher/main/` and its host and device tests.
Vendored and generated files keep their upstream or generator form. Apps follow the
guide but never supply its conventions or examples, since they can be deleted whole.

The structure follows [OpenBSD style(9)](https://man.openbsd.org/style.9), interfaces
[POSIX](https://pubs.opengroup.org/onlinepubs/9799919799/functions/V2_chap02.html), and
invariants [SQLite's assert](https://sqlite.org/assert.html). Their surface syntax never
overrides this repository's: four spaces, attached braces, pointer-left declarations, plain
names, and declarations near first use.

Cppcheck's MISRA addon (`launcher/tools/quality/misra_check.sh`, `misra_tidy_gate.py`)
compiler warnings and `scripts/gates/check_style_audit.py` report semantic findings, not
formatting or naming rules; commands are in
[`launcher/tools/quality/README.md`](../../launcher/tools/quality/README.md).

| Topic | Page |
|---|---|
| `IRAM_ATTR`, `noinline`, `always_inline`, constructors | [Function attributes](Function-Attributes.md) |
| Tasks, cores, spinlocks, atomics, queues | [Concurrency](Concurrency.md) |
| Driver, HAL and register access, wrapping a driver function | [Register access](Register-Access.md) |
| Placing code or data with a linker fragment | [Linker fragments](../memory/Linker-Fragments.md) |

## Mechanical rules

`.clang-format` defines these rules and `.github/workflows/format.yml` enforces them. It
lists only where the tree departs from LLVM style. If prose and formatter disagree, fix the
prose or change the formatter in its own reviewed PR; never hand-format around it.

| Rule | Note |
|---|---|
| Indent with four spaces, never tabs. | |
| Attach opening braces; brace every control-statement body. | |
| Bind `*` to the type: `gfx_color_t* fb`. | |
| Put a definition's return type on its own line above the name. | The least guessable rule. |
| Keep lines within 120 columns where no token, string or unflowed comment must change. | |
| Take the formatter's spacing, continuation, blank-line and `case` indentation. | |
| Let clang-format sort each include block, system headers first; keep the blank lines between blocks. | Sorting is per block: the blank line after the own header keeps it first. |

<!-- generated: c-style-clamp-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* Clamp a pixel coordinate to its upper bound. */
static int
clamp_px(int value, int max_px) {
    if (value > max_px) {
        return max_px;
    }
    return value;
}
```
<!-- /generated: c-style-clamp-example -->

Check, then format, only the first-party files you touched; never the whole tree or
vendored or generated sources:

```sh
scripts/gates/check-format.sh --check path/to/file.c path/to/file.h
scripts/gates/check-format.sh path/to/file.c path/to/file.h
```

### One formatter version

- Use the exact esp-clang the pinned ESP-IDF bundles (`launcher/idf-pin.json`); CI uses the same one.
- `check-format.sh` runs another patch release of that major with a note and refuses other majors.
- `CLANG_FORMAT_ANY_VERSION=1` forces another major, and reformats files you did not touch.
- An ESP-IDF bump that changes the esp-clang major reformats the tree in its own commit, listed in `.git-blame-ignore-revs`.

### Where the rules are checked

`scripts/gates/format-file-list.sh` is the one file list for hook and CI: vendored trees
by path, generated files by banner ([Generated files](../tools/Generated-Files.md)).

```sh
scripts/install-git-hooks.sh          # opt in to pre-commit and pre-push hooks
scripts/install-git-hooks.sh --status # is it active in this clone?
scripts/gates/format-file-list.sh | xargs scripts/gates/check-format.sh --check  # what CI runs
```

| Check | What it runs |
|---|---|
| Pre-commit | Format of the *staged* C and headers, executable bits, and the `STAGED_TEXT_GATE` steps of `comment-rules.yml` on an index snapshot. Run the text part alone with `scripts/gates/check-text-rules-staged.sh`; install its dependencies with `python -m pip install -r scripts/gates/requirements.txt`. Without ESP-IDF the citation check lists names it cannot verify. |
| Pre-push | Branch names, and sanitized host tests when the range changes `.c` or `.h`; see [Host tests: git hooks](../testing/Host-Tests.md#git-hooks). |
| CI | Every listed file on every pull request and push to `main`. Hooks are feedback (`--no-verify` skips them, and they never see merges or CI commits); CI is the gate. |

### Duplication gates

| Rule | Gate |
|---|---|
| Search for an owner before writing a helper; extend one that fits. | `autana docs "<the job>"`, `git grep -n -w <name>` |
| Never grow a file pair's copied tokens; extract a shared owner instead. | `python scripts/gates/check_clones.py` (jscpd: `npm ci --prefix scripts/gates`) |
| Never restate an integer a file already sees through its `#include "..."` chain, or a firmware string a Python tool repeats; use the named constant. | `python scripts/gates/check_magic_numbers.py` |
| Mark a deliberate literal on its line with `/* magic: reason */` (`# magic: reason` in Python); the reason may not be empty. | `check_magic_numbers.py` |

Both gates compare HEAD with its merge-base with `--base` (`origin/main` unless the pull
request targets another branch), or with `HEAD~1` on that branch. Both take `--report` and run
in the Linux comment-rules workflow.

Clone budgets, from the script's docstring:

- Each sorted file pair has its own budget; new fragments may spend only tokens of base fragments gone from HEAD.
- A surviving shrink, or removing another pair, gives no headroom; renames map to base names; line shifts change nothing.
- Code moved out of a file the change deletes keeps its fragments.
- Not clones: `#include` or `RUN_TEST` blocks, literal-only `#define`s, split declarations, added groupings, comments, whitespace.
- Scope: tracked C, C++ and Python under `launcher/` and `scripts/`, minus vendored, test-framework, generated and fixture-data files.
- `MIN_TOKENS` sets the threshold; `--min-tokens N` reports another one without checking. Review a reported pair before extracting.
- A shorter Python function is a clone when its body matches another's once its parameters and locals are renamed. Literals, attributes and globals must match exactly, and the docstring and annotations are ignored. Functions under `MIN_FUNCTION_NODES` AST nodes are skipped, since at that size they are idioms rather than helpers.

## Judgment rules

These need a reader who understands the code; never script a guess at them. Only the
decidable parts of the comment rules are scripted: length, cited names and app names.

### Comments

**A comment is the exception; most code needs none.** Prefer a better name or an
extracted function to prose explaining a mechanism.

| Rule | Gate |
|---|---|
| Never comment the HOW. Needing to comment parts of a function separately means the function is the problem. | `check_style_audit.py` HEADING-COMMENT warns on a ≤5-word label inside a function |
| Drop the WHAT; the names already say it. | |
| Write the WHY only for a constraint, an external fact, a measured number, or a decision someone would undo. | |
| Never narrate the journey ("a first attempt", "was reverted"); git log owns it. | `find_narrative_comments.py` lists cases |
| Never say where code moved; state what the code there must obey, or delete. | `find_narrative_comments.py --tombstones` (CI) |
| Update a comment in the edit that changes its code, or delete it. | |
| Open every firmware module with a one-sentence header, `name: ...`, in its .h or .c, and every header and Python module with a sentence saying what it is for. Test suites need none. | `check_file_headers.py` (CI) |
| Never write a header that declares nothing; describe a folder in its `README.md`. | `check_file_headers.py` (CI) |
| Use a style(9) shape: one-line `/* ... */`, a three-line `VERY important` block, or filled sentences. | |
| Never put headings or `/*====` rules in a comment; a comment needing sections belongs in `docs/`. | `check_style_audit.py` |
| Aim for 300 characters per comment; 500 is the ceiling. | `check_comment_length.py` (CI) |
| Aim for 30 lines in a file's header (its first comment); 50 is the ceiling and already too long. | `check_comment_length.py` (CI) |
| Cite only names that exist: full function or constant names, a family as `PREFIX_*` (never a shortened name), a file only where its code spells the name, a heading only where a comment carries it. | `check_comment_symbols.py` (CI) |
| Below `apps/`, never name an app, in a comment or an engine document; say what shape of caller needs the thing, or the rule it must follow. | `check_comment_layers.py` (CI) |
| Below `apps/`, never borrow an app's vocabulary either; state the constraint (block size, call rate). | read, not checked |

Length counts adjacent own-line comments as one, with markers and `*` gutters stripped, so
splitting or re-wrapping never changes the score. A header says what the module is and
what was rejected; prose about particular code goes beside that code.

A function the firmware calls but does not define resolves if ESP-IDF or its C library
declares it for any chip; runs without ESP-IDF count such names instead of failing.

The app rule is the comment half of the layering rule in
[Firmware architecture](../firmware/README.md). It covers every Markdown file under `docs/`
except an app's own folder and `docs/plans/`. An app's files may name anything below them.

```sh
scripts/gates/check-comment-length.sh                 # whole repo, 20 worst listed
scripts/gates/check-comment-length.sh --changed main  # only comments a change touches
scripts/gates/check-comment-length.sh --files         # per-file counts
```

### Names

| Rule | Example |
|---|---|
| Use lowercase `snake_case` for functions, variables and fields. | |
| Start a public function with its module name, then the action; keep the prefix stable. | `display_init()`, `imu_read()` |
| Give a static function the shortest clear name in its file; no `prv_`, Hungarian or linkage prefixes. | `qspi_bus_up()` |
| Name predicates so true is obvious. | `is_`, `has_`, `can_`, `may_have_`, `*_ready()` |
| Use `bool`, `true`, `false` for boolean state, never integers. | |
| Name project types lowercase with `_t`; enum constants and macros uppercase. | `gfx_color_t`, `DISPLAY_PORTRAIT` |
| Add a struct tag only when a forward declaration needs it; no typedefs no interface needs. | |
| Call a unit's ESP logging tag `TAG`. | |
| Name the domain quantity, not its width; add a unit when ambiguous. | `dt_ms`, `now_us`, `radius_px` |
| Prefix output parameters with `out_` where it separates them from inputs. | `out_size` |

### Integer and type discipline

| Use | For |
|---|---|
| Fixed-width types | width or signedness in the contract: pixels, registers, serialized bytes, protocol fields, deliberate wrap |
| `size_t` | object sizes, `sizeof`, allocation sizes, indexes bounded by an object's size |
| `int` | coordinates, small counts, loop indexes, enum-like values, `-1` sentinels, when the range fits; keeps subtraction signed |
| `unsigned` | bit masks, hashes, intentional modulo when no width is required |

- Convert at a boundary, then calculate in one type; never mix signedness to silence a warning.
- Make the range evident (a check, `_Static_assert`, or the source type) before narrowing.
- Write `sizeof object`, not `sizeof(type)`, when allocating or copying that object.
- Treat MISRA 10.x essential-type findings per expression; never swap every `int` for a fixed width.

### Headers and module boundaries

- Give each module one public header for its types, constants and declarations; add a private header only when several of its `.c` files share details.
- Never create forwarding or one-declaration headers.
- Use `#pragma once`; a header includes what its declarations need and never relies on includer order or transitive includes.
- Keep implementation-only includes in the `.c` file.
- Include order in a `.c`: its own header (if it has one), standard library, ESP-IDF and external, then project.
- Qualify project includes by layer, even within one folder: `"gfx/gfx.h"`, never `"gfx.h"`; see [Firmware architecture](../firmware/README.md).
- Never expose a lower layer to a higher-layer type to avoid passing a small value.
- Keep a module's public contract small and stable.
- Export a function only when another translation unit needs it, not to shorten a file.
- Give a function one job; split it when the new function's name and contract are clearer than the block.

### Assertions

- Use `_Static_assert` for what the compiler can prove: encodings, capacities, strides, sizes, constant relations.
- Use `assert()` for internal invariants no caller can break: a helper's established precondition, an index from validated state, a state-machine relationship.
- Never give an assertion side effects.
- Never `assert()` allocation failure, absent devices, I/O errors, untrusted input, or anything the product must handle; assertions may compile out, so keep a branch with a return, fallback or terminal action.
- Add a runtime assertion for a real internal contract only; never because a case seems unlikely or to reach a count.

### Errors and failure

- Return errors to the boundary that decides what they mean; driver-facing helpers keep `esp_err_t`.
- A module whose callers only need available/unavailable may return `bool` at its public boundary.
- Lookups and geometry return a documented sentinel (`false`, `NULL`, `-1`) when absence is ordinary.
- Log once, where the failure gains meaning, never at every level of a call chain; a helper whose caller adds context returns without logging.
- Optional peripheral or feature: log and continue degraded. Required startup resource: log and stop before the frame loop (park the task so the board stays flashable).
- Invalid internal state is an assertion; a recoverable hardware condition never is.
- Return early for failed preconditions and finished edge cases. MISRA 15.5 (single exit) is advisory and disapplied here.
- Never return while holding part of what a function acquired: acquire all, test once, release everything on the one failure path.
- Keep return codes small: no project-wide error enum; `esp_err_t`, `bool`, or a module enum only for three or more outcomes callers treat differently.

<!-- generated: c-style-acquire-all-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* Return both buffers to the caller, or release every partial acquisition. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static bool
acquire_all(size_t n, uint8_t** out_a, uint8_t** out_b) {
    uint8_t* a = malloc(n);
    uint8_t* b = malloc(n);
    if (a == NULL || b == NULL) {
        free(a); /* free(NULL) is a no-op */
        free(b);
        return false;
    }
    *out_a = a;
    *out_b = b;
    return true;
}
```
<!-- /generated: c-style-acquire-all-example -->
