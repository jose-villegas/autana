# Mermaid diagrams

GitHub renders ```` ```mermaid ```` fences natively in the repo view - the
only render target here. It fails silently: a broken diagram shows a parse
error where the picture should be, and nothing else in the tree catches it.

## Validate

```sh
node scripts/gates/validate-mermaid.mjs                 # docs/, README.md, editor/
node scripts/gates/validate-mermaid.mjs path/to/file.md  # one file or directory
node scripts/gates/validate-mermaid.mjs --verbose         # also list passes
```

Renders every fenced block through `mmdc` (mermaid-cli) and reports
`file:line` for each failure. Requires
`npm install -g @mermaid-js/mermaid-cli` and a Chrome Puppeteer can find; a
missing `mmdc` is reported once rather than once per block.

## Pre-commit

`scripts/gates/check-mermaid-staged.sh` runs on every staged `.md` file that
contains a mermaid fence, as one step of the hook
`scripts/install-git-hooks.sh` installs. A failing diagram blocks the
commit. A missing `node` or `mmdc` skips the check with a warning instead -
same as the rest of that hook, it is feedback, not the gate, and
`--no-verify` skips it entirely.

## Tests

`scripts/gates/tests/test_validate_mermaid.mjs` covers the pure parts -
fence extraction and the line-number rewrite - with `node --test`, no
`mmdc` involved. `scripts/run-tool-tests.sh` runs it alongside the Python
tool suites.

Not wired into CI: the validator needs a real Chrome for Puppeteer to
drive, which none of the existing GitHub Actions runners in this repo set
up. Adding that would mean a new job installing Chrome (or
`@mermaid-js/mermaid-cli`'s own bundled one) before running
`node scripts/gates/validate-mermaid.mjs`.
