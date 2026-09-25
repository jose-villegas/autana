# Tooling

To see real firmware screens without a board, start with the
[render harness](Render-Harness.md). To run portable logic tests, use the
[Testing Guide](../Testing-Guide.md). Board commands live in the
[`autana` CLI guide](Autana-CLI.md).

The rest of this index explains the repository's checks and tools in depth.

| | |
|---|---|
| [Autana-CLI.md](Autana-CLI.md) | The `autana` terminal command: every command it takes, where it lives, and how it is put on the PATH. |
| [Docs-Search.md](Docs-Search.md) | `autana docs`: answering a question from the documentation one section at a time, how it ranks, the optional local models, and the MCP server. |
| [Device-Lock.md](Device-Lock.md) | `scripts/device/device.py`, the only thing that opens the board's serial port: the lock, flashing and capturing under it, where captures land, and recovery. |
| [Complexity-Gate.md](Complexity-Gate.md) | The cognitive-complexity ratchet: what it measures, the committed baseline, and when a score fails or only warns. |
| [Doc-Drift.md](Doc-Drift.md) | Ranking documents for review by age and changed cited sources, and recording a review in the ledger. |
| [Live-Tuning.md](Live-Tuning.md) | Changing a number on a running device by name, with no build and no flash: the commands, the console protocol, and making a constant tunable. |
| [Frame-Cost.md](Frame-Cost.md) | Measuring named stages of the frame loop and reading their console report. |
| [Render-Harness.md](Render-Harness.md) | Rendering real firmware screens on a host: declaring a scene, what its pixels are pinned to, video output, the QEMU backend, and diffing against a device capture. |
| [Mermaid-Diagrams.md](Mermaid-Diagrams.md) | Validating ```` ```mermaid ```` diagrams with mermaid-cli: the command, the pre-commit step, and CI. |
| [Icon-Baker.md](Icon-Baker.md) | `gen_icons.py`: baking icons from a PNG atlas or SVG source into a generated header, what it rejects, and how the shipped artifact is tested. |
