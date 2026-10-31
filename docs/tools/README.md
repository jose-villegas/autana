# Tooling

To see real firmware screens without a board, start with the
[render harness](Render-Harness.md). To run portable logic tests, use the
[Testing Guide](../testing/README.md). Board commands live in the
[`autana` CLI guide](../cli/README.md), not here.

The rest of this index lists the repository's checks and tools.

## Where a script goes

| Kind of script | Goes in | Example |
|---|---|---|
| Gate: a check that fails a commit, push or CI run | `scripts/gates/` | `check-format.sh` |
| CI plumbing: what a workflow runs to reach a board or runner | `scripts/ci/` | `refresh_pr.py` |
| Firmware tool: builds, bakes, renders or reads the firmware | the folder under `launcher/tools/` for its job | `bake/bake.py` |
| App tool: serves one app only | the app's own folder, never the engine's tools | |
| Shared helper: sourced or imported by other scripts | `scripts/lib/` for gates and CI; `launcher/tools/` root for the firmware tools' shell helpers | `scripts/lib/pinned_tool.py` |

| | |
|---|---|
| [Build-Environment.md](Build-Environment.md) | Which ESP-IDF `autana build` picks and where its build folders live. |
| [Framework-Versions.md](Framework-Versions.md) | Where each framework's exact version lives, how a bump pull request arrives and what it regenerates, and how to match your own machine. |
| [Complexity-Gate.md](Complexity-Gate.md) | The cognitive-complexity ratchet: what it measures, the committed baseline, and when a score fails or only warns. |
| [Doc-Drift.md](Doc-Drift.md) | Ranking documents for review by age and changed cited sources, and recording a review in the ledger. |
| [Layout-Noise.md](Layout-Noise.md) | Why a neutral edit moves a timing, the seeded padding build (`--layout-seed`), and the pilot that measures how much a row moves with layout alone. |
| [Frame-Cost.md](Frame-Cost.md) | Measuring named stages of the frame loop and reading their console report. |
| [Console-Framing.md](Console-Framing.md) | How the device frames a block of data between begin and end lines on the console, and how a host script decodes it. |
| [Process-Trees.md](Process-Trees.md) | Starting and stopping a command as one process tree on Windows and POSIX, so no child outlives its parent. |
| [Render-Harness.md](Render-Harness.md) | Rendering real firmware screens on a host: declaring a scene, what its pixels are pinned to, video output, the QEMU backend, and diffing against a device capture. |
| [Pull-Requests.md](Pull-Requests.md) | The pull request description layout, the scaffold that fills its table from the branch, and its check. |
| [Citing-Sources.md](Citing-Sources.md) | Citing a paper or specification from a document or code: entry keys, shown numbers, "cited by" lists, and the check that rewrites them. |
| [Mermaid-Diagrams.md](Mermaid-Diagrams.md) | Validating ```` ```mermaid ```` diagrams with mermaid-cli: the command, the pre-commit step, and CI. |
| [Math-Formulas.md](Math-Formulas.md) | Writing maths GitHub renders, and validating every formula with GitHub's MathJax configuration: the command, the pre-commit step, and CI. |
| [Generated-Files.md](Generated-Files.md) | Checked-in generated headers: the banner that names their command, and the gate that reruns it and compares bytes. |
| [Icon-Baker.md](Icon-Baker.md) | Icons as pack content: the `.icons.toml` source, what the baker rejects, the one-bit image entry, drawing without the pack, and how the shipped icons are tested. |
