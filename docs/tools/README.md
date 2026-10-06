# Tooling

To see real firmware screens without a board, start with the
[render harness](Render-Harness.md). To run portable logic tests, use the
[Testing Guide](../Testing-Guide.md). Board commands live in the
[`autana` CLI guide](Autana-CLI.md).

The rest of this index explains the repository's checks and tools in depth.

| | |
|---|---|
| [Autana-CLI.md](Autana-CLI.md) | The `autana` terminal command: every command it takes, where it lives, and how it is put on the PATH. |
| [Docs-Search.md](Docs-Search.md) | `autana docs`: answering a question from the documentation one section at a time, how it ranks, and the optional local models. |
| [Device-Lock.md](Device-Lock.md) | The board lock in `scripts/device/`, the only code that opens the serial port: what to do when the board is busy, what is and is not guaranteed, failure modes, running a shared rig or CI, and how it works. |
| [Flash-and-Captures.md](Flash-and-Captures.md) | What a flash proves, measuring with one lock across a flash and its captures, `send` and screenshots, where captures and records land, and wait estimates. |
| [Complexity-Gate.md](Complexity-Gate.md) | The cognitive-complexity ratchet: what it measures, the committed baseline, and when a score fails or only warns. |
| [Doc-Drift.md](Doc-Drift.md) | Ranking documents for review by age and changed cited sources, and recording a review in the ledger. |
| [Live-Tuning.md](Live-Tuning.md) | Changing a number on a running device by name, with no build and no flash: the commands, the console protocol, and making a constant tunable. |
| [Layout-Noise.md](Layout-Noise.md) | Why a neutral edit moves a timing, the seeded padding build (`--layout-seed`), and the pilot that measures how much a row moves with layout alone. |
| [Frame-Cost.md](Frame-Cost.md) | Measuring named stages of the frame loop and reading their console report. |
| [Render-Harness.md](Render-Harness.md) | Rendering real firmware screens on a host: declaring a scene, what its pixels are pinned to, video output, the QEMU backend, and diffing against a device capture. |
| [Mermaid-Diagrams.md](Mermaid-Diagrams.md) | Validating ```` ```mermaid ```` diagrams with mermaid-cli: the command, the pre-commit step, and CI. |
| [Math-Formulas.md](Math-Formulas.md) | Writing maths GitHub renders, and validating every formula with GitHub's MathJax configuration: the command, the pre-commit step, and CI. |
| [Generated-Files.md](Generated-Files.md) | Checked-in generated headers: the banner that names their command, and the gate that reruns it and compares bytes. |
| [Icon-Baker.md](Icon-Baker.md) | `gen_icons.py`: baking icons from a PNG atlas or SVG source into a generated header, what it rejects, and how the shipped artifact is tested. |
