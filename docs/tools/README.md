# Tooling

How the repository's own checks work, where a script's header is not enough.

| | |
|---|---|
| [Autana-CLI.md](Autana-CLI.md) | The `autana` terminal command: every command it takes, where it lives, and how it is put on the PATH. |
| [Device-Lock.md](Device-Lock.md) | `scripts/device/device.py`, the only thing that opens the board's serial port: the lock, flashing and capturing under it, where captures land, and recovery. |
| [Complexity-Gate.md](Complexity-Gate.md) | The cognitive-complexity ratchet: what it measures, the committed baseline, and when a score fails or only warns. |
| [Doc-Drift.md](Doc-Drift.md) | Ranking documents for review by age and changed cited sources, and recording a review in the ledger. |
| [Live-Tuning.md](Live-Tuning.md) | Changing a number on a running device by name, with no build and no flash: the commands, the console protocol, and making a constant tunable. |
| [Render-Harness.md](Render-Harness.md) | Rendering real firmware screens on a host: declaring a scene, what its pixels are pinned to, video output, the QEMU backend, and diffing against a device capture. |
| [Mermaid-Diagrams.md](Mermaid-Diagrams.md) | Validating ```` ```mermaid ```` diagrams with mermaid-cli: the command, the pre-commit step, and CI. |
| [Icon-Baker.md](Icon-Baker.md) | `gen_icons.py`: baking icons from a PNG atlas or SVG source into a generated header, what it rejects, and how the shipped artifact is tested. |
