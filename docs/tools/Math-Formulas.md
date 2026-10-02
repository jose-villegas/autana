# Math formulas

GitHub renders maths in Markdown with MathJax, in the browser, and never
fails a check: a formula it cannot render shows as raw text or a red box.
`scripts/gates/check-math.mjs` holds every formula to the forms below and
renders each one with GitHub's own MathJax configuration.

## Write a formula

| Form | Use for | The gate |
|---|---|---|
| ```` ```math ```` fence | display maths | renders it |
| `` $`...`$ `` | inline maths | renders it |
| `$...$` | short inline maths | refuses `_`, `*` or a backslash before punctuation in it |
| `$$...$$` | nothing | refuses it |

GitHub parses the Markdown before it looks for maths, so in `$...$` two `_`
or `*` can pair up as emphasis and turn the formulas around them into plain
text, and a backslash before punctuation is an escape: `\,` reaches MathJax
as `,`, `\{` as `{`, `\\` as `\`. `$$` goes through the same parsing and
depends on its surroundings: a paragraph of its own is display maths, but
inside a sentence GitHub renders it inline. The fence and `` $`...`$ `` skip
all of that.

GitHub also leaves as text any formula inside emphasis, a link, `<b>` or a
footnote; the gate reports those. In a fence, write `\lt` or put a space
after `<`: GitHub reads `<b` as an HTML tag and drops the rest.

MathJax refuses what GitHub turned off: `\operatorname`, `\phantom`,
`\colorbox`, `\DeclareMathOperator`, `\unicode` and the rest of the list in
the script, plus `\newcommand`, `\def`, `\require`, `\bbox`, `\href` and
`\class`, whose packages are removed. Use `\mathrm{name}` for an operator.

## Validate

```sh
npm ci --prefix scripts/gates                     # once, and after package.json changes
node scripts/gates/check-math.mjs                 # every git-tracked *.md
node scripts/gates/check-math.mjs path/to/file.md # one file or directory
```

It reports `file:line`, the formula, and why: a refused form, a TeX error
(undefined control sequence, unbalanced braces, missing argument), a refused
macro, a swallowed tag, or the brace budget. `scripts/gates/package.json`
pins the exact versions, MathJax 3.2.0 being what GitHub serves; the script
refuses to run on any other.

Where GitHub's configuration comes from, and how to refresh it, is in the
script's header.

Two cases differ from GitHub: a single `~` pair (GitHub strikes it through;
the parser here needs `~~`), and a `` $` `` whose code span closes without
`` `$ `` (GitHub then matches across to a later one).

## Pre-commit and CI

`scripts/gates/check-math-staged.sh` checks the staged blob of every staged
`.md` file with a `$` or a math fence, as one step of the hook
`scripts/install-git-hooks.sh` installs. Without the pinned dependencies it
skips with a warning.

`.github/workflows/math.yml` runs `npm ci --prefix scripts/gates` and the
script over the whole repo, path-filtered to `.md` files and the gate's own
files. The tests run with the other tool suites in
`scripts/run-tool-tests.sh`.
