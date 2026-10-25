# Settings

A setting is a global option before the command, or a key in the optional,
gitignored `autana.local.toml` in the project folder, which covers every
command run from that checkout. `autana help config` lists the keys.

A CI job that should fail rather than queue writes `autana --wait 0 <command>`
and names itself with `--owner`. The other global flags are in
[the flag table](README.md#global-flags).

| Key | Meaning |
|---|---|
| `idf_installs` | ESP-IDF checkouts searched before known installs ([Build-Environment.md](../tools/Build-Environment.md)). |
| `build_root` | External build root. |
| `docs_extra` | Extra Markdown files or folders `autana docs` searches. |
| `records` | Where captures and `index.jsonl` land; `.records/device` without it ([Flash-and-Captures.md](Flash-and-Captures.md#where-a-capture-lands)). |
| `lock_hook` | A shell command run on every lock event ([Device-Lock.md](Device-Lock.md#lock-events)). |
| `session_vars` | Environment variables, after `AUTANA_SESSION`, whose value labels the session a lookup is logged under ([Docs-Search.md](Docs-Search.md#what-it-logs)). |

```toml
idf_installs = ['C:\Espressif\v6.1']
build_root = 'C:\autana-builds'
docs_extra = ["notes/bench.md", "~/Documents/design-notes"]
records = "~/autana-records"
lock_hook = "python ~/bin/on_lock.py"
session_vars = ["WT_SESSION"]
```

The file takes `key = "..."` lines (`'...'` keeps a Windows path as typed) and
one-line arrays of them; anything else is refused. No environment variable is a
setting.
