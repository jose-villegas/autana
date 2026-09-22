# Mermaid diagrams

GitHub renders ```` ```mermaid ```` fences natively in the repo view - the
only render target here. It fails silently: a broken diagram shows a parse
error where the picture should be, so `scripts/gates/check-mermaid.mjs`
renders every one through `mmdc` (mermaid-cli) and checks that it parses.

## Validate

```sh
node scripts/gates/check-mermaid.mjs                 # every git-tracked *.md
node scripts/gates/check-mermaid.mjs path/to/file.md  # one file or directory
node scripts/gates/check-mermaid.mjs --verbose        # also list passes
```

File discovery is `git ls-files`, so an untracked scratch file and a vendored
submodule's own working tree are never scanned. Reports `file:line` for each
failure. Requires `npm install -g @mermaid-js/mermaid-cli` and the Chrome it
bundles for Puppeteer; a missing `mmdc` is reported once rather than once
per block.

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
hook entirely; CI is what actually gates a diagram - see below.

## CI

`.github/workflows/mermaid.yml` installs `@mermaid-js/mermaid-cli`, points
it at a `--no-sandbox` Puppeteer config (the CI container has no
unprivileged user namespace for Chrome's own sandbox) via the
`CHECK_MERMAID_MMDC_ARGS` environment variable, and runs
`node scripts/gates/check-mermaid.mjs` over the whole repo. Path-filtered to
changes under any `.md` file or the validator itself, unlike this repo's
other workflows, because installing Chrome is real per-run cost that
nothing else here can affect.

## Tests

`scripts/gates/tests/test_check_mermaid.mjs` covers the pure parts - fence
extraction, the line-number rewrite, and the Windows argument-quoting helper
- with `node --test`, no `mmdc` or `git` involved. `scripts/run-tool-tests.sh`
runs it alongside the Python tool suites.
