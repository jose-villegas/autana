# Mermaid diagrams

GitHub renders ```` ```mermaid ```` fences natively in the repo view, the
only render target here. It fails silently: a broken diagram shows a parse
error where the picture should be, so `scripts/gates/check-mermaid.mjs`
renders every one through `mmdc` (mermaid-cli) and checks that it parses.

## Rendering the way GitHub does

GitHub draws each diagram in an iframe from
`viewscreen.githubusercontent.com/markdown/mermaid`, whose `mermaidMarkdown` bundle
carries its own mermaid and options.

The check renders with that mermaid version, pinned through `overrides` in
`scripts/gates/mermaid/package.json`, and those options, copied into
`scripts/gates/mermaid/github-config.json`, so a diagram comes out the shape GitHub
shows. `check-mermaid.mjs --github-version` reads the version GitHub runs today:

- exit 0 when it matches the pin
- exit 1 when GitHub moved: bump the override (`--write` does it and exits 0)
- exit 2 when the version cannot be read: a network failure, or the page or bundle changed shape

## Tall diagrams

A diagram more than `TALL_RATIO` (in `check-mermaid.mjs`) times taller than wide is
reported `TALL` with its size, and annotated on the pull request in CI. It is a
warning for now; `TALL_FAILS` turns it into a failure once no tracked diagram is tall.

The usual cause: mermaid drops a subgraph's `direction` once a link crosses the
subgraph's boundary, so a `flowchart TB` of `direction LR` subgraphs stacks into
one column. Link subgraph to subgraph, or set the flowchart's own direction.

## Validate

```sh
node scripts/gates/check-mermaid.mjs                 # every git-tracked *.md
node scripts/gates/check-mermaid.mjs path/to/file.md  # one file or directory
node scripts/gates/check-mermaid.mjs --verbose        # also list passes, with sizes
node scripts/gates/check-mermaid.mjs --github-version # GitHub's mermaid against the pin
```

File discovery is `git ls-files`, so an untracked scratch file and a vendored
submodule's own working tree are never scanned. Reports `file:line` for each
failure. Runs the mermaid-cli version `scripts/gates/mermaid/package.json` pins,
installed with `npm ci --prefix scripts/gates/mermaid`; without it, an `mmdc` on
`PATH` runs with a note naming both versions. It also needs a browser: the one
`PUPPETEER_EXECUTABLE_PATH` names if you set it, else a `chromium` or Chrome
on `PATH`, found by itself, else the Chrome mermaid-cli bundles for Puppeteer. A missing `mmdc` is reported once rather
than once per block.

## Pre-commit

`scripts/gates/check-mermaid-staged.sh` runs on every staged `.md` file that
contains a mermaid fence, as one step of the hook
`scripts/install-git-hooks.sh` installs. It checks the staged blob, not the
file on disk: `git show ":$file"` piped into `check-mermaid.mjs --stdin`, one
file at a time.

A failing diagram blocks the commit. A missing `node` or `mmdc` skips the
check with a warning instead. An `mmdc` that runs but cannot find a Chrome
still blocks the commit: the probe only proves the binary starts, not that
it can render, so every block then fails for real. `--no-verify` skips the
hook entirely; CI is what actually gates a diagram; see below.

## CI

`.github/workflows/mermaid.yml` installs `@mermaid-js/mermaid-cli`, points
it at a `--no-sandbox` Puppeteer config (the CI container has no
unprivileged user namespace for Chrome's own sandbox) via the
`CHECK_MERMAID_MMDC_ARGS` environment variable, and runs
`node scripts/gates/check-mermaid.mjs` over the whole repo. Path-filtered to
changes under any `.md` file or the validator itself, unlike this repo's
other workflows, because installing Chrome is real per-run cost that
nothing else here can affect.

A weekly scheduled run (or a manual one) runs `--github-version --write`: when GitHub
moved, it rewrites the override and lockfile and opens the `feature/mermaid-pin` pull
request, so the pin follows GitHub without waiting for a `.md` change. A version it cannot
read turns the run red.

## Tests

`scripts/gates/tests/test_check_mermaid.mjs` covers the pure parts: fence
extraction, the line-number rewrite, and the Windows argument-quoting
helper, the SVG size, the tall rule and the GitHub version parsing, with
`node --test`, no `mmdc` or `git` involved.
`scripts/run-tool-tests.sh` runs it alongside the Python tool suites.
