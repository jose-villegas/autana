# Math formulas

GitHub renders maths in Markdown with MathJax, in the browser, and never
fails a check: a formula it cannot render shows as raw text or a red box.
`scripts/gates/check-math.mjs` finds every formula the way GitHub does and
renders it with GitHub's own MathJax configuration.

## Write a formula

| Form | Use for | Markdown touches it? |
|---|---|---|
| ```` ```math ```` fence | display maths | no |
| `` $`...`$ `` | inline maths | no |
| `$...$` | short inline maths with no `_`, `*` or `\` escape | yes |
| `$$...$$` paragraph | nothing; use the fence | yes |

GitHub parses the Markdown first and looks for `$` in the text that comes
out. So in `$...$` and `$$` paragraphs:

- two `_` or `*` can pair up as emphasis, and the formulas around them become
  plain text: `$\mathcal{E}_{a}$ and $\mathcal{E}_{b}$` renders as text with
  the middle in italics;
- a backslash before ASCII punctuation is an escape: `\,` reaches MathJax as
  `,`, `\{` as `{`, `\\` as `\`;
- a `$` opens only after a space, `(` or the start of a line, and closes only
  when not followed by a letter, digit, `_` or a backtick;
- nothing inside emphasis, `<b>`, a link or code is maths.

The fence and `` $`...`$ `` skip all of that. In a fence, write `\lt` or put a
space after `<`: GitHub reads `<b` as the start of an HTML tag and drops the
rest of the formula.

MathJax itself refuses what GitHub turned off: `\operatorname`, `\phantom`,
`\colorbox`, `\DeclareMathOperator`, `\unicode` and the rest of the list in
the script, plus `\newcommand`, `\def`, `\require`, `\bbox`, `\href` and
`\class`, whose packages are removed. Use `\mathrm{name}` for an operator.

## Validate

```sh
node scripts/gates/check-math.mjs                 # every git-tracked *.md
node scripts/gates/check-math.mjs path/to/file.md # one file or directory
node scripts/gates/check-math.mjs --verbose       # also list passes
```

It reports `file:line`, the formula, and why GitHub fails it: a TeX error
(undefined control sequence, unbalanced braces, missing argument), a refused
macro, a swallowed tag, the brace budget, or what Markdown did to it.
Requires `npm install -g mathjax-full@3.2.0 markdown-it@14.1.0`; 3.2.0 is the
MathJax GitHub serves, and any other version is refused.

## Where GitHub's configuration comes from

The script header names its source: GitHub's `<math-renderer>` element,
served as `chunk-lazy-element-math-renderer-*.js`, which carries the refused
macro list, the packages removed from MathJax's full set, and the brace
limits. The header says how to refresh them. The delimiter rules were
measured by rendering probe files in a secret gist's file view, and
`scripts/gates/tests/test_check_math.mjs` pins them. An undefined macro,
which GitHub paints red rather than failing, fails here.

Two cases differ from GitHub: a single `~` pair (GitHub strikes it through;
the parser here needs `~~`), and a `` $` `` whose code span closes without
`` `$ `` (GitHub then matches across to a later one).

## Pre-commit and CI

`scripts/gates/check-math-staged.sh` checks the staged blob of every staged
`.md` file with a `$` or a math fence, as one step of the hook
`scripts/install-git-hooks.sh` installs. A missing `node`, `mathjax-full` or
`markdown-it` skips it with a warning.

`.github/workflows/math.yml` installs both packages and runs the script over
the whole repo, path-filtered to `.md` files and the validator. The tests run
with the other tool suites in `scripts/run-tool-tests.sh`.
